#include "asset_library.h"
#include "hash_util.h"

namespace SoundShop {

const char* assetKindTag(AssetKind k) {
    switch (k) {
        case AssetKind::Waveform:       return "waveform";
        case AssetKind::Instrument:     return "instrument";
        case AssetKind::AhdsrCurve:     return "ahdsr";
        case AssetKind::MorphAlgorithm: return "morph";
        case AssetKind::FrequencyGraph: return "frequency";
        case AssetKind::ConvolutionIR:  return "convolution_ir";
    }
    return "waveform";
}

bool assetKindFromTag(const std::string& tag, AssetKind& out) {
    if (tag == "waveform")        { out = AssetKind::Waveform;       return true; }
    if (tag == "instrument")      { out = AssetKind::Instrument;     return true; }
    if (tag == "ahdsr")           { out = AssetKind::AhdsrCurve;     return true; }
    if (tag == "morph")           { out = AssetKind::MorphAlgorithm; return true; }
    if (tag == "frequency")       { out = AssetKind::FrequencyGraph; return true; }
    if (tag == "convolution_ir")  { out = AssetKind::ConvolutionIR;  return true; }
    return false;
}

std::string AssetLibrary::computeHash(AssetKind kind, const std::string& subType,
                                      const std::string& payload) {
    // Hash a length-prefixed concatenation so field boundaries can't be confused
    // (e.g. subType="a", payload="bc" must differ from subType="ab", payload="c").
    // Kind is folded in as a leading byte. No normalization - conservative dedup.
    std::string buf;
    buf.reserve(payload.size() + subType.size() + 16);
    buf.push_back((char) assetKindTag(kind)[0]);
    buf.push_back(':');
    buf += std::to_string(subType.size());
    buf.push_back(':');
    buf += subType;
    buf.push_back(':');
    buf += std::to_string(payload.size());
    buf.push_back(':');
    buf += payload;
    return hash128(buf);
}

int AssetLibrary::allocId() { return nextId++; }

void AssetLibrary::noteId(int id) {
    if (id >= nextId) nextId = id + 1;
}

int AssetLibrary::add(AssetKind kind, const std::string& name,
                      const std::string& subType, const std::string& payload) {
    AssetEntry e;
    e.id = allocId();
    e.kind = kind;
    e.name = name;
    e.subType = subType;
    e.payload = payload;
    e.contentHash = computeHash(kind, subType, payload);
    e.archived = false;
    entries.push_back(std::move(e));
    return entries.back().id;
}

int AssetLibrary::duplicate(int srcId, const std::string& newName) {
    const AssetEntry* src = find(srcId);
    if (!src) return 0;
    // Copy fields before allocating (src may dangle after push_back reallocates).
    AssetKind kind = src->kind;
    std::string subType = src->subType;
    std::string payload = src->payload;
    return add(kind, newName, subType, payload);
}

bool AssetLibrary::update(int id, const std::string& subType,
                          const std::string& payload) {
    AssetEntry* e = find(id);
    if (!e) return false;
    e->subType = subType;
    e->payload = payload;
    e->contentHash = computeHash(e->kind, subType, payload);
    return true;
}

bool AssetLibrary::rename(int id, const std::string& newName) {
    AssetEntry* e = find(id);
    if (!e) return false;
    e->name = newName;
    return true;
}

bool AssetLibrary::archive(int id) {
    AssetEntry* e = find(id);
    if (!e) return false;
    e->archived = true;
    return true;
}

bool AssetLibrary::restore(int id) {
    AssetEntry* e = find(id);
    if (!e) return false;
    e->archived = false;
    return true;
}

bool AssetLibrary::setStarred(int id, bool starred) {
    AssetEntry* e = find(id);
    if (!e) return false;
    e->starred = starred;
    return true;
}

bool AssetLibrary::erase(int id) {
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == id) {
            entries.erase(entries.begin() + (long) i);
            return true;
        }
    }
    return false;
}

AssetEntry* AssetLibrary::find(int id) {
    for (auto& e : entries) if (e.id == id) return &e;
    return nullptr;
}

const AssetEntry* AssetLibrary::find(int id) const {
    for (auto& e : entries) if (e.id == id) return &e;
    return nullptr;
}

const AssetEntry* AssetLibrary::findByHash(const std::string& hash) const {
    for (auto& e : entries)
        if (!e.archived && e.contentHash == hash) return &e;
    return nullptr;
}

std::vector<const AssetEntry*> AssetLibrary::list(AssetKind kind,
                                                  bool includeArchived) const {
    std::vector<const AssetEntry*> out;
    for (auto& e : entries) {
        if (e.kind != kind) continue;
        if (e.archived && !includeArchived) continue;
        out.push_back(&e);
    }
    return out;
}

void AssetLibrary::insertRaw(AssetEntry e) {
    noteId(e.id);
    entries.push_back(std::move(e));
}

} // namespace SoundShop
