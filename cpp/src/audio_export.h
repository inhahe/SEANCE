#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <string>
#include <vector>
#include <random>

namespace SoundShop {

// Supported export formats
enum class ExportFormat {
    WAV,        // 24-bit PCM
    FLAC,       // 24-bit lossless
    OggVorbis,  // lossy, quality 0.0-1.0
    Opus,       // lossy, bitrate 64-320 kbps
    M4A_AAC,    // lossy, via Windows Media Foundation
    WMA,        // lossy, via Windows Media Foundation
};

struct ExportOptions {
    ExportFormat format = ExportFormat::WAV;
    int sampleRate = 48000;
    int bitsPerSample = 24;
    int numChannels = 2;        // 1 = mono, 2 = stereo
    float quality = 0.8f;      // Vorbis: 0.0-1.0, higher = better
    int bitrate = 192;          // Opus/AAC: kbps
    bool dither = true;         // Apply TPDF dither when reducing bit depth
};

// Apply triangular-PDF dithering to a float buffer in place. Dither
// noise is shaped to the target bit depth: each sample gets ±1 LSB of
// triangular noise added before the implicit truncation that happens
// when the audio format writer converts float→int. This converts
// correlated quantization distortion into uncorrelated hiss, which is
// inaudible at 16-bit and imperceptible at 24-bit.
//
// Call this once on the rendered buffer before handing it to
// AudioFormatWriter. Only meaningful when the target format is PCM
// (WAV, FLAC) — lossy codecs have their own noise floors.
inline void applyTPDFDither(juce::AudioBuffer<float>& buf, int targetBits) {
    if (targetBits >= 32 || targetBits <= 0) return;
    // 1 LSB at the target bit depth, in the float ±1 domain.
    const float lsb = 2.0f / (float)(1 << targetBits);
    std::mt19937 rng(42); // deterministic seed for reproducible exports
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    for (int c = 0; c < buf.getNumChannels(); ++c) {
        float* data = buf.getWritePointer(c);
        for (int s = 0; s < buf.getNumSamples(); ++s) {
            // TPDF = sum of two uniform random variables → triangular PDF.
            float noise = (dist(rng) + dist(rng)) * lsb;
            data[s] += noise;
        }
    }
}

class AudioExporter {
public:
    // Export a rendered buffer to file
    static bool exportToFile(const juce::File& file,
                              const juce::AudioBuffer<float>& buffer,
                              const ExportOptions& options);

    // Get file extension for a format
    static juce::String getExtension(ExportFormat fmt);

    // Get filter string for file chooser
    static juce::String getFileFilter();

    // Determine format from file extension
    static ExportFormat formatFromExtension(const juce::String& ext);

    // Export Opus using libopus (custom implementation)
    static bool exportOpus(const juce::File& file,
                            const juce::AudioBuffer<float>& buffer,
                            int sampleRate, int bitrate);

    // Export M4A/AAC using Windows Media Foundation
    static bool exportM4A(const juce::File& file,
                           const juce::AudioBuffer<float>& buffer,
                           int sampleRate, int bitrate);

    // Export WMA using Windows Media Foundation
    static bool exportWMA(const juce::File& file,
                           const juce::AudioBuffer<float>& buffer,
                           int sampleRate, int bitrate);
};

} // namespace SoundShop
