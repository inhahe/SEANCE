#define _USE_MATH_DEFINES
#include "hrtf_data.h"
#include "fft_util.h"
#include <cmath>
#include <complex>
#include <cstring>

namespace SoundShop {

// ==============================================================================
// Spherical head diffraction model — generates per-frequency HRTF
//
// Based on the Duda-Martens spherical head model:
// - ITD from Woodworth formula
// - Frequency-dependent ILD from sphere diffraction (simplified)
// - Pinna spectral notch from elevation-dependent comb
//
// This generates a proper impulse response (not just a delay + gain) that
// captures the frequency-dependent nature of head shadow.
// ==============================================================================

static constexpr double HEAD_RADIUS = 0.0875;  // meters
static constexpr double SPEED_OF_SOUND = 343.0;
static constexpr double SAMPLE_RATE = 44100.0;

HRTFTable::HRTFTable() {
    // Precompute HRTF for all grid positions
    for (int ai = 0; ai < HRTF_NUM_AZIMUTHS; ++ai) {
        for (int ei = 0; ei < HRTF_NUM_ELEVATIONS; ++ei) {
            float az = ai * HRTF_AZ_STEP;
            float el = HRTF_EL_MIN + ei * HRTF_EL_STEP;
            generateEntry(az, el, entries[ai][ei]);
        }
    }
}

void HRTFTable::generateEntry(float azimuthDeg, float elevationDeg, HRTFEntry& entry) {
    float azRad = azimuthDeg * (float)M_PI / 180.0f;
    float elRad = elevationDeg * (float)M_PI / 180.0f;

    // Effective angle of incidence (accounting for elevation)
    float cosIncidenceL = std::sin(azRad) * std::cos(elRad); // +1 = directly at left ear
    float cosIncidenceR = -cosIncidenceL; // opposite ear

    // === Build frequency-domain HRTF ===
    int N = HRTF_IR_LENGTH * 2; // zero-padded FFT size
    int halfN = N / 2 + 1;

    std::vector<std::complex<float>> specL(halfN), specR(halfN);

    for (int k = 0; k < halfN; ++k) {
        float freq = (float)k * (float)SAMPLE_RATE / (float)N;
        if (freq < 1.0f) freq = 1.0f;
        float omega = 2.0f * (float)M_PI * freq;

        // Normalized frequency: ka = omega * a / c
        float ka = (float)(omega * HEAD_RADIUS / SPEED_OF_SOUND);

        // === Sphere diffraction: frequency-dependent magnitude ===
        // Simplified model: at low frequencies (ka << 1), the head is
        // transparent. At high frequencies (ka >> 1), geometric shadow.
        // The transition is around ka ≈ 1 (f ≈ 600 Hz for human head).
        //
        // Near ear (ipsilateral): slight boost at ka ≈ 1 (bright spot)
        // Far ear (contralateral): increasing attenuation above ka ≈ 1

        // Compute per-ear magnitude using a sigmoid approximation of
        // sphere diffraction
        auto earMagnitude = [](float ka, float cosAngle) -> float {
            // cosAngle: +1 = source directly at this ear, -1 = opposite side
            if (cosAngle > 0) {
                // Near ear: slight boost around ka ≈ 1-3 (bright spot)
                float boost = 1.0f + 0.3f * cosAngle * std::exp(-std::abs(ka - 2.0f));
                return boost;
            } else {
                // Far ear: frequency-dependent shadow
                float shadow = 1.0f / (1.0f + ka * ka * cosAngle * cosAngle * 0.5f);
                return std::max(0.05f, shadow);
            }
        };

        float magL = earMagnitude(ka, cosIncidenceL);
        float magR = earMagnitude(ka, cosIncidenceR);

        // === ITD as phase shift ===
        // Woodworth formula for time difference
        float maxDelay = (float)(HEAD_RADIUS / SPEED_OF_SOUND);
        float itd = maxDelay * (std::sin(azRad) + azRad) * std::cos(elRad);
        // Apply as phase: positive ITD = left ear delayed
        float phaseShiftL = -(float)M_PI * freq * std::max(0.0f, itd);
        float phaseShiftR = -(float)M_PI * freq * std::max(0.0f, -itd);

        // === Pinna spectral notch (elevation cue) ===
        // The pinna creates a notch whose frequency depends on elevation.
        // Modeled as a spectral notch centered around 6-10 kHz that shifts
        // with elevation.
        float pinnaNotchFreq = 7000.0f + elevationDeg * 30.0f; // 5800 Hz (below) to 9700 Hz (above)
        float pinnaWidth = 1500.0f;
        float pinnaDepth = 0.4f;
        float pinnaNotch = 1.0f - pinnaDepth * std::exp(
            -(freq - pinnaNotchFreq) * (freq - pinnaNotchFreq) / (2.0f * pinnaWidth * pinnaWidth));

        magL *= pinnaNotch;
        magR *= pinnaNotch;

        // Build complex spectrum
        specL[k] = std::complex<float>(magL * std::cos(phaseShiftL),
                                        magL * std::sin(phaseShiftL));
        specR[k] = std::complex<float>(magR * std::cos(phaseShiftR),
                                        magR * std::sin(phaseShiftR));
    }

    // DC and Nyquist must be real
    specL[0] = std::complex<float>(std::abs(specL[0]), 0);
    specR[0] = std::complex<float>(std::abs(specR[0]), 0);
    specL[halfN-1] = std::complex<float>(std::abs(specL[halfN-1]), 0);
    specR[halfN-1] = std::complex<float>(std::abs(specR[halfN-1]), 0);

    // IFFT to get impulse response
    FFT fft(N);
    std::vector<float> irL, irR;
    fft.inverseReal(specL, irL);
    fft.inverseReal(specR, irR);

    // Copy first HRTF_IR_LENGTH samples, apply a half-Hann fade-out window
    for (int i = 0; i < HRTF_IR_LENGTH; ++i) {
        float window = (i < HRTF_IR_LENGTH / 2) ? 1.0f :
            0.5f * (1.0f + std::cos((float)M_PI * (i - HRTF_IR_LENGTH / 2) / (HRTF_IR_LENGTH / 2)));
        entry.left[i]  = (i < (int)irL.size()) ? irL[i] * window : 0.0f;
        entry.right[i] = (i < (int)irR.size()) ? irR[i] * window : 0.0f;
    }

    // Normalize so the IR has unity gain at DC
    float sumL = 0, sumR = 0;
    for (int i = 0; i < HRTF_IR_LENGTH; ++i) { sumL += entry.left[i]; sumR += entry.right[i]; }
    float normL = (std::abs(sumL) > 1e-6f) ? 1.0f / sumL : 1.0f;
    float normR = (std::abs(sumR) > 1e-6f) ? 1.0f / sumR : 1.0f;
    // Don't normalize too aggressively — keep relative level differences
    float normMax = std::max(std::abs(normL), std::abs(normR));
    normL /= normMax; normR /= normMax;
    for (int i = 0; i < HRTF_IR_LENGTH; ++i) {
        entry.left[i] *= normL;
        entry.right[i] *= normR;
    }
}

void HRTFTable::lookup(float azimuthDeg, float elevationDeg,
                        float* irLeft, float* irRight) const {
    // Handle negative azimuth: symmetric (swap L/R)
    bool swapLR = false;
    if (azimuthDeg < 0) {
        azimuthDeg = -azimuthDeg;
        swapLR = true;
    }
    azimuthDeg = std::min(180.0f, std::max(0.0f, azimuthDeg));
    elevationDeg = std::min(90.0f, std::max(-40.0f, elevationDeg));

    // Find grid indices
    float azFrac = azimuthDeg / HRTF_AZ_STEP;
    float elFrac = (elevationDeg - HRTF_EL_MIN) / HRTF_EL_STEP;
    int azIdx0 = std::min((int)azFrac, HRTF_NUM_AZIMUTHS - 2);
    int elIdx0 = std::min((int)elFrac, HRTF_NUM_ELEVATIONS - 2);
    float azF = azFrac - azIdx0;
    float elF = elFrac - elIdx0;

    // Bilinear interpolation of 4 nearest HRTF entries
    interpolate(entries[azIdx0][elIdx0],     entries[azIdx0+1][elIdx0],
                entries[azIdx0][elIdx0+1],   entries[azIdx0+1][elIdx0+1],
                azF, elF, irLeft, irRight);

    // Swap channels for negative azimuth
    if (swapLR) {
        for (int i = 0; i < HRTF_IR_LENGTH; ++i)
            std::swap(irLeft[i], irRight[i]);
    }
}

void HRTFTable::interpolate(const HRTFEntry& a, const HRTFEntry& b,
                              const HRTFEntry& c, const HRTFEntry& d,
                              float fracAz, float fracEl,
                              float* irLeft, float* irRight) const {
    for (int i = 0; i < HRTF_IR_LENGTH; ++i) {
        float topL = a.left[i] + fracAz * (b.left[i] - a.left[i]);
        float botL = c.left[i] + fracAz * (d.left[i] - c.left[i]);
        irLeft[i] = topL + fracEl * (botL - topL);

        float topR = a.right[i] + fracAz * (b.right[i] - a.right[i]);
        float botR = c.right[i] + fracAz * (d.right[i] - c.right[i]);
        irRight[i] = topR + fracEl * (botR - topR);
    }
}

bool HRTFTable::loadFromDirectory(const std::string& path) {
    // TODO: Load WAV files from MIT KEMAR format directory.
    // Each file: H{elev}e{azim}a.wav, stereo, L=left ear, R=right ear
    // For now, the generated spherical head model is used.
    // When real data is available, it overwrites the entries[] table
    // and sets externalLoaded = true.
    (void)path;
    return false;
}

} // namespace SoundShop
