#pragma once
#include "plugin_host.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace SoundShop {

class NodeGraph;
class GraphProcessor;

// =============================================================================
// PluginCopies - hosted plugins in the graphs that can't have a node's own
// =============================================================================
//
// A plugin instance can be in one audio graph only, and the live audio engine's
// graph holds each plugin node's own instance (GraphProcessor::setHostsPlugins).
// Two kinds of graph play copies of it instead:
//
//   - An offline render - Export Audio, a script's render(), Freeze, Bounce to
//     Audio Track, capture from playback - plays a copy of every plugin node,
//     loaded fresh and given that plugin's settings as they are when the render
//     starts (Use::render).
//   - A Voice container plays each plugin inside it once per voice
//     (Use::voices, the live graph's pool). The node's own instance is the
//     "master": it's never played, its window is the one the user edits in,
//     and its settings are the ones a project saves. Every voice plays a copy
//     that follows it - a knob moved in the master's window moves in every
//     copy at once, and the master's whole state is copied again when it
//     reports a program or other non-parameter change, when a preset is picked
//     from SEANCE's menu, and when its window is closed (syncFromMaster). The
//     host's own changes to its parameters - automation lanes, learned MIDI
//     CCs - are made on the master and every copy at once (setParameter), and
//     the master's knob moves are queued for the automation recorder
//     (drainParamEvents).
//
// Copies are loaded on the message thread - where plugins have to be made -
// before the graph that plays them is built, and handed to it through
// GraphProcessor::setPluginCopies; a graph plays one through a
// PluginCopyProcessor. They outlive graph rebuilds (the live graph rebuilds on
// every edit), so an edit doesn't load every voice's copy again; one no longer
// needed is destroyed on the message thread once no graph plays it.
class PluginCopies : private juce::AudioProcessorListener,
                     private juce::AsyncUpdater,
                     private juce::Timer {
public:
    // One loaded copy.
    struct Copy {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        // What it was last prepared with: a graph built around it again - every
        // rebuild - needn't prepare it again (PluginCopyProcessor).
        double preparedRate = 0.0;
        int preparedBlock = 0;
        // Whether it has played since it was last prepared or reset - so a
        // copy that hasn't isn't reset for nothing (on a render's thread).
        bool played = false;
    };

    enum class Use {
        render,   // an offline render: every plugin node (slot -1), and each voice's
        voices    // the live graph: only the plugins inside Voice containers
    };

    explicit PluginCopies(Use use);
    ~PluginCopies() override;

    Use getUse() const { return use; }

    // Load the copies `graph` needs and doesn't have yet, from each plugin's
    // current settings, and let go of the ones it no longer needs. A plugin node
    // outside any Voice container (Use::render) is copied from its instance in
    // `live` - the live audio graph, which plays it - or, not there yet, the one
    // its node holds. Voice copies are made for each of the container's voices
    // (Node::voicePolyphony). Message thread, holding no graph lock: loading a
    // plugin can take a while. A copy that fails to load isn't tried again for
    // the same plugin instance (only one refused as blocked is); see problems().
    void update(NodeGraph& graph, PluginHost& host, GraphProcessor* live,
                double sampleRate, int blockSize);

    // The copy for a plugin node: `slot` -1 in an offline render's main graph,
    // else the index of the voice. Null if there's none - the graph then builds
    // the node silent, like a plugin that didn't load. Any thread.
    std::shared_ptr<Copy> find(int nodeId, int slot) const;

    // Plugin nodes that play without some or all of their copies, each with a
    // sentence naming the plugin and saying why.
    std::map<int, std::string> problems() const;

    // For an offline render: "<what> went without 2 plugins. ..." naming each
    // one and why, or empty if it had them all.
    juce::String describeProblems(const juce::String& what) const;

    // Give a Voice container plugin's copies the master's whole state again.
    // Message thread.
    void syncFromMaster(int nodeId);

    // A change of the host's to a Voice container plugin's parameter - an
    // automation lane's value, a learned MIDI CC - made in every voice at
    // once: on the master and all its copies. setValue, not
    // setValueNotifyingHost: nothing is to hear of it (the automation recorder
    // would take it for the user's). False if node `nodeId` isn't a master
    // this pool follows. Any thread: the audio callback applies learned CCs -
    // holding the graph lock, so the master can't go meanwhile.
    bool setParameter(int nodeId, int index, float value);

    // The masters' parameter events - knobs in their windows grabbed (kind 0),
    // let go (1) and moved (2, `value` normalised) - for the automation
    // recorder, as GraphProcessor::drainParamEvents has the live graph's
    // plugins'. Queued from whatever thread the plugin reports on; drained,
    // and cleared, by the UI timer.
    struct ParamEvent { int nodeId; int paramIdx; int kind; float value; };
    std::vector<ParamEvent> drainParamEvents();

    // Every copy of plugin node `nodeId` the pool holds: an offline render's
    // main graph's (slot -1) and each voice's. For driving their parameters
    // (GraphProcessor::applyPluginAutomation). Any thread.
    std::vector<std::shared_ptr<Copy>> copiesOfNode(int nodeId) const;

    // A plugin instance no graph plays - a Voice container's master, or one
    // loaded and not taken into the live graph yet - takes a parameter change
    // in only when it next processes audio: JUCE's LV2 host moves it into the
    // plugin's port then, and a VST3 with a separate controller hears of it
    // then. Until then its state (getStateInformation) would miss a knob moved
    // in its window. This has it process one short silent block, so the state
    // is current. Call it before reading such a plugin's state - never on one
    // a graph plays. Message thread.
    static void catchUp(juce::AudioProcessor& plugin);

    // Destroy the copies let go of that no graph plays any more. Message thread;
    // runs by itself, on a timer, while there are any.
    void releaseUnused();

    // How many copies the pool holds (not counting ones let go of).
    int size() const;

private:
    using Key = std::pair<int, int>;   // node id, slot
    struct Entry {
        std::shared_ptr<Copy> copy;
        std::weak_ptr<PluginHost::LoadedPlugin> source;   // the node's plugin it copies
    };
    struct Master {
        juce::AudioProcessor* processor = nullptr;
        std::weak_ptr<PluginHost::LoadedPlugin> owner;
    };
    struct Failure {
        std::weak_ptr<PluginHost::LoadedPlugin> source;
        std::string error;
        // A copy that failed to load isn't tried again for the same plugin
        // instance - it would fail again, maybe slowly, on every edit. One
        // refused because the plugin is blocked is: unblocking it should work.
        bool retry = false;
    };

    const Use use;
    mutable std::mutex mtx;                  // guards everything below
    std::map<Key, Entry> entries;
    std::vector<std::shared_ptr<Copy>> unused;
    std::map<int, Master> masters;           // Use::voices: node id -> its master
    std::set<int> mastersChanged;            // whole-state changes to copy (message thread)
    std::map<int, Failure> failures;         // node id -> why a copy didn't load
    std::vector<ParamEvent> paramEvents;     // the masters', for the recorder (drainParamEvents)

    std::vector<std::shared_ptr<Copy>> copiesOf(int nodeId) const;   // mtx held
    int masterNodeOf(const juce::AudioProcessor* p) const;           // mtx held

    void queueEvent(int nodeId, int index, int kind, float value);   // mtx held

    // AudioProcessorListener (masters only)
    void audioProcessorParameterChanged(juce::AudioProcessor*, int index, float value) override;
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int index) override;
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int index) override;
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override;

    void handleAsyncUpdate() override;   // copies whole states on the message thread
    void timerCallback() override;       // releaseUnused

    JUCE_DECLARE_NON_COPYABLE(PluginCopies)
};

// How a graph plays one of PluginCopies' copies: a stand-in with the plugin's
// own buses that passes everything through to the copy - which it doesn't own,
// so the copy outlives the graph (a rebuild destroys every processor in it).
// It has no parameters of its own; code that drives a copy's parameters
// (GraphProcessor::applyPluginAutomation) goes to plugin().
class PluginCopyProcessor : public juce::AudioProcessor {
public:
    explicit PluginCopyProcessor(std::shared_ptr<PluginCopies::Copy> copy);

    juce::AudioPluginInstance& plugin() const { return *copy->plugin; }
    const std::shared_ptr<PluginCopies::Copy>& getCopy() const { return copy; }

    const juce::String getName() const override { return copy->plugin->getName(); }
    void prepareToPlay(double sampleRate, int blockSize) override;
    // Nothing to release: the copy stays prepared for the next graph that plays
    // it, and is released when the pool destroys it.
    void releaseResources() override {}
    // Holding the copy's callback lock, as a graph holds a processor's while
    // it plays it: a plugin's own code relies on it (JUCE's LV2 host restores
    // a state under it).
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override;
    void processBlock(juce::AudioBuffer<double>& b, juce::MidiBuffer& m) override;
    bool supportsDoublePrecisionProcessing() const override { return copy->plugin->supportsDoublePrecisionProcessing(); }
    void reset() override {
        copy->plugin->reset();
        copy->played = false;
    }
    void setNonRealtime(bool nonRealtime) noexcept override;

    double getTailLengthSeconds() const override { return copy->plugin->getTailLengthSeconds(); }
    bool acceptsMidi() const override { return copy->plugin->acceptsMidi(); }
    bool producesMidi() const override { return copy->plugin->producesMidi(); }
    bool isMidiEffect() const override { return copy->plugin->isMidiEffect(); }
    // Built with the plugin's buses (see the constructor), and never changed.
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return copy->plugin->getNumPrograms(); }
    int getCurrentProgram() override { return copy->plugin->getCurrentProgram(); }
    void setCurrentProgram(int i) override { copy->plugin->setCurrentProgram(i); }
    const juce::String getProgramName(int i) override { return copy->plugin->getProgramName(i); }
    void changeProgramName(int i, const juce::String& n) override { copy->plugin->changeProgramName(i, n); }
    void getStateInformation(juce::MemoryBlock& d) override { copy->plugin->getStateInformation(d); }
    void setStateInformation(const void* d, int n) override { copy->plugin->setStateInformation(d, n); }

private:
    std::shared_ptr<PluginCopies::Copy> copy;
    // Set when a copy that played in another graph is put into this one: its
    // first block here begins with All Sound Off / All Notes Off on every
    // channel, so notes it was sounding for voices that are gone stop. reset()
    // alone may not do it - JUCE's LV2 host has none.
    bool silenceNotes = false;

    template <typename Sample>
    void play(juce::AudioBuffer<Sample>& b, juce::MidiBuffer& m);
    static BusesProperties busesOf(const juce::AudioProcessor& plugin);
};

} // namespace SoundShop
