#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace juce { class AudioDeviceManager; }

namespace SoundShop {

// The app-wide LookAndFeel. Identical to LookAndFeel_V4 in every visual
// respect - it exists for one non-visual override.
//
// Every juce::AlertWindow in the app (there are ~25: rename prompts, save-to-
// library name boxes, export options, ...) creates its desktop peer from
// LookAndFeel::getAlertBoxWindowFlags(), whose stock value includes
// ComponentPeer::windowAppearsOnTaskbar. On Windows that becomes
// WS_EX_APPWINDOW with no owner HWND, which the shell reads as a second
// application and gives its own taskbar button - the thing CLAUDE.md's dialog
// rule forbids. Dropping the flag here fixes all of them at once, with no
// call-site changes.
//
// Why here and not on an AlertWindow subclass? Overriding
// getDesktopWindowStyleFlags() on a subclass is the documented recipe for
// TopLevelWindow (and is what ToolDialogWindow does for DialogWindow), but it
// does NOT work for AlertWindow, and fails in the worst possible way: the
// dialog silently never appears. Measured - with such a subclass, clicking the
// Song Length button produced no window whatsoever and EnumWindows found
// nothing; neutralising the override to a plain call to the base class brought
// the dialog straight back.
//
// The verified cause is that AlertWindow's peer is created *during its own
// construction* - TopLevelWindow's ctor, then again from
// AlertWindow::lookAndFeelChanged() -> setDropShadowEnabled() -> addToDesktop()
// - both of which run before the subclass vtable is live, so the peer is built
// from the base flags and the override is never consulted. What exactly then
// goes wrong with the late mismatch is not worth pinning down, because the
// right answer is not to create it: getAlertBoxWindowFlags() is the value
// AlertWindow actually reads, at the point it reads it, so it is the extension
// point that works. (DialogWindow gets away with the subclass recipe only
// because it re-applies its flags from several post-construction paths;
// AlertWindow's single re-apply is inside its own constructor.)
//
// Two things keep this from being re-broken: testDialogTaskbarFlags() in
// self_test.cpp asserts the flag is actually absent from a live AlertWindow's
// peer, and cpp/cmake/check_dialog_patterns.cmake fails the build if anyone
// adds an AlertWindow subclass or a stray getDesktopWindowStyleFlags override.
class AppLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    int getAlertBoxWindowFlags() override {
        return juce::LookAndFeel_V4::getAlertBoxWindowFlags()
             & ~juce::ComponentPeer::windowAppearsOnTaskbar;
    }
};

// Make AppLookAndFeel the default look and feel. Call once, early in
// JUCEApplication::initialise(), before any window exists. Returns a reference
// to the singleton; the caller must call juce::LookAndFeel::setDefaultLookAndFeel(nullptr)
// on shutdown before it is destroyed.
juce::LookAndFeel& installAppLookAndFeel();

// Native, owner-parented replacements for juce::AlertWindow::showAsync /
// AlertWindow::show, for popups that are just a message and some buttons.
//
// Why not call juce::NativeMessageBox directly? **The result codes differ.**
// AlertWindow numbers its buttons the way LookAndFeel::createAlertWindow wires
// them - with N buttons, button X reports ((X + 1) % N), so the *first* button
// is 1 and the *last* one (also escape / the close box) is 0.
// NativeMessageBox::showAsync reports a plain 0-based index instead. Swapping
// one name for the other at a call site therefore compiles cleanly and silently
// inverts the meaning of every button. These wrappers do the remap, so a call
// site can switch by changing `juce::AlertWindow::` to `SoundShop::` and leave
// its `if (result == 1)` checks alone.
//
// The other half of the job: `owner` is passed through as the options'
// associated component, which on Windows becomes the TaskDialog's hwndParent -
// that's what stops the popup earning its own taskbar button. Pass a component
// inside the main window.
void showAlertAsync(juce::MessageBoxOptions opts,
                    juce::Component* owner,
                    std::function<void(int)> callback = {});

// Blocking variant, for the handful of places that genuinely can't be async
// (e.g. answering "may I quit?" from within a close request). Same result-code
// convention as juce::AlertWindow::show.
int showAlert(juce::MessageBoxOptions opts, juce::Component* owner);

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
