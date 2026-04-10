#include "melody_player.h"
#include <algorithm>
#include <cstdio>

namespace SoundShop {

MelodyPlayer::MelodyPlayer() {}

void MelodyPlayer::loadFromNode(const Node& node) {
    events.clear();

    // Collect all notes from all clips, sorted by absolute beat position
    struct NoteEntry {
        float absBeat;
        int pitch;
        float duration;
    };
    std::vector<NoteEntry> allNotes;

    for (auto& clip : node.clips) {
        for (auto& note : clip.notes) {
            allNotes.push_back({
                clip.startBeat + note.getOffset(),
                note.pitch,
                note.getDuration()
            });
        }
    }

    if (allNotes.empty()) return;

    // Sort by beat position
    std::sort(allNotes.begin(), allNotes.end(),
              [](auto& a, auto& b) { return a.absBeat < b.absBeat; });

    // Group simultaneous notes into events (within a small tolerance)
    float tolerance = 0.01f; // notes within 1/100 beat are "simultaneous"
    float currentBeat = allNotes[0].absBeat;
    MelodyEvent currentEvt;

    for (auto& n : allNotes) {
        if (std::abs(n.absBeat - currentBeat) > tolerance) {
            // New event
            if (!currentEvt.pitches.empty())
                events.push_back(currentEvt);
            currentEvt = {};
            currentBeat = n.absBeat;
        }
        currentEvt.pitches.push_back(n.pitch);
        currentEvt.durations.push_back(n.duration);
    }
    if (!currentEvt.pitches.empty())
        events.push_back(currentEvt);

    currentEvent = 0;
    fprintf(stderr, "MelodyPlayer: loaded %d events from %d notes\n",
            (int)events.size(), (int)allNotes.size());
}

void MelodyPlayer::loadEvents(const std::vector<MelodyEvent>& evts) {
    events = evts;
    currentEvent = 0;
}

void MelodyPlayer::triggerEvent(int eventIdx, int velocity, juce::MidiBuffer& output, int samplePos) {
    if (eventIdx < 0 || eventIdx >= (int)events.size()) return;

    auto& evt = events[eventIdx];
    int vel = velocitySensitive ? velocity : 100;

    for (int pitch : evt.pitches) {
        output.addEvent(juce::MidiMessage::noteOn(1, pitch, (juce::uint8)vel), samplePos);
        activeNotes.insert(pitch);
    }
}

void MelodyPlayer::releaseAllNotes(juce::MidiBuffer& output, int samplePos) {
    for (int pitch : activeNotes)
        output.addEvent(juce::MidiMessage::noteOff(1, pitch), samplePos);
    activeNotes.clear();
}

void MelodyPlayer::processMidi(const juce::MidiBuffer& input, juce::MidiBuffer& output) {
    if (events.empty()) {
        // Pass through unchanged
        output.addEvents(input, 0, input.getLastEventTime() + 1, 0);
        return;
    }

    for (auto metadata : input) {
        auto msg = metadata.getMessage();
        int pos = metadata.samplePosition;

        if (msg.isNoteOn()) {
            int incomingKey = msg.getNoteNumber();
            bool wasEmpty = heldKeys.empty();
            heldKeys.insert(incomingKey);

            if (wasEmpty && currentEvent < (int)events.size()) {
                // First key pressed — trigger next event
                if (releaseMode == ReleaseMode::OnNextEvent)
                    releaseAllNotes(output, pos);

                triggerEvent(currentEvent, msg.getVelocity(), output, pos);
                currentEvent++;
            } else if (!wasEmpty && currentEvent > 0) {
                // Additional keys pressed while holding — for multi-key mode:
                // If the current event has more notes than keys previously pressed,
                // we could distribute. For now, additional keys are ignored since
                // the full chord was already triggered.
            }
        } else if (msg.isNoteOff()) {
            heldKeys.erase(msg.getNoteNumber());

            if (heldKeys.empty() && releaseMode == ReleaseMode::OnKeyUp) {
                // All keys released — stop current notes
                releaseAllNotes(output, pos);
            }
        } else {
            // Pass through non-note messages (CC, pitch bend, etc.)
            output.addEvent(msg, pos);
        }
    }
}

} // namespace SoundShop
