#include "asset_import.h"
#include <unordered_map>
#include <set>
#include <functional>

namespace SoundShop {

std::vector<int> assetChildIds(const AssetEntry&) {
    // Leaves only today (waveforms / AHDSR curves / morph chains carry no child-
    // asset ids). Composite instruments will parse their payload here.
    return {};
}

std::string assetRewriteChildIds(const AssetEntry& e,
                                 const std::vector<std::pair<int, int>>&) {
    // No child ids to rewrite for a leaf - the payload is returned verbatim.
    return e.payload;
}

// Lowest free "<base> N" (N>=2) name within `kind`, or `base` if it's free.
static std::string uniqueName(const AssetLibrary& dest, AssetKind kind,
                              const std::string& base) {
    auto taken = [&](const std::string& n) {
        for (const auto& e : dest.all())
            if (e.kind == kind && e.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        std::string cand = base + " " + std::to_string(i);
        if (!taken(cand)) return cand;
    }
}

AssetImportResult importAssets(AssetLibrary& dest,
                               const std::vector<AssetEntry>& source,
                               const std::vector<int>& selectedIds) {
    AssetImportResult res;

    // Index the source set by id for closure traversal.
    std::unordered_map<int, const AssetEntry*> byId;
    byId.reserve(source.size());
    for (const auto& e : source) byId[e.id] = &e;

    // ---- Step 1: closure expansion -----------------------------------------
    // Push children before parents so that, when we process `order` in sequence,
    // a parent's children already have final ids in the remap table (leaves-first
    // is a free by-product of post-order traversal; needed once composites exist).
    std::vector<int> order;
    std::set<int> seen;
    std::function<void(int)> expand = [&](int id) {
        if (seen.count(id)) return;
        auto it = byId.find(id);
        if (it == byId.end()) { ++res.skipped; return; } // unknown/built-in id
        seen.insert(id);
        for (int child : assetChildIds(*it->second)) expand(child);
        order.push_back(id);
    };
    if (selectedIds.empty()) {
        for (const auto& e : source) expand(e.id);
    } else {
        for (int id : selectedIds) expand(id);
    }

    // ---- Steps 2 & 3: decide final id (dedup) + rewrite child refs ----------
    for (int sid : order) {
        const AssetEntry& src = *byId[sid];

        // Step 3 first (so the hash is over the rewritten payload): substitute any
        // embedded child ids with their final destination ids. No-op for leaves.
        std::string payload = assetRewriteChildIds(src, res.remap);

        // Step 2: RE-DERIVE the hash from the (rewritten) payload - never trust
        // the source file's stored hash (false-merge safety).
        std::string hash = AssetLibrary::computeHash(src.kind, src.subType, payload);

        if (const AssetEntry* hit = dest.findByHash(hash)) {
            // Dedup: existing destination item wins (keeps its id and name).
            res.remap.push_back({sid, hit->id});
            ++res.deduped;
            continue;
        }

        // Genuinely new: allocate a fresh destination id and insert.
        AssetEntry e;
        e.id          = dest.allocId();
        e.kind        = src.kind;
        e.name        = uniqueName(dest, src.kind, src.name);
        e.subType     = src.subType;
        e.payload     = payload;
        e.contentHash = hash;
        e.archived    = src.archived; // preserve soft-delete state
        e.starred     = src.starred;  // preserve favourite flag
        if (e.name != src.name) ++res.renamed;
        int newId = e.id;
        dest.insertRaw(std::move(e));
        res.remap.push_back({sid, newId});
        ++res.added;
    }

    return res;
}

} // namespace SoundShop
