#pragma once
#include "asset_library.h"
#include <vector>
#include <string>

namespace SoundShop {

// ============================================================================
// Asset import/merge - pull a set of AssetEntry records (from another project's
// [AssetStore] or a dedicated library export) into a destination AssetLibrary,
// deduplicating by content hash and remapping ids so nothing collides.
//
// DESIGN (see agent-todo.md "Import / export between projects"): the whole merge
// is three FLAT, order-free passes - no bottom-up/topological pass - because an
// asset's stored structural hash already encodes its descendants' hashes:
//   1. CLOSURE EXPANSION - a selection concern, not a dedup one: importing a
//      parent pulls in its dependency closure (children, grandchildren). Every
//      asset is a leaf today (waveforms/AHDSR/morph contain no child-asset ids),
//      so the closure is just the selection itself; the recursion is in place for
//      the future composite-instrument case via assetChildIds().
//   2. DECIDE EACH ITEM'S FINAL ID - flat per-item: if its (re-derived) content
//      hash matches an existing destination asset, reuse that id (dedup); else
//      allocate a fresh destination id. Builds the complete remap table.
//   3. REWRITE CHILD-ID REFERENCES in inserted payloads using the now-complete
//      remap table - also flat (all final ids are known from step 2). No-op for
//      leaves; the hook is assetRewriteChildIds().
//
// SAFETY: false merge destroys data, false non-merge is harmless -> bias toward
// NOT merging. We therefore RE-DERIVE each source leaf's hash from its payload
// (never trust the source file's stored hash, which could be corrupt / hand-
// edited / from a future hash-algo version) before using it for a merge.
//
// DEDUP RULE: on hash match the existing destination item wins (its id AND name
// are kept); the imported dupe drops and any imported reference repoints to the
// existing id. Name clashes between genuinely-new imports and existing entries of
// the same kind get the lowest free numeric suffix ("Warm Pad" -> "Warm Pad 2").
// ============================================================================

struct AssetImportResult {
    int added   = 0;   // genuinely-new entries inserted into the destination
    int deduped = 0;   // source entries that matched existing destination content
    int renamed = 0;   // inserted entries that needed a name-clash suffix
    int skipped = 0;   // selected ids not present in the source set (ignored)
    // sourceId -> destinationId for every closure item (both deduped and inserted).
    std::vector<std::pair<int, int>> remap;
};

// Child-asset ids embedded in an entry's payload, for closure expansion + remap.
// Every AssetKind is a leaf today, so this returns {} for all of them; the
// composite-instrument case will populate it (and assetRewriteChildIds) later.
std::vector<int> assetChildIds(const AssetEntry& e);

// Rewrite child-asset-id references inside a payload using a complete remap
// table (sourceId -> destId). No-op for leaves; returns the payload unchanged.
std::string assetRewriteChildIds(const AssetEntry& e,
                                 const std::vector<std::pair<int, int>>& remap);

// Merge `source` assets into `dest`. If `selectedIds` is non-empty, only those
// source ids (expanded to their dependency closure) are imported; empty = import
// every source entry. Mutates `dest` in place and returns a summary + remap.
AssetImportResult importAssets(AssetLibrary& dest,
                               const std::vector<AssetEntry>& source,
                               const std::vector<int>& selectedIds = {});

} // namespace SoundShop
