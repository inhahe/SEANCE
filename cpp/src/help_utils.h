#pragma once
#include <juce_core/juce_core.h>

namespace SoundShop {

// Open a documentation HTML file in the OS's default browser. The file
// is expected to live in the docs/ folder next to the exe (copied there
// by CMake POST_BUILD). Silently fails if the file doesn't exist — the
// main Help menu has the same helper with a visible error dialog, so
// users discover the problem via that path.
inline void openHelpDocFile(juce::String docRelativePath) {
    auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                      .getParentDirectory();
    auto docFile = exeDir.getChildFile("docs").getChildFile(docRelativePath);
    if (docFile.existsAsFile())
        docFile.startAsProcess();
}

} // namespace SoundShop
