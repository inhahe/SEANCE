#pragma once
#include "adsr_envelope.h"
#include "envelope_presets.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

namespace SoundShop {

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
//   |  [Edit attack...] [Edit decay...] [Edit release...]                |
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

private:
    AHDSREnvelope& env;
    std::function<void()> onChanged;

    // Top row: preset picker + save/manage.
    juce::Label      presetLabel { {}, "Preset:" };
    juce::ComboBox   presetCombo;
    juce::TextButton saveAsBtn   { "Save as..." };
    juce::TextButton manageBtn   { "Manage..." };

    // Time / level sliders. We use vertical sliders with text boxes
    // below; this is the layout musicians expect and it packs cleanly
    // into a horizontal row.
    juce::Label attackLbl  { {}, "Attack"   };
    juce::Label holdLbl    { {}, "Hold"     };
    juce::Label decayLbl   { {}, "Decay"    };
    juce::Label sustainLbl { {}, "Sustain"  };
    juce::Label releaseLbl { {}, "Release"  };
    juce::Label velLbl     { {}, "Vel Sens" };
    juce::Slider attackS, holdS, decayS, sustainS, releaseS, velS;

    // Bottom: curve preview + per-segment edit buttons.
    juce::TextButton editAttackBtn  { "Edit Attack Curve..."  };
    juce::TextButton editDecayBtn   { "Edit Decay Curve..."   };
    juce::TextButton editReleaseBtn { "Edit Release Curve..." };
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
