#pragma once
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <string>

namespace SoundShop {

// ==============================================================================
// Precomputed HRTF lookup table
//
// Generated from a spherical head diffraction model (Woodworth ITD +
// frequency-dependent head shadow + pinna comb model). Stored as short
// impulse responses (64 samples per ear per direction) at a grid of
// azimuth/elevation positions.
//
// Azimuth:   0° to 180° in 15° steps (13 positions). Negative azimuths
//            are symmetric (swap L and R channels).
// Elevation: -40° to +90° in 10° steps (14 positions).
// Total:     13 × 14 = 182 direction pairs × 64 samples × 2 ears.
//
// At runtime, the spatializer finds the 4 nearest grid points and
// bilinearly interpolates their IRs, then convolves with the audio.
// ==============================================================================

static constexpr int HRTF_IR_LENGTH = 64;
static constexpr int HRTF_NUM_AZIMUTHS = 13;   // 0, 15, 30, ..., 180
static constexpr int HRTF_NUM_ELEVATIONS = 14;  // -40, -30, ..., 90
static constexpr float HRTF_AZ_STEP = 15.0f;
static constexpr float HRTF_EL_MIN = -40.0f;
static constexpr float HRTF_EL_STEP = 10.0f;

struct HRTFEntry {
    float left[HRTF_IR_LENGTH];
    float right[HRTF_IR_LENGTH];
};

// The full lookup table — generated at first access.
class HRTFTable {
public:
    static HRTFTable& instance() {
        static HRTFTable table;
        return table;
    }

    // Look up an interpolated HRTF for any azimuth/elevation.
    // azimuth: -180 to +180 (degrees), elevation: -90 to +90
    // Writes HRTF_IR_LENGTH samples into irLeft and irRight.
    void lookup(float azimuthDeg, float elevationDeg,
                float* irLeft, float* irRight) const;

    // Check if external HRTF data was loaded
    bool hasExternalData() const { return externalLoaded; }

    // Load HRTF IRs from a directory of WAV files (MIT KEMAR format:
    // files named like "H{elev}e{azimuth}a.wav", stereo, L=left ear)
    bool loadFromDirectory(const std::string& path);

private:
    HRTFTable();

    // Grid data: entries[azIdx][elIdx]
    HRTFEntry entries[HRTF_NUM_AZIMUTHS][HRTF_NUM_ELEVATIONS];
    bool externalLoaded = false;

    // Generate one HRTF entry from the spherical head model
    void generateEntry(float azimuthDeg, float elevationDeg, HRTFEntry& entry);

    // Bilinear interpolation helper
    void interpolate(const HRTFEntry& a, const HRTFEntry& b,
                     const HRTFEntry& c, const HRTFEntry& d,
                     float fracAz, float fracEl,
                     float* irLeft, float* irRight) const;
};

} // namespace SoundShop
