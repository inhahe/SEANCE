#include "graph_processor.h"
#include "wasm_script_processor.h"
#include "builtin_synth.h"
#include "terrain_synth.h"
#include "signal_shape_node.h"
#include "cache_processor.h"
#include "pan_processor.h"
#include "gain_processor.h"
#include "pitch_shift_processor.h"
#include "time_gate_processor.h"
#include "spectrum_tap.h"
#include "convolution_processor.h"
#include "soundfont_processor.h"
#include "builtin_effects.h"
#include "trigger_node.h"
#include "midi_mod_node.h"
#include "midi_input_node.h"
#include "drum_synth.h"
#include "spatializer_3d.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace SoundShop {

// ==============================================================================
// MidiTimelineProcessor — generates MIDI from timeline clips + audition
// ==============================================================================

MidiTimelineProcessor::MidiTimelineProcessor(Node& n, Transport& t) : node(n), transport(t) {
    // Load melody for performance mode
    if (!node.clips.empty())
        melodyPlayer.loadFromNode(node);
    melodyPlayer.setReleaseMode(node.performanceReleaseMode == 0
        ? MelodyPlayer::ReleaseMode::OnKeyUp
        : MelodyPlayer::ReleaseMode::OnNextEvent);
    melodyPlayer.setVelocitySensitive(node.performanceVelocity);
}

void MidiTimelineProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    buf.clear();

    // Performance mode — intercept incoming MIDI, replace with melody
    if (node.performanceMode && melodyPlayer.isActive()) {
        juce::MidiBuffer processedMidi;
        melodyPlayer.processMidi(midi, processedMidi);
        midi.swapWith(processedMidi);
        return; // don't add timeline events in performance mode
    }

    // Inject audition events
    {
        std::lock_guard<std::mutex> lock(*node.auditionMutex);
        for (auto& ev : node.pendingAudition) {
            if (ev.isNoteOn)
                midi.addEvent(juce::MidiMessage::noteOn(1, ev.pitch, (juce::uint8)ev.velocity), 0);
            else
                midi.addEvent(juce::MidiMessage::noteOff(1, ev.pitch), 0);
        }
        node.pendingAudition.clear();
    }

    // MPE pass-through: inject raw MIDI from hardware controller
    {
        std::lock_guard<std::mutex> lock(*node.mpePassthroughMutex);
        for (auto& [offset, msg] : node.pendingMpePassthrough)
            midi.addEvent(msg, offset);
        node.pendingMpePassthrough.clear();
    }

    // Generate MIDI from timeline clips when playing
    if (transport.playing) {
        double startBeat = transport.positionBeats();
        double endBeat = startBeat + transport.samplesToBeats(buf.getNumSamples());
        int numSamples = buf.getNumSamples();
        bool mpe = node.mpeEnabled;
        double nodeOffset = node.absoluteBeatOffset; // cascading group offset

        for (int ci = 0; ci < (int)node.clips.size(); ++ci) {
            auto& clip = node.clips[ci];
            for (int ni = 0; ni < (int)clip.notes.size(); ++ni) {
                auto& note = clip.notes[ni];
                double noteStart = nodeOffset + clip.startBeat + note.getOffset();
                double noteEnd = noteStart + note.getDuration();

                // Note on
                if (noteStart >= startBeat && noteStart < endBeat) {
                    int so = juce::jlimit(0, numSamples - 1,
                        (int)((noteStart - startBeat) / (endBeat - startBeat) * numSamples));
                    int ch = 1;
                    if (mpe) {
                        ch = allocMpeChannel(ci, ni, note.pitch);
                        // Send initial expression before note-on
                        emitExpression(midi, ch - 2, note.expression, 0.0f, so);
                    }
                    midi.addEvent(juce::MidiMessage::noteOn(ch,
                        juce::jlimit(0, 127, note.pitch), (juce::uint8)juce::jlimit(1, 127, note.velocity)), so);
                }

                // Note off
                if (noteEnd >= startBeat && noteEnd < endBeat) {
                    int so = juce::jlimit(0, numSamples - 1,
                        (int)((noteEnd - startBeat) / (endBeat - startBeat) * numSamples));
                    int ch = 1;
                    if (mpe) {
                        ch = findMpeChannel(ci, ni) + 2; // +2 because channels are 2-16
                        if (ch < 2) ch = 1;
                    }
                    midi.addEvent(juce::MidiMessage::noteOff(ch,
                        juce::jlimit(0, 127, note.pitch)), so);
                    if (mpe) freeMpeChannel(ci, ni);
                }

                // Continuous expression during note (MPE only)
                if (mpe && note.expression.hasData()) {
                    // Check if note is active during this block
                    if (noteStart < endBeat && noteEnd > startBeat &&
                        noteStart < startBeat) { // already started, not just starting
                        int mpeIdx = findMpeChannel(ci, ni);
                        if (mpeIdx >= 0) {
                            // Emit expression at sub-block intervals (~every 32 samples)
                            for (int s = 0; s < numSamples; s += 32) {
                                double beatAtS = startBeat + (endBeat - startBeat) * s / numSamples;
                                float beatInNote = (float)(beatAtS - noteStart);
                                emitExpression(midi, mpeIdx, note.expression, beatInNote,
                                               juce::jlimit(0, numSamples - 1, s));
                            }
                        }
                    }
                }
            }

            // CC events (non-MPE, channel as stored)
            for (auto& cc : clip.ccEvents) {
                double ccBeat = nodeOffset + clip.startBeat + cc.offset;
                if (ccBeat >= startBeat && ccBeat < endBeat) {
                    int so = juce::jlimit(0, numSamples - 1,
                        (int)((ccBeat - startBeat) / (endBeat - startBeat) * numSamples));
                    midi.addEvent(juce::MidiMessage::controllerEvent(
                        cc.channel, cc.controller, cc.value), so);
                }
            }
        }
    }

}

int MidiTimelineProcessor::allocMpeChannel(int ci, int ni, int pitch) {
    // Round-robin allocation across channels 2-16 (indices 0-14)
    for (int i = 0; i < kMpeChannels; ++i) {
        int idx = (nextMpeChannel + i) % kMpeChannels;
        if (!mpeChannels[idx].active) {
            mpeChannels[idx] = {ci, ni, pitch, true};
            nextMpeChannel = (idx + 1) % kMpeChannels;
            return idx + 2; // MIDI channel 2-16
        }
    }
    // All channels busy — steal the next one
    int idx = nextMpeChannel;
    mpeChannels[idx] = {ci, ni, pitch, true};
    nextMpeChannel = (idx + 1) % kMpeChannels;
    return idx + 2;
}

void MidiTimelineProcessor::freeMpeChannel(int ci, int ni) {
    for (int i = 0; i < kMpeChannels; ++i)
        if (mpeChannels[i].active && mpeChannels[i].clipIdx == ci && mpeChannels[i].noteIdx == ni)
            mpeChannels[i].active = false;
}

int MidiTimelineProcessor::findMpeChannel(int ci, int ni) const {
    for (int i = 0; i < kMpeChannels; ++i)
        if (mpeChannels[i].active && mpeChannels[i].clipIdx == ci && mpeChannels[i].noteIdx == ni)
            return i;
    return -1;
}

void MidiTimelineProcessor::emitExpression(juce::MidiBuffer& midi, int mpeIdx,
                                            const NoteExpression& expr, float beatInNote, int sampleOffset) {
    if (mpeIdx < 0 || mpeIdx >= kMpeChannels) return;
    int ch = mpeIdx + 2; // MIDI channel 2-16

    // Pitch bend: 0.5 = center (8192), range 0-16383
    if (!expr.pitchBend.empty()) {
        float val = NoteExpression::evaluate(expr.pitchBend, beatInNote, 0.5f);
        int pb = juce::jlimit(0, 16383, (int)(val * 16383.0f));
        midi.addEvent(juce::MidiMessage::pitchWheel(ch, pb), sampleOffset);
    }

    // Slide: CC74, 0-127
    if (!expr.slide.empty()) {
        float val = NoteExpression::evaluate(expr.slide, beatInNote, 0.5f);
        int cc = juce::jlimit(0, 127, (int)(val * 127.0f));
        midi.addEvent(juce::MidiMessage::controllerEvent(ch, 74, cc), sampleOffset);
    }

    // Pressure: channel aftertouch, 0-127
    if (!expr.pressure.empty()) {
        float val = NoteExpression::evaluate(expr.pressure, beatInNote, 0.0f);
        int pres = juce::jlimit(0, 127, (int)(val * 127.0f));
        midi.addEvent(juce::MidiMessage::channelPressureChange(ch, pres), sampleOffset);
    }
}

// ==============================================================================
// AudioTimelineProcessor — plays audio file clips
// ==============================================================================

AudioTimelineProcessor::AudioTimelineProcessor(Node& n, Transport& t, NodeGraph& g)
    : node(n), transport(t), graph(g) {
    formatManager.registerBasicFormats();
}

void AudioTimelineProcessor::prepareToPlay(double sr, int bs) {
    sampleRate = sr;
    blockSize = bs;
}

std::shared_ptr<AudioTimelineProcessor::LoadedAudio> AudioTimelineProcessor::getAudio(const std::string& path) {
    if (path.empty()) return nullptr;

    auto it = audioCache.find(path);
    if (it != audioCache.end()) return it->second;

    auto file = juce::File(path);
    if (!file.existsAsFile()) {
        fprintf(stderr, "Audio file not found: %s\n", path.c_str());
        return nullptr;
    }

    auto* reader = formatManager.createReaderFor(file);
    if (!reader) {
        fprintf(stderr, "Can't read audio file: %s\n", path.c_str());
        return nullptr;
    }

    auto loaded = std::make_shared<LoadedAudio>();
    loaded->reader.reset(reader);
    loaded->numChannels = (int)reader->numChannels;
    loaded->fileSampleRate = reader->sampleRate;
    loaded->totalSamples = reader->lengthInSamples;

    audioCache[path] = loaded;
    fprintf(stderr, "Loaded audio: %s (%d ch, %.0f Hz, %.1f sec)\n",
            path.c_str(), loaded->numChannels, loaded->fileSampleRate,
            loaded->totalSamples / loaded->fileSampleRate);
    return loaded;
}

void AudioTimelineProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    buf.clear();
    if (!transport.playing) return;

    int numSamples = buf.getNumSamples();
    int outChannels = buf.getNumChannels();
    double currentBeat = transport.positionBeats();
    double beatsPerSample = transport.bpm / (60.0 * sampleRate);
    double blockEndBeat = currentBeat + numSamples * beatsPerSample;

    // Auto-edge-fade so a clip never starts or ends with a hard sample edge.
    // The user's fadeIn/fadeOut settings are honored, but we always enforce
    // at least globalCrossfadeSec worth of fade to prevent clicks. Capped
    // to half the clip length so very short clips still play.
    double globalFadeBeats = std::max(0.0,
        (double)graph.globalCrossfadeSec * (double)transport.bpm / 60.0);

    for (auto& clip : node.clips) {
        if (clip.audioFilePath.empty()) continue;

        auto audio = getAudio(clip.audioFilePath);
        if (!audio || !audio->reader) continue;

        double clipStart = clip.startBeat;
        double clipEnd = clip.startBeat + clip.lengthBeats;
        double maxEdgeFade = std::max(0.0, clip.lengthBeats * 0.5);
        double effFadeIn  = std::min(maxEdgeFade,
            std::max((double)clip.fadeInBeats,  globalFadeBeats));
        double effFadeOut = std::min(maxEdgeFade,
            std::max((double)clip.fadeOutBeats, globalFadeBeats));

        // Skip if clip doesn't overlap this block
        if (currentBeat >= clipEnd || blockEndBeat <= clipStart) continue;

        float gainLinear = juce::Decibels::decibelsToGain(clip.gainDb);

        // Calculate which output samples correspond to this clip
        int firstSample = 0;
        if (currentBeat < clipStart)
            firstSample = (int)((clipStart - currentBeat) / beatsPerSample);

        int lastSample = numSamples;
        if (blockEndBeat > clipEnd)
            lastSample = (int)((clipEnd - currentBeat) / beatsPerSample);
        lastSample = std::min(lastSample, numSamples);

        if (firstSample >= lastSample) continue;
        int samplesToRead = lastSample - firstSample;

        // Calculate file position for the first sample
        double beatAtFirst = currentBeat + firstSample * beatsPerSample;
        double beatInClip = beatAtFirst - clipStart;
        double secondsInClip = beatInClip * 60.0 / transport.bpm;
        double fileSeconds = clip.slipOffset + secondsInClip;

        // Handle sample rate conversion
        double fileRatio = audio->fileSampleRate / sampleRate;
        int64_t fileSampleStart = (int64_t)(fileSeconds * audio->fileSampleRate);

        if (fileSampleStart < 0 || fileSampleStart >= audio->totalSamples) continue;

        // Read a block from the audio file
        int fileSamplesToRead = (int)(samplesToRead * fileRatio) + 2;
        fileSamplesToRead = std::min(fileSamplesToRead, (int)(audio->totalSamples - fileSampleStart));
        if (fileSamplesToRead <= 0) continue;

        int fileChannels = audio->numChannels;
        juce::AudioBuffer<float> readBuf(fileChannels, fileSamplesToRead);
        audio->reader->read(&readBuf, 0, fileSamplesToRead, fileSampleStart, true, true);

        // Copy to output with sample rate conversion (nearest neighbor for now)
        for (int s = 0; s < samplesToRead; ++s) {
            int outSample = firstSample + s;
            int fileSample = (int)(s * fileRatio);
            if (fileSample >= fileSamplesToRead) break;

            // Fade — uses effective edge-fade durations (max of user setting
            // and the project-wide globalCrossfadeSec) so clips never start
            // or end with a hard sample edge.
            float fade = 1.0f;
            double beatPos = currentBeat + outSample * beatsPerSample;
            double beatInC = beatPos - clipStart;
            if (effFadeIn > 0.0 && beatInC < effFadeIn)
                fade *= (float)(beatInC / effFadeIn);
            double beatsRemaining = clipEnd - beatPos;
            if (effFadeOut > 0.0 && beatsRemaining < effFadeOut)
                fade *= (float)(beatsRemaining / effFadeOut);

            float gain = gainLinear * fade;

            // Write to output channels
            for (int c = 0; c < outChannels; ++c) {
                int srcCh = std::min(c, fileChannels - 1);
                buf.addSample(c, outSample, readBuf.getSample(srcCh, fileSample) * gain);
            }
        }
    }
}

// ==============================================================================
// PassthroughProcessor — for nodes without plugins (test tone on MIDI)
// ==============================================================================

PassthroughProcessor::PassthroughProcessor(Node& n) : node(n) {}

void PassthroughProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    // If this is an instrument node with no plugin, generate a test sine tone
    if (node.type == NodeType::Instrument || node.type == NodeType::Effect) {
        for (auto metadata : midi) {
            auto msg = metadata.getMessage();
            if (msg.isNoteOn()) {
                float freq = (float)juce::MidiMessage::getMidiNoteInHertz(msg.getNoteNumber());
                for (int s = 0; s < buf.getNumSamples(); ++s) {
                    float sample = std::sin(phase) * 0.3f;
                    phase += 2.0f * juce::MathConstants<float>::pi * freq / (float)sampleRate;
                    if (phase > 2.0f * juce::MathConstants<float>::pi)
                        phase -= 2.0f * juce::MathConstants<float>::pi;
                    for (int c = 0; c < buf.getNumChannels(); ++c)
                        buf.addSample(c, s, sample);
                }
            } else if (msg.isNoteOff()) {
                phase = 0;
            }
        }
    }
    // For mixer/output: audio passes through (already summed by the graph)
}

// ==============================================================================
// GraphProcessor
// ==============================================================================

GraphProcessor::GraphProcessor() {
    processorGraph = std::make_unique<juce::AudioProcessorGraph>();
}

void GraphProcessor::prepare(NodeGraph& graph, double sr, int bs) {
    sampleRate = sr;
    blockSize = bs;
    processorGraph->setPlayConfigDetails(0, 2, sr, bs);
    processorGraph->prepareToPlay(sr, bs);
}

void GraphProcessor::rebuildGraph(NodeGraph& graph, Transport& transport) {
    processorGraph->clear();
    nodeMap.clear();
    nodeInputMap.clear();

    // Add an audio output node (graph sink)
    auto outNode = processorGraph->addNode(
        std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));
    outputNodeId = outNode->nodeID;

    // Create a processor for each of our nodes
    for (auto& node : graph.nodes) {
        std::unique_ptr<juce::AudioProcessor> proc;

        // Check cache: manual freeze or auto-cache
        bool useCache = false;
        if (node.cache.enabled && node.cache.valid) {
            useCache = true;
        } else if (node.cache.autoCache && cacheManager.isCacheValid(node, graph)) {
            // Auto-cache hit — try loading from disk if needed
            if (node.cache.useDisk && node.cache.left.empty())
                cacheManager.loadFromDisk(node);
            useCache = node.cache.hasCachedAudio();
        }

        if (useCache) {
            proc = std::make_unique<CachePlaybackProcessor>(node, transport);
            if (proc) {
                proc->enableAllBuses();
                auto graphNode = processorGraph->addNode(std::move(proc));
                if (graphNode) {
                    nodeMap[node.id] = graphNode->nodeID;
                    nodeInputMap[node.id] = graphNode->nodeID;
                }
            }
            continue;
        }

        if (node.type == NodeType::MidiTimeline) {
            proc = std::make_unique<MidiTimelineProcessor>(node, transport);
        } else if (node.type == NodeType::AudioTimeline) {
            proc = std::make_unique<AudioTimelineProcessor>(node, transport, graph);
        } else if (node.type == NodeType::Output) {
            // Our output node maps to the graph's audio output — skip creating a processor
            nodeMap[node.id] = outputNodeId;
            nodeInputMap[node.id] = outputNodeId;
            continue;
        } else if (node.type == NodeType::Script && !node.script.empty()) {
            // WASM script node — load .wasm file
            auto wasmProc = std::make_unique<WasmScriptProcessor>(node, transport);
            std::ifstream wf(node.script, std::ios::binary);
            if (wf) {
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(wf)),
                                            std::istreambuf_iterator<char>());
                if (wasmProc->loadWasm(bytes))
                    wasmProc->populateNodePins(node);
            }
            proc = std::move(wasmProc);
        } else if (node.plugin && node.plugin->instance) {
            // Real plugin — transfer ownership to the graph
            auto graphNode = processorGraph->addNode(std::move(node.plugin->instance));
            if (graphNode) {
                nodeMap[node.id] = graphNode->nodeID;
                nodeInputMap[node.id] = graphNode->nodeID;
                // Store the graph node ID so we can retrieve the processor later
                node.plugin->graphNodeId = graphNode->nodeID.uid;
            }
            continue;
        } else if (node.type == NodeType::Instrument && node.script == "__drumsynth__") {
            proc = std::make_unique<DrumSynthProcessor>(node);
        } else if (node.type == NodeType::Instrument &&
                   (node.script.rfind("__sf2__:", 0) == 0 ||
                    node.script.rfind("__sfz__:", 0) == 0)) {
            proc = std::make_unique<SoundFontProcessor>(node);
        } else if (node.type == NodeType::TerrainSynth || node.type == NodeType::Instrument) {
            // Unified synth: TerrainSynthProcessor handles everything
            // 1D waveforms (simple synths) and N-D terrains
            proc = std::make_unique<TerrainSynthProcessor>(node, transport);
        } else if (node.type == NodeType::SignalShape) {
            proc = std::make_unique<SignalShapeProcessor>(node, transport);
        } else if (node.type == NodeType::MidiInput) {
            proc = std::make_unique<MidiInputProcessor>(node);
        } else if (node.type == NodeType::Effect && node.script == "__spectrumtap__") {
            proc = std::make_unique<SpectrumTapProcessor>(node);
        } else if (node.type == NodeType::Effect &&
                   node.script.rfind("__convolution__:", 0) == 0) {
            proc = std::make_unique<ConvolutionProcessor>(node);
        } else if (node.type == NodeType::Effect && node.script == "__tremolo__") {
            proc = std::make_unique<TremoloProcessor>(node);
        } else if (node.type == NodeType::Effect && node.script == "__vibrato__") {
            proc = std::make_unique<VibratoProcessor>(node);
        } else if (node.type == NodeType::Effect && node.script == "__flanger__") {
            proc = std::make_unique<FlangerProcessor>(node);
        } else if (node.type == NodeType::Effect && node.script == "__phaser__") {
            proc = std::make_unique<PhaserProcessor>(node);
        } else if (node.type == NodeType::Effect && node.script == "__echo__") {
            proc = std::make_unique<EchoProcessor>(node);
        } else if (node.type == NodeType::Effect && node.script == "__compressor__") {
            proc = std::make_unique<CompressorProcessor>(node);
        } else if (node.type == NodeType::Effect && node.script == "__limiter__") {
            proc = std::make_unique<LimiterProcessor>(node);
        } else if (node.type == NodeType::Effect && node.script == "__gate__") {
            proc = std::make_unique<GateProcessor>(node);
        } else if (node.type == NodeType::Effect && node.script == "__arpeggiator__") {
            proc = std::make_unique<ArpeggiatorProcessor>(node);
        } else if (node.type == NodeType::Effect && node.script == "__mixture__") {
            proc = std::make_unique<MixtureProcessor>(node);
        } else if (node.type == NodeType::Effect &&
                   (node.script == "__velscale__" ||
                    node.script.rfind("__midimod__:", 0) == 0)) {
            proc = std::make_unique<MidiModulatorProcessor>(node);
        } else if (node.type == NodeType::Effect &&
                   node.script.rfind("__trigger__:", 0) == 0) {
            proc = std::make_unique<TriggerProcessor>(node, transport);
        } else if (node.type == NodeType::Effect && node.script == "__spatializer3d__") {
            proc = std::make_unique<Spatializer3DProcessor>(node);
        } else if (node.type == NodeType::Effect && node.script == "__pitchshift__") {
            proc = std::make_unique<PitchShiftProcessor>(node);
        } else {
            // No plugin — passthrough
            proc = std::make_unique<PassthroughProcessor>(node);
        }

        if (proc) {
            proc->enableAllBuses();
            auto graphNode = processorGraph->addNode(std::move(proc));
            if (graphNode) {
                // Insert a pan processor after audio-producing nodes
                if (node.type != NodeType::Output) {
                    auto panProc = std::make_unique<PanProcessor>(node, graph);
                    panProc->enableAllBuses();
                    auto panNode = processorGraph->addNode(std::move(panProc));
                    if (panNode) {
                        // Chain: node -> pan -> (downstream will connect to panNode)
                        processorGraph->addConnection({{graphNode->nodeID, 0}, {panNode->nodeID, 0}});
                        processorGraph->addConnection({{graphNode->nodeID, 1}, {panNode->nodeID, 1}});
                        processorGraph->addConnection({
                            {graphNode->nodeID, juce::AudioProcessorGraph::midiChannelIndex},
                            {panNode->nodeID, juce::AudioProcessorGraph::midiChannelIndex}});
                        nodeMap[node.id] = panNode->nodeID;       // downstream pulls audio from pan
                        nodeInputMap[node.id] = graphNode->nodeID; // upstream pushes MIDI/audio into the actual processor
                    } else {
                        nodeMap[node.id] = graphNode->nodeID;
                        nodeInputMap[node.id] = graphNode->nodeID;
                    }
                } else {
                    nodeMap[node.id] = graphNode->nodeID;
                    nodeInputMap[node.id] = graphNode->nodeID;
                }
            }
        }
    }

    // Create connections based on our links
    for (auto& link : graph.links) {
        // Find source node and pin
        int srcNodeId = -1, dstNodeId = -1;
        PinKind srcKind = PinKind::Audio;
        int srcPinIdx = 0, dstPinIdx = 0;

        for (auto& node : graph.nodes) {
            int outIdx = 0;
            for (auto& pin : node.pinsOut) {
                if (pin.id == link.startPin) {
                    srcNodeId = node.id;
                    srcKind = pin.kind;
                    srcPinIdx = outIdx;
                }
                outIdx++;
            }
            int inIdx = 0;
            for (auto& pin : node.pinsIn) {
                if (pin.id == link.endPin) {
                    dstNodeId = node.id;
                    dstPinIdx = inIdx;
                }
                inIdx++;
            }
        }

        if (srcNodeId < 0 || dstNodeId < 0) continue;
        if (nodeMap.find(srcNodeId) == nodeMap.end() || nodeInputMap.find(dstNodeId) == nodeInputMap.end()) continue;

        // Source: pull audio FROM the (pan-extended) output side via nodeMap.
        // Destination: push MIDI/audio INTO the actual processor via
        // nodeInputMap. Otherwise MIDI events would dead-end at a Pan
        // processor and never reach the destination synth.
        auto srcGraphId = nodeMap[srcNodeId];
        auto dstGraphId = nodeInputMap[dstNodeId];

        // If this link has time-gated effect regions on any node, insert a
        // TimeGateProcessor that silences audio outside the active regions.
        auto effectiveSrc = srcGraphId;
        {
            bool hasRegions = false;
            for (auto& node : graph.nodes)
                for (auto& region : node.effectRegions)
                    if (region.linkId == link.id ||
                        (region.groupId >= 0 && graph.findEffectGroup(region.groupId))) {
                        // Check if this group contains our link
                        if (region.linkId == link.id) { hasRegions = true; break; }
                        auto* grp = graph.findEffectGroup(region.groupId);
                        if (grp) {
                            for (int lid : grp->linkIds)
                                if (lid == link.id) { hasRegions = true; break; }
                        }
                        if (hasRegions) break;
                    }

            if (hasRegions) {
                // Find the source node for the TimeGateProcessor's node reference
                Node* srcNode = nullptr;
                for (auto& n : graph.nodes)
                    if (n.id == srcNodeId) { srcNode = &n; break; }

                if (srcNode) {
                    auto gateProc = std::make_unique<TimeGateProcessor>(
                        link.id, *srcNode, graph, transport);
                    gateProc->enableAllBuses();
                    auto gateNode = processorGraph->addNode(std::move(gateProc));
                    if (gateNode) {
                        processorGraph->addConnection({{effectiveSrc, 0}, {gateNode->nodeID, 0}});
                        processorGraph->addConnection({{effectiveSrc, 1}, {gateNode->nodeID, 1}});
                        processorGraph->addConnection({
                            {effectiveSrc, juce::AudioProcessorGraph::midiChannelIndex},
                            {gateNode->nodeID, juce::AudioProcessorGraph::midiChannelIndex}});
                        effectiveSrc = gateNode->nodeID;
                    }
                }
            }
        }

        // If this link has gain != 0 dB, insert a gain processor
        if (link.gainDb != 0.0f) {
            auto gainProc = std::make_unique<GainProcessor>(link.gainDb);
            gainProc->enableAllBuses();
            auto gainNode = processorGraph->addNode(std::move(gainProc));
            if (gainNode) {
                // Route: src -> gain -> dst
                processorGraph->addConnection({{srcGraphId, 0}, {gainNode->nodeID, 0}});
                processorGraph->addConnection({{srcGraphId, 1}, {gainNode->nodeID, 1}});
                processorGraph->addConnection({
                    {srcGraphId, juce::AudioProcessorGraph::midiChannelIndex},
                    {gainNode->nodeID, juce::AudioProcessorGraph::midiChannelIndex}});
                effectiveSrc = gainNode->nodeID;
            }
        }

        // Connect based on pin kind. Param and Signal both flow through the
        // audio-rate control slot (channels 2+) — they are interchangeable at
        // the cable level (task #82). The receiver decides per-block vs
        // per-sample consumption; the channel layout is identical either way.
        bool srcIsControl = (srcKind == PinKind::Signal || srcKind == PinKind::Param);
        if (srcIsControl) {
            // Find which control-input slot this is on the destination — count
            // Param + Signal pins encountered before the matching pin id.
            int signalChIdx = 2;
            for (auto& dstNode : graph.nodes) {
                if (dstNode.id != dstNodeId) continue;
                int sigCount = 0;
                for (auto& pin : dstNode.pinsIn) {
                    if (pin.id == link.endPin) { signalChIdx = 2 + sigCount; break; }
                    if (pin.kind == PinKind::Signal || pin.kind == PinKind::Param) sigCount++;
                }
                break;
            }
            processorGraph->addConnection({{effectiveSrc, 0}, {dstGraphId, signalChIdx}});
        } else {
            // Audio + MIDI: connect as before
            processorGraph->addConnection({{effectiveSrc, 0}, {dstGraphId, 0}});
            processorGraph->addConnection({{effectiveSrc, 1}, {dstGraphId, 1}});
            processorGraph->addConnection({
                {effectiveSrc, juce::AudioProcessorGraph::midiChannelIndex},
                {dstGraphId, juce::AudioProcessorGraph::midiChannelIndex}
            });
        }
    }

    lastNodeCount = (int)graph.nodes.size();
    lastLinkCount = (int)graph.links.size();

    auto dbg = [](const juce::String& s) {
        // Goes to VS Debug Output window AND stderr (when run from a console).
        juce::Logger::writeToLog(s);
    };

    dbg("[GraphProcessor] Rebuilt: " + juce::String((int)nodeMap.size())
        + " nodes mapped, "
        + juce::String((int)processorGraph->getConnections().size())
        + " connections");

    // Diagnostic: dump bus layouts for every node so we can see if a Waveform
    // Synth is reporting an unexpected channel count.
    for (auto& kv : nodeMap) {
        if (auto* gn = processorGraph->getNodeForId(kv.second)) {
            if (auto* p = gn->getProcessor()) {
                dbg("  node " + juce::String(kv.first)
                    + " (" + p->getName() + "): inCh="
                    + juce::String(p->getTotalNumInputChannels())
                    + " outCh=" + juce::String(p->getTotalNumOutputChannels())
                    + " acceptsMidi=" + juce::String((int)p->acceptsMidi())
                    + " producesMidi=" + juce::String((int)p->producesMidi()));
            }
        }
    }
}

void GraphProcessor::processBlock(NodeGraph& graph, Transport& transport,
                                   float* const* outputData, int numChannels, int numSamples) {
    // Check if graph needs rebuilding
    if (rebuildRequested.exchange(false) ||
        (int)graph.nodes.size() != lastNodeCount ||
        (int)graph.links.size() != lastLinkCount) {
        rebuildGraph(graph, transport);
        if (sampleRate > 0)
            processorGraph->prepareToPlay(sampleRate, numSamples);
    }

    // Process the graph
    juce::AudioBuffer<float> buf(numChannels, numSamples);
    buf.clear();
    juce::MidiBuffer midi;
    processorGraph->processBlock(buf, midi);

    // Copy to output
    for (int c = 0; c < std::min(numChannels, buf.getNumChannels()); ++c)
        std::memcpy(outputData[c], buf.getReadPointer(c), sizeof(float) * numSamples);

    // Metronome: generate clicks on beat boundaries
    if (graph.metronomeEnabled && transport.playing) {
        double startBeat = transport.positionBeats();
        double beatsPerSample = transport.bpm / (60.0 * sampleRate);

        for (int s = 0; s < numSamples; ++s) {
            double beat = startBeat + s * beatsPerSample;
            double beatFrac = beat - std::floor(beat);

            // Click at the start of each beat (within first ~2ms)
            if (beatFrac < 0.01 || beatFrac > 0.99) {
                double bpb = transport.timeSigMap.beatsPerBar(beat);
                bool isDownbeat = std::abs(std::fmod(beat + 0.01, bpb) - 0.01) < 0.1;
                float freq = isDownbeat ? 1500.0f : 1000.0f; // higher pitch on downbeat
                float amp = isDownbeat ? 0.4f : 0.25f;

                // Short click: ~3ms exponential decay sine
                float clickPhase = (float)(beatFrac * sampleRate / (transport.bpm / 60.0));
                if (beatFrac > 0.5) clickPhase = (float)((1.0 - beatFrac) * sampleRate / (transport.bpm / 60.0));
                float env = std::exp(-clickPhase * 0.03f);
                float sample = std::sin(clickPhase * freq * 2.0f * 3.14159265f / (float)sampleRate) * amp * env;

                for (int c = 0; c < numChannels; ++c)
                    outputData[c][s] += sample;
            }
        }
    }
}

void GraphProcessor::applyAutomation(const std::vector<AutomationValue>& values) {
    if (!processorGraph) return;
    automation.applyValues(values, *processorGraph, nodeMap);
    automation.setLatestValues(values);
}

juce::AudioProcessor* GraphProcessor::getProcessorForNode(int nodeId) {
    if (!processorGraph) return nullptr;
    auto it = nodeMap.find(nodeId);
    if (it == nodeMap.end()) return nullptr;
    auto graphNode = processorGraph->getNodeForId(it->second);
    return graphNode ? graphNode->getProcessor() : nullptr;
}

} // namespace SoundShop
