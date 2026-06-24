#include "spectrum_tap.h"
#include "effect_regions.h" // for getDistinctColor
#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

namespace SoundShop {

// ==============================================================================
// Helpers
// ==============================================================================

static constexpr const char* kSpectrumTapPrefix = "__spectrumtap__";

static std::vector<std::string> splitScriptFields(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t n = s.find(sep, p);
        if (n == std::string::npos) n = s.size();
        out.push_back(s.substr(p, n - p));
        p = n + 1;
    }
    return out;
}

static bool isPureInteger(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

std::string SpectrumTapProcessor::encodeScript(const std::vector<FrequencyBin>& bins,
                                               int fftSize)
{
    // Drop trailing biquad bins so the encoded form is as compact as possible.
    // A bin that references an asset is always custom, so referenced slots are
    // covered by lastCustom too.
    int lastCustom = -1;
    for (int i = 0; i < (int)bins.size(); ++i)
        if (bins[i].useCustomResponse || bins[i].responseCurveAssetId >= 0)
            lastCustom = i;

    // Nothing custom AND default FFT size -> bare tag (round-trips legacy).
    if (lastCustom < 0 && fftSize == kDefaultFftSize) return kSpectrumTapPrefix;

    std::ostringstream o;
    o << kSpectrumTapPrefix << '|' << fftSize;
    for (int i = 0; i <= lastCustom; ++i) {
        o << '|';
        if (bins[i].useCustomResponse)
            o << bins[i].responseCurve.encode();
    }

    // Trailing reference section: `#refs|<slot>:<assetId>|...`. Only emitted
    // when at least one bin live-references a FrequencyGraph asset.
    bool anyRef = false;
    for (int i = 0; i <= lastCustom; ++i)
        if (bins[i].responseCurveAssetId >= 0) { anyRef = true; break; }
    if (anyRef) {
        o << '|' << "#refs";
        for (int i = 0; i <= lastCustom; ++i)
            if (bins[i].responseCurveAssetId >= 0)
                o << '|' << i << ':' << bins[i].responseCurveAssetId;
    }
    return o.str();
}

void SpectrumTapProcessor::decodeScript(const std::string& script,
                                        int& outFftSize,
                                        std::vector<SpectralCurve>& outCurves,
                                        std::vector<bool>& outUseCustom,
                                        std::vector<int>& outAssetIds)
{
    outFftSize = kDefaultFftSize;
    outCurves.clear();
    outUseCustom.clear();
    outAssetIds.clear();
    if (script.rfind(kSpectrumTapPrefix, 0) != 0) return;
    if (script.size() == std::strlen(kSpectrumTapPrefix)) return;
    if (script[std::strlen(kSpectrumTapPrefix)] != '|') return;
    std::string body = script.substr(std::strlen(kSpectrumTapPrefix) + 1);
    auto fields = splitScriptFields(body, '|');
    if (fields.empty()) return;

    // The very first iteration of the per-bin-curve format had no FFT
    // size header: the body was just the curve list. Detect this by
    // checking whether the first field looks like a curve payload
    // (starts with "eq,"/"drawnpt,"/"drawnfh,") rather than a bare int.
    size_t curveStart = 0;
    if (isPureInteger(fields[0])) {
        try { outFftSize = std::stoi(fields[0]); } catch (...) {}
        // Snap to a sane power-of-two range.
        if (outFftSize < kMinFftSize) outFftSize = kMinFftSize;
        if (outFftSize > kMaxFftSize) outFftSize = kMaxFftSize;
        curveStart = 1;
    }

    // The curve list runs until the optional `#refs` marker (or end of fields).
    size_t refsMarker = fields.size();
    for (size_t i = curveStart; i < fields.size(); ++i)
        if (fields[i] == "#refs") { refsMarker = i; break; }

    size_t nCurves = refsMarker - curveStart;
    outCurves.resize(nCurves);
    outUseCustom.assign(nCurves, false);
    outAssetIds.assign(nCurves, -1);
    for (size_t i = 0; i < nCurves; ++i) {
        const std::string& f = fields[curveStart + i];
        if (f.empty()) continue;
        SpectralCurve c;
        if (SpectralCurve::decode(f, c)) {
            outCurves[i] = std::move(c);
            outUseCustom[i] = true;
        }
    }

    // Parse the trailing `<slot>:<assetId>` reference entries.
    for (size_t i = refsMarker + 1; i < fields.size(); ++i) {
        const std::string& f = fields[i];
        size_t colon = f.find(':');
        if (colon == std::string::npos) continue;
        std::string slotStr = f.substr(0, colon);
        std::string idStr   = f.substr(colon + 1);
        if (!isPureInteger(slotStr) || !isPureInteger(idStr)) continue;
        size_t slot = 0; int id = -1;
        try { slot = (size_t) std::stoul(slotStr); id = std::stoi(idStr); }
        catch (...) { continue; }
        if (slot < outAssetIds.size()) outAssetIds[slot] = id;
    }
}

int resolveSpectrumTapReferences(NodeGraph& graph) {
    int resolved = 0;
    for (auto& node : graph.nodes) {
        if (node.type != NodeType::Effect) continue;
        if (node.script.rfind("__spectrumtap__", 0) != 0) continue;

        int fft = SpectrumTapProcessor::kDefaultFftSize;
        std::vector<SpectralCurve> curves;
        std::vector<bool> useCustom;
        std::vector<int> assetIds;
        SpectrumTapProcessor::decodeScript(node.script, fft, curves, useCustom, assetIds);

        bool changed = false;
        for (size_t i = 0; i < assetIds.size(); ++i) {
            if (assetIds[i] < 0) continue;
            const AssetEntry* e = graph.assets.find(assetIds[i]);
            if (e && e->kind == AssetKind::FrequencyGraph) {
                SpectralCurve c;
                if (SpectralCurve::decode(e->payload, c)) {
                    curves[i] = std::move(c);
                    useCustom[i] = true;
                    ++resolved;
                    changed = true;
                }
            } else {
                // Referenced asset is gone -> detach to independent, keeping the
                // last cached curve so the bin doesn't dangle.
                assetIds[i] = -1;
                changed = true;
            }
        }

        if (changed) {
            std::vector<FrequencyBin> tmp(curves.size());
            for (size_t i = 0; i < curves.size(); ++i) {
                tmp[i].useCustomResponse  = useCustom[i];
                tmp[i].responseCurve      = curves[i];
                tmp[i].responseCurveAssetId = (i < assetIds.size()) ? assetIds[i] : -1;
            }
            node.script = SpectrumTapProcessor::encodeScript(tmp, fft);
        }
    }
    return resolved;
}

// ==============================================================================
// SpectrumTapProcessor - inline audio passthrough + frequency bin analysis
// ==============================================================================

SpectrumTapProcessor::SpectrumTapProcessor(Node& n) : node(n) {}

void SpectrumTapProcessor::prepareToPlay(double sr, int) {
    sampleRate = sr;
    rebuildBins();
}

void SpectrumTapProcessor::rebuildBins() {
    // Each param named "Bin N: xxxHz" defines a bin. Parse center freq and
    // bandwidth from the param's min/max: min = centerFreq, max = bandwidth.
    // The param's value is the current amplitude (written by processBlock).
    //
    // Per-bin custom response curves live in node.script using the format
    // "__spectrumtap__|<curve1>|<curve2>|..." - empty slots leave the bin
    // in biquad mode. Curves are paired with bins positionally (i-th bin
    // gets i-th curve slot).
    bins.clear();

    int decodedFftSize = kDefaultFftSize;
    std::vector<SpectralCurve> curves;
    std::vector<bool> useCustom;
    std::vector<int> assetIds;
    decodeScript(node.script, decodedFftSize, curves, useCustom, assetIds);
    if (decodedFftSize != fftSize) {
        fftSize = decodedFftSize;
        // Force re-allocation of the FFT machinery on the next ensureFFTReady()
        // by dropping the existing instance. (rebuildBins also calls
        // ensureFFTReady when anyCustomBin is true, which will then allocate
        // a new FFT with the updated size.)
        fft.reset();
    }

    int binIdx = 0;
    for (int i = 0; i < (int)node.params.size(); ++i) {
        auto& p = node.params[i];
        if (p.name.rfind("Bin ", 0) != 0) continue;
        FrequencyBin bin;
        bin.centerFreq = p.minVal;
        bin.bandwidth = p.maxVal;
        bin.color = getDistinctColor(binIdx);
        bin.computeCoeffs(sampleRate);
        if (binIdx < (int)useCustom.size() && useCustom[binIdx]) {
            bin.useCustomResponse = true;
            bin.responseCurve = curves[binIdx];
        }
        if (binIdx < (int)assetIds.size())
            bin.responseCurveAssetId = assetIds[binIdx];
        bins.push_back(std::move(bin));
        binIdx++;
    }

    anyCustomBin = false;
    for (const auto& b : bins) if (b.useCustomResponse) { anyCustomBin = true; break; }

    if (anyCustomBin) {
        ensureFFTReady();
        rebuildBinFftWeights();
    } else {
        binFftWeights.clear();
    }
}

void SpectrumTapProcessor::ensureFFTReady() {
    if (!fft || fft->size() != fftSize) {
        fft = std::make_unique<FFT>(fftSize);
        fftInputRing.assign(fftSize, 0.0f);
        fftWriteIdx = 0;
        fftScratchTime.assign(fftSize, {0.0f, 0.0f});
        fftScratchFreq.assign(fftSize / 2 + 1, {0.0f, 0.0f});
        fftMag.assign(fftSize / 2 + 1, 0.0f);
    }
}

void SpectrumTapProcessor::rebuildBinFftWeights() {
    const int halfBins = fftSize / 2 + 1;
    binFftWeights.assign(bins.size(), {});
    for (size_t bi = 0; bi < bins.size(); ++bi) {
        const auto& b = bins[bi];
        if (!b.useCustomResponse) continue;
        const float leftHz  = std::max(0.0f, b.centerFreq - b.bandwidth * 0.5f);
        const float rightHz = std::max(leftHz + 1.0f, b.centerFreq + b.bandwidth * 0.5f);
        const float spanHz  = rightHz - leftHz;

        // Sample the curve at a moderately dense grid then linearly interp
        // to FFT-bin positions. Keeps the per-block math cheap.
        const int N = 256;
        auto curveSamples = b.responseCurve.evaluate(N);
        if ((int)curveSamples.size() != N) curveSamples.assign(N, 0.0f);

        std::vector<float> weights(halfBins, 0.0f);
        for (int k = 0; k < halfBins; ++k) {
            float fHz = (float)k * (float)sampleRate / (float)fftSize;
            if (fHz < leftHz || fHz > rightHz) continue;
            float x = (fHz - leftHz) / spanHz;
            float idx = x * (float)(N - 1);
            int i0 = juce::jlimit(0, N - 1, (int)idx);
            int i1 = std::min(N - 1, i0 + 1);
            float frac = idx - (float)i0;
            float w = curveSamples[i0] * (1.0f - frac) + curveSamples[i1] * frac;
            weights[k] = std::max(0.0f, w);
        }
        binFftWeights[bi] = std::move(weights);
    }
}

void SpectrumTapProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) {
    // Check if bins need rebuilding (params may have changed from UI)
    if ((int)bins.size() != [&]() {
        int count = 0;
        for (auto& p : node.params)
            if (p.name.rfind("Bin ", 0) == 0) count++;
        return count;
    }()) {
        rebuildBins();
    }

    int numSamples = buf.getNumSamples();
    if (numSamples == 0 || bins.empty()) return;

    // Use channel 0 (mono analysis). Audio passes through unchanged.
    const float* input = buf.getReadPointer(0);
    int numChannels = buf.getNumChannels();

    // Cache the param indices for each bin once per block (the bins vector
    // is rebuilt in lock-step with the "Bin " params, so the i-th bin maps
    // to the i-th "Bin " param).
    std::vector<int> binParamIdx(bins.size(), -1);
    {
        int count = 0;
        for (int pi = 0; pi < (int)node.params.size(); ++pi) {
            if (node.params[pi].name.rfind("Bin ", 0) == 0) {
                if (count < (int)bins.size())
                    binParamIdx[count] = pi;
                count++;
            }
        }
    }

    // Each bin writes its scaled amplitude into channel 2 + binIndex of the
    // output buffer. Audio channels 0+1 pass through untouched. Downstream
    // nodes connect to the bin's Signal Out pin and receive the per-sample
    // envelope; signal_modulation.h reads channel 2+slot at the destination.
    // Signal pins use the standard 0..1 range here (spectrum amplitudes are
    // inherently unipolar - silence = 0, loud = 1 - and consumers using the
    // bipolar additive modulation in signal_modulation.h scale this by depth
    // anyway).
    std::vector<float*> sigOut(bins.size(), nullptr);
    for (int bi = 0; bi < (int)bins.size(); ++bi) {
        int ch = 2 + bi;
        if (ch < numChannels)
            sigOut[bi] = buf.getWritePointer(ch);
    }

    // Push samples into the FFT ring buffer and trigger one FFT per block
    // (only when custom-response bins exist). The FFT writes a per-block
    // envelope target for each custom bin; per-sample smoothing toward
    // that target lives in the inner loop below.
    std::vector<float> customTarget(bins.size(), 0.0f);
    bool haveCustomTarget = false;
    if (anyCustomBin && fft) {
        ensureFFTReady();
        for (int s = 0; s < numSamples; ++s) {
            fftInputRing[fftWriteIdx] = input[s];
            fftWriteIdx = (fftWriteIdx + 1) % fftSize;
        }
        // Copy ring -> contiguous time-domain buffer with a Hann window so
        // strong tones don't smear across half the spectrum.
        const float twoPi = 6.28318530717958647692f;
        for (int i = 0; i < fftSize; ++i) {
            int src = (fftWriteIdx + i) % fftSize;
            float win = 0.5f * (1.0f - std::cos(twoPi * (float)i / (float)(fftSize - 1)));
            fftScratchTime[i] = { fftInputRing[src] * win, 0.0f };
        }
        fft->forward(fftScratchTime);
        const int halfBins = fftSize / 2 + 1;
        const float normalise = 2.0f / (float)fftSize;
        for (int k = 0; k < halfBins; ++k)
            fftMag[k] = std::abs(fftScratchTime[k]) * normalise;
        for (int bi = 0; bi < (int)bins.size(); ++bi) {
            if (!bins[bi].useCustomResponse) continue;
            if ((int)binFftWeights.size() <= bi) continue;
            const auto& w = binFftWeights[bi];
            if ((int)w.size() != halfBins) continue;
            double sum = 0.0;
            for (int k = 0; k < halfBins; ++k) sum += (double)fftMag[k] * w[k];
            customTarget[bi] = (float)sum;
        }
        haveCustomTarget = true;
    }

    for (int s = 0; s < numSamples; ++s) {
        float sample = input[s];
        for (int bi = 0; bi < (int)bins.size(); ++bi) {
            float scaled;
            if (bins[bi].useCustomResponse) {
                // Smooth toward the per-block FFT target. This reuses the
                // same one-pole smoothing as the biquad path so the two
                // modes have consistent attack/release character.
                float target = haveCustomTarget ? customTarget[bi] : 0.0f;
                bins[bi].envelope += bins[bi].smoothCoeff * (target - bins[bi].envelope);
                scaled = std::min(1.0f, bins[bi].envelope * 4.0f);
            } else {
                float amp = bins[bi].process(sample);
                scaled = std::min(1.0f, amp * 4.0f); // scale for visibility
            }
            if (sigOut[bi]) sigOut[bi][s] = scaled;
            // Also write into the param's value for the UI bar display.
            // Stomping the same float per sample is wasted work but the cost
            // is negligible compared to the biquad/envelope math and it
            // keeps the UI in lock-step with the latest envelope value.
            if (binParamIdx[bi] >= 0)
                node.params[binParamIdx[bi]].value = scaled;
        }
    }
    // Audio passes through unchanged on channels 0+1.
}

// ==============================================================================
// SpectrumTapComponent - UI for managing bins and viewing levels
// ==============================================================================

SpectrumTapComponent::SpectrumTapComponent(NodeGraph& g, int nid,
                                            std::function<void()> onTopo)
    : graph(g), nodeId(nid), onTopologyChanged(std::move(onTopo))
{
    addAndMakeVisible(addBinBtn);
    addBinBtn.setTooltip("Add a new frequency band to monitor. Each bin outputs an amplitude signal you can use "
                         "to drive other parameters - useful for spectrum-following effects, vocoder-like routing, etc.");
    addBinBtn.onClick = [this]() {
        auto* nd = graph.findNode(nodeId);
        if (!nd) return;
        int binCount = 0;
        for (auto& p : nd->params)
            if (p.name.rfind("Bin ", 0) == 0) binCount++;
        // New bin at a default frequency, spaced logarithmically
        float freq = 200.0f * std::pow(2.0f, (float)binCount);
        freq = std::min(freq, 16000.0f);
        float bw = freq * 0.3f; // 30% bandwidth
        // Param: name="Bin N: XHz", min=centerFreq, max=bandwidth, value=amplitude
        juce::String name = "Bin " + juce::String(binCount + 1) + ": "
                          + juce::String((int)freq) + "Hz";
        nd->params.push_back({name.toStdString(), 0.0f, freq, bw});
        // syncBinPins will append a matching Signal Out pin so the bin's
        // amplitude is routable as a control signal.
        syncBinPins();
        syncCurveStateCount();
        syncCurvesToScript();
        graph.dirty = true;
        if (onTopologyChanged) onTopologyChanged();
        repaint();
    };

    // Reconcile pins on open. Projects saved before per-bin Signal pins
    // existed will have bin params but no matching pins - syncBinPins
    // creates the pins (and fires the topology callback so the audio
    // graph rebuilds to route them).
    loadCurveStatesFromScript();
    syncCurveStateCount();
    if (syncBinPins() && onTopologyChanged) onTopologyChanged();

    // FFT size selector. Only affects bins with a custom response curve
    // (biquad bins are per-sample and unaffected). Smaller sizes -> lower
    // latency and CPU, but coarser frequency resolution per FFT bin, so
    // narrow bins or sharply detailed response curves lose accuracy.
    addAndMakeVisible(fftSizeLabel);
    fftSizeLabel.setJustificationType(juce::Justification::centredRight);
    fftSizeLabel.setFont(11.0f);

    addAndMakeVisible(fftSizeCombo);
    int comboId = 1;
    int currentId = 1;
    for (int sz : { 128, 256, 512, 1024, 2048, 4096, 8192 }) {
        fftSizeCombo.addItem(juce::String(sz), comboId);
        if (sz == fftSize) currentId = comboId;
        ++comboId;
    }
    fftSizeCombo.setSelectedId(currentId, juce::dontSendNotification);
    fftSizeCombo.setTooltip(
        "Number of frequency bins used by the custom-response FFT. "
        "Smaller sizes reduce latency (latency = size / sample rate, "
        "so 256 @ 44.1 kHz is ~6 ms while 4096 is ~93 ms) and CPU cost, "
        "but give coarser frequency resolution per bin. Has no effect on "
        "bins using the default biquad response.");
    fftSizeCombo.onChange = [this]() {
        int sel = fftSizeCombo.getSelectedId();
        // Map back to the size by walking the same list used to populate.
        int idx = 0;
        for (int sz : { 128, 256, 512, 1024, 2048, 4096, 8192 }) {
            if (++idx == sel) { fftSize = sz; break; }
        }
        syncCurvesToScript();
    };

    startTimerHz(30);
    setSize(500, 300);
}

int SpectrumTapComponent::countBinParams() const {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return 0;
    int count = 0;
    for (auto& p : nd->params)
        if (p.name.rfind("Bin ", 0) == 0) ++count;
    return count;
}

void SpectrumTapComponent::syncCurveStateCount() {
    int target = countBinParams();
    if ((int)binCurveStates.size() == target) return;
    if ((int)binCurveStates.size() < target)
        binCurveStates.resize(target, BinCurveState{});
    else
        binCurveStates.resize(target);
}

void SpectrumTapComponent::loadCurveStatesFromScript() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    int decodedFft = SpectrumTapProcessor::kDefaultFftSize;
    std::vector<SpectralCurve> curves;
    std::vector<bool> useCustom;
    std::vector<int> assetIds;
    SpectrumTapProcessor::decodeScript(nd->script, decodedFft, curves, useCustom, assetIds);
    fftSize = decodedFft;
    binCurveStates.clear();
    binCurveStates.reserve(std::max(curves.size(), useCustom.size()));
    for (size_t i = 0; i < curves.size(); ++i) {
        BinCurveState st;
        st.useCustom = (i < useCustom.size()) ? useCustom[i] : false;
        st.curve     = std::move(curves[i]);
        st.assetId   = (i < assetIds.size()) ? assetIds[i] : -1;
        binCurveStates.push_back(std::move(st));
    }
}

void SpectrumTapComponent::syncCurvesToScript() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    // Build a transient FrequencyBin vector just to reuse the canonical
    // encoder. We only need the useCustomResponse/responseCurve fields.
    std::vector<FrequencyBin> tmp(binCurveStates.size());
    for (size_t i = 0; i < binCurveStates.size(); ++i) {
        tmp[i].useCustomResponse    = binCurveStates[i].useCustom;
        tmp[i].responseCurve        = binCurveStates[i].curve;
        tmp[i].responseCurveAssetId = binCurveStates[i].assetId;
    }
    std::string newScript = SpectrumTapProcessor::encodeScript(tmp, fftSize);
    if (nd->script == newScript) return;
    nd->script = std::move(newScript);
    graph.dirty = true;
    // The audio processor caches the curves in rebuildBins(), so request a
    // rebuild so it picks up the changes. This is what onTopologyChanged
    // already does.
    if (onTopologyChanged) onTopologyChanged();
}

void SpectrumTapComponent::commitCurveEdit(int binSlotIdx) {
    // No write-back: a linked curve is read-only, so edits only happen on an
    // independent curve with no asset to propagate to. A shared library item is
    // changed only by editing it in the library.
    juce::ignoreUnused(binSlotIdx);
    syncCurvesToScript();
}

void SpectrumTapComponent::openResponseEditor(int binSlotIdx) {
    syncCurveStateCount();
    if (binSlotIdx < 0 || binSlotIdx >= (int)binCurveStates.size()) return;
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;

    // Capture the bin's frequency range and color for the dialog title /
    // curve colour. The slot index also indexes the i-th "Bin " param.
    int paramIdx = -1, seen = 0;
    for (int i = 0; i < (int)nd->params.size(); ++i) {
        if (nd->params[i].name.rfind("Bin ", 0) != 0) continue;
        if (seen == binSlotIdx) { paramIdx = i; break; }
        ++seen;
    }
    if (paramIdx < 0) return;
    const float center = nd->params[paramIdx].minVal;
    const float bw     = nd->params[paramIdx].maxVal;
    const float leftHz  = std::max(0.0f, center - bw * 0.5f);
    const float rightHz = center + bw * 0.5f;
    const juce::String binLabel = juce::String(nd->params[paramIdx].name);

    // Flip the slot into custom mode (so the curve takes effect immediately
    // even before any edits) and ensure the curve has sensible defaults.
    auto& state = binCurveStates[binSlotIdx];
    if (!state.useCustom) {
        state.useCustom = true;
        if (state.curve.expression.empty())
            state.curve.expression = "1 - abs(2*f - 1)"; // triangle peak
    }
    syncCurvesToScript();

    // Build a small container: the panel + Library / Close / "Reset" buttons
    // plus a status label showing the current library link.
    class ResponseEditorContainer : public juce::Component {
    public:
        SpectralCurvePanel panel;
        juce::Label      linkLabel;
        juce::TextButton libraryBtn { "Library..." };
        juce::TextButton closeBtn { "Close" };
        juce::TextButton resetBtn { "Use Default (bandpass)" };
        ResponseEditorContainer(SpectralCurve& curve,
                                const juce::String& title,
                                juce::Colour colour,
                                std::function<void()> onChange)
            : panel(curve, title, 0.0f, 1.0f, colour, std::move(onChange))
        {
            addAndMakeVisible(panel);
            addAndMakeVisible(linkLabel);
            addAndMakeVisible(libraryBtn);
            addAndMakeVisible(closeBtn);
            addAndMakeVisible(resetBtn);
            linkLabel.setFont(11.0f);
            linkLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFAAAAAA));
            libraryBtn.setTooltip(
                "Publish this response curve to the project's Frequency Graphs "
                "library, load a copy of a library curve (independent), or sync "
                "this bin to one as a live, read-only mirror. A synced curve is "
                "edited only in the library; click the panel's read-only badge to "
                "fork an editable copy.");
            setSize(560, 388);
        }
        void resized() override {
            auto a = getLocalBounds().reduced(6);
            auto bottom = a.removeFromBottom(28);
            closeBtn.setBounds(bottom.removeFromRight(80));
            bottom.removeFromRight(6);
            resetBtn.setBounds(bottom.removeFromRight(180));
            bottom.removeFromRight(6);
            libraryBtn.setBounds(bottom.removeFromRight(90));
            auto linkRow = a.removeFromBottom(20);
            linkLabel.setBounds(linkRow);
            panel.setBounds(a);
        }
    };

    auto* container = new ResponseEditorContainer(
        state.curve,
        "Response - " + binLabel,
        juce::Colour(state.useCustom ? 0xFF55C8FF : 0xFFAAAAAA),
        [this, binSlotIdx]() { commitCurveEdit(binSlotIdx); });

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(container);
    opts.dialogTitle = "Spectrum Tap: Custom Response (" +
        juce::String((int)leftHz) + "-" + juce::String((int)rightHz) + " Hz)";
    opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.resizable = true;
    opts.componentToCentreAround = this;
    auto* dlg = opts.launchAsync();

    int slotCapture = binSlotIdx;

    // Refresh the "Independent / Linked to ..." status line from current state.
    auto updateLinkLabel = [this, slotCapture, container]() {
        if (slotCapture < 0 || slotCapture >= (int)binCurveStates.size()) return;
        int aid = binCurveStates[slotCapture].assetId;
        if (aid < 0) {
            container->linkLabel.setText("Independent curve (not in library)",
                                         juce::dontSendNotification);
        } else {
            const AssetEntry* e = graph.assets.find(aid);
            juce::String nm = e ? juce::String(e->name) : juce::String("(missing)");
            container->linkLabel.setText("Linked to library curve: " + nm +
                                         " (#" + juce::String(aid) +
                                         ")  - read only (click the badge to edit a copy)",
                                         juce::dontSendNotification);
        }
        container->panel.setReadOnly(aid >= 0);
    };
    updateLinkLabel();

    // Read-only badge → break the library link and keep an independent copy
    // (the only unlink path; see SpectralCurvePanel::onUnlink).
    container->panel.onUnlink = [this, slotCapture, container, updateLinkLabel]() {
        if (slotCapture < 0 || slotCapture >= (int)binCurveStates.size()) return;
        auto& s = binCurveStates[slotCapture];
        s.assetId = -1;      // detach from library
        s.useCustom = true;  // keep the (now independent) curve active
        container->panel.syncFromModel();
        syncCurvesToScript();
        updateLinkLabel();
        container->panel.repaint();
    };

    container->closeBtn.onClick = [dlg]() {
        if (dlg) dlg->exitModalState(0);
    };
    container->resetBtn.onClick = [this, slotCapture, dlg]() {
        if (slotCapture >= 0 && slotCapture < (int)binCurveStates.size()) {
            binCurveStates[slotCapture].useCustom = false;
            binCurveStates[slotCapture].assetId = -1;  // detach from library too
        }
        syncCurvesToScript();
        if (dlg) dlg->exitModalState(0);
        repaint();
    };
    container->libraryBtn.onClick = [this, slotCapture, container, updateLinkLabel,
                                     binLabel]() {
        if (slotCapture < 0 || slotCapture >= (int)binCurveStates.size()) return;
        auto& st = binCurveStates[slotCapture];
        // Shared publish / link / detach menu (curve_editor.cpp). It mirrors a
        // picked asset's curve into st.curve directly; our callback persists the
        // new link id and refreshes the UI.
        showFrequencyGraphLibraryMenu(&container->libraryBtn, graph, st.curve,
            st.assetId, "Response " + binLabel,
            [this, slotCapture, container, updateLinkLabel](int newId) {
                if (slotCapture < 0 || slotCapture >= (int)binCurveStates.size()) return;
                auto& s = binCurveStates[slotCapture];
                s.assetId = newId;
                s.useCustom = true;   // any publish/link/detach keeps the curve active
                container->panel.syncFromModel();
                syncCurvesToScript();
                updateLinkLabel();
                container->panel.repaint();
            });
    };
}

bool SpectrumTapComponent::syncBinPins() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return false;
    bool changed = false;

    // Snapshot the current bin names in order.
    std::vector<std::string> binNames;
    for (auto& p : nd->params)
        if (p.name.rfind("Bin ", 0) == 0)
            binNames.push_back(p.name);

    // Find existing Signal Out pin positions (indices into pinsOut),
    // in the order they appear.
    std::vector<int> sigPinPositions;
    for (int i = 0; i < (int)nd->pinsOut.size(); ++i) {
        auto& pin = nd->pinsOut[i];
        if (pin.kind == PinKind::Signal && pin.name.rfind("Bin ", 0) == 0)
            sigPinPositions.push_back(i);
    }

    // Add missing pins (one per bin without a pin yet).
    while (sigPinPositions.size() < binNames.size()) {
        int binIdx = (int)sigPinPositions.size();
        Pin pin;
        pin.id = graph.allocId();
        pin.name = binNames[binIdx];
        pin.kind = PinKind::Signal;
        pin.isInput = false;
        pin.channels = 1;
        nd->pinsOut.push_back(pin);
        sigPinPositions.push_back((int)nd->pinsOut.size() - 1);
        changed = true;
    }

    // Drop excess pins (bin deleted while pin still around) and any
    // links connected to those pins.
    while (sigPinPositions.size() > binNames.size()) {
        int posInPinsOut = sigPinPositions.back();
        int pinId = nd->pinsOut[posInPinsOut].id;
        nd->pinsOut.erase(nd->pinsOut.begin() + posInPinsOut);
        {
            std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
            graph.links.erase(
                std::remove_if(graph.links.begin(), graph.links.end(),
                    [pinId](const auto& l) { return l.startPin == pinId; }),
                graph.links.end());
        }
        sigPinPositions.pop_back();
        changed = true;
    }

    // Sync pin names to their bin's current name (so frequency drags update
    // the pin label without a full topology change).
    for (int i = 0; i < (int)binNames.size(); ++i) {
        auto& pin = nd->pinsOut[sigPinPositions[i]];
        if (pin.name != binNames[i]) {
            pin.name = binNames[i];
            changed = true;
        }
    }

    return changed;
}

juce::Rectangle<float> SpectrumTapComponent::getSpectrumArea() const {
    return getLocalBounds().toFloat().reduced(12).withTrimmedTop(32).withTrimmedBottom(16);
}

float SpectrumTapComponent::hzToX(float hz) const {
    auto area = getSpectrumArea();
    // Log scale: 20 Hz -> left edge, 20000 Hz -> right edge
    float logMin = std::log2(20.0f);
    float logMax = std::log2(20000.0f);
    float logHz = std::log2(std::max(20.0f, hz));
    float frac = (logHz - logMin) / (logMax - logMin);
    return area.getX() + frac * area.getWidth();
}

float SpectrumTapComponent::xToHz(float x) const {
    auto area = getSpectrumArea();
    float frac = (x - area.getX()) / area.getWidth();
    float logMin = std::log2(20.0f);
    float logMax = std::log2(20000.0f);
    return std::pow(2.0f, logMin + frac * (logMax - logMin));
}

void SpectrumTapComponent::resized() {
    auto top = getLocalBounds().reduced(12).removeFromTop(28);
    addBinBtn.setBounds(top.removeFromLeft(100));
    top.removeFromLeft(8);
    // FFT-size group on the right of the top bar.
    auto right = top.removeFromRight(180);
    fftSizeCombo.setBounds(right.removeFromRight(100));
    right.removeFromRight(4);
    fftSizeLabel.setBounds(right);
}

void SpectrumTapComponent::mouseDown(const juce::MouseEvent& e) {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    auto area = getSpectrumArea();
    if (!area.contains(e.position)) return;

    // Right-click on a bin opens a context menu with delete and response-curve
    // editing options. Clicks off any bin do nothing.
    if (e.mods.isRightButtonDown()) {
        float clickHz = xToHz(e.position.x);
        int hitParamIdx = -1;
        int hitBinSlot = -1;
        int binSlot = 0;
        for (int i = 0; i < (int)nd->params.size(); ++i) {
            if (nd->params[i].name.rfind("Bin ", 0) != 0) continue;
            float center = nd->params[i].minVal;
            float bw = nd->params[i].maxVal;
            if (std::abs(clickHz - center) < bw) {
                hitParamIdx = i;
                hitBinSlot = binSlot;
                break;
            }
            ++binSlot;
        }
        if (hitParamIdx < 0) return;

        syncCurveStateCount();
        const bool isCustom = (hitBinSlot < (int)binCurveStates.size())
                           && binCurveStates[hitBinSlot].useCustom;

        juce::PopupMenu menu;
        menu.addItem(1, "Edit response curve...");
        menu.addItem(2, "Use default response (bandpass)",
                     /*isActive*/ isCustom);
        menu.addSeparator();
        menu.addItem(3, "Delete bin");

        int paramIdxCap = hitParamIdx;
        int binSlotCap  = hitBinSlot;
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
            [this, paramIdxCap, binSlotCap](int result) {
                auto* nd2 = graph.findNode(nodeId);
                if (!nd2) return;
                if (result == 1) {
                    openResponseEditor(binSlotCap);
                } else if (result == 2) {
                    if (binSlotCap >= 0 && binSlotCap < (int)binCurveStates.size()) {
                        binCurveStates[binSlotCap].useCustom = false;
                        binCurveStates[binSlotCap].assetId = -1;
                    }
                    syncCurvesToScript();
                    repaint();
                } else if (result == 3) {
                    if (paramIdxCap >= 0 && paramIdxCap < (int)nd2->params.size()) {
                        nd2->params.erase(nd2->params.begin() + paramIdxCap);
                        if (binSlotCap >= 0 && binSlotCap < (int)binCurveStates.size())
                            binCurveStates.erase(binCurveStates.begin() + binSlotCap);
                        syncBinPins();
                        syncCurvesToScript();
                        graph.dirty = true;
                        if (onTopologyChanged) onTopologyChanged();
                        repaint();
                    }
                }
            });
        return;
    }

    // Left-click: find if clicking on a bin's center or edge
    float clickHz = xToHz(e.position.x);
    int binIdx = 0;
    for (int i = 0; i < (int)nd->params.size(); ++i) {
        if (nd->params[i].name.rfind("Bin ", 0) != 0) continue;
        float center = nd->params[i].minVal;
        float bw = nd->params[i].maxVal;
        float left = center - bw * 0.5f;
        float right = center + bw * 0.5f;

        float leftX = hzToX(left);
        float rightX = hzToX(right);
        float centerX = hzToX(center);

        if (std::abs(e.position.x - leftX) < 8) {
            dragBinIdx = i; dragWhat = DragLeft; return;
        }
        if (std::abs(e.position.x - rightX) < 8) {
            dragBinIdx = i; dragWhat = DragRight; return;
        }
        if (e.position.x >= leftX && e.position.x <= rightX) {
            dragBinIdx = i; dragWhat = DragCenter; return;
        }
        binIdx++;
    }
}

void SpectrumTapComponent::mouseDrag(const juce::MouseEvent& e) {
    auto* nd = graph.findNode(nodeId);
    if (!nd || dragBinIdx < 0 || dragBinIdx >= (int)nd->params.size()) return;
    auto& p = nd->params[dragBinIdx];

    float hz = std::max(20.0f, std::min(20000.0f, xToHz(e.position.x)));
    float center = p.minVal;
    float bw = p.maxVal;

    if (dragWhat == DragCenter) {
        p.minVal = hz; // move center
    } else if (dragWhat == DragLeft) {
        float right = center + bw * 0.5f;
        float newLeft = std::min(hz, right - 10.0f);
        p.minVal = (newLeft + right) * 0.5f;
        p.maxVal = right - newLeft;
    } else if (dragWhat == DragRight) {
        float left = center - bw * 0.5f;
        float newRight = std::max(hz, left + 10.0f);
        p.minVal = (left + newRight) * 0.5f;
        p.maxVal = newRight - left;
    }

    // Update the param name to reflect new frequency
    p.name = ("Bin " + juce::String(dragBinIdx + 1) + ": "
             + juce::String((int)p.minVal) + "Hz").toStdString();
    // Keep the Signal Out pin's name in sync (just a rename, no
    // topology change - no rebuild needed).
    syncBinPins();
    graph.dirty = true;
    repaint();
}

void SpectrumTapComponent::mouseUp(const juce::MouseEvent&) {
    dragBinIdx = -1;
}

void SpectrumTapComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));
    auto area = getSpectrumArea();

    // Background
    g.setColour(juce::Colour(18, 20, 28));
    g.fillRoundedRectangle(area, 4.0f);
    g.setColour(juce::Colour(50, 55, 70));
    g.drawRoundedRectangle(area, 4.0f, 1.0f);

    // Frequency axis labels (log scale)
    g.setColour(juce::Colours::grey);
    g.setFont(9.0f);
    for (float hz : {20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f,
                     5000.0f, 10000.0f, 20000.0f}) {
        float x = hzToX(hz);
        g.setColour(juce::Colour(35, 38, 50));
        g.drawVerticalLine((int)x, area.getY(), area.getBottom());
        g.setColour(juce::Colours::grey);
        juce::String label = (hz >= 1000) ? juce::String((int)(hz / 1000)) + "k"
                                          : juce::String((int)hz);
        g.drawText(label, (int)x - 15, (int)area.getBottom() + 1, 30, 14,
                   juce::Justification::centred);
    }

    // Draw bins
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;

    int binCount = 0;
    for (int i = 0; i < (int)nd->params.size(); ++i) {
        auto& p = nd->params[i];
        if (p.name.rfind("Bin ", 0) != 0) continue;

        float center = p.minVal;
        float bw = p.maxVal;
        float amplitude = p.value; // 0..1, written by processor

        float leftX = hzToX(center - bw * 0.5f);
        float rightX = hzToX(center + bw * 0.5f);
        float binW = rightX - leftX;

        auto binColor = juce::Colour(getDistinctColor(binCount));

        // Bin background (full height, dim)
        g.setColour(binColor.withAlpha(0.15f));
        g.fillRect(leftX, area.getY(), binW, area.getHeight());

        // Amplitude fill (from bottom up)
        float fillH = amplitude * area.getHeight();
        g.setColour(binColor.withAlpha(0.6f));
        g.fillRect(leftX, area.getBottom() - fillH, binW, fillH);

        // Border
        g.setColour(binColor.withAlpha(0.8f));
        g.drawRect(leftX, area.getY(), binW, area.getHeight(), 1.0f);

        // Custom-response overlay: trace the user-drawn curve across the
        // bin's range so the chosen response shape is visible at a glance.
        bool isCustom = (binCount < (int)binCurveStates.size())
                     && binCurveStates[binCount].useCustom;
        if (isCustom && binW > 4.0f) {
            const int N = std::max(8, (int)binW);
            auto samples = binCurveStates[binCount].curve.evaluate(N);
            juce::Path curvePath;
            for (int i = 0; i < (int)samples.size(); ++i) {
                float t = (samples.size() > 1) ? (float)i / (float)(samples.size() - 1) : 0.0f;
                float px = leftX + t * binW;
                float v = juce::jlimit(0.0f, 1.0f, samples[i]);
                float py = area.getBottom() - v * area.getHeight();
                if (i == 0) curvePath.startNewSubPath(px, py);
                else        curvePath.lineTo(px, py);
            }
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.strokePath(curvePath, juce::PathStrokeType(1.4f));
            // Marker chip at the top-right of the bin
            g.setColour(juce::Colours::white.withAlpha(0.7f));
            g.setFont(8.5f);
            g.drawText("custom", (int)rightX - 38, (int)area.getY() + 2, 36, 12,
                       juce::Justification::centredRight, false);
        }

        // Label
        g.setColour(juce::Colours::white);
        g.setFont(9.0f);
        g.drawText(juce::String((int)center) + " Hz",
                   (int)leftX + 2, (int)area.getY() + 2, (int)binW - 4, 12,
                   juce::Justification::centredLeft, false);

        // Resize handles (small bars at left/right edges)
        g.setColour(binColor.brighter(0.4f));
        g.fillRect(leftX - 1, area.getY(), 3.0f, area.getHeight());
        g.fillRect(rightX - 1, area.getY(), 3.0f, area.getHeight());

        binCount++;
    }

    // Title
    g.setColour(juce::Colours::white.withAlpha(0.7f));
    g.setFont(12.0f);
    g.drawText("Spectrum Tap - drag bins, edges to resize, right-click for menu (delete / custom response)",
               getLocalBounds().reduced(120, 6).removeFromTop(24),
               juce::Justification::centredLeft);
}

} // namespace SoundShop
