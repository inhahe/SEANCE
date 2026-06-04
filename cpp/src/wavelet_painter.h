#pragma once
#include "node_graph.h"
#include "wavelet.h"
#include "fft_util.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <cmath>

namespace SoundShop {

// ==============================================================================
// Wavelet Space Painter (#65): draw directly on a scales × time grid
// of DWT coefficients, hear the result in real time via IDWT.
//
// The canvas shows wavelet decomposition levels as rows (coarse at top,
// fine at bottom) and time positions as columns. The user paints
// coefficient values with the mouse (click/drag), and the node's
// waveform is updated in real time by inverse-DWT'ing the edited
// coefficients back to the time domain.
//
// Intended as a creative sound-design tool: instead of drawing a
// waveform directly, you draw in the time-frequency plane, getting
// intuitive control over which frequencies exist at which moments.
// ==============================================================================

class WaveletPainterComponent : public juce::Component {
public:
    WaveletPainterComponent(NodeGraph& g, int nid, std::function<void()> onApply)
        : graph(g), nodeId(nid), applyCallback(onApply)
    {
        // Initialize a blank coefficient grid: 6 levels, 1024 total samples.
        numLevels = 5;
        totalSize = 1 << (numLevels + 2); // 128 at level 5 -> total = 128 * 2 = 256...
        // Actually for 5 levels of DWT on a 1024-sample signal:
        totalSize = 1024;
        coefficients.assign(totalSize, 0.0f);

        // Seed with a simple impulse so there's something to hear.
        coefficients[totalSize / 4] = 0.5f;
        coefficients[totalSize / 2] = -0.3f;

        updateWaveform();
        setSize(700, 400);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(18, 20, 28));

        auto area = getLocalBounds().toFloat().reduced(4);
        auto gridArea = area.removeFromTop(area.getHeight() * 0.7f);
        auto waveArea = area.reduced(0, 4);

        // Draw coefficient grid: rows = levels, columns = positions within each level.
        int approxLen = totalSize;
        for (int l = 0; l < numLevels; ++l) approxLen /= 2;

        float rowH = gridArea.getHeight() / (float)(numLevels + 1); // +1 for approximation
        float maxCoeff = 0;
        for (float v : coefficients) maxCoeff = std::max(maxCoeff, std::abs(v));
        if (maxCoeff < 1e-6f) maxCoeff = 1.0f;

        // Draw approximation level.
        {
            float y = gridArea.getY();
            float colW = gridArea.getWidth() / approxLen;
            for (int i = 0; i < approxLen; ++i) {
                float v = coefficients[i] / maxCoeff;
                auto col = juce::Colour::fromHSV(0.6f + v * 0.2f, 0.7f, std::abs(v) * 0.8f + 0.1f, 1.0f);
                g.setColour(col);
                g.fillRect(gridArea.getX() + i * colW, y, colW - 0.5f, rowH - 0.5f);
            }
            g.setColour(juce::Colours::grey);
            g.setFont(8.0f);
            g.drawText("Approx", (int)gridArea.getX(), (int)y, 50, (int)rowH, juce::Justification::centredLeft);
        }

        // Draw detail levels.
        int bandStart = approxLen;
        for (int band = 0; band < numLevels; ++band) {
            int bandLen = approxLen * (1 << band);
            float y = gridArea.getY() + (band + 1) * rowH;
            float colW = gridArea.getWidth() / bandLen;
            for (int i = 0; i < bandLen && (bandStart + i) < totalSize; ++i) {
                float v = coefficients[bandStart + i] / maxCoeff;
                auto col = juce::Colour::fromHSV(0.3f - v * 0.3f, 0.8f, std::abs(v) * 0.8f + 0.1f, 1.0f);
                g.setColour(col);
                g.fillRect(gridArea.getX() + i * colW, y, std::max(1.0f, colW - 0.5f), rowH - 0.5f);
            }
            g.setColour(juce::Colours::grey);
            g.setFont(8.0f);
            g.drawText("D" + juce::String(band + 1), (int)gridArea.getX(), (int)y, 30, (int)rowH,
                        juce::Justification::centredLeft);
            bandStart += bandLen;
        }

        // Draw resulting waveform.
        g.setColour(juce::Colour(30, 32, 40));
        g.fillRoundedRectangle(waveArea, 4.0f);
        g.setColour(juce::Colours::cornflowerblue);
        juce::Path p;
        float cy = waveArea.getCentreY();
        for (int i = 0; i < (int)waveform.size(); ++i) {
            float x = waveArea.getX() + (float)i / waveform.size() * waveArea.getWidth();
            float y = cy - waveform[i] * waveArea.getHeight() * 0.45f;
            if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
        g.strokePath(p, juce::PathStrokeType(1.0f));
    }

    void mouseDown(const juce::MouseEvent& e) override { paintCoeff(e); }
    void mouseDrag(const juce::MouseEvent& e) override { paintCoeff(e); }

    void paintCoeff(const juce::MouseEvent& e) {
        auto area = getLocalBounds().toFloat().reduced(4);
        auto gridArea = area.removeFromTop(area.getHeight() * 0.7f);

        float relX = (e.position.x - gridArea.getX()) / gridArea.getWidth();
        float relY = (e.position.y - gridArea.getY()) / gridArea.getHeight();
        if (relX < 0 || relX > 1 || relY < 0 || relY > 1) return;

        int approxLen = totalSize;
        for (int l = 0; l < numLevels; ++l) approxLen /= 2;

        int rowIdx = (int)(relY * (numLevels + 1));
        rowIdx = juce::jlimit(0, numLevels, rowIdx);

        if (rowIdx == 0) {
            // Approximation level.
            int col = (int)(relX * approxLen);
            col = juce::jlimit(0, approxLen - 1, col);
            coefficients[col] = e.mods.isRightButtonDown() ? 0.0f : 0.8f;
        } else {
            int band = rowIdx - 1;
            int bandLen = approxLen * (1 << band);
            int bandStart = approxLen;
            for (int b = 0; b < band; ++b) bandStart += approxLen * (1 << b);
            int col = (int)(relX * bandLen);
            col = juce::jlimit(0, bandLen - 1, col);
            if (bandStart + col < totalSize)
                coefficients[bandStart + col] = e.mods.isRightButtonDown() ? 0.0f : 0.5f;
        }

        updateWaveform();
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override {
        // Commit to node on mouse-up.
        if (auto* nd = graph.findNode(nodeId)) {
            // Encode the waveform into node.script as a layered waveform
            // (single drawn layer with the IDWT result).
            // For now, just update the terrain data directly.
            nd->script = "__waveletpaint__:" + encodeCoefficients();
        }
        if (applyCallback) applyCallback();
    }

private:
    NodeGraph& graph;
    int nodeId;
    std::function<void()> applyCallback;

    int numLevels = 5;
    int totalSize = 1024;
    std::vector<float> coefficients;
    std::vector<float> waveform;

    void updateWaveform() {
        waveform = coefficients;
        auto filt = getWaveletFilter("db4");
        idwt(waveform, numLevels, filt);
        // Normalize.
        float peak = 0;
        for (float v : waveform) peak = std::max(peak, std::abs(v));
        if (peak > 1e-6f) for (float& v : waveform) v /= peak;
    }

    std::string encodeCoefficients() const {
        std::string s;
        for (int i = 0; i < totalSize; ++i) {
            if (i > 0) s += ",";
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.4f", coefficients[i]);
            s += buf;
        }
        return s;
    }
};

} // namespace SoundShop
