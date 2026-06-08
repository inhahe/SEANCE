#include "wavelet_painter.h"
#include "help_utils.h"
#include <algorithm>
#include <cmath>

namespace SoundShop {

// ==============================================================================
// Construction
// ==============================================================================

WaveletPainterComponent::WaveletPainterComponent(NodeGraph& g, int id,
                                                  std::function<void()> apply)
    : graph(&g), nodeId(id), externalFrame(nullptr), onApply(std::move(apply))
{
    // Load existing state if the node already has a wavelet-paint script.
    bool loaded = false;
    if (auto* nd = graph->findNode(nodeId)) {
        loaded = decodeWaveletPaint(nd->script, coefficients,
                                    numLevels, totalSize, filterName);
    }
    if (!loaded) {
        coefficients.assign(totalSize, 0.0f);
        seedDefaultCoefficients();
    }
    initUI();
}

WaveletPainterComponent::WaveletPainterComponent(WaveletFrame& frame,
                                                  std::function<void()> apply)
    : graph(nullptr), nodeId(0), externalFrame(&frame), onApply(std::move(apply))
{
    // Seed the painter from the frame's existing coefficient grid.
    coefficients = frame.coefficients;
    numLevels    = frame.numLevels;
    totalSize    = frame.totalSize;
    filterName   = frame.filterName;
    if ((int)coefficients.size() != totalSize)
        coefficients.assign((size_t)totalSize, 0.0f);
    initUI();
}

void WaveletPainterComponent::initUI() {
    // ---- Toolbar ----
    addAndMakeVisible(ampLabel);
    ampLabel.setText("Amp", juce::dontSendNotification);
    ampLabel.setFont(12.0f);

    addAndMakeVisible(ampSlider);
    ampSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    ampSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 55, 18);
    ampSlider.setRange(-1.0, 1.0, 0.01);
    ampSlider.setValue(0.5, juce::dontSendNotification);
    ampSlider.setTooltip("Paint amplitude (-1 to +1). Left-click paints this value into the cell under the cursor. "
                         "Shift-click paints the negated value. Right-click erases (sets the cell to 0).");

    addAndMakeVisible(filterLabel);
    filterLabel.setText("Filter", juce::dontSendNotification);
    filterLabel.setFont(12.0f);

    addAndMakeVisible(filterCombo);
    filterCombo.addItem("db1 (Haar)", 1);
    filterCombo.addItem("db2",        2);
    filterCombo.addItem("db4",        3);
    filterCombo.addItem("sym4",       4);
    int sel = (filterName == "db1") ? 1 : (filterName == "db2") ? 2
            : (filterName == "sym4") ? 4 : 3;
    filterCombo.setSelectedId(sel, juce::dontSendNotification);
    filterCombo.setTooltip(
        "Wavelet family used for the inverse transform. The same painted coefficient grid "
        "reconstructs into different waveform shapes depending on which family is chosen, "
        "because each family has a different mother-wavelet shape.\n"
        "\n"
        "  db1 (Haar)  - the simplest wavelet: a single up-then-down step. "
        "Reconstruction is blocky and stair-stepped, since every basis function is a square "
        "pulse. Good for percussive / chiptune / square-edged sounds; bad for smooth tones.\n"
        "\n"
        "  db2  - 4-tap Daubechies wavelet. Slightly smoother than Haar but still sharp; "
        "introduces small asymmetric ripples. A middle ground between blocky and smooth.\n"
        "\n"
        "  db4  - 8-tap Daubechies wavelet. Noticeably smoother reconstruction with rounded "
        "transitions. A balanced default that works for most painted shapes; asymmetric "
        "(slight left/right lean in the basis shape).\n"
        "\n"
        "  sym4  - 8-tap symmetric (Symlet) wavelet. Similar smoothness to db4 but with a "
        "near-symmetric basis function, so reconstructed peaks are centered rather than leaning. "
        "Best for clean tonal sounds and when you want painted peaks to sit where you put them.");
    filterCombo.onChange = [this]() {
        switch (filterCombo.getSelectedId()) {
            case 1: filterName = "db1"; break;
            case 2: filterName = "db2"; break;
            case 4: filterName = "sym4"; break;
            default: filterName = "db4"; break;
        }
        updateWaveform();
        commitToNode();
        if (onApply) onApply();
        repaint();
    };

    addAndMakeVisible(levelsLabel);
    levelsLabel.setText("Levels", juce::dontSendNotification);
    levelsLabel.setFont(12.0f);

    addAndMakeVisible(levelsCombo);
    // Min 2 levels (must have at least 1 detail band + approx); max 9 leaves
    // an approx row of size totalSize/2^9 = 2 cells at the default 1024 size.
    for (int n = 2; n <= 9; ++n)
        levelsCombo.addItem(juce::String(n), n);
    levelsCombo.setSelectedId(numLevels, juce::dontSendNotification);
    levelsCombo.setTooltip("Number of wavelet decomposition levels (rows in the grid). "
                           "More levels = more detail bands stacked below the approximation row, with "
                           "the finest band's cells getting narrower. The same painted coefficients are "
                           "reinterpreted under the new layout, so changing this remixes the sound rather "
                           "than clearing it.");
    levelsCombo.onChange = [this]() {
        int newLevels = levelsCombo.getSelectedId();
        if (newLevels < 2) newLevels = 2;
        // Guard: approximation row must have at least 1 cell.
        int approx = totalSize;
        for (int l = 0; l < newLevels; ++l) approx /= 2;
        if (approx < 1) {
            // Snap selection back to the previous valid value.
            levelsCombo.setSelectedId(numLevels, juce::dontSendNotification);
            return;
        }
        numLevels = newLevels;
        updateWaveform();
        commitToNode();
        if (onApply) onApply();
        repaint();
    };

    addAndMakeVisible(clearBtn);
    clearBtn.setTooltip("Zero every coefficient. The waveform becomes silent until you paint new coefficients.");
    clearBtn.onClick = [this]() {
        std::fill(coefficients.begin(), coefficients.end(), 0.0f);
        updateWaveform();
        commitToNode();
        if (onApply) onApply();
        repaint();
    };

    addAndMakeVisible(helpBtn);
    helpBtn.setTooltip("Open documentation for the wavelet painter.");
    helpBtn.onClick = []() { openHelpDocFile("wavetables.html"); };

    updateWaveform();
    setSize(820, 460);
}

WaveletPainterComponent::~WaveletPainterComponent() {
    stopTimer();
}

// ==============================================================================
// Seed
// ==============================================================================

void WaveletPainterComponent::seedDefaultCoefficients() {
    // Drop a few coefficients in a couple of detail bands so a fresh painter
    // already makes audible sound the user can iterate from.
    int approxLen = totalSize;
    for (int l = 0; l < numLevels; ++l) approxLen /= 2;
    int idx0 = approxLen + approxLen / 4;
    int idx1 = approxLen + (approxLen / 4) * 2;
    if (idx0 < totalSize) coefficients[idx0] = 0.5f;
    if (idx1 < totalSize) coefficients[idx1] = -0.3f;
}

// ==============================================================================
// Layout
// ==============================================================================

void WaveletPainterComponent::resized() {
    auto a = getLocalBounds().reduced(8);

    auto top = a.removeFromTop(28);
    ampLabel    .setBounds(top.removeFromLeft(30));
    ampSlider   .setBounds(top.removeFromLeft(220));
    top.removeFromLeft(8);
    filterLabel .setBounds(top.removeFromLeft(38));
    filterCombo .setBounds(top.removeFromLeft(110));
    top.removeFromLeft(8);
    levelsLabel .setBounds(top.removeFromLeft(44));
    levelsCombo .setBounds(top.removeFromLeft(56));
    top.removeFromLeft(8);
    clearBtn    .setBounds(top.removeFromLeft(60));
    helpBtn     .setBounds(top.removeFromRight(24));
    toolbarBounds = a; // currently unused for hit-tests, but kept for clarity

    a.removeFromTop(6);

    // Bottom strip: time-domain preview waveform
    waveBounds = a.removeFromBottom(110);
    a.removeFromBottom(4);

    // Remaining area: scale x time coefficient grid
    gridBounds = a;
}

// ==============================================================================
// Painting
// ==============================================================================

static juce::Colour cellColour(float v, bool isApprox) {
    float a = juce::jlimit(0.0f, 1.0f, std::abs(v));
    // Approximation row: blue-leaning. Detail rows: green-to-magenta by sign.
    if (isApprox) {
        return juce::Colour::fromHSV(0.6f + v * 0.15f, 0.7f, a * 0.8f + 0.1f, 1.0f);
    }
    float h = (v >= 0.0f) ? 0.33f : 0.85f;   // green for +, magenta for -
    return juce::Colour::fromHSV(h, 0.85f, a * 0.85f + 0.1f, 1.0f);
}

void WaveletPainterComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(18, 20, 28));

    // ---- Grid ----
    auto grid = gridBounds.toFloat();
    g.setColour(juce::Colour(28, 30, 38));
    g.fillRoundedRectangle(grid, 4.0f);
    g.setColour(juce::Colour(70, 70, 90));
    g.drawRoundedRectangle(grid, 4.0f, 1.0f);

    int approxLen = totalSize;
    for (int l = 0; l < numLevels; ++l) approxLen /= 2;
    if (approxLen < 1) approxLen = 1;

    int rows = numLevels + 1; // approx + N detail bands
    float rowH = grid.getHeight() / (float)rows;

    // Approximation row
    {
        float y = grid.getY();
        float colW = grid.getWidth() / (float)approxLen;
        for (int i = 0; i < approxLen; ++i) {
            g.setColour(cellColour(coefficients[i], true));
            g.fillRect(grid.getX() + (float)i * colW, y,
                       std::max(1.0f, colW - 0.5f), rowH - 0.5f);
        }
        g.setColour(juce::Colours::lightgrey);
        g.setFont(10.0f);
        g.drawText("Approx", (int)grid.getX() + 4, (int)y, 60, (int)rowH,
                   juce::Justification::centredLeft);
    }

    // Detail bands - finer bands lower down
    int bandStart = approxLen;
    for (int band = 0; band < numLevels; ++band) {
        int bandLen = approxLen * (1 << band);
        float y = grid.getY() + (band + 1) * rowH;
        float colW = grid.getWidth() / (float)bandLen;
        for (int i = 0; i < bandLen && (bandStart + i) < totalSize; ++i) {
            g.setColour(cellColour(coefficients[bandStart + i], false));
            g.fillRect(grid.getX() + (float)i * colW, y,
                       std::max(1.0f, colW - 0.5f), rowH - 0.5f);
        }
        g.setColour(juce::Colours::lightgrey);
        g.setFont(10.0f);
        g.drawText("D" + juce::String(band + 1),
                   (int)grid.getX() + 4, (int)y, 40, (int)rowH,
                   juce::Justification::centredLeft);
        bandStart += bandLen;
    }

    // ---- Waveform preview ----
    auto wave = waveBounds.toFloat().reduced(2.0f);
    g.setColour(juce::Colour(24, 24, 30));
    g.fillRoundedRectangle(wave, 3.0f);
    g.setColour(juce::Colour(70, 70, 90));
    g.drawRoundedRectangle(wave, 3.0f, 1.0f);

    float cy = wave.getCentreY();
    g.setColour(juce::Colours::grey.withAlpha(0.25f));
    g.drawHorizontalLine((int)cy, wave.getX(), wave.getRight());

    if (!waveform.empty()) {
        juce::Path p;
        int n = (int)waveform.size();
        float w = wave.getWidth() - 4;
        float h = wave.getHeight() - 4;
        float cx = wave.getX() + 2;
        for (int i = 0; i < n; ++i) {
            float x = cx + (float)i / (float)(n - 1) * w;
            float y = cy - waveform[i] * h * 0.45f;
            if (i == 0) p.startNewSubPath(x, y);
            else        p.lineTo(x, y);
        }
        g.setColour(juce::Colour(150, 200, 255));
        g.strokePath(p, juce::PathStrokeType(1.3f));
    }

    g.setColour(juce::Colours::lightgrey);
    g.setFont(11.0f);
    g.drawText("Resulting waveform (one cycle, IDWT of the grid above)",
               wave.translated(0, -14), juce::Justification::topLeft);
}

// ==============================================================================
// Mouse
// ==============================================================================

void WaveletPainterComponent::mouseDown(const juce::MouseEvent& e) { paintCoeff(e); }
void WaveletPainterComponent::mouseDrag(const juce::MouseEvent& e) { paintCoeff(e); }

void WaveletPainterComponent::mouseUp(const juce::MouseEvent&) {
    // Debounce the apply call so we don't fight JUCE's graph rebuild on
    // every micro-drag. Half a second after the last edit, push the new
    // script to the node and ping the engine.
    startTimer(500);
}

void WaveletPainterComponent::paintCoeff(const juce::MouseEvent& e) {
    auto grid = gridBounds.toFloat();
    if (!grid.contains(e.position)) return;

    float relX = (e.position.x - grid.getX()) / grid.getWidth();
    float relY = (e.position.y - grid.getY()) / grid.getHeight();
    relX = juce::jlimit(0.0f, 0.9999f, relX);
    relY = juce::jlimit(0.0f, 0.9999f, relY);

    int approxLen = totalSize;
    for (int l = 0; l < numLevels; ++l) approxLen /= 2;
    if (approxLen < 1) approxLen = 1;

    int rows = numLevels + 1;
    int rowIdx = juce::jlimit(0, rows - 1, (int)(relY * (float)rows));

    float value;
    if (e.mods.isRightButtonDown())     value =  0.0f;
    else if (e.mods.isShiftDown())      value = -(float)ampSlider.getValue();
    else                                value =  (float)ampSlider.getValue();

    if (rowIdx == 0) {
        // Approximation
        int col = juce::jlimit(0, approxLen - 1, (int)(relX * (float)approxLen));
        coefficients[col] = value;
    } else {
        int band = rowIdx - 1;
        int bandLen = approxLen * (1 << band);
        int bandStart = approxLen;
        for (int b = 0; b < band; ++b) bandStart += approxLen * (1 << b);
        int col = juce::jlimit(0, bandLen - 1, (int)(relX * (float)bandLen));
        if (bandStart + col < totalSize)
            coefficients[bandStart + col] = value;
    }

    updateWaveform();
    repaint();
}

// ==============================================================================
// Synthesis + commit
// ==============================================================================

void WaveletPainterComponent::updateWaveform() {
    waveform = waveletPaintToWaveform(coefficients, numLevels, filterName);
}

void WaveletPainterComponent::commitToNode() {
    if (externalFrame) {
        // Frame-backed: mirror straight into the owning WaveletFrame.
        externalFrame->coefficients = coefficients;
        externalFrame->numLevels    = numLevels;
        externalFrame->totalSize    = totalSize;
        externalFrame->filterName   = filterName;
    } else if (graph) {
        if (auto* nd = graph->findNode(nodeId)) {
            // Synchronised: a standalone wavelet-paint node classifies as a
            // wavetable source, so TerrainSynthProcessor polls its script on
            // the audio thread. Lock the per-node mutex around the write.
            setNodeScriptSynced(*nd, encodeWaveletPaint(coefficients, numLevels, filterName));
            // Mark the project dirty so close/quit prompts the user to save
            // and autosave captures these edits. (The externalFrame branch
            // above writes into a frame inside the owning LayeredWaveEditor,
            // which has its own commitToNode that bumps the dirty flag.)
            graph->dirty = true;
        }
    }
}

void WaveletPainterComponent::timerCallback() {
    stopTimer();
    commitToNode();
    if (onApply) onApply();
}

} // namespace SoundShop
