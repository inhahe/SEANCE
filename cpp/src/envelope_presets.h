#pragma once
#include "adsr_envelope.h"
#include <juce_core/juce_core.h>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace SoundShop {

// ==============================================================================
// EnvelopePreset - a named AHDSR envelope users can recall in any synth.
//
// `builtIn = true` is the visible "factory preset" badge: shown as a gold
// ★ in the dropdown and a tinted label in the manage dialog. Any user
// action that mutates a factory preset - saving over its name with new
// values, renaming it, or deleting it - clears `builtIn` (the entry no
// longer counts as pristine), and a deletion removes the entry entirely.
//
// `builtInOriginId` is an INVISIBLE origin marker that persists across
// rename and edit. For a freshly-seeded factory preset it equals the
// canonical name ("Pluck", "Pad", "Bass", ...); for user-created presets
// it's empty. Rename and addOrReplace leave it alone, so the Restore
// button can tell that the original factory entry is still represented
// in the library under whatever name and values the user has now, and
// skip re-adding it. Without this, renaming "Pluck" -> "MyPluck" and
// then hitting Restore would produce both a "MyPluck" (the user's
// renamed entry) and a fresh "Pluck" (the canonical), which is the
// redundancy we're avoiding. Cleared only when the preset is deleted
// outright - that's the explicit "I don't want this any more" signal
// the Restore button is allowed to act on.
// ==============================================================================
struct EnvelopePreset {
    std::string name;
    AHDSREnvelope envelope;
    bool builtIn = false;
    std::string builtInOriginId;
};

// ==============================================================================
// EnvelopePresetManager - app-wide singleton for the user's envelope-
// preset library. Backed by a small XML file alongside Preferences.xml.
//
// Lifetime:
//   - `instance()` returns the singleton; the first call seeds the
//     built-in defaults and tries to load any saved user presets.
//   - load() / save() are explicit so the host can decide when to
//     persist (e.g. after the management dialog closes), avoiding
//     a disk write per slider tweak.
//
// Threading: all accessors take an internal mutex. Snapshot copies
// returned by `getAll()` are safe to use without holding the lock.
// ==============================================================================
class EnvelopePresetManager {
public:
    static EnvelopePresetManager& instance();

    // Snapshot of every preset (built-in first, then user, both in
    // insertion order). Safe to use without holding any lock.
    std::vector<EnvelopePreset> getAll() const;
    int  size() const;
    bool getByName(const std::string& name, EnvelopePreset& out) const;

    // Add a new preset or overwrite an existing one of the same name.
    // If the matched entry was a built-in, this demotes it: the saved
    // values replace the canonical ones and `builtIn` is cleared so the
    // UI no longer marks it as factory. Returns the name actually used
    // (always equal to `name` now that there's no collision suffix).
    std::string addOrReplace(const std::string& name,
                             const AHDSREnvelope& env);

    // Add a NEW preset with a unique name. Never overwrites: if
    // `desiredName` is taken, returns a disambiguated variant like
    // "<desiredName> (2)". The new entry is always a user preset
    // (builtIn=false, originId empty), so duplicates of a factory
    // entry don't claim the factory lineage and Restore can still
    // re-add the canonical if needed. Returns the actual name used.
    std::string addNew(const std::string& desiredName,
                       const AHDSREnvelope& env);

    // Remove a preset by name. Built-ins are no longer protected from
    // deletion - deleting one simply removes it; the user can bring it
    // back with the Restore Built-ins button in the manage dialog.
    // Returns true if anything was removed.
    bool remove(const std::string& name);

    // Rename a preset. If the renamed entry was a built-in, this demotes
    // it (`builtIn` cleared), since after the rename it no longer matches
    // any canonical factory entry. Collisions append " (2)", " (3)", etc.
    bool rename(const std::string& oldName, const std::string& newName);

    // Canonical factory preset list. Read-only snapshot of what
    // seedBuiltIns() inserts on first run; used by the Restore button
    // and to render the "(built-in)" hint label.
    static std::vector<EnvelopePreset> getBuiltInDefaults();

    // Re-add any factory presets that have been deleted outright. This
    // function NEVER replaces or modifies an existing entry - even one
    // the user has edited under a factory name. The rules:
    //
    //   - For each canonical, if any existing preset carries
    //     `builtInOriginId == canonical.name`, the canonical is
    //     considered "still represented" (even if the user renamed it
    //     or saved different values over it) - skip.
    //   - Otherwise, if there's no preset whose `name` collides with
    //     the canonical name, add a fresh canonical copy.
    //   - Otherwise (the user has a same-named entry that ISN'T tied
    //     to this canonical's origin - e.g. they deleted "Pluck" and
    //     then created their own preset called "Pluck"), skip with no
    //     change. Restore won't clobber that entry.
    //
    // Returns the number of canonicals actually re-added. To get the
    // original Pluck back after editing it in place, the user has to
    // delete the edited entry first - the explicit deletion is what
    // clears the origin marker and unblocks restoration.
    int restoreBuiltIns();

    // Path to the on-disk XML, derived from the JUCE app-data dir.
    static juce::File getDefaultFile();

    // Read / write the preset library. The XML stores the full current
    // list including any demoted built-ins (so a deletion or edit
    // persists across launches); each <Preset> entry records its own
    // `builtIn` flag. On load, any canonical factory preset that isn't
    // listed in the XML at all is auto-added with builtIn=true, so a
    // brand-new SEANCE version that ships extra defaults will surface
    // them for existing users. Both calls take the file path so the
    // host can override for tests.
    void load(const juce::File& f = getDefaultFile());
    void save(const juce::File& f = getDefaultFile()) const;

    // Fired whenever the preset list changes (add / remove / rename /
    // load). UI components subscribe to refresh their dropdowns.
    void addListener(std::function<void()> cb);

private:
    EnvelopePresetManager();
    void seedBuiltIns();
    void notifyListeners();

    mutable std::mutex mutex;
    std::vector<EnvelopePreset> presets;
    std::vector<std::function<void()>> listeners;
};

} // namespace SoundShop
