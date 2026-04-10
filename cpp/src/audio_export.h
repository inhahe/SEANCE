#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <string>
#include <vector>

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
    float quality = 0.8f;      // Vorbis: 0.0-1.0, higher = better
    int bitrate = 192;          // Opus/AAC: kbps
};

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
