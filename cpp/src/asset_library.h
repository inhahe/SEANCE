#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace SoundShop {

// ============================================================================
// AssetLibrary - the project-level "stores" for reusable assets.
//
// WHY IT EXISTS: SEANCE has several kinds of reusable building blocks that today
// can only live inside one node at a time - wavetable frames / individual waves
// (IWavetableFrame), independent instruments, ADHSR curves, and morph/warp
// algorithms. The asset library lets a project publish any of these once and
// reference it by a stable integer id from many places. Editing the stored item
// propagates live to every reference (references are by id, names are display-
// only). GLSL and other id-keyed consumers can address an asset with a plain int.
//
// CORE MODEL (agreed design, see agent-todo.md "Project-level asset library"):
//   * ADD IS EXPLICIT. Items enter a store only via an "Add to Library" action,
//     which publishes a NEW id. Nothing is auto-added.
//   * REFERENCE BY ID, LIVE. Consumers hold the id; editing the stored payload
//     propagates to all references. Names are display-only (pickers poll live).
//   * NO DETACH. Divergence = duplicate() (new id) + repoint. Don't-want-
//     propagation = simply don't edit the shared item.
//   * SOFT-DELETE. archive() hides an item from pickers but keeps it resolvable
//     so existing references stay valid; restore() un-hides. No auto-purge.
//   * DISJOINT ID SPACE. User-store ids start at kUserIdBase, well above the
//     built-in id ranges (factory waveforms, warp methods) so the two never
//     clash and an id alone tells you which space it belongs to.
//
// The store is payload-agnostic: each entry holds an opaque encoded `payload`
// string plus a `subType` tag (e.g. an IWavetableFrame typeId like "layered").
// Decoding back into a concrete object is the caller's job - the library only
// owns identity, naming, lifecycle, and content-hash dedup.
// ============================================================================

enum class AssetKind {
    Waveform,       // IWavetableFrame generator (individual wave OR wavetable frame)
    Instrument,     // independent instrument (generator + owned ADHSR/articulation)
    AhdsrCurve,     // an AHDSR / envelope curve
    MorphAlgorithm, // a warp / morph algorithm (WarpOp chain)
    FrequencyGraph  // a 1-D frequency-domain curve (SpectralCurve): EQ response,
                    // FFT magnitude/phase, Spectrum-Tap per-bin response, etc.
                    // Payload = SpectralCurve::encode() (source-preserving:
                    // formula+language OR drawn points OR freehand samples).
};

const char* assetKindTag(AssetKind k);          // stable string for serialization
bool        assetKindFromTag(const std::string& tag, AssetKind& out);

struct AssetEntry {
    int         id = 0;
    AssetKind   kind = AssetKind::Waveform;
    std::string name;        // display-only; may be renamed freely
    std::string subType;     // concrete type tag within the kind (e.g. "layered")
    std::string payload;     // opaque encoded body (caller decodes)
    std::string contentHash; // hash128 over (kind, subType, payload) - NOT name/id
    bool        archived = false;
    bool        starred  = false; // user favourite; the user-side analogue of the
                                  // built-in WaveformBank "curated" flag. Pickers
                                  // surface a "starred only" filter over both.
};

class AssetLibrary {
public:
    // User-published ids start here, disjoint from all built-in id ranges.
    static constexpr int kUserIdBase = 1000000;

    // Publish a new asset (always a NEW id - "add is explicit"). The content hash
    // is computed and stored. Returns the new id.
    int add(AssetKind kind, const std::string& name,
            const std::string& subType, const std::string& payload);

    // Duplicate an existing entry under a new id and (optionally) a new name.
    // Used for the "diverge" workflow (edit the copy, leave the original shared).
    // Returns the new id, or 0 if `srcId` is unknown.
    int duplicate(int srcId, const std::string& newName);

    // Replace the payload/subType of an existing entry in place (this is what
    // makes references "live" - editing the store updates every consumer). The
    // content hash is recomputed. Returns false if `id` is unknown.
    bool update(int id, const std::string& subType, const std::string& payload);

    // Rename (display-only). Returns false if `id` is unknown.
    bool rename(int id, const std::string& newName);

    // Soft-delete / restore. archive() keeps the entry resolvable by find() but
    // hides it from list(includeArchived=false). Returns false if id unknown.
    bool archive(int id);
    bool restore(int id);

    // Toggle/set the user "starred" favourite flag (display/filter only - does not
    // affect resolution or hashing). Returns false if id unknown.
    bool setStarred(int id, bool starred);

    // Hard-remove an entry entirely (explicit purge only - callers must warn the
    // user; existing references will dangle). Returns false if id unknown.
    bool erase(int id);

    AssetEntry*       find(int id);
    const AssetEntry* find(int id) const;

    // First non-archived entry whose content hash matches, or nullptr. Used by
    // import dedup and by "Add to Library" exact-content prompts.
    const AssetEntry* findByHash(const std::string& hash) const;

    // All entries of a kind (archived excluded unless includeArchived).
    std::vector<const AssetEntry*> list(AssetKind kind,
                                        bool includeArchived = false) const;

    const std::vector<AssetEntry>& all() const { return entries; }
    size_t size() const { return entries.size(); }
    void clear() { entries.clear(); nextId = kUserIdBase; }

    // Id allocation. allocId() reserves and returns the next user id; noteId()
    // bumps the counter past an id seen on load so future allocs never collide.
    int  allocId();
    void noteId(int id);
    int  peekNextId() const { return nextId; }
    void setNextId(int id) { nextId = id; }

    // Insert a fully-formed entry (used on project load - id/hash come from the
    // saved file and are trusted, not recomputed). Bumps nextId past the id.
    void insertRaw(AssetEntry e);

    // Content hash over (kind, subType, payload) only - identity of the content,
    // independent of id/name/archived. Conservative: no normalization, the bytes
    // are hashed exactly as serialized.
    static std::string computeHash(AssetKind kind, const std::string& subType,
                                   const std::string& payload);

private:
    std::vector<AssetEntry> entries;
    int nextId = kUserIdBase;
};

} // namespace SoundShop
