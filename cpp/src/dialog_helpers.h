#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace juce { class AudioDeviceManager; }

namespace SoundShop {

// Open the standard Audio Device Settings dialog (driver type / input /
// output / sample rate / buffer size + MIDI input selection) as a taskbar-
// less tool dialog centred on `centreAround`. Shared by the Settings menu
// item and the microphone-capture dialog's "Audio device..." button so
// there's a single place that builds the AudioDeviceSelectorComponent.
// centreAround should be a component inside the main window so the popup is
// owned by the main HWND (no second taskbar entry). Returns the dialog, or
// nullptr if there's no device manager.
juce::DialogWindow* launchAudioDeviceSettings(juce::AudioDeviceManager& dm,
                                              juce::Component* centreAround);

// Launch a JUCE DialogWindow with the given options, but WITHOUT a Windows
// taskbar entry. Use this for editor sub-windows (waveform editor, spectral
// editor, wavelet painter, plugin UI, etc.) - they're conceptually part of
// the main app, not separate programs.
//
// Why this exists: JUCE's DialogWindow::LaunchOptions::launchAsync() creates
// a top-level window with windowAppearsOnTaskbar in its style flags, which
// on Windows produces WS_EX_APPWINDOW and a separate taskbar slot. There's
// no option in LaunchOptions to suppress this - the only supported override
// is via getDesktopWindowStyleFlags() on a DialogWindow subclass (see the
// guidance in juce_TopLevelWindow.cpp). This helper wraps that subclassing
// so call sites don't each need a one-off class.
//
// componentToCentreAround on the options struct still works for centering.
//
// Returns the dialog window. JUCE deletes it automatically when the user
// closes it (deleteWhenDismissed=true on the modal state).
juce::DialogWindow* launchToolDialog(juce::DialogWindow::LaunchOptions& opts);

// Same as launchToolDialog, but does NOT enter modal state. The caller
// owns the dialog's lifetime explicitly (must `delete` it when done).
//
// Why this exists: launchToolDialog uses deleteWhenDismissed=true on the
// modal state, which means calling setVisible(false) on the returned
// dialog triggers JUCE to delete it. That's incompatible with hide/show
// patterns (e.g. the wavetable editor flow where the per-waveform editor
// dialog is hidden by default and shown only when the user enters edit
// mode). Use this variant when you need to flip visibility on/off without
// killing the window.
//
// Closing via the title-bar X just hides the dialog (closeButtonPressed
// in ToolDialogWindow calls setVisible(false), which without modal state
// is a plain hide). The caller is responsible for delete-on-close if
// that's the desired behavior.
juce::DialogWindow* launchManagedToolDialog(juce::DialogWindow::LaunchOptions& opts);

// Non-modal dialog with delete-on-close behavior. Like launchToolDialog (the
// title-bar X deletes the dialog), but WITHOUT entering modal state. Use
// this when the dialog hosts a JUCE DragAndDropContainer that needs to
// dispatch drops to targets inside itself: JUCE's DragImageComponent lives
// on the desktop, and the modal input filtering breaks its mouseUp
// listener forwarding, so itemDropped never fires when the parent dialog
// is modal.
juce::DialogWindow* launchNonModalToolDialog(juce::DialogWindow::LaunchOptions& opts);

} // namespace SoundShop
