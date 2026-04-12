#include "spectrum_tap.h"
#include "effect_regions.h" // for getDistinctColor
#include <algorithm>

namespace SoundShop {

// ==============================================================================
// SpectrumTapProcessor — inline audio passthrough + frequency bin analysis
// ==============================================================================

SpectrumTapProcessor::SpectrumTapProcessor(Node& n) : node(n) {}

void SpectrumTapProcessor::prepareToPlay(double sr, int) {
    sampleRate = sr;
    rebuildBins();
}

void SpectrumTapProcessor::rebuildBins() {
    // Each param named "Bin N: xxxHz" defines a bin. Parse center freq and
    // bandwidth from the param's min/max: min = centerFreq, max = bandwidth.
    // The param's value is the current amplitude (written by processBlock).
    bins.clear();
    for (int i = 0; i < (int)node.params.size(); ++i) {
        auto& p = node.params[i];
        if (p.name.rfind("Bin ", 0) == 0) {
            FrequencyBin bin;
            bin.centerFreq = p.minVal;  // hijack min for center freq
            bin.bandwidth = p.maxVal;   // hijack max for bandwidth
            bin.color = getDistinctColor(i);
            bin.computeCoeffs(sampleRate);
            bins.push_back(bin);
        }
    }
}

void SpectrumTapProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) {
    // Check if bins need rebuilding (params may have changed from UI)
    if ((int)bins.size() != [&]() {
        int count = 0;
        for (auto& p : node.params)
            if (p.name.rfind("Bin ", 0) == 0) count++;
        return count;
    }()) {
        rebuildBins();
    }

    int numSamples = buf.getNumSamples();
    if (numSamples == 0 || bins.empty()) return;

    // Use channel 0 (mono analysis). Audio passes through unchanged.
    const float* input = buf.getReadPointer(0);

    for (int s = 0; s < numSamples; ++s) {
        float sample = input[s];
        for (int bi = 0; bi < (int)bins.size(); ++bi) {
            float amp = bins[bi].process(sample);
            // Write amplitude back to the node's param value. The param acts
            // as a real-time output: downstream nodes or signal connections
            // can read it.
            int paramIdx = -1, count = 0;
            for (int pi = 0; pi < (int)node.params.size(); ++pi) {
                if (node.params[pi].name.rfind("Bin ", 0) == 0) {
                    if (count == bi) { paramIdx = pi; break; }
                    count++;
                }
            }
            if (paramIdx >= 0)
                node.params[paramIdx].value = std::min(1.0f, amp * 4.0f); // scale for visibility
        }
    }
    // Audio passes through unchanged — no modification to buf
}

// ==============================================================================
// SpectrumTapComponent — UI for managing bins and viewing levels
// ==============================================================================

SpectrumTapComponent::SpectrumTapComponent(NodeGraph& g, int nid)
    : graph(g), nodeId(nid)
{
    addAndMakeVisible(addBinBtn);
    addBinBtn.setTooltip("Add a new frequency band to monitor. Each bin outputs an amplitude signal you can use "
                         "to drive other parameters — useful for spectrum-following effects, vocoder-like routing, etc.");
    addBinBtn.onClick = [this]() {
        auto* nd = graph.findNode(nodeId);
        if (!nd) return;
        int binCount = 0;
        for (auto& p : nd->params)
            if (p.name.rfind("Bin ", 0) == 0) binCount++;
        // New bin at a default frequency, spaced logarithmically
        float freq = 200.0f * std::pow(2.0f, (float)binCount);
        freq = std::min(freq, 16000.0f);
        float bw = freq * 0.3f; // 30% bandwidth
        // Param: name="Bin N: XHz", min=centerFreq, max=bandwidth, value=amplitude
        juce::String name = "Bin " + juce::String(binCount + 1) + ": "
                          + juce::String((int)freq) + "Hz";
        nd->params.push_back({name.toStdString(), 0.0f, freq, bw});
        graph.dirty = true;
        repaint();
    };
    startTimerHz(30);
    setSize(500, 300);
}

juce::Rectangle<float> SpectrumTapComponent::getSpectrumArea() const {
    return getLocalBounds().toFloat().reduced(12).withTrimmedTop(32).withTrimmedBottom(16);
}

float SpectrumTapComponent::hzToX(float hz) const {
    auto area = getSpectrumArea();
    // Log scale: 20 Hz → left edge, 20000 Hz → right edge
    float logMin = std::log2(20.0f);
    float logMax = std::log2(20000.0f);
    float logHz = std::log2(std::max(20.0f, hz));
    float frac = (logHz - logMin) / (logMax - logMin);
    return area.getX() + frac * area.getWidth();
}

float SpectrumTapComponent::xToHz(float x) const {
    auto area = getSpectrumArea();
    float frac = (x - area.getX()) / area.getWidth();
    float logMin = std::log2(20.0f);
    float logMax = std::log2(20000.0f);
    return std::pow(2.0f, logMin + frac * (logMax - logMin));
}

void SpectrumTapComponent::resized() {
    addBinBtn.setBounds(getLocalBounds().reduced(12).removeFromTop(28).removeFromLeft(100));
}

void SpectrumTapComponent::mouseDown(const juce::MouseEvent& e) {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    auto area = getSpectrumArea();
    if (!area.contains(e.position)) return;

    // Right-click on a bin to delete it
    if (e.mods.isRightButtonDown()) {
        float clickHz = xToHz(e.position.x);
        int binIdx = 0;
        for (int i = 0; i < (int)nd->params.size(); ++i) {
            if (nd->params[i].name.rfind("Bin ", 0) != 0) continue;
            float center = nd->params[i].minVal;
            float bw = nd->params[i].maxVal;
            if (std::abs(clickHz - center) < bw) {
                nd->params.erase(nd->params.begin() + i);
                graph.dirty = true;
                repaint();
                return;
            }
            binIdx++;
        }
        return;
    }

    // Left-click: find if clicking on a bin's center or edge
    float clickHz = xToHz(e.position.x);
    int binIdx = 0;
    for (int i = 0; i < (int)nd->params.size(); ++i) {
        if (nd->params[i].name.rfind("Bin ", 0) != 0) continue;
        float center = nd->params[i].minVal;
        float bw = nd->params[i].maxVal;
        float left = center - bw * 0.5f;
        float right = center + bw * 0.5f;

        float leftX = hzToX(left);
        float rightX = hzToX(right);
        float centerX = hzToX(center);

        if (std::abs(e.position.x - leftX) < 8) {
            dragBinIdx = i; dragWhat = DragLeft; return;
        }
        if (std::abs(e.position.x - rightX) < 8) {
            dragBinIdx = i; dragWhat = DragRight; return;
        }
        if (e.position.x >= leftX && e.position.x <= rightX) {
            dragBinIdx = i; dragWhat = DragCenter; return;
        }
        binIdx++;
    }
}

void SpectrumTapComponent::mouseDrag(const juce::MouseEvent& e) {
    auto* nd = graph.findNode(nodeId);
    if (!nd || dragBinIdx < 0 || dragBinIdx >= (int)nd->params.size()) return;
    auto& p = nd->params[dragBinIdx];

    float hz = std::max(20.0f, std::min(20000.0f, xToHz(e.position.x)));
    float center = p.minVal;
    float bw = p.maxVal;

    if (dragWhat == DragCenter) {
        p.minVal = hz; // move center
    } else if (dragWhat == DragLeft) {
        float right = center + bw * 0.5f;
        float newLeft = std::min(hz, right - 10.0f);
        p.minVal = (newLeft + right) * 0.5f;
        p.maxVal = right - newLeft;
    } else if (dragWhat == DragRight) {
        float left = center - bw * 0.5f;
        float newRight = std::max(hz, left + 10.0f);
        p.minVal = (left + newRight) * 0.5f;
        p.maxVal = newRight - left;
    }

    // Update the param name to reflect new frequency
    p.name = ("Bin " + juce::String(dragBinIdx + 1) + ": "
             + juce::String((int)p.minVal) + "Hz").toStdString();
    graph.dirty = true;
    repaint();
}

void SpectrumTapComponent::mouseUp(const juce::MouseEvent&) {
    dragBinIdx = -1;
}

void SpectrumTapComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));
    auto area = getSpectrumArea();

    // Background
    g.setColour(juce::Colour(18, 20, 28));
    g.fillRoundedRectangle(area, 4.0f);
    g.setColour(juce::Colour(50, 55, 70));
    g.drawRoundedRectangle(area, 4.0f, 1.0f);

    // Frequency axis labels (log scale)
    g.setColour(juce::Colours::grey);
    g.setFont(9.0f);
    for (float hz : {20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f,
                     5000.0f, 10000.0f, 20000.0f}) {
        float x = hzToX(hz);
        g.setColour(juce::Colour(35, 38, 50));
        g.drawVerticalLine((int)x, area.getY(), area.getBottom());
        g.setColour(juce::Colours::grey);
        juce::String label = (hz >= 1000) ? juce::String((int)(hz / 1000)) + "k"
                                          : juce::String((int)hz);
        g.drawText(label, (int)x - 15, (int)area.getBottom() + 1, 30, 14,
                   juce::Justification::centred);
    }

    // Draw bins
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;

    int binCount = 0;
    for (int i = 0; i < (int)nd->params.size(); ++i) {
        auto& p = nd->params[i];
        if (p.name.rfind("Bin ", 0) != 0) continue;

        float center = p.minVal;
        float bw = p.maxVal;
        float amplitude = p.value; // 0..1, written by processor

        float leftX = hzToX(center - bw * 0.5f);
        float rightX = hzToX(center + bw * 0.5f);
        float binW = rightX - leftX;

        auto binColor = juce::Colour(getDistinctColor(binCount));

        // Bin background (full height, dim)
        g.setColour(binColor.withAlpha(0.15f));
        g.fillRect(leftX, area.getY(), binW, area.getHeight());

        // Amplitude fill (from bottom up)
        float fillH = amplitude * area.getHeight();
        g.setColour(binColor.withAlpha(0.6f));
        g.fillRect(leftX, area.getBottom() - fillH, binW, fillH);

        // Border
        g.setColour(binColor.withAlpha(0.8f));
        g.drawRect(leftX, area.getY(), binW, area.getHeight(), 1.0f);

        // Label
        g.setColour(juce::Colours::white);
        g.setFont(9.0f);
        g.drawText(juce::String((int)center) + " Hz",
                   (int)leftX + 2, (int)area.getY() + 2, (int)binW - 4, 12,
                   juce::Justification::centredLeft, false);

        // Resize handles (small bars at left/right edges)
        g.setColour(binColor.brighter(0.4f));
        g.fillRect(leftX - 1, area.getY(), 3.0f, area.getHeight());
        g.fillRect(rightX - 1, area.getY(), 3.0f, area.getHeight());

        binCount++;
    }

    // Title
    g.setColour(juce::Colours::white.withAlpha(0.7f));
    g.setFont(12.0f);
    g.drawText("Spectrum Tap — click to drag bins, right-click to delete, edges to resize",
               getLocalBounds().reduced(120, 6).removeFromTop(24),
               juce::Justification::centredLeft);
}

} // namespace SoundShop
