#include "plugin_copies.h"
#include "graph_processor.h"
#include "node_graph.h"
#include <climits>

namespace SoundShop {

PluginCopies::PluginCopies(Use u) : use(u) {}

PluginCopies::~PluginCopies() {
    stopTimer();
    cancelPendingUpdate();
    std::map<int, Master> following;
    {
        std::lock_guard<std::mutex> lk(mtx);
        following.swap(masters);
    }
    for (auto& [id, m] : following)
        if (auto owner = m.owner.lock(); owner && owner->instance.get() == m.processor)
            m.processor->removeListener(this);
    // Copies a graph still plays go with that graph (it shares them); the rest
    // go here.
}

void PluginCopies::update(NodeGraph& graph, PluginHost& host, GraphProcessor* live,
                          double sampleRate, int blockSize) {
    // 1. What the graph needs: each plugin node with a plugin to copy, where
    //    that plugin is, and how many copies.
    struct Want {
        int nodeId = -1;
        int voices = 0;   // 0: a render's main graph, one copy (slot -1)
        juce::AudioProcessor* source = nullptr;
        std::shared_ptr<PluginHost::LoadedPlugin> owner;
        juce::PluginDescription description;
    };
    std::vector<Want> wants;
    for (auto& n : graph.nodes) {
        if (!n.isPluginNode() || !n.plugin) continue;   // not loaded (yet), or failed to
        Want w;
        w.nodeId = n.id;
        w.owner = n.plugin;
        w.description = n.plugin->info.description;
        if (n.voiceContainerId >= 0) {
            const Node* container = graph.findNode(n.voiceContainerId);
            if (container == nullptr || container->type != NodeType::VoiceContainer) continue;
            w.voices = juce::jlimit(1, 64, container->voicePolyphony);   // as PolyVoiceProcessor
            w.source = n.plugin->instance.get();                        // the master
        } else {
            if (use != Use::render) continue;
            // Playing in the live graph - or loaded, and not taken in by it yet.
            w.source = n.plugin->instance ? n.plugin->instance.get()
                                          : (live != nullptr ? live->hostedPluginOf(n) : nullptr);
        }
        if (w.source == nullptr) continue;
        wants.push_back(std::move(w));
    }

    // 2. Keep the copies that are still right; let go of the rest.
    std::vector<std::pair<size_t, int>> toLoad;   // (index into wants, slot)
    {
        std::lock_guard<std::mutex> lk(mtx);
        std::set<Key> wanted;
        for (size_t i = 0; i < wants.size(); ++i) {
            const auto& w = wants[i];
            const int count = w.voices > 0 ? w.voices : 1;
            for (int k = 0; k < count; ++k) {
                const Key key { w.nodeId, w.voices > 0 ? k : -1 };
                wanted.insert(key);
                auto it = entries.find(key);
                if (it != entries.end()) {
                    if (it->second.source.lock() == w.owner) continue;   // a copy of this very plugin
                    unused.push_back(std::move(it->second.copy));       // of one the node had before
                    entries.erase(it);
                }
                toLoad.push_back({ i, key.second });
            }
        }
        for (auto it = entries.begin(); it != entries.end();) {
            if (wanted.count(it->first)) { ++it; continue; }
            unused.push_back(std::move(it->second.copy));
            it = entries.erase(it);
        }
        // A failure stands only for the plugin instance it happened with, and
        // only while copies are missing (fewer voices may need no more).
        std::set<int> missing;
        for (const auto& [index, slot] : toLoad)
            missing.insert(wants[index].nodeId);
        for (auto it = failures.begin(); it != failures.end();) {
            bool current = false;
            for (const auto& w : wants)
                if (w.nodeId == it->first && it->second.source.lock() == w.owner)
                    current = true;
            it = current && missing.count(it->first) ? std::next(it) : failures.erase(it);
        }
    }

    // 3. Load the missing ones - the slow part, outside the lock.
    std::map<const juce::AudioProcessor*, juce::MemoryBlock> states;   // one query per plugin
    for (const auto& [index, slot] : toLoad) {
        const auto& w = wants[index];
        {
            std::lock_guard<std::mutex> lk(mtx);
            auto f = failures.find(w.nodeId);
            if (f != failures.end() && f->second.source.lock() == w.owner) {
                if (!f->second.retry)
                    continue;       // it didn't load for this plugin before; it won't now
                failures.erase(f);  // try again - recorded again if it fails again
            }
        }
        const bool blocked = host.isBlocked(w.description);
        std::string error;
        auto instance = host.instantiate(w.description, sampleRate, blockSize, &error);
        if (!instance) {
            std::lock_guard<std::mutex> lk(mtx);
            failures[w.nodeId] = { w.owner, error, blocked };
            continue;
        }
        if (!instance->setBusesLayout(w.source->getBusesLayout()))
            instance->enableAllBuses();
        instance->setNonRealtime(use == Use::render);
        auto st = states.find(w.source);
        if (st == states.end()) {
            st = states.emplace(w.source, juce::MemoryBlock()).first;
            if (w.source == w.owner->instance.get())   // no graph plays it
                catchUp(*w.source);
            w.source->getStateInformation(st->second);
        }
        if (st->second.getSize() > 0)
            instance->setStateInformation(st->second.getData(), (int) st->second.getSize());
        instance->prepareToPlay(sampleRate, blockSize);

        auto copy = std::make_shared<Copy>();
        copy->plugin = std::move(instance);
        copy->preparedRate = sampleRate;
        copy->preparedBlock = blockSize;
        std::lock_guard<std::mutex> lk(mtx);
        entries[{ w.nodeId, slot }] = { std::move(copy), w.owner };
    }

    // 4. Follow the masters, for their parameter and state changes.
    if (use == Use::voices) {
        std::map<int, Master> now;
        for (const auto& w : wants)
            if (w.voices > 0)
                now[w.nodeId] = { w.source, w.owner };
        auto same = [](const Master& a, const Master& b) {
            return a.processor == b.processor && a.owner.lock() == b.owner.lock();
        };
        std::vector<Master> stopFollowing, startFollowing;
        {
            std::lock_guard<std::mutex> lk(mtx);
            for (const auto& [id, m] : masters) {
                auto it = now.find(id);
                if (it == now.end() || !same(it->second, m)) stopFollowing.push_back(m);
            }
            for (const auto& [id, m] : now) {
                auto it = masters.find(id);
                if (it == masters.end() || !same(it->second, m)) startFollowing.push_back(m);
            }
            masters = now;
        }
        // Not under the lock: JUCE calls listeners holding none of its own, but
        // a listener call in flight would wait on ours.
        for (auto& m : stopFollowing)
            if (auto owner = m.owner.lock(); owner && owner->instance.get() == m.processor)
                m.processor->removeListener(this);
        for (auto& m : startFollowing)
            m.processor->addListener(this);
    }

    // A render also names the plugins it goes without because they hadn't
    // finished loading (an export straight after opening a project).
    if (use == Use::render) {
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& n : graph.nodes) {
            if (!n.isPluginNode() || n.plugin
                || (n.pluginLoadState != PluginLoadState::Pending
                    && n.pluginLoadState != PluginLoadState::Loading))
                continue;
            const auto who = n.pluginDescription.name.isNotEmpty()
                                 ? PluginHost::describeForUser(n.pluginDescription)
                                 : "'" + n.name + "'";
            failures[n.id] = { {}, who + " hadn't finished loading.", false };
        }
    }

    releaseUnused();
}

std::shared_ptr<PluginCopies::Copy> PluginCopies::find(int nodeId, int slot) const {
    std::lock_guard<std::mutex> lk(mtx);
    auto it = entries.find({ nodeId, slot });
    return it != entries.end() ? it->second.copy : nullptr;
}

std::map<int, std::string> PluginCopies::problems() const {
    std::lock_guard<std::mutex> lk(mtx);
    std::map<int, std::string> out;
    for (const auto& [id, f] : failures)
        out[id] = f.error;
    return out;
}

juce::String PluginCopies::describeProblems(const juce::String& what) const {
    const auto list = problems();
    if (list.empty()) return {};
    const bool one = list.size() == 1;
    juce::String text = what + " went without "
                        + (one ? juce::String("a plugin") : juce::String((int) list.size()) + " plugins")
                        + ". A render plays its own copy of each plugin, and "
                        + (one ? "this one's" : "these") + " didn't load:\n";
    for (const auto& [id, error] : list)
        text += "\n" + juce::String::fromUTF8(error.c_str());
    return text;
}

int PluginCopies::size() const {
    std::lock_guard<std::mutex> lk(mtx);
    return (int) entries.size();
}

std::vector<std::shared_ptr<PluginCopies::Copy>> PluginCopies::copiesOf(int nodeId) const {
    std::vector<std::shared_ptr<Copy>> out;
    for (auto it = entries.lower_bound({ nodeId, INT_MIN });
         it != entries.end() && it->first.first == nodeId; ++it)
        out.push_back(it->second.copy);
    return out;
}

int PluginCopies::masterNodeOf(const juce::AudioProcessor* p) const {
    for (const auto& [id, m] : masters)
        if (m.processor == p)
            return id;
    return -1;
}

void PluginCopies::catchUp(juce::AudioProcessor& plugin) {
    // A few samples: within any block size a plugin can have been prepared
    // for. Not getBlockSize(), which JUCE's LV2 host leaves at 0 for an
    // instance prepared directly (it never calls setRateAndBufferSizeDetails).
    const int known = plugin.getBlockSize();
    const int block = known > 0 ? juce::jmin(64, known) : 16;
    juce::AudioBuffer<float> silence(juce::jmax(1, plugin.getTotalNumInputChannels(),
                                                plugin.getTotalNumOutputChannels()),
                                     block);
    silence.clear();
    juce::MidiBuffer noMidi;
    const juce::ScopedLock sl(plugin.getCallbackLock());
    plugin.processBlock(silence, noMidi);
}

void PluginCopies::syncFromMaster(int nodeId) {
    std::shared_ptr<PluginHost::LoadedPlugin> owner;   // keeps the master alive meanwhile
    juce::AudioProcessor* master = nullptr;
    std::vector<std::shared_ptr<Copy>> targets;
    {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = masters.find(nodeId);
        if (it == masters.end()) return;
        owner = it->second.owner.lock();
        if (!owner || owner->instance.get() != it->second.processor) return;
        master = it->second.processor;
        targets = copiesOf(nodeId);
    }
    if (targets.empty()) return;
    catchUp(*master);
    juce::MemoryBlock state;
    master->getStateInformation(state);
    if (state.getSize() == 0) return;
    for (auto& c : targets)
        c->plugin->setStateInformation(state.getData(), (int) state.getSize());
}

bool PluginCopies::setParameter(int nodeId, int index, float value, bool* changed) {
    std::shared_ptr<PluginHost::LoadedPlugin> owner;   // keeps the master alive meanwhile
    juce::AudioProcessor* master = nullptr;
    std::vector<std::shared_ptr<Copy>> targets;
    {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = masters.find(nodeId);
        if (it == masters.end()) return false;
        owner = it->second.owner.lock();
        if (!owner || owner->instance.get() != it->second.processor) return false;
        master = it->second.processor;
        targets = copiesOf(nodeId);
    }
    auto set = [index, value](juce::AudioProcessor& p) {
        const auto& params = p.getParameters();
        if (index >= 0 && index < params.size())
            params[index]->setValue(value);
    };
    if (changed != nullptr) {
        const auto& params = master->getParameters();
        *changed = index >= 0 && index < params.size()
                   && std::abs(params[index]->getValue() - value) > 1.0e-5f;
    }
    set(*master);
    for (auto& c : targets)
        if (c && c->plugin) set(*c->plugin);
    return true;
}

std::vector<PluginCopies::ParamEvent> PluginCopies::drainParamEvents() {
    std::lock_guard<std::mutex> lk(mtx);
    std::vector<ParamEvent> out;
    out.swap(paramEvents);
    return out;
}

std::vector<std::shared_ptr<PluginCopies::Copy>> PluginCopies::copiesOfNode(int nodeId) const {
    std::lock_guard<std::mutex> lk(mtx);
    return copiesOf(nodeId);
}

void PluginCopies::queueEvent(int nodeId, int index, int kind, float value) {
    if (paramEvents.size() < 4096)   // bounded, should the UI stall
        paramEvents.push_back({ nodeId, index, kind, value });
}

void PluginCopies::releaseUnused() {
    std::vector<std::shared_ptr<Copy>> done;
    bool more = false;
    {
        std::lock_guard<std::mutex> lk(mtx);
        // Only the pool holds it: no graph plays it any more, and none can
        // start to - find() no longer hands it out.
        for (auto it = unused.begin(); it != unused.end();) {
            if (it->use_count() == 1) {
                done.push_back(std::move(*it));
                it = unused.erase(it);
            } else {
                ++it;
            }
        }
        more = !unused.empty();
    }
    if (more) {
        if (!isTimerRunning()) startTimer(1000);
    } else {
        stopTimer();
    }
    for (auto& c : done)
        if (c->plugin) c->plugin->releaseResources();
    // `done` destroys them as it goes: here, on the message thread.
}

void PluginCopies::timerCallback() {
    releaseUnused();
}

void PluginCopies::audioProcessorParameterChanged(juce::AudioProcessor* p, int index, float value) {
    // A knob moved in the master's window (or by the plugin itself): the same
    // in every copy, at once. setValue, not setValueNotifyingHost - nobody
    // listens to the copies.
    std::vector<std::shared_ptr<Copy>> targets;
    {
        std::lock_guard<std::mutex> lk(mtx);
        const int nodeId = masterNodeOf(p);
        if (nodeId < 0) return;
        targets = copiesOf(nodeId);
        queueEvent(nodeId, index, 2, value);   // for the automation recorder
    }
    for (auto& c : targets) {
        const auto& params = c->plugin->getParameters();
        if (index >= 0 && index < params.size())
            params[index]->setValue(value);
    }
}

void PluginCopies::audioProcessorParameterChangeGestureBegin(juce::AudioProcessor* p, int index) {
    std::lock_guard<std::mutex> lk(mtx);
    if (const int nodeId = masterNodeOf(p); nodeId >= 0)
        queueEvent(nodeId, index, 0, 0.0f);
}

void PluginCopies::audioProcessorParameterChangeGestureEnd(juce::AudioProcessor* p, int index) {
    std::lock_guard<std::mutex> lk(mtx);
    if (const int nodeId = masterNodeOf(p); nodeId >= 0)
        queueEvent(nodeId, index, 1, 0.0f);
}

void PluginCopies::audioProcessorChanged(juce::AudioProcessor* p, const ChangeDetails& details) {
    // A program change, or a setting that isn't a parameter (a sample loaded,
    // a mode switched): copy the whole state - on the message thread.
    if (!details.programChanged && !details.nonParameterStateChanged) return;
    {
        std::lock_guard<std::mutex> lk(mtx);
        const int nodeId = masterNodeOf(p);
        if (nodeId < 0) return;
        mastersChanged.insert(nodeId);
    }
    triggerAsyncUpdate();
}

void PluginCopies::handleAsyncUpdate() {
    std::set<int> changed;
    {
        std::lock_guard<std::mutex> lk(mtx);
        changed.swap(mastersChanged);
    }
    for (int id : changed)
        syncFromMaster(id);
}

// =============================================================================
// PluginCopyProcessor
// =============================================================================

PluginCopyProcessor::PluginCopyProcessor(std::shared_ptr<PluginCopies::Copy> c)
    : AudioProcessor(busesOf(*c->plugin)), copy(std::move(c)) {
    // Prepared when it was loaded, so it already knows: reporting it now saves
    // the graph a latency-changed rebuild after preparing this.
    setLatencySamples(copy->plugin->getLatencySamples());
}

juce::AudioProcessor::BusesProperties PluginCopyProcessor::busesOf(const juce::AudioProcessor& p) {
    BusesProperties props;
    for (const bool isInput : { true, false })
        for (int i = 0; i < p.getBusCount(isInput); ++i)
            if (const auto* bus = p.getBus(isInput, i))
                props.addBus(isInput, bus->getName(),
                             bus->isEnabled() ? bus->getCurrentLayout() : bus->getLastEnabledLayout(),
                             bus->isEnabled());
    return props;
}

void PluginCopyProcessor::prepareToPlay(double sampleRate, int blockSize) {
    auto& p = *copy->plugin;
    // Not while an older graph might still be playing the copy.
    const juce::ScopedLock sl(p.getCallbackLock());
    if (copy->preparedRate != sampleRate || copy->preparedBlock < blockSize) {
        if (copy->preparedRate > 0.0)
            p.releaseResources();
        if (p.supportsDoublePrecisionProcessing())
            p.setProcessingPrecision(getProcessingPrecision());
        p.setRateAndBufferSizeDetails(sampleRate, blockSize);
        p.prepareToPlay(sampleRate, blockSize);
        copy->preparedRate = sampleRate;
        copy->preparedBlock = blockSize;
        copy->played = false;
    }
    // It may still be sounding from the graph it played in before this one.
    if (copy->played) {
        p.reset();
        copy->played = false;
        silenceNotes = p.acceptsMidi();
    }
    setLatencySamples(p.getLatencySamples());
}

template <typename Sample>
void PluginCopyProcessor::play(juce::AudioBuffer<Sample>& b, juce::MidiBuffer& m) {
    auto& p = *copy->plugin;
    const juce::ScopedLock sl(p.getCallbackLock());
    if (silenceNotes) {
        // Ahead of anything already in the buffer - a note starting at
        // sample 0 must survive it.
        silenceNotes = false;
        juce::MidiBuffer first;
        for (int channel = 1; channel <= 16; ++channel) {
            first.addEvent(juce::MidiMessage::allSoundOff(channel), 0);
            first.addEvent(juce::MidiMessage::allNotesOff(channel), 0);
        }
        first.addEvents(m, 0, -1, 0);
        m.swapWith(first);
    }
    copy->played = true;
    p.processBlock(b, m);
}

void PluginCopyProcessor::processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer& m) {
    play(b, m);
}

void PluginCopyProcessor::processBlock(juce::AudioBuffer<double>& b, juce::MidiBuffer& m) {
    play(b, m);
}

void PluginCopyProcessor::setNonRealtime(bool nonRealtime) noexcept {
    AudioProcessor::setNonRealtime(nonRealtime);
    copy->plugin->setNonRealtime(nonRealtime);
}

} // namespace SoundShop
