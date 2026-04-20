#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace SoundShop {

// Window that hosts a plugin's native editor UI
class PluginWindow : public juce::DocumentWindow {
public:
    PluginWindow(juce::AudioProcessor& processor, const juce::String& name)
        : DocumentWindow(name, juce::Colours::darkgrey, DocumentWindow::closeButton) {

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
    }

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
    void showWindowFor(juce::AudioProcessor& processor, const juce::String& name) {
        // Check if already open
        for (auto& w : windows) {
            if (w->isForProcessor(&processor)) {
                w->setVisible(true);
                w->toFront(true);
                return;
            }
        }
        // Create new
        windows.push_back(std::make_unique<PluginWindow>(processor, name));
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
