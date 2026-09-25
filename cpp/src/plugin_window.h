#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>

namespace SoundShop {

// Window that hosts a plugin's native editor UI
class PluginWindow : public juce::DocumentWindow {
public:
    PluginWindow(juce::AudioProcessor& processor, const juce::String& name,
                 std::function<void()> closed = {})
        : DocumentWindow(name, juce::Colours::darkgrey, DocumentWindow::closeButton),
          onClosed(std::move(closed)) {

        if (auto* editor = processor.createEditorIfNeeded()) {
            setContentOwned(editor, true);
            setResizable(editor->isResizable(), false);
            setUsingNativeTitleBar(true);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
            toFront(true);
        }
    }

    void closeButtonPressed() override {
        setVisible(false);
        if (onClosed) onClosed();
    }

    // Called when the user closes the window (it's only hidden: the editor
    // stays alive for the next Show Plugin UI).
    std::function<void()> onClosed;

    // Check if this window is for a given processor
    bool isForProcessor(juce::AudioProcessor* proc) const {
        if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*>(getContentComponent()))
            return editor->getAudioProcessor() == proc;
        return false;
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginWindow)
};

// Manages open plugin windows
class PluginWindowManager {
public:
    // `closed`: see PluginWindow::onClosed.
    void showWindowFor(juce::AudioProcessor& processor, const juce::String& name,
                       std::function<void()> closed = {}) {
        // Check if already open
        for (auto& w : windows) {
            if (w->isForProcessor(&processor)) {
                if (closed) w->onClosed = std::move(closed);
                w->setVisible(true);
                w->toFront(true);
                return;
            }
        }
        // Create new
        windows.push_back(std::make_unique<PluginWindow>(processor, name, std::move(closed)));
    }

    void closeWindowFor(juce::AudioProcessor* processor) {
        windows.erase(
            std::remove_if(windows.begin(), windows.end(),
                [processor](auto& w) { return w->isForProcessor(processor); }),
            windows.end());
    }

    void closeAll() {
        windows.clear();
    }

private:
    std::vector<std::unique_ptr<PluginWindow>> windows;
};

} // namespace SoundShop
