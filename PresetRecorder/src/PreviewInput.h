#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace PresetRecorder {

//==============================================================================
// What instruments play.
//
// The built-in phrase is written to show a preset off in a few seconds whatever
// kind of sound it is: a held note (attack and sustain), a quick rising
// arpeggio (articulation, glide, mono/poly behaviour), then a held four-note
// chord (polyphony and release), after which the renderer lets the tail ring.
// Plugins whose category says "Drum" get a two-bar General MIDI groove instead,
// because drum machines typically map only a few dozen notes around C1-C3.

struct PreviewPhrase
{
    juce::MidiMessageSequence events; // timestamps in seconds
    double lengthSeconds = 0.0;       // when the last note ends; the tail comes after
    juce::String description;
};

enum class PhraseKind { melodic, drums };

PreviewPhrase makeBuiltInPhrase(PhraseKind kind, double bpm);

// True for "Instrument|Drum", "Drum Machine", "Drums"...
bool looksLikeDrumInstrument(const juce::String& category, const juce::String& name);

// All tracks of a Standard MIDI File merged into one phrase. Program changes are
// dropped (they would switch the plugin away from the preset being recorded) and
// the phrase is capped at 60 seconds.
bool loadMidiFilePhrase(const juce::File& file, PreviewPhrase& phrase, juce::String& error);

//==============================================================================
// What effects process: an excerpt of the input song.

// Decodes `song`, cuts [startSeconds, startSeconds + lengthSeconds), converts it
// to stereo at `sampleRate`, peak-normalises it to -1 dBFS and fades the edges
// (5 ms in, 30 ms out) so the cut itself never clicks.
bool prepareSongExcerpt(const juce::File& song, double startSeconds, double lengthSeconds,
                        double sampleRate, juce::AudioBuffer<float>& excerpt, juce::String& error);

// Length of an audio file in seconds, or 0.
double getAudioFileLength(const juce::File& file);

// FLAC (16/24-bit) or WAV (bits 16/24, or 32 = float) by extension. Writes to a
// temporary file first and renames it into place, so a partially written file
// never carries the final name.
bool writeAudioFile(const juce::File& file, const juce::AudioBuffer<float>& audio, int numChannels,
                    double sampleRate, int bitDepth, juce::String& error);

bool readAudioFile(const juce::File& file, juce::AudioBuffer<float>& audio, double& sampleRate,
                   juce::String& error);

// The formats the tool reads: WAV, AIFF, FLAC, Ogg Vorbis, MP3 (+ Windows Media on Windows).
void registerReadableFormats(juce::AudioFormatManager&);

} // namespace PresetRecorder
