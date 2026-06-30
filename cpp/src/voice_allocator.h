#pragma once
#include <vector>

namespace SoundShop {

// =============================================================================
// VoiceAllocator  -  pure voice-allocation bookkeeping for PolyVoiceProcessor
// =============================================================================
//
// This is the policy half of the Voice container engine, deliberately split out
// from the audio-graph machinery (the GraphProcessor clones, VoiceInProcessor,
// scratch buffers) so it has NO JUCE dependency and can be unit-tested in
// isolation (see testVoiceAllocator in self_test.cpp). PolyVoiceProcessor keeps
// a parallel std::vector<Voice> for the per-slot audio objects and indexes it by
// the same slot number this allocator hands back.
//
// note policy (mirrors poly-voice-architecture.md):
//   - note-on   -> first inactive slot, else STEAL an active slot per stealMode.
//   - note-off  -> release the first still-held slot playing that note (its
//                  envelope tail keeps sounding; the slot stays active).
//   - free      -> a released slot is reclaimed once its output RMS has stayed
//                  below a floor for kFreeMs (envelope-tail done).
//
// Steal mode (set per block from VoiceContainer.voiceStealMode):
//   - StealOldest     : reuse the longest-sounding slot (musical default).
//   - StealQuietest    : reuse the slot whose last block was quietest (least
//                       audible interruption; great for pads/long tails).
//   - StealRoundRobin  : cycle through slots in order, ignoring age/level
//                       (predictable, useful for drum-style patches).
struct VoiceAllocator {
    enum StealMode { StealOldest = 0, StealQuietest = 1, StealRoundRobin = 2 };

    struct Slot {
        bool active     = false; // currently sounding (held or in release tail)
        bool gateHeld   = false; // key still down
        int  note       = -1;    // MIDI note this slot is playing
        int  channel    = 1;     // MIDI channel of the note (for MPE routing)
        long long age   = 0;     // allocation order, for steal-oldest
        long long group = -1;    // unison-stack id: every slot of one struck note
                                 // shares it, so note-off releases the whole stack
        float silenceMs = 0.0f;  // time below the RMS floor after release
        float lastRms   = 0.0f;  // last block's RMS, for steal-quietest
    };

    std::vector<Slot> slots;
    long long ageCounter = 0;
    long long groupCounter = 0;  // hands out unison-stack ids
    int stealMode = StealOldest; // refreshed each block by PolyVoiceProcessor
    int rrCursor  = -1;          // last round-robin slot handed out

    // (Re)create n empty slots. Resets allocation history.
    void resize(int n) {
        slots.assign((size_t) (n < 0 ? 0 : n), Slot{});
        ageCounter = 0;
        groupCounter = 0;
        rrCursor = -1;
    }

    int size() const { return (int) slots.size(); }

    int activeCount() const {
        int c = 0;
        for (const auto& s : slots) if (s.active) ++c;
        return c;
    }

    // Choose the slot a new note would use. A free (inactive) slot always wins
    // over stealing; round-robin still prefers a free slot first so an idle
    // synth doesn't needlessly cut voices. When every slot is busy, the active
    // victim is chosen by stealMode. Returns -1 only when there are no slots.
    int pickSlot() {
        for (int i = 0; i < (int) slots.size(); ++i)
            if (!slots[i].active) return i;
        if (slots.empty()) return -1;

        switch (stealMode) {
            case StealQuietest: {
                int best = 0;
                float minRms = slots[0].lastRms;
                for (int i = 1; i < (int) slots.size(); ++i)
                    if (slots[i].lastRms < minRms) { minRms = slots[i].lastRms; best = i; }
                return best;
            }
            case StealRoundRobin: {
                rrCursor = (rrCursor + 1) % (int) slots.size();
                return rrCursor;
            }
            case StealOldest:
            default: {
                int oldest = 0;
                long long minAge = slots[0].age;
                for (int i = 1; i < (int) slots.size(); ++i)
                    if (slots[i].age < minAge) { minAge = slots[i].age; oldest = i; }
                return oldest;
            }
        }
    }

    struct NoteOnResult {
        int  slot  = -1;     // slot to use, or -1 if there are no slots
        bool stole = false;  // true when an already-sounding voice was reused
    };

    // Allocate a slot for `note`. The caller should reset that slot's VoiceIn
    // when `stole` is true (it was mid-flight) before issuing the new note-on.
    NoteOnResult noteOn(int note) {
        NoteOnResult r;
        r.slot = pickSlot();
        if (r.slot < 0) return r;
        Slot& s = slots[(size_t) r.slot];
        r.stole = s.active;          // reusing a still-sounding voice == a steal
        s.active = true;
        s.gateHeld = true;
        s.note = note;
        s.age = ++ageCounter;
        s.silenceMs = 0.0f;
        return r;
    }

    // ---- Unison: allocate / release a STACK of slots per struck note --------
    //
    // A note with unison U grabs U slots in one go, all sharing a fresh `group`
    // id so the matching note-off releases the whole stack together. PolyVoice-
    // Processor then detunes/pans each slot of the stack. The result is fixed-
    // size (no heap on the audio thread); kMaxUnison caps a stack.
    struct GroupResult {
        static constexpr int kMaxUnison = 8;
        int  slot[kMaxUnison];
        bool stole[kMaxUnison];
        int  count = 0;
    };

    GroupResult noteOnGroup(int note, int channel, int unison) {
        GroupResult g;
        if (unison < 1) unison = 1;
        if (unison > GroupResult::kMaxUnison) unison = GroupResult::kMaxUnison;
        if (unison > (int) slots.size()) unison = (int) slots.size();
        const long long grp = ++groupCounter;
        for (int u = 0; u < unison; ++u) {
            const int slot = pickSlot();
            if (slot < 0) break;
            Slot& s = slots[(size_t) slot];
            g.stole[g.count] = s.active; // reusing a sounding voice == a steal
            g.slot[g.count]  = slot;
            ++g.count;
            s.active = true;
            s.gateHeld = true;
            s.note = note;
            s.channel = channel;
            s.group = grp;
            s.age = ++ageCounter;
            s.silenceMs = 0.0f;
        }
        return g;
    }

    struct ReleaseResult {
        int slot[GroupResult::kMaxUnison];
        int count = 0;
    };

    // Release the newest still-held stack matching `note` (and `channel`, unless
    // channel <= 0 = any). Drops the gate on every slot of that stack and returns
    // them so the caller can drive each VoiceIn's note-off. The slots stay active
    // for their release tails.
    ReleaseResult noteOffGroup(int note, int channel) {
        ReleaseResult rr;
        long long bestGrp = -1, bestAge = -1;
        for (auto& s : slots)
            if (s.active && s.gateHeld && s.note == note
                && (channel <= 0 || s.channel == channel) && s.age > bestAge) {
                bestAge = s.age; bestGrp = s.group;
            }
        if (bestGrp < 0) return rr;
        for (int i = 0; i < (int) slots.size(); ++i) {
            Slot& s = slots[(size_t) i];
            if (s.active && s.gateHeld && s.group == bestGrp) {
                s.gateHeld = false;
                if (rr.count < GroupResult::kMaxUnison) rr.slot[rr.count++] = i;
            }
        }
        return rr;
    }

    // Release the first still-held slot playing `note`. Returns its slot, or -1.
    // The slot stays active so its envelope release tail can finish.
    int noteOff(int note) {
        for (int i = 0; i < (int) slots.size(); ++i) {
            Slot& s = slots[(size_t) i];
            if (s.active && s.gateHeld && s.note == note) {
                s.gateHeld = false;
                return i;
            }
        }
        return -1;
    }

    // Drop the gate on every active slot (panic / all-notes-off). Slots keep
    // their note number so the caller can issue per-voice note-offs.
    void allNotesOff() {
        for (auto& s : slots) if (s.active) s.gateHeld = false;
    }

    // Update one slot's free-detection state after it rendered `rms` over a
    // block of `blockMs`. Returns true exactly when the slot has just been
    // freed (so the caller can reset its VoiceIn). A still-held slot never
    // frees; its silence timer is held at zero.
    bool postRender(int slot, float rms, float blockMs, float floorRms, float freeMs) {
        if (slot < 0 || slot >= (int) slots.size()) return false;
        Slot& s = slots[(size_t) slot];
        if (!s.active) return false;
        s.lastRms = rms; // remembered for steal-quietest, held or not
        if (!s.gateHeld && rms < floorRms) {
            s.silenceMs += blockMs;
            if (s.silenceMs >= freeMs) {
                s.active = false;
                return true;
            }
        } else {
            s.silenceMs = 0.0f;
        }
        return false;
    }
};

} // namespace SoundShop
