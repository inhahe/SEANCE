#include "content_store.h"
#include "hash_util.h"
#include <juce_core/juce_core.h>
#include <cstdio>
#include <cstring>

namespace SoundShop {
// 128-bit content hash lives in hash_util.h now (single source of truth, shared
// with the asset library). hash128(...) call sites below resolve to it via the
// enclosing SoundShop namespace, so they are unchanged.
namespace {

// ---- byte-plane shuffle filter (4-byte elements) ---------------------------
// Groups the 4 bytes of each float32 into separate planes so the high-entropy
// mantissa low bytes and the low-entropy exponent/sign high bytes cluster -
// DEFLATE then compresses smooth float grids far better. Pure permutation, so
// it's exactly reversible and never changes the byte count.
std::vector<uint8_t> shuffle4(const std::vector<uint8_t>& in) {
    const size_t n = in.size(), count = n / 4, rem = n % 4;
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < count; ++i)
        for (size_t p = 0; p < 4; ++p)
            out[p * count + i] = in[i * 4 + p];
    for (size_t i = 0; i < rem; ++i)              // trailing bytes verbatim
        out[4 * count + i] = in[4 * count + i];
    return out;
}
std::vector<uint8_t> unshuffle4(const std::vector<uint8_t>& in) {
    const size_t n = in.size(), count = n / 4, rem = n % 4;
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < count; ++i)
        for (size_t p = 0; p < 4; ++p)
            out[i * 4 + p] = in[p * count + i];
    for (size_t i = 0; i < rem; ++i)
        out[4 * count + i] = in[4 * count + i];
    return out;
}

// ---- DEFLATE (JUCE gzip streams) -------------------------------------------
std::vector<uint8_t> deflateBytes(const std::vector<uint8_t>& raw) {
    juce::MemoryOutputStream compressed;
    {
        juce::GZIPCompressorOutputStream gz(compressed, 9);
        gz.write(raw.data(), raw.size());
    } // gz destructor flushes
    const uint8_t* p = (const uint8_t*) compressed.getData();
    return std::vector<uint8_t>(p, p + compressed.getDataSize());
}
bool inflateBytes(const std::vector<uint8_t>& comp, std::vector<uint8_t>& out) {
    if (comp.empty()) { out.clear(); return false; }
    juce::MemoryInputStream mis(comp.data(), comp.size(), false);
    juce::GZIPDecompressorInputStream gz(mis);
    juce::MemoryBlock raw;
    gz.readIntoMemoryBlock(raw);
    if (raw.getSize() == 0) return false;
    const uint8_t* p = (const uint8_t*) raw.getData();
    out.assign(p, p + raw.getSize());
    return true;
}

// ---- CRC-32 (ZIP / PNG polynomial 0xEDB88320) ------------------------------
uint32_t crc32Of(const uint8_t* d, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Little-endian append helpers for the ZIP record fields.
void put16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back((uint8_t)(v & 0xff));
    o.push_back((uint8_t)((v >> 8) & 0xff));
}
void put32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)(v & 0xff));
    o.push_back((uint8_t)((v >> 8) & 0xff));
    o.push_back((uint8_t)((v >> 16) & 0xff));
    o.push_back((uint8_t)((v >> 24) & 0xff));
}

} // namespace

// ---- canonical .npy codec --------------------------------------------------
std::vector<uint8_t> ContentStore::makeNpy(const std::vector<float>& data,
                                           const std::vector<int>& shape) {
    // shape tuple, numpy-exact: (5,) for 1-D, (2, 3) for N-D, () for 0-D.
    std::string tuple = "(";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i) tuple += ", ";
        tuple += std::to_string(shape[i]);
    }
    if (shape.size() == 1) tuple += ",";
    tuple += ")";

    std::string dict = "{'descr': '<f4', 'fortran_order': False, 'shape': "
                     + tuple + ", }";
    // Pad so (10 + headerLen) is a multiple of 64 and the header ends in '\n'.
    const size_t preamble = 10;                 // magic(6)+version(2)+len(2)
    size_t total = preamble + dict.size() + 1;  // +1 for the trailing newline
    size_t pad = (64 - (total % 64)) % 64;
    dict.append(pad, ' ');
    dict += '\n';

    std::vector<uint8_t> out;
    out.reserve(preamble + dict.size() + data.size() * 4);
    const uint8_t magic[6] = { 0x93, 'N', 'U', 'M', 'P', 'Y' };
    out.insert(out.end(), magic, magic + 6);
    out.push_back(0x01); out.push_back(0x00);                 // version 1.0
    uint16_t hlen = (uint16_t) dict.size();
    out.push_back((uint8_t)(hlen & 0xff));
    out.push_back((uint8_t)((hlen >> 8) & 0xff));
    out.insert(out.end(), dict.begin(), dict.end());
    const uint8_t* fb = (const uint8_t*) data.data();
    out.insert(out.end(), fb, fb + data.size() * sizeof(float));
    return out;
}

bool ContentStore::parseNpy(const uint8_t* b, size_t n,
                            std::vector<float>& outData,
                            std::vector<int>& outShape) {
    outData.clear();
    outShape.clear();
    if (n < 10) return false;
    static const uint8_t magic[6] = { 0x93, 'N', 'U', 'M', 'P', 'Y' };
    if (std::memcmp(b, magic, 6) != 0) return false;
    if (b[6] != 0x01) return false;                       // only v1.x handled
    uint16_t hlen = (uint16_t)(b[8] | (b[9] << 8));
    size_t dataStart = 10 + (size_t) hlen;
    if (dataStart > n) return false;
    std::string header((const char*) b + 10, hlen);

    // dtype must be little-endian float32.
    if (header.find("'<f4'") == std::string::npos
        && header.find("'|f4'") == std::string::npos) return false;

    // shape tuple between 'shape': ( ... )
    size_t sp = header.find("'shape':");
    if (sp == std::string::npos) return false;
    size_t lp = header.find('(', sp);
    size_t rp = header.find(')', lp);
    if (lp == std::string::npos || rp == std::string::npos) return false;
    std::string inside = header.substr(lp + 1, rp - lp - 1);
    long long product = 1;
    {
        std::string num;
        auto flush = [&]() {
            if (!num.empty()) { int v = std::atoi(num.c_str());
                                outShape.push_back(v); product *= (v > 0 ? v : 0);
                                num.clear(); }
        };
        for (char c : inside) {
            if (c >= '0' && c <= '9') num += c;
            else flush();
        }
        flush();
    }
    if (outShape.empty()) return false;
    size_t avail = n - dataStart;
    if (avail != (size_t) product * sizeof(float)) return false;
    outData.resize((size_t) product);
    std::memcpy(outData.data(), b + dataStart, avail);
    return true;
}

// ---- .npz container (ZIP of STORED .npy members) ---------------------------
std::vector<uint8_t> ContentStore::makeNpz(
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members) {
    std::vector<uint8_t> out;

    struct CentralRec {
        std::string name;
        uint32_t crc, size, offset;
    };
    std::vector<CentralRec> central;
    central.reserve(members.size());

    // Local file headers + data, STORED (method 0, comp size == uncomp size).
    for (const auto& m : members) {
        const std::string& name = m.first;
        const std::vector<uint8_t>& data = m.second;
        uint32_t crc  = crc32Of(data.data(), data.size());
        uint32_t size = (uint32_t) data.size();           // < 4 GiB by contract
        uint32_t off  = (uint32_t) out.size();

        put32(out, 0x04034b50);            // local file header signature
        put16(out, 20);                    // version needed to extract (2.0)
        put16(out, 0);                     // general purpose bit flag
        put16(out, 0);                     // compression method = stored
        put16(out, 0);                     // last mod file time
        put16(out, 0x21);                  // last mod file date (1980-01-01)
        put32(out, crc);                   // crc-32
        put32(out, size);                  // compressed size
        put32(out, size);                  // uncompressed size
        put16(out, (uint16_t) name.size());// file name length
        put16(out, 0);                     // extra field length
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());

        central.push_back({ name, crc, size, off });
    }

    // Central directory.
    uint32_t cdStart = (uint32_t) out.size();
    for (const auto& c : central) {
        put32(out, 0x02014b50);            // central directory header signature
        put16(out, 20);                    // version made by
        put16(out, 20);                    // version needed to extract
        put16(out, 0);                     // general purpose bit flag
        put16(out, 0);                     // compression method = stored
        put16(out, 0);                     // last mod file time
        put16(out, 0x21);                  // last mod file date
        put32(out, c.crc);                 // crc-32
        put32(out, c.size);                // compressed size
        put32(out, c.size);                // uncompressed size
        put16(out, (uint16_t) c.name.size()); // file name length
        put16(out, 0);                     // extra field length
        put16(out, 0);                     // file comment length
        put16(out, 0);                     // disk number start
        put16(out, 0);                     // internal file attributes
        put32(out, 0);                     // external file attributes
        put32(out, c.offset);              // relative offset of local header
        out.insert(out.end(), c.name.begin(), c.name.end());
    }
    uint32_t cdSize = (uint32_t) out.size() - cdStart;

    // End of central directory record.
    put32(out, 0x06054b50);                // EOCD signature
    put16(out, 0);                         // number of this disk
    put16(out, 0);                         // disk with start of central dir
    put16(out, (uint16_t) central.size()); // entries on this disk
    put16(out, (uint16_t) central.size()); // total entries
    put32(out, cdSize);                    // size of central directory
    put32(out, cdStart);                   // offset of central directory
    put16(out, 0);                         // .ZIP file comment length

    return out;
}

// ---- store API -------------------------------------------------------------
std::string ContentStore::putFloatGrid(const std::vector<float>& data,
                                       const std::vector<int>& shape) {
    std::vector<uint8_t> npy = makeNpy(data, shape);
    std::string hash = hash128(npy.data(), npy.size());
    if (blobs.find(hash) == blobs.end())
        blobs[hash] = Entry{ deflateBytes(shuffle4(npy)) };
    return hash;
}

bool ContentStore::getFloatGrid(const std::string& hash,
                                std::vector<float>& outData,
                                std::vector<int>& outShape) const {
    auto it = blobs.find(hash);
    if (it == blobs.end()) return false;
    std::vector<uint8_t> infl;
    if (!inflateBytes(it->second.compressed, infl)) return false;
    std::vector<uint8_t> npy = unshuffle4(infl);
    return parseNpy(npy.data(), npy.size(), outData, outShape);
}

bool ContentStore::has(const std::string& hash) const {
    return blobs.find(hash) != blobs.end();
}

void ContentStore::insertRaw(const std::string& hash,
                             std::vector<uint8_t> compressed) {
    blobs[hash] = Entry{ std::move(compressed) };
}

void ContentStore::retainOnly(const std::vector<std::string>& keep) {
    std::map<std::string, Entry> kept;
    for (const auto& h : keep) {
        auto it = blobs.find(h);
        if (it != blobs.end()) kept[h] = it->second;
    }
    blobs.swap(kept);
}

} // namespace SoundShop
