#include "waveform_bank.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

WaveformBank& WaveformBank::get() {
    static WaveformBank instance;
    return instance;
}

bool WaveformBank::ensureLoaded() {
    if (loaded) return !entries.empty();
    loaded = true;  // attempt-once: even on failure we don't retry every open

    // Search order: next to the executable (where CMake copies the asset for a
    // packaged/installed build), then a couple of dev-tree locations so running
    // straight out of the build folder also works.
    std::vector<juce::File> candidates;
    auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                      .getParentDirectory();
    candidates.push_back(exeDir.getChildFile("resources").getChildFile("waveforms.bin"));
    candidates.push_back(exeDir.getChildFile("waveforms.bin"));
    // Dev tree: <repo>/cpp/resources/waveforms.bin reached from the build output.
    candidates.push_back(exeDir.getParentDirectory().getParentDirectory()
                             .getParentDirectory()
                             .getChildFile("resources").getChildFile("waveforms.bin"));

    for (const auto& f : candidates) {
        if (f.existsAsFile() && loadFrom(f))
            return !entries.empty();
    }

    if (error.empty())
        error = "Factory waveform library (waveforms.bin) not found next to the "
                "application. Run cpp/tools/pack_waveforms.py to build it.";
    return false;
}

bool WaveformBank::loadFrom(const juce::File& f) {
    juce::MemoryBlock raw;
    if (!f.loadFileAsData(raw)) {
        error = "Could not read " + f.getFullPathName().toStdString();
        return false;
    }
    const auto* base = static_cast<const uint8_t*>(raw.getData());
    const size_t size = raw.getSize();
    size_t pos = 0;

    auto need = [&](size_t n) -> bool { return pos + n <= size; };
    auto rdU32 = [&](uint32_t& out) -> bool {
        if (!need(4)) return false;
        out = (uint32_t)base[pos] | ((uint32_t)base[pos + 1] << 8)
            | ((uint32_t)base[pos + 2] << 16) | ((uint32_t)base[pos + 3] << 24);
        pos += 4;
        return true;
    };
    auto rdU16 = [&](uint16_t& out) -> bool {
        if (!need(2)) return false;
        out = (uint16_t)(base[pos] | (base[pos + 1] << 8));
        pos += 2;
        return true;
    };

    if (!need(4) || std::memcmp(base, "SSWB", 4) != 0) {
        error = "Bad magic in " + f.getFileName().toStdString();
        return false;
    }
    pos += 4;

    uint32_t version = 0, sampleCount = 0, entryCount = 0;
    if (!rdU32(version) || !rdU32(sampleCount) || !rdU32(entryCount)) {
        error = "Truncated header in " + f.getFileName().toStdString();
        return false;
    }
    if (version != 1 || sampleCount != (uint32_t)kSampleCount) {
        error = "Unsupported waveform bank version/format.";
        return false;
    }

    entries.clear();
    entries.reserve(entryCount);
    categoryOrder.clear();
    std::vector<std::string> seenCats;

    for (uint32_t i = 0; i < entryCount; ++i) {
        if (!need(1)) { error = "Truncated index."; return false; }
        const uint8_t curated = base[pos++];
        uint16_t nameLen = 0, catLen = 0;
        if (!rdU16(nameLen) || !need(nameLen)) { error = "Truncated name."; return false; }
        std::string name((const char*)base + pos, nameLen);
        pos += nameLen;
        if (!rdU16(catLen) || !need(catLen)) { error = "Truncated category."; return false; }
        std::string cat((const char*)base + pos, catLen);
        pos += catLen;

        Entry e;
        e.name = std::move(name);
        e.category = std::move(cat);
        e.curated = (curated != 0);
        e.sampleStart = (int)i * kSampleCount;
        entries.push_back(std::move(e));
    }

    // Sample blob: entryCount * kSampleCount int16 normalised by 32767.
    const size_t totalSamples = (size_t)entryCount * (size_t)kSampleCount;
    if (!need(totalSamples * 2)) {
        error = "Truncated sample blob.";
        entries.clear();
        return false;
    }
    sampleBlob.resize(totalSamples);
    for (size_t i = 0; i < totalSamples; ++i) {
        const int16_t v = (int16_t)(base[pos] | (base[pos + 1] << 8));
        pos += 2;
        sampleBlob[i] = (float)v / 32767.0f;
    }

    // First-seen category order (the packer already emits entries grouped by
    // category and curated-first within a category, so just collect uniques).
    for (const auto& e : entries) {
        if (std::find(categoryOrder.begin(), categoryOrder.end(), e.category)
            == categoryOrder.end())
            categoryOrder.push_back(e.category);
    }

    error.clear();
    return true;
}

std::vector<int> WaveformBank::entriesInCategory(const std::string& cat) const {
    std::vector<int> out;
    for (int i = 0; i < (int)entries.size(); ++i)
        if (entries[(size_t)i].category == cat)
            out.push_back(i);
    return out;
}

std::vector<float> WaveformBank::samples(int i) const {
    std::vector<float> out((size_t)kSampleCount, 0.0f);
    if (i < 0 || i >= (int)entries.size()) return out;
    const int start = entries[(size_t)i].sampleStart;
    if (start < 0 || start + kSampleCount > (int)sampleBlob.size()) return out;
    std::copy(sampleBlob.begin() + start,
              sampleBlob.begin() + start + kSampleCount, out.begin());
    return out;
}

// Lowercase + trim, so "  AKWF Oboe 0001 " and "akwf oboe 0001" both resolve to
// the same entry. Used for both the stored keys and the query.
static std::string normaliseName(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    std::string out;
    out.reserve(b - a);
    for (size_t i = a; i < b; ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out.push_back(c);
    }
    return out;
}

int WaveformBank::indexForName(const std::string& name) const {
    if (entries.empty()) return -1;
    if (nameLookup.empty()) {
        // First-seen wins on a duplicate name (matches browser ordering).
        for (int i = 0; i < (int)entries.size(); ++i)
            nameLookup.emplace(normaliseName(entries[(size_t)i].name), i);
    }
    auto it = nameLookup.find(normaliseName(name));
    return it == nameLookup.end() ? -1 : it->second;
}

const float* WaveformBank::sampleData(int i) const {
    if (i < 0 || i >= (int)entries.size()) return nullptr;
    const int start = entries[(size_t)i].sampleStart;
    if (start < 0 || start + kSampleCount > (int)sampleBlob.size()) return nullptr;
    return sampleBlob.data() + start;
}

float WaveformBank::sampleAtPhase(int i, float phase) const {
    const float* d = sampleData(i);
    if (!d) return 0.0f;
    // Wrap phase into [0,1) (handles negative and >1 inputs), then linearly
    // interpolate across the cycle with wrap-around at the seam.
    float p = phase - std::floor(phase);          // [0,1)
    float fp = p * (float)kSampleCount;            // [0, kSampleCount)
    int i0 = (int)fp;
    if (i0 >= kSampleCount) i0 = kSampleCount - 1; // guard fp == kSampleCount via fp rounding
    int i1 = i0 + 1; if (i1 >= kSampleCount) i1 = 0;
    float frac = fp - (float)i0;
    return d[i0] + (d[i1] - d[i0]) * frac;
}
