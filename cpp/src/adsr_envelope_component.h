#pragma once
#include "adsr_envelope.h"
#include "envelope_presets.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

namespace SoundShop {

class AssetLibrary;

// ==============================================================================
// AHDSREnvelopeComponent - the shared editor for an AHDSREnvelope, designed
// to be embedded as a section inside any synth dialog and also opened as a
// stand-alone dialog. The component edits an external AHDSREnvelope by
// reference and fires onChanged() after any user action so the host can
// commit changes to the underlying node and snapshot undo.
//
// Layout (roughly):
//
//   +--------------------------------------------------------------------+
//   |  Preset: [dropdown]  [Save as...] [Manage...]                      |
//   +--------------------------------------------------------------------+
//   |  Attack   Hold   Decay   Sustain  Release  Velocity                |
//   |  [slider] [...]  [...]   [...]    [...]    [slider]                |
//   +--------------------------------------------------------------------+
//   |  [          envelope preview (waveform shape)          ]           |
//   |                                                                    |
//   |  [Attack Curve...] [Decay Curve...] [Release Curve...]             |
//   +--------------------------------------------------------------------+
//
// The preview lights up the segment being edited; clicking an "Edit ..."
// button opens a modal SpectralCurvePanel-backed dialog for that segment's
// curve. The dialog is parented to this component so it groups under the
// main SEANCE taskbar entry (per CLAUDE.md dialog rule).
// ==============================================================================
class AHDSREnvelopeComponent : public juce::Component {
public:
    AHDSREnvelopeComponent(AHDSREnvelope& env, std::function<void()> onChanged);
    ~AHDSREnvelopeComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Reload all UI controls from the underlying envelope. Call this if
    // the envelope was changed externally (e.g. a preset was applied via
    // some other path, or undo/redo restored a different state).
    void syncFromModel();

    // Optional integration with the project asset library's AHDSR-curve store.
    // When set, the editor shows a "Library" row: a picker to reference a stored
    // curve (live) and an "Add to Library" button to publish the current shape.
    // While a stored curve is referenced, edits write back to it and propagate
    // to every node sharing the id (the "live reference" model). When the picker
    // is "(Independent)", the editor behaves exactly as before (edits the node's
    // own local envelope). Distinct from the app-global *preset* row above, which
    // is copy-on-apply and not project-scoped.
    struct LibraryContext {
        AssetLibrary* lib = nullptr;
        std::function<int()>     getAssetId;   // node's current ahdsrAssetId
        std::function<void(int)> setAssetId;   // set node's ahdsrAssetId
        std::function<void()>    propagate;    // re-resolve all live references
    };
    void setLibraryContext(LibraryContext ctx);

private:
    AHDSREnvelope& env;
    std::function<void()> onChanged;
    LibraryContext libCtx;
    bool libraryRowVisible = false;

    // Top row: preset picker + save/manage.
    juce::Label      presetLabel { {}, "Preset:" };
    juce::ComboBox   presetCombo;
    juce::TextButton saveAsBtn   { "Save as..." };
    juce::TextButton manageBtn   { "Manage..." };

    // Library row (asset-library AHDSR-curve store). Hidden until setLibraryContext.
    juce::Label      libraryLbl  { {}, "Library:" };
    juce::ComboBox   libraryCombo;
    juce::TextButton addToLibBtn { "Add to Library" };
    void rebuildLibraryCombo();
    void onLibrarySelected(int comboId);
    void openAddToLibraryDialog();

    // Time / level sliders. We use vertical sliders with text boxes
    // below; this is the layout musicians expect and it packs cleanly
    // into a horizontal row.
    juce::Label attackLbl  { {}, "Attack"   };
    juce::Label holdLbl    { {}, "Hold"     };
    juce::Label decayLbl   { {}, "Decay"    };
    juce::Label sustainLbl { {}, "Sustain"  };
    juce::Label releaseLbl { {}, "Release"  };
    juce::Label velLbl     { {}, "Velocity" };
    juce::Slider attackS, holdS, decayS, sustainS, releaseS, velS;

    // Plain-language sub-labels under each technical term. SEANCE is built
    // for non-musicians, so A/H/D/S/R jargon gets a 2-3 word descriptor that
    // stays on screen (the full sentence still lives in the slider tooltips).
    juce::Label attackSub  { {}, "(fade in)"        };
    juce::Label holdSub    { {}, "(hold at peak)"   };
    juce::Label decaySub   { {}, "(fall to sustain)"};
    juce::Label sustainSub { {}, "(held level)"     };
    juce::Label releaseSub { {}, "(fade out)"       };
    juce::Label velSub     { {}, "(touch response)" };

    // Per-segment tension (curve-bend) knobs - the single-linear-control
    // way to reshape a ramp without opening the full curve editor. Each
    // drives env.{attack,decay,release}Tension in [-1, 1] (0 = linear).
    juce::Label  attackTenLbl  { {}, "A Curve" };
    juce::Label  decayTenLbl   { {}, "D Curve" };
    juce::Label  releaseTenLbl { {}, "R Curve" };
    juce::Slider attackTenS, decayTenS, releaseTenS;

    // Bottom: curve preview + per-segment edit buttons.
    juce::TextButton editAttackBtn  { "Attack Curve..."  };
    juce::TextButton editDecayBtn   { "Decay Curve..."   };
    juce::TextButton editReleaseBtn { "Release Curve..." };
    juce::Rectangle<int> previewBounds;

    // Listener token for the preset manager so the dropdown stays in
    // sync if another instance of this component adds / renames a preset.
    int presetGeneration = -1;

    void rebuildPresetCombo();
    void onPresetChosen(int id);
    void openSaveAsDialog();
    void openManageDialog();
    void openCurveEditor(const juce::String& title, SpectralCurve& curve,
                         juce::Colour colour);

    void paintPreview(juce::Graphics& g, juce::Rectangle<int> r);
    void commitChange(); // calls onChanged + refreshes preview
};

} // namespace SoundShop
