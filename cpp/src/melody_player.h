#pragma once
#include "node_graph.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <set>

namespace SoundShop {

// A melody event: one or more notes that play simultaneously
struct MelodyEvent {
    std::vector<int> pitches;    // MIDI pitches
    std::vector<float> durations; // optional per-note durations (0 = until release)
};

// Performance mode: play a preset melody by pressing any keys
class MelodyPlayer {
public:
    MelodyPlayer();

    // Load melody from a node's clips
    void loadFromNode(const Node& node);

    // Load from explicit pitch/duration list
    void loadEvents(const std::vector<MelodyEvent>& events);

    // Set mode
    enum class ReleaseMode {
        OnKeyUp,    // notes off when key released
        OnNextEvent // notes off when next event triggered (legato)
    };
    void setReleaseMode(ReleaseMode mode) { releaseMode = mode; }

    // Enable/disable velocity sensitivity
    void setVelocitySensitive(bool v) { velocitySensitive = v; }

    // Process incoming MIDI — replaces pitches with melody events
    // Returns the MIDI buffer to send to the instrument
    void processMidi(const juce::MidiBuffer& input, juce::MidiBuffer& output);

    // Reset to beginning
    void reset() { currentEvent = 0; activeNotes.clear(); }

    bool isActive() const { return !events.empty(); }
    int getCurrentEvent() const { return currentEvent; }
    int getTotalEvents() const { return (int)events.size(); }

private:
    std::vector<MelodyEvent> events;
    int currentEvent = 0;
    ReleaseMode releaseMode = ReleaseMode::OnNextEvent;
    bool velocitySensitive = true;

    // Currently sounding notes (pitches we sent note-on for)
    std::set<int> activeNotes;

    // Keys currently held down
    std::set<int> heldKeys;

    void triggerEvent(int eventIdx, int velocity, juce::MidiBuffer& output, int samplePos);
    void releaseAllNotes(juce::MidiBuffer& output, int samplePos);
};

} // namespace SoundShop
