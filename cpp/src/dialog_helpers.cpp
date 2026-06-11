#include "dialog_helpers.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>  // AudioDeviceSelectorComponent

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

namespace SoundShop {

// Mirror of JUCE's private DefaultDialogWindow, with one change: drop the
// windowAppearsOnTaskbar flag from getDesktopWindowStyleFlags so Windows
// doesn't give us a separate taskbar entry. The rest of the constructor
// body matches juce_DialogWindow.cpp's DefaultDialogWindow verbatim so
// behavior (content ownership, centering, native title bar, resizable,
// escape key) stays identical to LaunchOptions::launchAsync.
class ToolDialogWindow final : public juce::DialogWindow {
public:
    explicit ToolDialogWindow(juce::DialogWindow::LaunchOptions& options)
        : juce::DialogWindow(options.dialogTitle, options.dialogBackgroundColour,
                             options.escapeKeyTriggersCloseButton, true,
                             options.componentToCentreAround != nullptr
                                 ? juce::Component::getApproximateScaleFactorForComponent(
                                       options.componentToCentreAround)
                                 : 1.0f)
    {
        if (options.content.willDeleteObject())
            setContentOwned(options.content.release(), true);
        else
            setContentNonOwned(options.content.release(), true);

        centreAroundComponent(options.componentToCentreAround, getWidth(), getHeight());
        setResizable(options.resizable, options.useBottomRightCornerResizer);

        setUsingNativeTitleBar(options.useNativeTitleBar);

        // Tool dialogs live in their own top-level window, separate from the
        // main SEANCE window's TooltipWindow hierarchy. Without a local
        // TooltipWindow, every setTooltip() call inside any tool dialog
        // (wavetable editor, spectral editor, plugin sandbox, etc.) is
        // silently dead code - the tooltip text exists but never shows.
        // Install one here so all tool dialogs get tooltips for free.
        tooltipWindow = std::make_unique<juce::TooltipWindow>(this, 600);
    }

    int getDesktopWindowStyleFlags() const override {
        // Per the guidance in juce_TopLevelWindow.cpp: take the base class's
        // flags and remove what we don't want. Stripping windowAppearsOnTaskbar
        // makes Windows assign WS_EX_TOOLWINDOW instead of WS_EX_APPWINDOW
        // (see juce_Windowing_windows.cpp:2433) - no taskbar entry, smaller
        // native frame (irrelevant since we override the title bar anyway).
        return juce::DialogWindow::getDesktopWindowStyleFlags()
             & ~juce::ComponentPeer::windowAppearsOnTaskbar;
    }

    void closeButtonPressed() override {
        // Modal path (launchToolDialog): setVisible(false) exits the modal
        // state, which - with deleteWhenDismissed=true - triggers self-
        // deletion. Same behavior as JUCE's LaunchOptions::launchAsync.
        //
        // Non-modal path (launchNonModalToolDialog): no modal state to
        // dismiss, so setVisible(false) would just hide the window. The
        // caller asked for delete-on-close behavior, so schedule a deferred
        // delete via callAsync (can't delete `this` from within a callback
        // chain that still touches `this` after this method returns).
        if (isCurrentlyModal()) {
            setVisible(false);
        } else if (deleteOnClose) {
            juce::Component::SafePointer<ToolDialogWindow> safe(this);
            juce::MessageManager::callAsync([safe]() {
                if (safe) delete safe.getComponent();
            });
        } else {
            // launchManagedToolDialog mode: caller owns the lifetime. Just
            // hide; caller decides when to delete.
            setVisible(false);
        }
    }

    void setDeleteOnClose(bool b) { deleteOnClose = b; }

private:
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    bool deleteOnClose = false;

    JUCE_DECLARE_NON_COPYABLE(ToolDialogWindow)
};

juce::DialogWindow* launchAudioDeviceSettings(juce::AudioDeviceManager& dm,
                                              juce::Component* centreAround) {
    // JUCE's built-in selector covers driver type / input / output / sample
    // rate / buffer size + MIDI input in one panel. Owned by the dialog.
    auto* selector = new juce::AudioDeviceSelectorComponent(
        dm,
        0, 2,    // min/max input channels
        0, 2,    // min/max output channels
        true,    // show MIDI input options
        false,   // don't show MIDI output options
        true,    // show channels as stereo pairs
        false);  // don't hide advanced options
    selector->setSize(500, 400);

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(selector);
    opts.dialogTitle = "Audio Device Settings";
    opts.dialogBackgroundColour = juce::Colour(40, 40, 45);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.resizable = true;
    opts.componentToCentreAround = centreAround;
    return launchToolDialog(opts);
}

juce::DialogWindow* launchToolDialog(juce::DialogWindow::LaunchOptions& opts) {
    jassert(opts.content != nullptr);
    auto* d = new ToolDialogWindow(opts);
    d->enterModalState(true, nullptr, true);
    return d;
}

juce::DialogWindow* launchManagedToolDialog(juce::DialogWindow::LaunchOptions& opts) {
    jassert(opts.content != nullptr);
    auto* d = new ToolDialogWindow(opts);
    // setVisible(true) on a TopLevelWindow adds it to the desktop (the OS
    // window manager). No enterModalState means setVisible(false) is a
    // pure hide and doesn't trigger the modal-state delete-on-dismiss
    // pathway. Lifetime is the caller's problem.
    d->setVisible(true);
    return d;
}

juce::DialogWindow* launchNonModalToolDialog(juce::DialogWindow::LaunchOptions& opts) {
    jassert(opts.content != nullptr);
    auto* d = new ToolDialogWindow(opts);
    // Non-modal but delete-on-close. Difference vs launchToolDialog: no
    // modal state, so child dialogs (drag-image components on the desktop,
    // file pickers, etc.) aren't blocked by JUCE's modal input filtering.
    // Difference vs launchManagedToolDialog: the title-bar X button
    // deletes the dialog instead of just hiding it - matches what the
    // user expects when they click X.
    //
    // Why this matters: the modal path blocks DragAndDropContainer's
    // DragImageComponent (which lives directly on the desktop, not inside
    // the modal dialog) from receiving mouseUp via the mouseListener
    // forwarding chain, so itemDropped never fires for drags that
    // originate inside the dialog. Non-modal sidesteps that entirely.
    d->setDeleteOnClose(true);
    d->setVisible(true);

    // Make the OS-level owner of this popup the main SEANCE HWND. Without an
    // owner, an unowned top-level window doesn't follow the main window
    // through alt-tab cycles: when you tab away from SEANCE and tab back,
    // Windows brings the main app window to the front but leaves this dialog
    // behind it (effectively invisible to the user, who reads "the wavetable
    // editor closed itself"). With an owner relationship, Windows treats the
    // two windows as a group: alt-tab brings them together, the dialog stays
    // on top of the owner, and the OS also suppresses any taskbar entry
    // (owned popups never get one).
    //
    // Why SetWindowLongPtr directly instead of Component::addToDesktop's
    // nativeWindowToAttachTo parameter? Because by the time we get here the
    // dialog already has a peer (TopLevelWindow's constructor calls
    // addToDesktop with shouldAddToDesktop=true), and Component::addToDesktop
    // short-circuits when the existing peer's style flags match the
    // requested ones - the nativeWindowToAttachTo argument is silently
    // ignored on the no-op path. Forcing a peer recreation just to set the
    // owner would tear down and rebuild the OS window mid-launch, which
    // costs us the position we just centred and creates a visible flash.
    // SetWindowLongPtr(GWLP_HWNDPARENT) reassigns the owner on the existing
    // HWND without recreating it.
#ifdef _WIN32
    void* ownerHwnd = nullptr;
    if (opts.componentToCentreAround != nullptr) {
        if (auto* top = opts.componentToCentreAround->getTopLevelComponent()) {
            if (auto* peer = top->getPeer())
                ownerHwnd = peer->getNativeHandle();
        }
    }
    if (ownerHwnd != nullptr) {
        if (auto* peer = d->getPeer()) {
            HWND childHwnd = (HWND) peer->getNativeHandle();
            SetWindowLongPtr(childHwnd, GWLP_HWNDPARENT, (LONG_PTR) ownerHwnd);
        }
    }
#endif

    return d;
}

} // namespace SoundShop
