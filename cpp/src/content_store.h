#pragma once
#include <vector>
#include <string>
#include <map>
#include <utility>
#include <cstdint>

namespace SoundShop {

// ============================================================================
// ContentStore - content-addressed store for large immutable per-node binary
// payloads (baked terrain grids, and later decoded video / wavetable PCM).
//
// WHY IT EXISTS: large baked blobs used to live as base64 *inside* node.script.
// Because every undo step serializes the whole graph (node.script included),
// each blob was copied into every snapshot string, ballooning undo memory; the
// same blob was also base64-inflated (+33%) in RAM and on disk. The store fixes
// all three: the blob lives here once, keyed by a hash of its content, and
// node.script holds only the short hash. Undo snapshots therefore carry the
// hash, not the bytes (mirrors how plugin state is already excluded from undo
// serialization). See known-issues.md "baked terrain blobs ... undo snapshot".
//
// CANONICAL FORM = .npy. A grid is serialized to a NumPy .npy payload (tiny
// header describing dtype/shape + raw little-endian float32, C-order). That
// payload is what gets HASHED (so the hash is independent of compression /
// framing and is deterministic), and it is also exactly what an .npz export
// needs - export is "zip the canonical payload", no recompute.
//
// STORED FORM = shuffle + DEFLATE. Internally each entry holds the canonical
// payload after a byte-plane "shuffle" filter (groups the 4 bytes of each
// float32 into separate planes - the same trick HDF5/EXR use, which makes
// smooth float data compress far better) followed by zlib/gzip DEFLATE. The
// shuffle+compression are pure storage details *under* the hash: the hash never
// sees them, so DEFLATE's nondeterministic timestamp can't affect dedup.
// ============================================================================
class ContentStore {
public:
    // Store a float grid (+ shape) as a canonical .npy payload, compressed.
    // Returns the content hash (32 hex chars). Idempotent: identical (data,
    // shape) -> identical hash, stored exactly once.
    std::string putFloatGrid(const std::vector<float>& data,
                             const std::vector<int>& shape);

    // Resolve a hash back to floats (+ shape). Returns false if the hash is
    // absent or the stored bytes are corrupt.
    bool getFloatGrid(const std::string& hash,
                      std::vector<float>& outData,
                      std::vector<int>& outShape) const;

    bool has(const std::string& hash) const;

    // ---- Serialization support (project save/load) ------------------------
    // Entries are persisted as the compressed bytes keyed by hash. On load the
    // hash is trusted from the file (not recomputed) so loading never has to
    // decompress; integrity is only checked lazily when a node resolves a blob.
    struct Entry { std::vector<uint8_t> compressed; };
    const std::map<std::string, Entry>& entries() const { return blobs; }
    void insertRaw(const std::string& hash, std::vector<uint8_t> compressed);

    void clear() { blobs.clear(); }
    size_t size() const { return blobs.size(); }

    // Drop every entry whose hash is NOT in `keep`. Called at save time so the
    // on-disk store only carries blobs the current graph references (orphans
    // created by edits this session are pruned). The in-memory store is NOT
    // pruned mid-session - undo needs blobs that the current graph no longer
    // references - so this is applied to a copy when writing, never live.
    void retainOnly(const std::vector<std::string>& keep);

    // ---- Canonical .npy codec (also used by export) -----------------------
    // Build the canonical .npy payload bytes for a float32 C-order grid.
    static std::vector<uint8_t> makeNpy(const std::vector<float>& data,
                                        const std::vector<int>& shape);
    // Parse a canonical .npy payload (float32, C-order) back to floats + shape.
    static bool parseNpy(const uint8_t* bytes, size_t n,
                         std::vector<float>& outData,
                         std::vector<int>& outShape);

    // ---- .npz export ------------------------------------------------------
    // Build a NumPy .npz container (a ZIP archive of .npy members) from a list
    // of (memberName, npyBytes) pairs. Members are STORED (uncompressed) - this
    // is exactly what numpy's np.savez() writes (np.savez_compressed deflates),
    // so the result loads with np.load(...) -> dict keyed by member name minus
    // the ".npy" suffix. memberName should already include ".npy". Each member
    // payload is typically the output of makeNpy(). Returns the full file bytes.
    // Caps at 4 GiB total / per member (no ZIP64) - far past any terrain grid.
    static std::vector<uint8_t> makeNpz(
        const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members);

private:
    std::map<std::string, Entry> blobs;
};

} // namespace SoundShop
