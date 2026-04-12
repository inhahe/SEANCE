#include "tracker_file_parser.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace SoundShop {

// ==============================================================================
// Binary reading helpers
// ==============================================================================
//
// Everything in tracker files is little-endian. We work on a std::vector<uint8_t>
// loaded into memory up front — files are small (a few MB max) so streaming
// isn't worth the complexity.

namespace {

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& b) : buf(b) {}

    bool ok(size_t n) const { return pos + n <= buf.size(); }
    size_t tell() const { return pos; }
    size_t size() const { return buf.size(); }
    void seek(size_t p) { pos = p; }

    uint8_t u8() {
        if (!ok(1)) return 0;
        return buf[pos++];
    }
    uint16_t u16() {
        if (!ok(2)) return 0;
        uint16_t v = (uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8);
        pos += 2;
        return v;
    }
    int16_t s16() { return (int16_t)u16(); }
    uint32_t u32() {
        if (!ok(4)) return 0;
        uint32_t v = (uint32_t)buf[pos]
                   | ((uint32_t)buf[pos + 1] << 8)
                   | ((uint32_t)buf[pos + 2] << 16)
                   | ((uint32_t)buf[pos + 3] << 24);
        pos += 4;
        return v;
    }
    void copyBytes(void* dst, size_t n) {
        if (!ok(n)) { std::memset(dst, 0, n); pos = buf.size(); return; }
        std::memcpy(dst, buf.data() + pos, n);
        pos += n;
    }
    bool compareTag(const char* tag, size_t n) const {
        if (pos + n > buf.size()) return false;
        return std::memcmp(buf.data() + pos, tag, n) == 0;
    }

private:
    const std::vector<uint8_t>& buf;
    size_t pos = 0;
};

// Load a file into a byte buffer. Returns empty vector on failure.
static std::vector<uint8_t> loadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto size = (std::streamsize)f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf((size_t)size);
    if (size > 0 && !f.read(reinterpret_cast<char*>(buf.data()), size))
        return {};
    return buf;
}

// Trim trailing nulls/spaces from a fixed-width tracker string (names
// are often padded with spaces or zero-filled).
static std::string trimTrackerString(const char* s, size_t maxLen) {
    size_t len = 0;
    while (len < maxLen && s[len] != 0) ++len;
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == 0)) --len;
    return std::string(s, len);
}

// ==============================================================================
// IT parsing
// ==============================================================================

// Parse a single IT envelope from the current reader position. IT
// envelope headers are 82 bytes each:
//
//   [0]     flags   — bit0 enabled, bit1 loop, bit2 sustain loop,
//                     bit7 (pitch env only) = filter env
//   [1]     num_nodes
//   [2]     loop_start
//   [3]     loop_end
//   [4]     sustain_start
//   [5]     sustain_end
//   [6..75] 25 × (int8 value, int16 tick) = 75 bytes
//   [76]    reserved (0)
// Total: 82 bytes
static TrackerEnvelope parseITEnvelope(Reader& r) {
    TrackerEnvelope e;
    size_t start = r.tell();

    uint8_t flags = r.u8();
    e.enabled     = (flags & 0x01) != 0;
    e.hasLoop     = (flags & 0x02) != 0;
    e.hasSustain  = (flags & 0x04) != 0;
    e.isFilterEnv = (flags & 0x80) != 0;

    int n = r.u8();
    e.loopStart    = r.u8();
    e.loopEnd      = r.u8();
    e.sustainStart = r.u8();
    e.sustainEnd   = r.u8();

    n = std::min(n, 25);
    for (int i = 0; i < 25; ++i) {
        int8_t value = (int8_t)r.u8();
        int16_t tick = r.s16();
        if (i < n) {
            TrackerEnvelopePoint p;
            p.tick  = (float)tick;
            p.value = (float)value;
            e.points.push_back(p);
        }
    }
    // Reserved / padding: advance to start + 82 regardless.
    r.seek(start + 82);
    return e;
}

static bool parseITFile(Reader& r, ParsedTrackerFile& out) {
    if (!r.compareTag("IMPM", 4)) return false;
    out.format = ParsedTrackerFile::Format::IT;
    r.seek(0);
    r.u32(); // "IMPM"

    // Song name (26 bytes)
    char songName[26];
    r.copyBytes(songName, 26);
    (void)songName;

    r.u16(); // PHiliht (pattern highlight)
    // Re-read from known offsets instead of sequential reading (more robust).
    r.seek(0x20);
    uint16_t ordNum = r.u16();
    uint16_t insNum = r.u16();
    uint16_t smpNum = r.u16();
    uint16_t patNum = r.u16();
    r.u16(); // Cwt/v
    r.u16(); // Cmwt
    uint16_t flags   = r.u16();
    uint16_t special = r.u16();
    (void)flags;
    r.seek(0xC0);
    // Skip the order list.
    for (uint32_t i = 0; i < ordNum; ++i) r.u8();

    out.numInstruments = insNum;

    // Read instrument offset table (insNum × uint32).
    std::vector<uint32_t> insOffsets(insNum);
    for (uint16_t i = 0; i < insNum; ++i)
        insOffsets[i] = r.u32();
    // Skip sample offsets + pattern offsets.
    for (uint32_t i = 0; i < smpNum; ++i) r.u32();
    for (uint32_t i = 0; i < patNum; ++i) r.u32();

    // MIDI macro configuration: present if bit 3 of the "Special" header
    // field is set. It sits right after the offset tables — i.e. here.
    // Layout: 16 × 32-byte SF macros + 128 × 32-byte Z macros = 4608 bytes.
    if ((special & 0x08) && r.ok(4608)) {
        out.hasMidiMacros = true;
        char macBuf[32];
        for (int m = 0; m < 16; ++m) {
            r.copyBytes(macBuf, 32);
            out.midiMacros.sfMacros[m] = trimTrackerString(macBuf, 31);
        }
        for (int m = 0; m < 128; ++m) {
            r.copyBytes(macBuf, 32);
            out.midiMacros.zMacros[m] = trimTrackerString(macBuf, 31);
        }
    }

    if (insNum == 0)
        return true;

    out.instruments.resize(insNum + 1); // 1-based

    for (uint16_t i = 0; i < insNum; ++i) {
        uint32_t off = insOffsets[i];
        if (off == 0 || off + 0x200 > r.size()) continue;
        r.seek(off);
        if (!r.compareTag("IMPI", 4)) continue;
        r.u32(); // "IMPI"

        char dosName[12];
        r.copyBytes(dosName, 12);
        (void)dosName;
        r.u8(); // 0x1C: zero byte

        TrackerInstrument inst;

        uint8_t nna = r.u8(); // 0x1D NNA
        inst.newNoteAction = (TrackerNNA)std::min<uint8_t>(nna, 3);
        r.u8(); // DCT
        r.u8(); // DCA
        inst.fadeOut = r.u16();  // FadeOut (0..1024 in IT)
        r.u8(); // PPS
        r.u8(); // PPC
        int gv = r.u8();         // Global volume (0..128)
        inst.defaultVolume = gv * 128 / 128; // already 0..128
        uint8_t dp = r.u8();     // Default pan (0..64, bit 7 = don't use)
        if ((dp & 0x80) == 0) {
            inst.hasDefaultPan = true;
            inst.defaultPan = (float)(dp & 0x7F) / 32.0f - 1.0f;
        }
        inst.randomVol = r.u8();
        inst.randomPan = r.u8();
        r.u16(); // TrkVers
        r.u8();  // NOS (num of samples referenced)
        r.u8();  // x

        char instName[26];
        r.copyBytes(instName, 26);
        inst.name = trimTrackerString(instName, 26);

        // Initial filter cutoff / resonance (IT > 2.04). High bit = "use".
        uint8_t ifc = r.u8();
        uint8_t ifr = r.u8();
        inst.useFilterCutoff = (ifc & 0x80) != 0;
        inst.filterCutoff    = ifc & 0x7F;
        inst.useFilterResonance = (ifr & 0x80) != 0;
        inst.filterResonance    = ifr & 0x7F;
        r.u8(); // MCh
        r.u8(); // MPr
        r.u16(); // MIDIBnk

        // Note-sample keymap: 120 × (note, sample)
        inst.noteMap.resize(120);
        for (int k = 0; k < 120; ++k) {
            inst.noteMap[k].note   = r.u8();
            inst.noteMap[k].sample = r.u8();
        }

        // Three envelopes: volume, pan, pitch (each 82 bytes).
        inst.volumeEnv = parseITEnvelope(r);
        inst.panEnv    = parseITEnvelope(r);
        inst.pitchEnv  = parseITEnvelope(r);

        out.instruments[i + 1] = std::move(inst);
    }
    return true;
}

// ==============================================================================
// XM parsing
// ==============================================================================
//
// XM layout: 60-byte header, then an extended header, then sequential
// pattern blocks, then sequential instrument blocks. Each instrument
// block starts with an instrument header (29 bytes + variable
// extension), followed by per-sample headers, followed by raw sample
// data. We walk the whole file sequentially to find instrument N.

static TrackerEnvelope parseXMEnvelope(const uint8_t* env, int numPoints,
                                        uint8_t type, uint8_t sustain,
                                        uint8_t loopStart, uint8_t loopEnd) {
    // XM envelopes: 12 × (x: u16, y: u16) = 48 bytes, little-endian.
    // `type` bits: 0=enabled, 1=sustain enabled, 2=loop enabled.
    TrackerEnvelope e;
    e.enabled    = (type & 0x01) != 0;
    e.hasSustain = (type & 0x02) != 0;
    e.hasLoop    = (type & 0x04) != 0;
    e.sustainStart = e.sustainEnd = sustain;
    e.loopStart = loopStart;
    e.loopEnd   = loopEnd;
    int n = std::min(12, (int)numPoints);
    for (int i = 0; i < n; ++i) {
        uint16_t x = (uint16_t)env[i * 4] | ((uint16_t)env[i * 4 + 1] << 8);
        uint16_t y = (uint16_t)env[i * 4 + 2] | ((uint16_t)env[i * 4 + 3] << 8);
        TrackerEnvelopePoint p;
        p.tick  = (float)x;
        // XM volume envelope y is 0..64; pan envelope y is 0..64 too
        // (0=left, 32=center, 64=right). The caller interprets.
        p.value = (float)y;
        e.points.push_back(p);
    }
    return e;
}

static bool parseXMFile(Reader& r, ParsedTrackerFile& out) {
    if (!r.compareTag("Extended Module: ", 17)) return false;
    out.format = ParsedTrackerFile::Format::XM;

    r.seek(0);
    char idText[17];
    r.copyBytes(idText, 17);          // "Extended Module: "
    (void)idText;
    char moduleName[20];
    r.copyBytes(moduleName, 20);       // 20-byte module name
    (void)moduleName;
    r.u8();                            // 0x1A marker
    char trackerName[20];
    r.copyBytes(trackerName, 20);      // 20-byte tracker name
    (void)trackerName;
    r.u16();                           // version number
    uint32_t headerSize = r.u32();     // header size (incl. these 4 bytes)
    r.u16();                           // song length
    r.u16();                           // restart position
    /*uint16_t numChannels =*/ r.u16();
    uint16_t numPatterns = r.u16();
    uint16_t numInstruments = r.u16();
    r.u16();                           // flags
    r.u16();                           // default tempo
    r.u16();                           // default BPM
    // Pattern order table is 256 bytes.
    // Seek past: start of file + headerSize (header) + 60 (fixed part)
    // headerSize measures from the field itself, so the next item is
    // at offset 60 + headerSize.
    size_t afterHeader = 60 + headerSize;
    r.seek(afterHeader);

    out.numInstruments = numInstruments;
    if (numInstruments == 0) return true;

    out.instruments.resize(numInstruments + 1);

    // Skip all patterns (each: 9-byte header + packed data).
    for (int p = 0; p < numPatterns; ++p) {
        size_t patStart = r.tell();
        uint32_t patHdrLen = r.u32();
        r.u8();                        // packing type
        /*uint16_t numRows =*/ r.u16();
        uint16_t packedSize = r.u16();
        r.seek(patStart + patHdrLen);
        // Advance past packed pattern data (may be zero length).
        r.seek(r.tell() + packedSize);
    }

    // Now instruments are laid out sequentially. Each instrument:
    //   [4] instrumentSize
    //   [22] instrumentName
    //   [1]  type byte (always 0)
    //   [2]  numSamples
    //   [if numSamples > 0]:
    //       [4] sampleHeaderSize
    //       [96] sampleMap (note→sample)
    //       [48] volume envelope
    //       [48] pan envelope
    //       [1] numVolumePoints
    //       [1] numPanPoints
    //       [1] volSustain
    //       [1] volLoopStart
    //       [1] volLoopEnd
    //       [1] panSustain
    //       [1] panLoopStart
    //       [1] panLoopEnd
    //       [1] volType
    //       [1] panType
    //       [1] vibratoType
    //       [1] vibratoSweep
    //       [1] vibratoDepth
    //       [1] vibratoRate
    //       [2] volumeFadeout
    //       [22] reserved
    //       Then per-sample: numSamples × 40-byte sample header
    //       Then per-sample: numSamples × sample data
    for (int i = 0; i < numInstruments; ++i) {
        size_t instStart = r.tell();
        uint32_t instSize = r.u32();
        if (instSize == 0 || instStart + instSize > r.size()) break;

        char instName[22];
        r.copyBytes(instName, 22);
        r.u8();   // type
        uint16_t numSamples = r.u16();

        TrackerInstrument inst;
        inst.name = trimTrackerString(instName, 22);

        if (numSamples > 0) {
            uint32_t sampleHeaderSize = r.u32();
            (void)sampleHeaderSize;
            // Sample map: 96 bytes, one per note (indexed from 0 = C-0 in XM).
            inst.noteMap.resize(120);
            for (int k = 0; k < 120; ++k) {
                inst.noteMap[k].note   = k;
                inst.noteMap[k].sample = 0;
            }
            uint8_t sampleMap[96];
            r.copyBytes(sampleMap, 96);
            // Copy map into the 120-slot map; XM covers notes 0..95.
            for (int k = 0; k < 96; ++k) {
                inst.noteMap[k].note   = k;
                inst.noteMap[k].sample = sampleMap[k] + 1; // XM is 0-based
            }

            uint8_t volEnv[48], panEnv[48];
            r.copyBytes(volEnv, 48);
            r.copyBytes(panEnv, 48);
            uint8_t numVol   = r.u8();
            uint8_t numPan   = r.u8();
            uint8_t volSus   = r.u8();
            uint8_t volLoopS = r.u8();
            uint8_t volLoopE = r.u8();
            uint8_t panSus   = r.u8();
            uint8_t panLoopS = r.u8();
            uint8_t panLoopE = r.u8();
            uint8_t volType  = r.u8();
            uint8_t panType  = r.u8();
            r.u8(); // vibratoType
            r.u8(); // vibratoSweep
            r.u8(); // vibratoDepth
            r.u8(); // vibratoRate
            uint16_t volFade = r.u16();
            inst.fadeOut = volFade;
            // Advance past the reserved bytes in the instrument header
            // to the start of the per-sample headers. The header size
            // from `instSize` tells us where the sample headers start.
            r.seek(instStart + instSize);

            inst.volumeEnv = parseXMEnvelope(volEnv, numVol, volType,
                                              volSus, volLoopS, volLoopE);
            inst.panEnv    = parseXMEnvelope(panEnv, numPan, panType,
                                              panSus, panLoopS, panLoopE);

            // Skip sample headers + sample data. Each sample header is
            // 40 bytes and is followed by its raw sample data.
            std::vector<uint32_t> sampleLens(numSamples);
            std::vector<uint8_t>  sampleBits(numSamples);
            for (int s = 0; s < numSamples; ++s) {
                size_t shStart = r.tell();
                sampleLens[s] = r.u32();
                r.u32(); // loop start
                r.u32(); // loop length
                r.u8();  // volume
                r.u8();  // finetune
                uint8_t type = r.u8();
                sampleBits[s] = (type & 0x10) ? 16 : 8;
                r.u8();  // panning
                r.u8();  // relative note
                r.u8();  // reserved
                char sampleName[22];
                r.copyBytes(sampleName, 22);
                (void)sampleName;
                r.seek(shStart + 40);
            }
            for (int s = 0; s < numSamples; ++s) {
                r.seek(r.tell() + sampleLens[s]);
            }
        } else {
            // No samples — advance past the instrument header block.
            r.seek(instStart + instSize);
        }

        out.instruments[i + 1] = std::move(inst);
    }
    return true;
}

} // anonymous namespace

ParsedTrackerFile parseTrackerFile(const std::string& path) {
    ParsedTrackerFile out;
    auto buf = loadFile(path);
    if (buf.empty()) {
        out.errorMessage = "Cannot open file";
        return out;
    }
    Reader r(buf);

    // Dispatch by file signature. IT files begin with "IMPM", XM files
    // with the literal "Extended Module: " string. Other formats (MOD,
    // S3M, mo3-packed, etc.) aren't supported yet — we return
    // format=Unknown so the caller falls back to sample-mode import.
    if (r.compareTag("IMPM", 4)) {
        if (parseITFile(r, out)) return out;
        out.errorMessage = "IT header present but parse failed";
    } else if (r.compareTag("Extended Module: ", 17)) {
        if (parseXMFile(r, out)) return out;
        out.errorMessage = "XM header present but parse failed";
    } else {
        out.errorMessage = "Not an IT or XM file (instrument mode only)";
    }
    return out;
}

} // namespace SoundShop
