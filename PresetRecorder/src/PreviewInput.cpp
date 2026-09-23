#include "PreviewInput.h"

namespace PresetRecorder {

//==============================================================================
static void addNote(juce::MidiMessageSequence& seq, int channel, int note,
                    double onSeconds, double offSeconds, juce::uint8 velocity)
{
    seq.addEvent(juce::MidiMessage::noteOn(channel, note, velocity), onSeconds);
    seq.addEvent(juce::MidiMessage::noteOff(channel, note), offSeconds);
}

PreviewPhrase makeBuiltInPhrase(PhraseKind kind, double bpm)
{
    PreviewPhrase p;
    const double beat = 60.0 / juce::jlimit(20.0, 400.0, bpm);

    if (kind == PhraseKind::melodic)
    {
        // Held C3: attack, body and sustain of the sound.
        addNote(p.events, 1, 48, 0.0, 1.5 * beat, 100);

        // C4 E4 G4 C5 in eighths: articulation, glide, mono vs poly.
        const int arpeggio[] = { 60, 64, 67, 72 };
        for (int i = 0; i < 4; ++i)
        {
            const double on = 1.5 + 0.5 * i;
            addNote(p.events, 1, arpeggio[i], on * beat, (on + 0.45) * beat, 92);
        }

        // C major chord (C3 G3 C4 E4), held: polyphony, then the release.
        for (int note : { 48, 55, 60, 64 })
            addNote(p.events, 1, note, 3.5 * beat, 7.0 * beat, 96);

        p.lengthSeconds = 7.0 * beat;
        p.description = "built-in phrase (held C3, C4-E4-G4-C5 arpeggio, C major chord)";
    }
    else
    {
        // Two bars of a General MIDI groove on channel 10, a short fill, and a
        // crash on the downbeat of bar 3 to show the cymbal decay.
        constexpr int kick = 36, snare = 38, closedHat = 42, openHat = 46,
                      crash = 49, highTom = 50, lowTom = 45;

        auto hit = [&](int note, double beatPos, juce::uint8 velocity)
        {
            addNote(p.events, 10, note, beatPos * beat, (beatPos + 0.25) * beat, velocity);
        };

        for (int bar = 0; bar < 2; ++bar)
        {
            const double b0 = bar * 4.0;

            for (int eighth = 0; eighth < 8; ++eighth)
            {
                const double pos = b0 + eighth * 0.5;

                if (bar == 0 && eighth == 7)
                    hit(openHat, pos, 90);
                else if (! (bar == 1 && eighth >= 6)) // leave room for the fill
                    hit(closedHat, pos, eighth % 2 == 0 ? 92 : 70);
            }

            hit(kick, b0 + 0.0, 112);
            hit(snare, b0 + 1.0, 104);
            hit(kick, b0 + 2.0, 108);
            hit(snare, b0 + 3.0, 104);
            hit(kick, b0 + (bar == 0 ? 1.5 : 2.5), 96);
        }

        hit(snare, 7.25, 88);
        hit(highTom, 7.5, 100);
        hit(lowTom, 7.75, 104);
        hit(crash, 8.0, 112);
        hit(kick, 8.0, 112);

        p.lengthSeconds = 8.25 * beat;
        p.description = "built-in General MIDI drum groove (2 bars and a crash)";
    }

    p.events.updateMatchedPairs();
    return p;
}

bool looksLikeDrumInstrument(const juce::String& category, const juce::String& name)
{
    return category.containsIgnoreCase("drum") || category.containsIgnoreCase("percussion")
        || name.containsIgnoreCase("drum") || name.containsIgnoreCase("808")
        || name.containsIgnoreCase("909") || name.containsIgnoreCase("beatbox");
}

bool loadMidiFilePhrase(const juce::File& file, PreviewPhrase& phrase, juce::String& error)
{
    constexpr double kMaxSeconds = 60.0;

    juce::FileInputStream in(file);
    if (! in.openedOk())
    {
        error = "can't open the MIDI file " + file.getFullPathName();
        return false;
    }

    juce::MidiFile midi;
    if (! midi.readFrom(in))
    {
        error = file.getFileName() + " is not a valid Standard MIDI File";
        return false;
    }

    midi.convertTimestampTicksToSeconds();

    juce::MidiMessageSequence merged;
    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        const auto* track = midi.getTrack(t);
        for (int i = 0; i < track->getNumEvents(); ++i)
        {
            const auto& m = track->getEventPointer(i)->message;
            if (m.isMetaEvent() || m.isSysEx() || m.isProgramChange() || m.getTimeStamp() > kMaxSeconds)
                continue;
            merged.addEvent(m);
        }
    }

    merged.updateMatchedPairs();

    // Notes cut off by the 60 s cap still need their note-off.
    const double end = juce::jmin(kMaxSeconds, merged.getEndTime());
    for (int i = merged.getNumEvents(); --i >= 0;)
        if (merged.getEventPointer(i)->message.isNoteOn() && merged.getIndexOfMatchingKeyUp(i) < 0)
        {
            const auto& on = merged.getEventPointer(i)->message;
            merged.addEvent(juce::MidiMessage::noteOff(on.getChannel(), on.getNoteNumber()), end);
        }

    merged.updateMatchedPairs();

    bool hasNotes = false;
    for (int i = 0; i < merged.getNumEvents(); ++i)
        hasNotes = hasNotes || merged.getEventPointer(i)->message.isNoteOn();

    if (! hasNotes)
    {
        error = file.getFileName() + " contains no notes";
        return false;
    }

    phrase.events = merged;
    phrase.lengthSeconds = merged.getEndTime();
    phrase.description = "MIDI file " + file.getFileName();
    return true;
}

//==============================================================================
void registerReadableFormats(juce::AudioFormatManager& fm)
{
    fm.registerBasicFormats();
}

double getAudioFileLength(const juce::File& file)
{
    juce::AudioFormatManager fm;
    registerReadableFormats(fm);
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    return reader != nullptr && reader->sampleRate > 0 ? (double) reader->lengthInSamples / reader->sampleRate : 0.0;
}

bool readAudioFile(const juce::File& file, juce::AudioBuffer<float>& audio, double& sampleRate, juce::String& error)
{
    juce::AudioFormatManager fm;
    registerReadableFormats(fm);
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));

    if (reader == nullptr)
    {
        error = "can't read " + file.getFullPathName();
        return false;
    }

    const auto numChannels = (int) juce::jmax(1u, reader->numChannels);
    const auto numSamples = (int) juce::jmin<juce::int64>(reader->lengthInSamples, std::numeric_limits<int>::max() / 2);
    audio.setSize(numChannels, numSamples);
    audio.clear();
    reader->read(&audio, 0, numSamples, 0, true, true);
    sampleRate = reader->sampleRate;
    return true;
}

bool prepareSongExcerpt(const juce::File& song, double startSeconds, double lengthSeconds,
                        double sampleRate, juce::AudioBuffer<float>& excerpt, juce::String& error)
{
    juce::AudioFormatManager fm;
    registerReadableFormats(fm);
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(song));

    if (reader == nullptr)
    {
        error = song.existsAsFile() ? "can't decode " + song.getFileName() + " (unsupported format?)"
                                    : "the effect input song " + song.getFullPathName() + " doesn't exist";
        return false;
    }

    const double srcRate = reader->sampleRate;
    const double total = (double) reader->lengthInSamples / srcRate;

    if (lengthSeconds <= 0.05)
    {
        error = "the excerpt must be longer than 0.05 s";
        return false;
    }

    startSeconds = juce::jmax(0.0, startSeconds);
    if (startSeconds >= total - 0.05)
    {
        error = "the excerpt starts at " + juce::String(startSeconds, 2) + " s, but the song is only "
              + juce::String(total, 2) + " s long";
        return false;
    }

    lengthSeconds = juce::jmin(lengthSeconds, total - startSeconds);

    const auto startSample = (juce::int64) std::llround(startSeconds * srcRate);
    const auto numSrc = (int) std::llround(lengthSeconds * srcRate);

    juce::AudioBuffer<float> src(2, numSrc);
    src.clear();
    reader->read(&src, 0, numSrc, startSample, true, true);
    if (reader->numChannels == 1)
        src.copyFrom(1, 0, src, 0, 0, numSrc);

    if (std::abs(srcRate - sampleRate) > 0.5)
    {
        const double ratio = srcRate / sampleRate;
        const int numOut = juce::jmax(1, (int) std::floor((numSrc - 1) / ratio));
        excerpt.setSize(2, numOut);

        for (int ch = 0; ch < 2; ++ch)
        {
            juce::WindowedSincInterpolator interpolator;
            interpolator.process(ratio, src.getReadPointer(ch), excerpt.getWritePointer(ch), numOut);
        }
    }
    else
    {
        excerpt.makeCopyOf(src);
    }

    const auto peak = excerpt.getMagnitude(0, excerpt.getNumSamples());
    if (peak > 1.0e-6f)
        excerpt.applyGain(juce::Decibels::decibelsToGain(-1.0f) / peak);

    const int n = excerpt.getNumSamples();
    const int fadeIn = juce::jmin(n / 4, (int) (0.005 * sampleRate));
    const int fadeOut = juce::jmin(n / 4, (int) (0.030 * sampleRate));
    excerpt.applyGainRamp(0, fadeIn, 0.0f, 1.0f);
    excerpt.applyGainRamp(n - fadeOut, fadeOut, 1.0f, 0.0f);
    return true;
}

bool writeAudioFile(const juce::File& file, const juce::AudioBuffer<float>& audio, int numChannels,
                    double sampleRate, int bitDepth, juce::String& error)
{
    std::unique_ptr<juce::AudioFormat> format;
    if (file.hasFileExtension("flac"))     format = std::make_unique<juce::FlacAudioFormat>();
    else if (file.hasFileExtension("wav")) format = std::make_unique<juce::WavAudioFormat>();
    else
    {
        error = "unsupported output format: " + file.getFileName();
        return false;
    }

    numChannels = juce::jlimit(1, audio.getNumChannels(), numChannels);

    if (! file.getParentDirectory().createDirectory())
    {
        error = "can't create the folder " + file.getParentDirectory().getFullPathName();
        return false;
    }

    // "<name>.flac.partial" until complete. The GUI deletes stale *.partial
    // files a crashed worker leaves behind.
    const auto partial = file.getSiblingFile(file.getFileName() + ".partial");
    partial.deleteFile();

    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(partial);
        if (! static_cast<juce::FileOutputStream*>(stream.get())->openedOk())
        {
            error = "can't write " + partial.getFullPathName();
            return false;
        }

        auto options = juce::AudioFormatWriterOptions{}
                           .withSampleRate(sampleRate)
                           .withNumChannels(numChannels)
                           .withBitsPerSample(bitDepth);

        if (bitDepth == 32)
            options = options.withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        if (file.hasFileExtension("flac"))
            options = options.withQualityOptionIndex(5);

        auto writer = format->createWriterFor(stream, options);
        if (writer == nullptr)
        {
            error = "can't create a " + format->getFormatName() + " writer ("
                  + juce::String(sampleRate) + " Hz, " + juce::String(bitDepth) + "-bit)";
            partial.deleteFile();
            return false;
        }

        juce::AudioBuffer<float> view(const_cast<float* const*>(audio.getArrayOfReadPointers()),
                                      numChannels, audio.getNumSamples());
        if (! writer->writeFromAudioSampleBuffer(view, 0, view.getNumSamples()))
        {
            error = "writing " + file.getFileName() + " failed (disk full?)";
            writer.reset();
            partial.deleteFile();
            return false;
        }
    } // the writer finalises the file here

    if (file.exists() && ! file.deleteFile())
    {
        error = "can't replace " + file.getFullPathName() + " (is it open in another program?)";
        partial.deleteFile();
        return false;
    }

    if (! partial.moveFileTo(file))
    {
        error = "can't rename " + partial.getFileName() + " to " + file.getFileName();
        return false;
    }

    return true;
}

} // namespace PresetRecorder
