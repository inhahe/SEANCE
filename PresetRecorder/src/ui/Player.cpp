#include "Player.h"
#include "Theme.h"
#include "../PreviewInput.h"

namespace PresetRecorder {

Player::Player()
{
    registerReadableFormats(formats);
    readAheadThread.startThread(juce::Thread::Priority::normal);

    deviceError = deviceManager.initialiseWithDefaultDevices(0, 2);
    sourcePlayer.setSource(&transport);
    deviceManager.addAudioCallback(&sourcePlayer);
    transport.addChangeListener(this);
}

Player::~Player()
{
    transport.removeChangeListener(this);
    transport.stop();
    deviceManager.removeAudioCallback(&sourcePlayer);
    sourcePlayer.setSource(nullptr);
    transport.setSource(nullptr);
    readerSource.reset();
    thumbnail.setSource(nullptr);
    readAheadThread.stopThread(2000);
}

void Player::restoreDeviceState(const juce::XmlElement* state)
{
    if (state != nullptr)
        deviceError = deviceManager.initialise(0, 2, state, true);
}

bool Player::play(const juce::File& file, juce::String& error)
{
    transport.stop();
    transport.setSource(nullptr);
    readerSource.reset();

    auto* reader = formats.createReaderFor(file);
    if (reader == nullptr)
    {
        error = file.existsAsFile() ? "can't read " + file.getFileName() : file.getFileName() + " is missing";
        currentFile = juce::File();
        thumbnail.setSource(nullptr);
        sendChangeMessage();
        return false;
    }

    const double rate = reader->sampleRate;
    readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    readerSource->setLooping(looping);
    transport.setSource(readerSource.get(), 32768, &readAheadThread, rate, 2);

    if (currentFile != file)
    {
        currentFile = file;
        thumbnail.setSource(new juce::FileInputSource(file));
    }

    transport.setPosition(0.0);
    transport.start();
    sendChangeMessage();
    return true;
}

void Player::stop()
{
    transport.stop();
    transport.setPosition(0.0);
    sendChangeMessage();
}

void Player::togglePlayPause()
{
    if (readerSource == nullptr)
        return;

    if (transport.isPlaying())
    {
        transport.stop();
    }
    else
    {
        if (transport.getCurrentPosition() >= transport.getLengthInSeconds() - 0.01)
            transport.setPosition(0.0);
        transport.start();
    }
    sendChangeMessage();
}

void Player::setLooping(bool shouldLoop)
{
    looping = shouldLoop;
    if (readerSource != nullptr)
        readerSource->setLooping(shouldLoop);
}

void Player::changeListenerCallback(juce::ChangeBroadcaster*)
{
    sendChangeMessage();
}

//==============================================================================
WaveformView::WaveformView(Player& p) : player(p)
{
    player.getThumbnail().addChangeListener(this);
    player.addChangeListener(this);
    startTimerHz(30);
    setTooltip("The recording that is playing. Click or drag here to jump to a point in it.");
}

WaveformView::~WaveformView()
{
    player.getThumbnail().removeChangeListener(this);
    player.removeChangeListener(this);
}

void WaveformView::paint(juce::Graphics& g)
{
    auto area = getLocalBounds();
    g.setColour(Theme::panel);
    g.fillRoundedRectangle(area.toFloat(), 4.0f);

    auto& thumb = player.getThumbnail();
    const double length = thumb.getTotalLength();

    if (player.getCurrentFile() == juce::File() || length <= 0.0)
    {
        g.setColour(Theme::dimText);
        g.setFont(Theme::font(13.0f));
        g.drawText(emptyText, area, juce::Justification::centred);
        return;
    }

    auto wave = area.reduced(4, 6);
    g.setColour(Theme::accent.withAlpha(0.85f));
    thumb.drawChannels(g, wave, 0.0, length, 1.0f);

    const double pos = player.getPosition();
    const float x = (float) wave.getX() + (float) (pos / length) * (float) wave.getWidth();
    g.setColour(juce::Colours::white);
    g.drawLine(x, (float) area.getY() + 2.0f, x, (float) area.getBottom() - 2.0f, 1.5f);

    g.setColour(Theme::dimText);
    g.setFont(Theme::font(11.0f));
    g.drawText(juce::String(pos, 1) + " / " + juce::String(length, 1) + " s",
               area.reduced(6, 2), juce::Justification::topRight);
}

void WaveformView::seekTo(float x)
{
    const double length = player.getLength();
    if (length <= 0.0)
        return;

    auto wave = getLocalBounds().reduced(4, 6);
    const double t = juce::jlimit(0.0, 1.0, (double) (x - (float) wave.getX()) / (double) wave.getWidth()) * length;
    player.setPosition(t);
    repaint();
}

void WaveformView::mouseDown(const juce::MouseEvent& e) { seekTo((float) e.x); }
void WaveformView::mouseDrag(const juce::MouseEvent& e) { seekTo((float) e.x); }

void WaveformView::changeListenerCallback(juce::ChangeBroadcaster*) { repaint(); }

void WaveformView::timerCallback()
{
    if (player.isPlaying())
        repaint();
}

} // namespace PresetRecorder
