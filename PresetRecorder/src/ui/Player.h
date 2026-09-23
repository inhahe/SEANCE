#pragma once
#include <juce_audio_utils/juce_audio_utils.h>

namespace PresetRecorder {

// Plays recordings for auditioning: the default output device, a transport
// that resamples to the device rate, and a waveform thumbnail of whatever is
// loaded. Broadcasts a change when playback starts or stops.
class Player : public juce::ChangeBroadcaster,
               private juce::ChangeListener
{
public:
    Player();
    ~Player() override;

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }
    juce::String getDeviceError() const { return deviceError; }

    // Loads and starts playing `file` from the beginning.
    bool play(const juce::File& file, juce::String& error);
    void stop();
    void togglePlayPause();
    bool isPlaying() const { return transport.isPlaying(); }

    void setLooping(bool shouldLoop);
    bool isLooping() const { return looping; }
    void setGain(float gain) { transport.setGain(gain); }

    double getPosition() const { return transport.getCurrentPosition(); }
    double getLength() const { return transport.getLengthInSeconds(); }
    void setPosition(double seconds) { transport.setPosition(seconds); }

    const juce::File& getCurrentFile() const { return currentFile; }
    juce::AudioThumbnail& getThumbnail() { return thumbnail; }

    // The device-state XML, to persist across runs.
    std::unique_ptr<juce::XmlElement> getDeviceState() const { return deviceManager.createStateXml(); }
    void restoreDeviceState(const juce::XmlElement* state);

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formats;
    juce::AudioSourcePlayer sourcePlayer;
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::TimeSliceThread readAheadThread { "preview read-ahead" };
    juce::AudioThumbnailCache thumbnailCache { 32 };
    juce::AudioThumbnail thumbnail { 256, formats, thumbnailCache };
    juce::File currentFile;
    juce::String deviceError;
    bool looping = false;
};

// Waveform of the loaded recording with a moving playhead. Click or drag to seek.
class WaveformView : public juce::Component,
                     public juce::SettableTooltipClient,
                     private juce::ChangeListener,
                     private juce::Timer
{
public:
    explicit WaveformView(Player&);
    ~WaveformView() override;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;

    juce::String emptyText = "Click a preset to hear it.";

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void seekTo(float x);

    Player& player;
};

} // namespace PresetRecorder
