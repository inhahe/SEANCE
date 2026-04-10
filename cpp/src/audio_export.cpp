#include "audio_export.h"
#include <cstdio>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#ifdef _MSC_VER
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#endif
#endif

#if __has_include("opus.h")
#define HAS_OPUS 1
#include "opus.h"
#include "ogg/ogg.h"
#else
#define HAS_OPUS 0
#endif

namespace SoundShop {

juce::String AudioExporter::getExtension(ExportFormat fmt) {
    switch (fmt) {
        case ExportFormat::WAV:       return ".wav";
        case ExportFormat::FLAC:      return ".flac";
        case ExportFormat::OggVorbis: return ".ogg";
        case ExportFormat::Opus:      return ".opus";
        case ExportFormat::M4A_AAC:   return ".m4a";
        case ExportFormat::WMA:       return ".wma";
    }
    return ".wav";
}

juce::String AudioExporter::getFileFilter() {
    return "*.wav;*.flac;*.ogg;*.opus;*.m4a;*.wma";
}

ExportFormat AudioExporter::formatFromExtension(const juce::String& ext) {
    auto e = ext.toLowerCase();
    if (e.endsWith(".flac")) return ExportFormat::FLAC;
    if (e.endsWith(".ogg"))  return ExportFormat::OggVorbis;
    if (e.endsWith(".opus")) return ExportFormat::Opus;
    if (e.endsWith(".m4a"))  return ExportFormat::M4A_AAC;
    if (e.endsWith(".wma"))  return ExportFormat::WMA;
    return ExportFormat::WAV;
}

bool AudioExporter::exportToFile(const juce::File& file,
                                  const juce::AudioBuffer<float>& buffer,
                                  const ExportOptions& options) {
    auto format = options.format;
    int sr = options.sampleRate;
    int numSamples = buffer.getNumSamples();
    int numChannels = std::min(buffer.getNumChannels(), 2);

    if (format == ExportFormat::WAV || format == ExportFormat::FLAC ||
        format == ExportFormat::OggVorbis) {
        // Use JUCE's built-in formats
        std::unique_ptr<juce::AudioFormat> audioFormat;
        if (format == ExportFormat::FLAC)
            audioFormat = std::make_unique<juce::FlacAudioFormat>();
        else if (format == ExportFormat::OggVorbis)
            audioFormat = std::make_unique<juce::OggVorbisAudioFormat>();
        else
            audioFormat = std::make_unique<juce::WavAudioFormat>();

        file.deleteFile();
        auto stream = file.createOutputStream();
        if (!stream) {
            fprintf(stderr, "Export: failed to create file %s\n", file.getFullPathName().toRawUTF8());
            return false;
        }

        int bits = (format == ExportFormat::OggVorbis) ? 0 : options.bitsPerSample; // Vorbis ignores bits

        // For Ogg Vorbis, quality is passed via metadata
        juce::StringPairArray metadata;
        if (format == ExportFormat::OggVorbis) {
            // Quality: 0-10 scale for vorbis, we map 0.0-1.0 to 0-10
            int q = juce::jlimit(0, 10, (int)(options.quality * 10));
            bits = q; // OggVorbisAudioFormat uses bitsPerSample as quality index
        }

        auto* writer = audioFormat->createWriterFor(stream.release(), sr, numChannels,
                                                      bits, metadata, 0);
        if (!writer) {
            fprintf(stderr, "Export: failed to create writer for %s\n", file.getFullPathName().toRawUTF8());
            return false;
        }

        std::unique_ptr<juce::AudioFormatWriter> writerPtr(writer);
        writerPtr->writeFromAudioSampleBuffer(buffer, 0, numSamples);

        fprintf(stderr, "Exported %s (%d samples, %d Hz)\n",
                file.getFullPathName().toRawUTF8(), numSamples, sr);
        return true;
    }

    if (format == ExportFormat::Opus)
        return exportOpus(file, buffer, sr, options.bitrate);

    if (format == ExportFormat::M4A_AAC)
        return exportM4A(file, buffer, sr, options.bitrate);

    if (format == ExportFormat::WMA)
        return exportWMA(file, buffer, sr, options.bitrate);

    return false;
}

// ==============================================================================
// Opus export using libopus + libogg
// ==============================================================================

bool AudioExporter::exportOpus(const juce::File& file,
                                const juce::AudioBuffer<float>& buffer,
                                int sampleRate, int bitrate) {
#if HAS_OPUS
    int numChannels = std::min(buffer.getNumChannels(), 2);
    int numSamples = buffer.getNumSamples();

    int error;
    OpusEncoder* encoder = opus_encoder_create(sampleRate, numChannels, OPUS_APPLICATION_AUDIO, &error);
    if (!encoder || error != OPUS_OK) {
        fprintf(stderr, "Opus: failed to create encoder: %s\n", opus_strerror(error));
        return false;
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate * 1000));

    // Open output file
    auto f = fopen(file.getFullPathName().toRawUTF8(), "wb");
    if (!f) { opus_encoder_destroy(encoder); return false; }

    // Initialize Ogg stream
    ogg_stream_state os;
    ogg_stream_init(&os, rand());

    // Write Opus header
    // OpusHead packet
    unsigned char header[19] = {};
    memcpy(header, "OpusHead", 8);
    header[8] = 1; // version
    header[9] = (unsigned char)numChannels;
    // pre-skip (little-endian)
    int preSkip = 3840; // standard
    header[10] = preSkip & 0xFF;
    header[11] = (preSkip >> 8) & 0xFF;
    // sample rate (little-endian)
    header[12] = sampleRate & 0xFF;
    header[13] = (sampleRate >> 8) & 0xFF;
    header[14] = (sampleRate >> 16) & 0xFF;
    header[15] = (sampleRate >> 24) & 0xFF;
    // output gain = 0
    header[16] = 0; header[17] = 0;
    header[18] = 0; // channel mapping family

    ogg_packet op;
    op.packet = header;
    op.bytes = 19;
    op.b_o_s = 1;
    op.e_o_s = 0;
    op.granulepos = 0;
    op.packetno = 0;
    ogg_stream_packetin(&os, &op);

    // OpusTags packet
    const char* vendor = "SoundShop2";
    int vendorLen = (int)strlen(vendor);
    std::vector<unsigned char> tags(8 + 4 + vendorLen + 4);
    memcpy(tags.data(), "OpusTags", 8);
    tags[8] = vendorLen & 0xFF; tags[9] = (vendorLen >> 8) & 0xFF;
    tags[10] = (vendorLen >> 16) & 0xFF; tags[11] = (vendorLen >> 24) & 0xFF;
    memcpy(tags.data() + 12, vendor, vendorLen);
    int commentCount = 0;
    tags[12 + vendorLen] = 0; tags[13 + vendorLen] = 0;
    tags[14 + vendorLen] = 0; tags[15 + vendorLen] = 0;

    op.packet = tags.data();
    op.bytes = (long)tags.size();
    op.b_o_s = 0;
    op.e_o_s = 0;
    op.granulepos = 0;
    op.packetno = 1;
    ogg_stream_packetin(&os, &op);

    // Flush header pages
    ogg_page og;
    while (ogg_stream_flush(&os, &og)) {
        fwrite(og.header, 1, og.header_len, f);
        fwrite(og.body, 1, og.body_len, f);
    }

    // Encode audio in 20ms frames
    int frameSize = sampleRate / 50; // 20ms
    std::vector<float> interleaved(frameSize * numChannels);
    std::vector<unsigned char> encodedBuf(4000);
    long long granulePos = 0;
    long packetNo = 2;

    for (int pos = 0; pos < numSamples; pos += frameSize) {
        int thisFrame = std::min(frameSize, numSamples - pos);

        // Interleave
        for (int s = 0; s < thisFrame; ++s) {
            for (int c = 0; c < numChannels; ++c)
                interleaved[s * numChannels + c] = buffer.getSample(c, pos + s);
        }
        // Pad if short
        for (int s = thisFrame * numChannels; s < frameSize * numChannels; ++s)
            interleaved[s] = 0;

        int encoded = opus_encode_float(encoder, interleaved.data(), frameSize,
                                         encodedBuf.data(), (int)encodedBuf.size());
        if (encoded < 0) continue;

        granulePos += frameSize;
        bool isLast = (pos + frameSize >= numSamples);

        op.packet = encodedBuf.data();
        op.bytes = encoded;
        op.b_o_s = 0;
        op.e_o_s = isLast ? 1 : 0;
        op.granulepos = granulePos;
        op.packetno = packetNo++;
        ogg_stream_packetin(&os, &op);

        while (ogg_stream_pageout(&os, &og)) {
            fwrite(og.header, 1, og.header_len, f);
            fwrite(og.body, 1, og.body_len, f);
        }
    }

    // Flush remaining
    while (ogg_stream_flush(&os, &og)) {
        fwrite(og.header, 1, og.header_len, f);
        fwrite(og.body, 1, og.body_len, f);
    }

    ogg_stream_clear(&os);
    opus_encoder_destroy(encoder);
    fclose(f);

    fprintf(stderr, "Exported Opus: %s (%d kbps)\n", file.getFullPathName().toRawUTF8(), bitrate);
    return true;
#else
    fprintf(stderr, "Opus export not available (libopus not compiled)\n");
    return false;
#endif
}

// ==============================================================================
// M4A/AAC export using Windows Media Foundation
// ==============================================================================

bool AudioExporter::exportM4A(const juce::File& file,
                               const juce::AudioBuffer<float>& buffer,
                               int sampleRate, int bitrate) {
#ifdef _WIN32
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        fprintf(stderr, "M4A: MFStartup failed\n");
        return false;
    }

    int numChannels = std::min(buffer.getNumChannels(), 2);
    int numSamples = buffer.getNumSamples();

    // Create sink writer
    IMFSinkWriter* writer = nullptr;
    auto wpath = file.getFullPathName().toWideCharPointer();
    hr = MFCreateSinkWriterFromURL(wpath, nullptr, nullptr, &writer);
    if (FAILED(hr) || !writer) {
        fprintf(stderr, "M4A: failed to create sink writer\n");
        MFShutdown();
        return false;
    }

    // Set output type (AAC)
    IMFMediaType* outType = nullptr;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, numChannels);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate * 1000 / 8);

    DWORD streamIdx;
    hr = writer->AddStream(outType, &streamIdx);
    outType->Release();
    if (FAILED(hr)) {
        fprintf(stderr, "M4A: AddStream failed\n");
        writer->Release();
        MFShutdown();
        return false;
    }

    // Set input type (PCM float)
    IMFMediaType* inType = nullptr;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, numChannels);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, numChannels * 4);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * numChannels * 4);

    hr = writer->SetInputMediaType(streamIdx, inType, nullptr);
    inType->Release();
    if (FAILED(hr)) {
        fprintf(stderr, "M4A: SetInputMediaType failed\n");
        writer->Release();
        MFShutdown();
        return false;
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        fprintf(stderr, "M4A: BeginWriting failed\n");
        writer->Release();
        MFShutdown();
        return false;
    }

    // Write audio in chunks
    int chunkSize = 4096;
    std::vector<float> interleaved(chunkSize * numChannels);
    LONGLONG timestamp = 0;

    for (int pos = 0; pos < numSamples; pos += chunkSize) {
        int thisChunk = std::min(chunkSize, numSamples - pos);

        // Interleave
        for (int s = 0; s < thisChunk; ++s)
            for (int c = 0; c < numChannels; ++c)
                interleaved[s * numChannels + c] = buffer.getSample(c, pos + s);

        IMFSample* sample = nullptr;
        IMFMediaBuffer* mediaBuf = nullptr;
        DWORD bufSize = thisChunk * numChannels * sizeof(float);

        MFCreateMemoryBuffer(bufSize, &mediaBuf);
        BYTE* data = nullptr;
        mediaBuf->Lock(&data, nullptr, nullptr);
        memcpy(data, interleaved.data(), bufSize);
        mediaBuf->Unlock();
        mediaBuf->SetCurrentLength(bufSize);

        MFCreateSample(&sample);
        sample->AddBuffer(mediaBuf);
        sample->SetSampleTime(timestamp);
        LONGLONG duration = (LONGLONG)thisChunk * 10000000LL / sampleRate;
        sample->SetSampleDuration(duration);
        timestamp += duration;

        writer->WriteSample(streamIdx, sample);
        sample->Release();
        mediaBuf->Release();
    }

    writer->Finalize();
    writer->Release();
    MFShutdown();

    fprintf(stderr, "Exported M4A: %s (%d kbps)\n", file.getFullPathName().toRawUTF8(), bitrate);
    return true;
#else
    fprintf(stderr, "M4A export only available on Windows\n");
    return false;
#endif
}

// ==============================================================================
// WMA export using Windows Media Foundation
// ==============================================================================

bool AudioExporter::exportWMA(const juce::File& file,
                               const juce::AudioBuffer<float>& buffer,
                               int sampleRate, int bitrate) {
#ifdef _WIN32
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        fprintf(stderr, "WMA: MFStartup failed\n");
        return false;
    }

    int numChannels = std::min(buffer.getNumChannels(), 2);
    int numSamples = buffer.getNumSamples();

    IMFSinkWriter* writer = nullptr;
    auto wpath = file.getFullPathName().toWideCharPointer();
    hr = MFCreateSinkWriterFromURL(wpath, nullptr, nullptr, &writer);
    if (FAILED(hr) || !writer) {
        fprintf(stderr, "WMA: failed to create sink writer\n");
        MFShutdown();
        return false;
    }

    // Set output type (WMA)
    IMFMediaType* outType = nullptr;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_WMAudioV9);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, numChannels);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate * 1000 / 8);

    DWORD streamIdx;
    hr = writer->AddStream(outType, &streamIdx);
    outType->Release();
    if (FAILED(hr)) {
        fprintf(stderr, "WMA: AddStream failed\n");
        writer->Release();
        MFShutdown();
        return false;
    }

    // Set input type (PCM float)
    IMFMediaType* inType = nullptr;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, numChannels);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, numChannels * 4);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * numChannels * 4);

    hr = writer->SetInputMediaType(streamIdx, inType, nullptr);
    inType->Release();
    if (FAILED(hr)) {
        fprintf(stderr, "WMA: SetInputMediaType failed\n");
        writer->Release();
        MFShutdown();
        return false;
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        fprintf(stderr, "WMA: BeginWriting failed\n");
        writer->Release();
        MFShutdown();
        return false;
    }

    int chunkSize = 4096;
    std::vector<float> interleaved(chunkSize * numChannels);
    LONGLONG timestamp = 0;

    for (int pos = 0; pos < numSamples; pos += chunkSize) {
        int thisChunk = std::min(chunkSize, numSamples - pos);

        for (int s = 0; s < thisChunk; ++s)
            for (int c = 0; c < numChannels; ++c)
                interleaved[s * numChannels + c] = buffer.getSample(c, pos + s);

        IMFSample* sample = nullptr;
        IMFMediaBuffer* mediaBuf = nullptr;
        DWORD bufSize = thisChunk * numChannels * sizeof(float);

        MFCreateMemoryBuffer(bufSize, &mediaBuf);
        BYTE* data = nullptr;
        mediaBuf->Lock(&data, nullptr, nullptr);
        memcpy(data, interleaved.data(), bufSize);
        mediaBuf->Unlock();
        mediaBuf->SetCurrentLength(bufSize);

        MFCreateSample(&sample);
        sample->AddBuffer(mediaBuf);
        sample->SetSampleTime(timestamp);
        LONGLONG duration = (LONGLONG)thisChunk * 10000000LL / sampleRate;
        sample->SetSampleDuration(duration);
        timestamp += duration;

        writer->WriteSample(streamIdx, sample);
        sample->Release();
        mediaBuf->Release();
    }

    writer->Finalize();
    writer->Release();
    MFShutdown();

    fprintf(stderr, "Exported WMA: %s (%d kbps)\n", file.getFullPathName().toRawUTF8(), bitrate);
    return true;
#else
    fprintf(stderr, "WMA export only available on Windows\n");
    return false;
#endif
}

} // namespace SoundShop
