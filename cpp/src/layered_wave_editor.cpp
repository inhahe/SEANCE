#define _USE_MATH_DEFINES
#include "layered_wave_editor.h"
#include "help_utils.h"
#include <cmath>
#include <sstream>
#include <algorithm>
#include <random>

namespace SoundShop {

// ==============================================================================
// LayeredWaveform — data model
// ==============================================================================

// Seed a fresh Drawn layer with a handful of points arranged like a sine so
// the user has something visible to grab and move.
static std::vector<std::pair<float, float>> defaultDrawnPoints() {
    return {
        {0.00f,  0.0f},
        {0.25f,  1.0f},
        {0.50f,  0.0f},
        {0.75f, -1.0f}
    };
}

static float evalShape(WaveLayer::Shape s, float x /*phase 0..1*/, std::mt19937& rng) {
    const float TWOPI = 2.0f * (float)M_PI;
    switch (s) {
        case WaveLayer::Sine:     return std::sin(TWOPI * x);
        case WaveLayer::Saw:      return 2.0f * (x - std::floor(x + 0.5f));
        case WaveLayer::Square:   return (x - std::floor(x) < 0.5f) ? 1.0f : -1.0f;
        case WaveLayer::Triangle: {
            float t = x - std::floor(x);
            return 4.0f * std::abs(t - 0.5f) - 1.0f;
        }
        case WaveLayer::Noise: {
            std::uniform_real_distribution<float> d(-1.0f, 1.0f);
            return d(rng);
        }
        case WaveLayer::Drawn:    return 0.0f; // handled by sampleLayer below
    }
    return 0.0f;
}

// Catmull-Rom interpolation through a periodic sequence of (x, y) points.
// `pts` must be sorted by x. x values in [0, 1); we treat the list as
// cyclic, so p[-1] == p[n-1] and p[n] == p[0] (x-shifted by ±1 accordingly).
static float sampleDrawnPoints(const std::vector<std::pair<float, float>>& pts,
                               float x)
{
    int n = (int)pts.size();
    if (n == 0) return 0.0f;
    if (n == 1) return pts[0].second;

    // Wrap x into [0, 1)
    x = x - std::floor(x);

    // Find the segment [p1, p2] such that p1.x <= x < p2.x (handling wrap).
    int i1 = -1;
    for (int i = 0; i < n; ++i) {
        float a = pts[i].first;
        float b = (i + 1 < n) ? pts[i + 1].first : pts[0].first + 1.0f;
        float xx = x;
        if (a > b) { // shouldn't happen if sorted, but just in case
            if (xx < a) xx += 1.0f;
        }
        if (xx >= a && xx < b) { i1 = i; break; }
    }
    if (i1 < 0) i1 = n - 1;
    int i0 = (i1 - 1 + n) % n;
    int i2 = (i1 + 1) % n;
    int i3 = (i1 + 2) % n;

    float x1 = pts[i1].first;
    float x2 = (i1 + 1 < n) ? pts[i2].first : pts[i2].first + 1.0f;
    float xx = x;
    if (xx < x1) xx += 1.0f;
    float t = (x2 - x1 > 1e-6f) ? (xx - x1) / (x2 - x1) : 0.0f;
    t = juce::jlimit(0.0f, 1.0f, t);

    float y0 = pts[i0].second, y1 = pts[i1].second;
    float y2 = pts[i2].second, y3 = pts[i3].second;

    // Catmull-Rom
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * y1)
                 + (-y0 + y2) * t
                 + (2.0f*y0 - 5.0f*y1 + 4.0f*y2 - y3) * t2
                 + (-y0 + 3.0f*y1 - 3.0f*y2 + y3) * t3);
}

static float sampleLayer(const WaveLayer& layer, float x, std::mt19937& rng) {
    if (layer.shape == WaveLayer::Drawn)
        return sampleDrawnPoints(layer.drawnPoints, x);
    return evalShape(layer.shape, x, rng);
}

void LayeredWaveform::render(std::vector<float>& out) const {
    out.assign(tableSize, 0.0f);
    if (layers.empty()) return;

    for (const auto& layer : layers) {
        // Use a layer-specific deterministic seed so noise layers are stable
        // across renders (not changing on every edit).
        std::mt19937 rng(1234u + (unsigned)layer.ratio * 31u + (unsigned)layer.shape * 7u);
        int r = std::max(1, layer.ratio);
        for (int i = 0; i < tableSize; ++i) {
            float phase = (float)i / (float)tableSize;    // 0..1 over base period
            float x = phase * (float)r + layer.phase;     // ratio + phase offset
            out[i] += layer.amp * sampleLayer(layer, x, rng);
        }
    }

    // Normalize to peak 1.0
    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : out) v *= inv;
    }
}

static const char* shapeName(WaveLayer::Shape s) {
    switch (s) {
        case WaveLayer::Sine:     return "sine";
        case WaveLayer::Saw:      return "saw";
        case WaveLayer::Square:   return "square";
        case WaveLayer::Triangle: return "triangle";
        case WaveLayer::Noise:    return "noise";
        case WaveLayer::Drawn:    return "drawn";
    }
    return "sine";
}

static WaveLayer::Shape parseShape(const std::string& s) {
    if (s == "saw")      return WaveLayer::Saw;
    if (s == "square")   return WaveLayer::Square;
    if (s == "triangle") return WaveLayer::Triangle;
    if (s == "noise")    return WaveLayer::Noise;
    if (s == "drawn")    return WaveLayer::Drawn;
    return WaveLayer::Sine;
}

// Emit one layer as a comma-separated field list (no trailing `|`).
static void encodeLayer(std::ostringstream& o, const WaveLayer& l) {
    o << shapeName(l.shape)
      << "," << l.ratio
      << "," << l.phase
      << "," << l.amp;
    if (l.shape == WaveLayer::Drawn) {
        o << "," << l.drawnPoints.size();
        for (auto& p : l.drawnPoints)
            o << "," << p.first << "," << p.second;
    }
}

// Parse a single layer from a comma-separated string. Returns true on success.
static bool parseLayer(const std::string& lp, WaveLayer& out) {
    std::vector<std::string> f;
    size_t p = 0;
    while (p <= lp.size()) {
        size_t n = lp.find(',', p);
        if (n == std::string::npos) n = lp.size();
        f.push_back(lp.substr(p, n - p));
        p = n + 1;
    }
    if (f.size() < 4) return false;
    out = WaveLayer{};
    out.shape = parseShape(f[0]);
    try { out.ratio = std::stoi(f[1]); } catch (...) { out.ratio = 1; }
    try { out.phase = std::stof(f[2]); } catch (...) { out.phase = 0.0f; }
    try { out.amp   = std::stof(f[3]); } catch (...) { out.amp   = 1.0f; }
    if (out.shape == WaveLayer::Drawn && f.size() > 4) {
        int count = 0;
        try { count = std::stoi(f[4]); } catch (...) { count = 0; }
        for (int k = 0; k < count; ++k) {
            size_t xi = 5 + (size_t)k * 2;
            size_t yi = xi + 1;
            if (yi >= f.size()) break;
            float x = 0, y = 0;
            try { x = std::stof(f[xi]); } catch (...) {}
            try { y = std::stof(f[yi]); } catch (...) {}
            out.drawnPoints.emplace_back(x, y);
        }
    }
    return true;
}

std::string LayeredWaveform::encode() const {
    std::ostringstream o;
    o << "__layered__:" << tableSize;
    for (const auto& l : layers) {
        o << "|";
        encodeLayer(o, l);
    }
    return o.str();
}

bool LayeredWaveform::decode(const std::string& s) {
    layers.clear();
    std::string body = s;
    if (body.rfind("__layered__:", 0) == 0) body = body.substr(12);
    if (body.empty()) return false;

    // split by '|'
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t next = body.find('|', pos);
        if (next == std::string::npos) next = body.size();
        parts.push_back(body.substr(pos, next - pos));
        pos = next + 1;
    }
    if (parts.empty()) return false;
    try {
        tableSize = std::stoi(parts[0]);
    } catch (...) { tableSize = 2048; }

    for (size_t i = 1; i < parts.size(); ++i) {
        WaveLayer layer;
        if (parseLayer(parts[i], layer))
            layers.push_back(layer);
    }
    return !layers.empty();
}

// ==============================================================================
// WavetableDoc
// ==============================================================================

// Format reference:
//   Grid: __wavetable__:<tableSize>:g;<numDims>;<dim0>;<dim1>;...:<frameCount>:<gridFrame0>:<gridFrame1>:...
//     where gridFrameN = layer1|layer2|...
//   Scatter: __wavetable__:<tableSize>:s;<scatterDims>;<radius>:<frameCount>:<scatterFrame0>:...
//     where scatterFrameN = pos0;pos1;...;posN@<label>@<layer1|layer2|...>
//   Legacy (no g/s prefix): treated as grid format with parts[1] containing dims OR a frame count.
std::string WavetableDoc::encode() const {
    std::ostringstream o;
    o << "__wavetable__:" << tableSize << ":";
    if (mode == WavetableMode::Grid) {
        o << "g;" << gridDims.size();
        for (int d : gridDims) o << ";" << d;
        o << ":" << frames.size();
        for (const auto& frame : frames) {
            o << ":";
            for (size_t i = 0; i < frame.layers.size(); ++i) {
                if (i > 0) o << "|";
                encodeLayer(o, frame.layers[i]);
            }
        }
    } else {
        o << "s;" << scatterDims << ";" << scatterRadius;
        o << ":" << scatterFrames.size();
        for (const auto& sf : scatterFrames) {
            o << ":";
            // positions
            for (int i = 0; i < (int)sf.position.size(); ++i) {
                if (i > 0) o << ";";
                o << sf.position[i];
            }
            o << "@" << sf.label << "@";
            for (size_t i = 0; i < sf.wave.layers.size(); ++i) {
                if (i > 0) o << "|";
                encodeLayer(o, sf.wave.layers[i]);
            }
        }
    }
    return o.str();
}

// Helper: parse the layer string `layer1|layer2|...` into a LayeredWaveform.
static LayeredWaveform parseLayerString(const std::string& fb, int tableSize) {
    LayeredWaveform lw;
    lw.tableSize = tableSize;
    size_t lp = 0;
    while (lp <= fb.size()) {
        size_t ln = fb.find('|', lp);
        if (ln == std::string::npos) ln = fb.size();
        std::string layerStr = fb.substr(lp, ln - lp);
        if (!layerStr.empty()) {
            WaveLayer layer;
            if (parseLayer(layerStr, layer))
                lw.layers.push_back(layer);
        }
        lp = ln + 1;
    }
    return lw;
}

bool WavetableDoc::decode(const std::string& s) {
    frames.clear();
    gridDims.clear();
    scatterFrames.clear();
    mode = WavetableMode::Grid;

    std::string body = s;
    if (body.rfind("__wavetable__:", 0) == 0)
        body = body.substr(14);
    else
        return false;

    // Split body on ':' → [tableSize, modeAndDims, frameCount, frame0, frame1, ...]
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t next = body.find(':', pos);
        if (next == std::string::npos) next = body.size();
        parts.push_back(body.substr(pos, next - pos));
        pos = next + 1;
    }
    if (parts.size() < 2) return false;
    try { tableSize = std::stoi(parts[0]); } catch (...) { tableSize = 2048; }

    // Detect mode: parts[1] starts with "g;" (grid v2), "s;" (scatter), or
    // is bare digits (grid v1 legacy with semicolons OR very-old single
    // frame count).
    int frameStartIdx = 2;
    int frameCount = 0;
    bool isScatter = false;

    std::string dimsField = parts[1];
    if (!dimsField.empty() && (dimsField[0] == 'g' || dimsField[0] == 's')) {
        isScatter = (dimsField[0] == 's');
        dimsField = dimsField.substr(1);
        if (!dimsField.empty() && dimsField[0] == ';') dimsField.erase(0, 1);
    }

    // Split dims field on ';'
    auto splitSemi = [](const std::string& src) {
        std::vector<std::string> out;
        size_t dp = 0;
        while (dp <= src.size()) {
            size_t dn = src.find(';', dp);
            if (dn == std::string::npos) dn = src.size();
            out.push_back(src.substr(dp, dn - dp));
            dp = dn + 1;
        }
        return out;
    };

    if (isScatter) {
        // Scatter: dimsField = "scatterDims;radius"
        auto dp = splitSemi(dimsField);
        mode = WavetableMode::Scatter;
        if (!dp.empty())     try { scatterDims   = std::stoi(dp[0]); } catch (...) { scatterDims = 2; }
        if (dp.size() > 1)   try { scatterRadius = std::stof(dp[1]); } catch (...) { scatterRadius = 0.45f; }
        if (parts.size() > 2) try { frameCount = std::stoi(parts[2]); } catch (...) {}
        frameStartIdx = 3;

        for (int f = 0; f < frameCount && (size_t)(frameStartIdx + f) < parts.size(); ++f) {
            const std::string& fb = parts[frameStartIdx + f];
            // Split on '@': posList @ label @ layers
            size_t a1 = fb.find('@');
            size_t a2 = (a1 != std::string::npos) ? fb.find('@', a1 + 1) : std::string::npos;
            ScatterFrame sf;
            std::string posStr = (a1 != std::string::npos) ? fb.substr(0, a1) : "";
            sf.label = (a1 != std::string::npos && a2 != std::string::npos)
                       ? fb.substr(a1 + 1, a2 - a1 - 1) : "";
            std::string layerStr = (a2 != std::string::npos) ? fb.substr(a2 + 1) : "";
            auto pp = splitSemi(posStr);
            for (auto& p : pp) {
                try { sf.position.push_back(std::stof(p)); } catch (...) { sf.position.push_back(0.5f); }
            }
            // Pad / truncate to scatterDims
            while ((int)sf.position.size() < scatterDims) sf.position.push_back(0.5f);
            if ((int)sf.position.size() > scatterDims) sf.position.resize(scatterDims);
            sf.wave = parseLayerString(layerStr, tableSize);
            scatterFrames.push_back(std::move(sf));
        }
        return !scatterFrames.empty();
    }

    // Grid path (v2 with explicit prefix, v1 with bare numDims;dims, or
    // very-old with bare frameCount).
    if (dimsField.find(';') != std::string::npos) {
        auto dp = splitSemi(dimsField);
        int numDims = 0;
        if (!dp.empty()) try { numDims = std::stoi(dp[0]); } catch (...) {}
        for (int d = 0; d < numDims && d + 1 < (int)dp.size(); ++d) {
            try { gridDims.push_back(std::stoi(dp[d + 1])); } catch (...) { gridDims.push_back(1); }
        }
        if (parts.size() > 2) try { frameCount = std::stoi(parts[2]); } catch (...) {}
        frameStartIdx = 3;
    } else {
        // Very old: parts[1] is just a frame count, default 1D
        try { frameCount = std::stoi(dimsField); } catch (...) {}
        gridDims = {frameCount};
        frameStartIdx = 2;
    }

    for (int f = 0; f < frameCount && (size_t)(frameStartIdx + f) < parts.size(); ++f) {
        frames.push_back(parseLayerString(parts[frameStartIdx + f], tableSize));
    }
    if (gridDims.empty()) gridDims = {(int)frames.size()};

    return !frames.empty();
}

LayeredWaveform* WavetableDoc::frameAt(int idx) {
    if (mode == WavetableMode::Grid) {
        if (idx < 0 || idx >= (int)frames.size()) return nullptr;
        return &frames[idx];
    } else {
        if (idx < 0 || idx >= (int)scatterFrames.size()) return nullptr;
        return &scatterFrames[idx].wave;
    }
}

const LayeredWaveform* WavetableDoc::frameAt(int idx) const {
    if (mode == WavetableMode::Grid) {
        if (idx < 0 || idx >= (int)frames.size()) return nullptr;
        return &frames[idx];
    } else {
        if (idx < 0 || idx >= (int)scatterFrames.size()) return nullptr;
        return &scatterFrames[idx].wave;
    }
}

WavetableDoc WavetableDoc::defaultSingleSine() {
    WavetableDoc d;
    d.tableSize = 2048;
    d.gridDims = {1}; // 1D with 1 frame
    d.frames.push_back(LayeredWaveform::defaultSine());
    return d;
}

LayeredWaveform LayeredWaveform::defaultSine() {
    LayeredWaveform w;
    WaveLayer l;
    l.shape = WaveLayer::Sine;
    l.ratio = 1;
    l.phase = 0.0f;
    l.amp   = 1.0f;
    w.layers.push_back(l);
    return w;
}

// ==============================================================================
// LayerRow — one row per layer
// ==============================================================================

// Sample one layer's contribution over one cycle (amp-scaled, not normalized).
// Used for the per-layer mini preview.
static void renderSingleLayer(const WaveLayer& layer, int tableSize,
                              std::vector<float>& out)
{
    out.assign(tableSize, 0.0f);
    std::mt19937 rng(1234u + (unsigned)layer.ratio * 31u + (unsigned)layer.shape * 7u);
    int r = std::max(1, layer.ratio);
    for (int i = 0; i < tableSize; ++i) {
        float phase = (float)i / (float)tableSize;
        float x = phase * (float)r + layer.phase;
        out[i] = layer.amp * sampleLayer(layer, x, rng);
    }
}

class LayeredWaveEditorComponent::LayerRow : public juce::Component {
public:
    LayerRow(LayeredWaveEditorComponent& owner_, int index_)
        : owner(owner_), index(index_)
    {
        addAndMakeVisible(label);
        label.setJustificationType(juce::Justification::centredLeft);
        label.setFont(13.0f);

        auto addShapeBtn = [this](juce::TextButton& b, const char* name, WaveLayer::Shape s) {
            addAndMakeVisible(b);
            b.setButtonText(name);
            b.setClickingTogglesState(true);
            b.setRadioGroupId(0); // we'll handle toggling manually
            b.onClick = [this, s]() {
                auto& l = owner.currentLayers()[index];
                l.shape = s;
                // Seed a fresh Drawn layer with a few points so the user has
                // something grabable instead of an empty canvas.
                if (s == WaveLayer::Drawn && l.drawnPoints.empty())
                    l.drawnPoints = defaultDrawnPoints();
                updateShapeButtons();
                refreshPreview();
                owner.onLayerChanged();
            };
        };
        addShapeBtn(sineBtn,     "Sine",     WaveLayer::Sine);
        addShapeBtn(sawBtn,      "Saw",      WaveLayer::Saw);
        addShapeBtn(squareBtn,   "Square",   WaveLayer::Square);
        addShapeBtn(triangleBtn, "Triangle", WaveLayer::Triangle);
        addShapeBtn(noiseBtn,    "Noise",    WaveLayer::Noise);
        addShapeBtn(drawnBtn,    "Draw",     WaveLayer::Drawn);

        auto setupSlider = [this](juce::Slider& sl, double lo, double hi, double step, const char* suffix) {
            addAndMakeVisible(sl);
            sl.setSliderStyle(juce::Slider::LinearHorizontal);
            sl.setTextBoxStyle(juce::Slider::TextBoxRight, false, 55, 18);
            sl.setRange(lo, hi, step);
            sl.setTextValueSuffix(suffix);
            sl.onValueChange = [this]() {
                auto& l = owner.currentLayers()[index];
                l.ratio = (int)ratioSlider.getValue();
                l.phase = (float)phaseSlider.getValue();
                l.amp   = (float)ampSlider.getValue();
                refreshPreview();
                owner.onLayerChanged();
            };
        };
        setupSlider(ratioSlider, 1.0, 16.0, 1.0, "x");
        setupSlider(phaseSlider, 0.0, 1.0, 0.01, "");
        setupSlider(ampSlider,   0.0, 1.0, 0.01, "");
        ratioSlider.setTooltip("Harmonic ratio: how many times faster this layer cycles than the fundamental. "
                               "1 = root pitch, 2 = one octave up, 3 = one octave + a fifth, etc. Higher numbers add brighter overtones.");
        phaseSlider.setTooltip("Phase offset (0 to 1): shifts where in its cycle this layer starts. "
                               "Affects how layers add up when summed — different phases give different timbres.");
        ampSlider.setTooltip("Amplitude (0 to 1): how loud this layer is in the final sum. 0 = silent, 1 = full volume. "
                             "Use to balance layers against each other.");

        addAndMakeVisible(ratioLabel);
        addAndMakeVisible(phaseLabel);
        addAndMakeVisible(ampLabel);
        ratioLabel.setText("Harmonic",  juce::dontSendNotification);
        phaseLabel.setText("Phase",     juce::dontSendNotification);
        ampLabel  .setText("Amplitude", juce::dontSendNotification);
        for (auto* l : { &ratioLabel, &phaseLabel, &ampLabel }) {
            l->setFont(11.0f);
            l->setJustificationType(juce::Justification::centredLeft);
        }

        addAndMakeVisible(deleteBtn);
        deleteBtn.setButtonText("X");
        deleteBtn.onClick = [this]() {
            owner.currentLayers().erase(owner.currentLayers().begin() + index);
            owner.rebuildRows();
            owner.onLayerChanged();
        };
    }

    void syncFromModel() {
        auto& l = owner.currentLayers()[index];
        label.setText("Layer " + juce::String(index + 1), juce::dontSendNotification);
        ratioSlider.setValue(l.ratio, juce::dontSendNotification);
        phaseSlider.setValue(l.phase, juce::dontSendNotification);
        ampSlider  .setValue(l.amp,   juce::dontSendNotification);
        updateShapeButtons();
        refreshPreview();
    }

    void refreshPreview() {
        if (index < 0 || index >= (int)owner.currentLayers().size()) return;
        renderSingleLayer(owner.currentLayers()[index], 512, previewSamples);
        repaint();
    }

    void updateShapeButtons() {
        auto shape = owner.currentLayers()[index].shape;
        sineBtn    .setToggleState(shape == WaveLayer::Sine,     juce::dontSendNotification);
        sawBtn     .setToggleState(shape == WaveLayer::Saw,      juce::dontSendNotification);
        squareBtn  .setToggleState(shape == WaveLayer::Square,   juce::dontSendNotification);
        triangleBtn.setToggleState(shape == WaveLayer::Triangle, juce::dontSendNotification);
        noiseBtn   .setToggleState(shape == WaveLayer::Noise,    juce::dontSendNotification);
        drawnBtn   .setToggleState(shape == WaveLayer::Drawn,    juce::dontSendNotification);
    }

    void resized() override {
        auto a = getLocalBounds().reduced(4);
        auto top = a.removeFromTop(22);
        label.setBounds(top.removeFromLeft(70));
        deleteBtn.setBounds(top.removeFromRight(22));

        // Shape button row
        auto btnRow = a.removeFromTop(24);
        int bw = btnRow.getWidth() / 6;
        sineBtn    .setBounds(btnRow.removeFromLeft(bw));
        sawBtn     .setBounds(btnRow.removeFromLeft(bw));
        squareBtn  .setBounds(btnRow.removeFromLeft(bw));
        triangleBtn.setBounds(btnRow.removeFromLeft(bw));
        noiseBtn   .setBounds(btnRow.removeFromLeft(bw));
        drawnBtn   .setBounds(btnRow);

        // Reserve space for the mini preview (bottom of row)
        a.removeFromBottom(previewHeight);

        // Slider rows
        auto sliderRow = [&](juce::Label& lab, juce::Slider& sl) {
            auto r = a.removeFromTop(20);
            lab.setBounds(r.removeFromLeft(70));
            sl.setBounds(r);
        };
        sliderRow(ratioLabel, ratioSlider);
        sliderRow(phaseLabel, phaseSlider);
        sliderRow(ampLabel,   ampSlider);
    }

    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colour(40, 40, 50));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        g.setColour(juce::Colour(70, 70, 90));
        g.drawRoundedRectangle(getLocalBounds().toFloat(), 4.0f, 1.0f);

        // Mini waveform preview for just this layer's contribution
        auto bounds = getLocalBounds().reduced(6).toFloat();
        auto previewArea = bounds.removeFromBottom((float)previewHeight).reduced(2.0f);

        g.setColour(juce::Colour(24, 24, 30));
        g.fillRoundedRectangle(previewArea, 3.0f);

        // Center line
        float cy = previewArea.getCentreY();
        g.setColour(juce::Colours::grey.withAlpha(0.25f));
        g.drawHorizontalLine((int)cy, previewArea.getX(), previewArea.getRight());

        if (!previewSamples.empty()) {
            float cx = previewArea.getX() + 2;
            float w  = previewArea.getWidth() - 4;
            float h  = previewArea.getHeight() - 4;

            juce::Path p;
            int n = (int)previewSamples.size();
            for (int i = 0; i < n; ++i) {
                float x = cx + (float)i / (float)(n - 1) * w;
                float y = cy - previewSamples[i] * h * 0.45f;
                if (i == 0) p.startNewSubPath(x, y);
                else p.lineTo(x, y);
            }
            g.setColour(juce::Colour(150, 200, 255));
            g.strokePath(p, juce::PathStrokeType(1.3f));

            // For Drawn layers, overlay the control points so the user can
            // see and grab them.
            const auto& layer = owner.currentLayers()[index];
            if (layer.shape == WaveLayer::Drawn) {
                for (int i = 0; i < (int)layer.drawnPoints.size(); ++i) {
                    const auto& pt = layer.drawnPoints[i];
                    // Scale y by amp because the preview renders amp*shape,
                    // so the visible curve is also amp-scaled.
                    float x = cx + pt.first * w;
                    float y = cy - (pt.second * layer.amp) * h * 0.45f;
                    bool isDragged = (i == draggingIdx);
                    g.setColour(isDragged ? juce::Colours::yellow : juce::Colours::white);
                    g.fillEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f);
                    g.setColour(juce::Colour(60, 90, 140));
                    g.drawEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f, 1.0f);
                }
            }
        }
    }

    static constexpr int previewHeight = 92;
    static int rowHeight() { return 22 + 24 + 20 * 3 + 12 + previewHeight + 4; }

    juce::Rectangle<float> getPreviewAreaBounds() const {
        auto bounds = getLocalBounds().reduced(6).toFloat();
        return bounds.removeFromBottom((float)previewHeight).reduced(2.0f);
    }

    // Convert a mouse position in component coordinates to (x, y) in the
    // normalized space used by drawnPoints: x in [0, 1), y in [-1, 1].
    // Returns true if p is inside the preview area.
    bool mouseToPointXY(juce::Point<float> p, float& outX, float& outY) const {
        auto area = getPreviewAreaBounds();
        if (!area.contains(p)) return false;
        outX = (p.x - area.getX()) / juce::jmax(1.0f, area.getWidth());
        outY = 1.0f - 2.0f * (p.y - area.getY()) / juce::jmax(1.0f, area.getHeight());
        outX = juce::jlimit(0.0f, 0.999f, outX);
        outY = juce::jlimit(-1.0f, 1.0f, outY);
        return true;
    }

    // Find the closest point to (x, y), returning its index, or -1 if none
    // is within `radius` (in normalized coordinates, where x spans 1 unit
    // and y spans 2 units).
    int findPointNear(float x, float y, float radius = 0.05f) const {
        auto& pts = owner.currentLayers()[index].drawnPoints;
        int best = -1;
        float bestD2 = radius * radius;
        for (int i = 0; i < (int)pts.size(); ++i) {
            float dx = pts[i].first - x;
            float dy = (pts[i].second - y) * 0.5f; // compress y to match x scale
            float d2 = dx * dx + dy * dy;
            if (d2 < bestD2) { bestD2 = d2; best = i; }
        }
        return best;
    }

    void sortPointsByX() {
        auto& pts = owner.currentLayers()[index].drawnPoints;
        std::sort(pts.begin(), pts.end(),
                  [](const std::pair<float,float>& a, const std::pair<float,float>& b) {
                      return a.first < b.first;
                  });
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (owner.currentLayers()[index].shape != WaveLayer::Drawn) return;
        float x, y;
        if (!mouseToPointXY(e.position, x, y)) return;
        auto& pts = owner.currentLayers()[index].drawnPoints;
        int hit = findPointNear(x, y);
        if (e.mods.isShiftDown() && hit >= 0) {
            // Shift-click a point to delete it (keep at least 2 points so
            // interpolation has something to work with).
            if ((int)pts.size() > 2) {
                pts.erase(pts.begin() + hit);
                draggingIdx = -1;
                refreshPreview();
                owner.onLayerChanged();
            }
            return;
        }
        if (hit >= 0) {
            draggingIdx = hit;
        } else {
            // Add a new point at the cursor, then sort by x so interpolation stays valid.
            pts.emplace_back(x, y);
            sortPointsByX();
            // After sorting, re-find the point we just added so we can continue
            // dragging it.
            draggingIdx = -1;
            for (int i = 0; i < (int)pts.size(); ++i)
                if (std::abs(pts[i].first - x) < 1e-5f && std::abs(pts[i].second - y) < 1e-5f)
                    { draggingIdx = i; break; }
            refreshPreview();
            owner.onLayerChanged();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (owner.currentLayers()[index].shape != WaveLayer::Drawn) return;
        if (draggingIdx < 0) return;
        auto& pts = owner.currentLayers()[index].drawnPoints;
        if (draggingIdx >= (int)pts.size()) { draggingIdx = -1; return; }
        float x, y;
        // Use clamped conversion so dragging outside the area still moves the point.
        auto area = getPreviewAreaBounds();
        auto cp = e.position;
        cp.x = juce::jlimit(area.getX(), area.getRight() - 1.0f, cp.x);
        cp.y = juce::jlimit(area.getY(), area.getBottom(), cp.y);
        mouseToPointXY(cp, x, y);
        pts[draggingIdx] = { x, y };
        // Re-sort after movement since x may have changed order.
        // Remember old position so we can re-find after sort.
        float ox = x, oy = y;
        sortPointsByX();
        draggingIdx = -1;
        for (int i = 0; i < (int)pts.size(); ++i)
            if (std::abs(pts[i].first - ox) < 1e-5f && std::abs(pts[i].second - oy) < 1e-5f)
                { draggingIdx = i; break; }
        refreshPreview();
        owner.onLayerChanged();
    }

    void mouseUp(const juce::MouseEvent&) override { draggingIdx = -1; }

private:
    LayeredWaveEditorComponent& owner;
    int index;

    juce::Label label;
    juce::TextButton sineBtn, sawBtn, squareBtn, triangleBtn, noiseBtn, drawnBtn;
    juce::Slider ratioSlider, phaseSlider, ampSlider;
    juce::Label  ratioLabel, phaseLabel, ampLabel;
    juce::TextButton deleteBtn;
    std::vector<float> previewSamples;
    int draggingIdx = -1;
};

// ==============================================================================
// ScatterView — N-D scatter wavetable viewport
// ==============================================================================
//
// Renders frames as labeled colored dots projected to 2D, with the current
// Position cursor as a crosshair. The user can:
//   - Click empty space to add a frame at that point
//   - Click a frame dot to select it (so its layers become editable below)
//   - Drag a frame dot to move it
//   - Drag the cursor (or click empty space with a modifier) to move Position
//   - Pick which axis pair (or triple, in 3D) to view via the parent's
//     projection combo
//   - Toggle 3D anaglyph mode (red/cyan stereo) via the parent's 3D button
//
// In 3D anaglyph mode each dot is rendered twice — once with horizontal eye
// offset to the left in the red channel, once to the right in the cyan
// channel — so red/cyan glasses give true stereo depth.
class LayeredWaveEditorComponent::ScatterView : public juce::Component {
public:
    explicit ScatterView(LayeredWaveEditorComponent& o) : owner(o) {}

    // Which axes are projected to screen X / Y / Z (Z only used in 3D mode).
    int axisX = 0, axisY = 1, axisZ = 2;
    bool anaglyph3D = false;
    float yawDeg = 25.0f, pitchDeg = 20.0f; // 3D camera orbit (when anaglyph3D)

    // Real-world stereo geometry. The parent editor pushes these in from
    // its sliders so the parallax matches the user's actual eyes / screen.
    float ipdMm = 63.0f;          // interpupillary distance
    float viewingDistMm = 600.0f; // distance from eyes to screen
    float sceneDepthMm  = 60.0f;  // physical depth of the unit cube
    float dpi = 96.0f;            // screen DPI (auto-detected)

    void setProjection(int ax, int ay, int az) { axisX = ax; axisY = ay; axisZ = az; repaint(); }

    // A monoscopic projection (one camera, no parallax) plus the
    // post-rotation depth so applyParallax can compute disparity.
    struct Projected { float x, y; float worldZ; };

    // Project an N-D position to a 2D screen point — without stereo offset.
    // In 3D mode this includes yaw/pitch rotation and perspective scaling
    // (size only — both eyes get the same shape).
    Projected projectPoint(const std::vector<float>& pos,
                           const juce::Rectangle<float>& area) const
    {
        int nd = (int)pos.size();
        float px = (axisX < nd) ? pos[axisX] : 0.5f;
        float py = (axisY < nd) ? pos[axisY] : 0.5f;
        float pz = (anaglyph3D && axisZ < nd) ? pos[axisZ] : 0.5f;

        float x = px - 0.5f, y = py - 0.5f, z = pz - 0.5f;

        if (anaglyph3D) {
            float yaw   = juce::degreesToRadians(yawDeg);
            float pitch = juce::degreesToRadians(pitchDeg);
            float cy = std::cos(yaw),   sy = std::sin(yaw);
            float cp = std::cos(pitch), sp = std::sin(pitch);
            // Rotate around Y (yaw) then X (pitch)
            float x1 =  cy * x + sy * z;
            float z1 = -sy * x + cy * z;
            float y2 =  cp * y - sp * z1;
            float z2 =  sp * y + cp * z1;
            x = x1; y = y2; z = z2;

            // Same perspective (size scaling) for both eyes — parallax is
            // applied separately so the disparity is purely horizontal.
            float fov = 1.6f;
            float pers = fov / std::max(0.1f, fov - z);
            x *= pers;
            y *= pers;
        }

        float cx = area.getCentreX();
        float cyc = area.getCentreY();
        float scale = std::min(area.getWidth(), area.getHeight()) * 0.42f;
        return { cx + x * scale, cyc + y * scale, z };
    }

    // Compute the stereoscopic disparity in screen pixels for a point at
    // post-rotation depth `worldZ` (in [-0.5..+0.5] world units).
    //
    // Geometry: place the screen plane at z=0, viewer's eyes at -D (mm in
    // front), eye separation = IPD. A point at depth z_mm behind the screen
    // appears at parallax (IPD/2) * z_mm/(D+z_mm) per eye (positive = right
    // eye image moves right, left eye image moves left → uncrossed disparity
    // → object fuses behind the screen). For a point in front (z_mm<0) the
    // formula naturally yields crossed disparity.
    juce::Point<float> applyParallax(const Projected& pp, int eyeSign) const {
        if (!anaglyph3D || eyeSign == 0) return { pp.x, pp.y };
        float z_mm = pp.worldZ * sceneDepthMm; // post-rotation z mapped to physical mm
        float denom = viewingDistMm + z_mm;
        if (std::abs(denom) < 1e-3f) return { pp.x, pp.y };
        float disparity_mm = (ipdMm * 0.5f) * z_mm / denom;
        float disparity_px = disparity_mm * dpi / 25.4f;
        return { pp.x + (float)eyeSign * disparity_px, pp.y };
    }

    // Backward-compat wrapper used by drawSquare2D / drawCube3D etc.
    juce::Point<float> projectToScreen(const std::vector<float>& pos,
                                       const juce::Rectangle<float>& area) const
    {
        auto p = projectPoint(pos, area);
        return { p.x, p.y };
    }

    static juce::Colour autoColorForFrame(const ScatterFrame& sf, int idx) {
        if (sf.colorIdx >= 0) {
            // user-picked palette index
            const juce::Colour palette[] = {
                juce::Colour(0xff5fb3ff), juce::Colour(0xffff7373),
                juce::Colour(0xff8aff80), juce::Colour(0xffffd24c),
                juce::Colour(0xffd084ff), juce::Colour(0xff6effe0),
                juce::Colour(0xffff9ad1), juce::Colour(0xffffb86b),
            };
            return palette[sf.colorIdx % 8];
        }
        // Spectral centroid → hue (low = warm red, high = cool blue)
        float centroid = 0.0f, mass = 0.0f;
        for (auto& l : sf.wave.layers) {
            float w = std::abs(l.amp);
            centroid += (float)std::max(1, l.ratio) * w;
            mass += w;
        }
        if (mass < 1e-9f) centroid = 1.0f; else centroid /= mass;
        // Map centroid 1..16 → hue 0.05 (warm orange) .. 0.66 (blue)
        float t = juce::jlimit(0.0f, 1.0f, (centroid - 1.0f) / 15.0f);
        float hue = 0.05f + t * 0.61f;
        return juce::Colour::fromHSV(hue, 0.65f, 0.95f, 1.0f);
    }

    void paint(juce::Graphics& g) override {
        auto area = getLocalBounds().toFloat().reduced(2.0f);
        g.setColour(juce::Colour(18, 18, 24));
        g.fillRoundedRectangle(area, 4.0f);
        g.setColour(juce::Colour(60, 60, 78));
        g.drawRoundedRectangle(area, 4.0f, 1.0f);

        // Grid lines / unit cube edges
        if (anaglyph3D) drawCube3D(g, area);
        else            drawSquare2D(g, area);

        // Axis labels
        g.setColour(juce::Colours::grey);
        g.setFont(11.0f);
        auto axName = [](int i) {
            const char* n[] = { "X", "Y", "Z", "W", "V", "U", "T", "S" };
            return juce::String((i >= 0 && i < 8) ? n[i] : "?");
        };
        if (anaglyph3D) {
            g.drawText("axes: " + axName(axisX) + " x " + axName(axisY) + " x " + axName(axisZ),
                       area.reduced(6, 4).toNearestInt(), juce::Justification::topLeft);
            g.drawText("(red-cyan glasses)",
                       area.reduced(6, 4).toNearestInt(), juce::Justification::topRight);
        } else {
            g.drawText("axes: " + axName(axisX) + " x " + axName(axisY),
                       area.reduced(6, 4).toNearestInt(), juce::Justification::topLeft);
        }

        if (owner.wave.mode != WavetableMode::Scatter) return;

        // Draw frames
        auto& frames = owner.wave.scatterFrames;
        for (int i = 0; i < (int)frames.size(); ++i) {
            auto base = autoColorForFrame(frames[i], i);
            bool sel = (i == owner.currentFrameIdx);
            auto pp = projectPoint(frames[i].position, area);

            if (anaglyph3D) {
                auto pL = applyParallax(pp, -1);
                auto pR = applyParallax(pp, +1);
                float r = sel ? 7.0f : 5.0f;
                // Red channel for left eye
                g.setColour(juce::Colour::fromRGBA(255, 0, 0, sel ? 230 : 180));
                g.fillEllipse(pL.x - r, pL.y - r, 2*r, 2*r);
                // Cyan channel for right eye
                g.setColour(juce::Colour::fromRGBA(0, 255, 255, sel ? 230 : 180));
                g.fillEllipse(pR.x - r, pR.y - r, 2*r, 2*r);

                if (sel) {
                    g.setColour(juce::Colours::white.withAlpha(0.7f));
                    g.drawEllipse(pL.x - r - 2, pL.y - r - 2, 2*r + 4, 2*r + 4, 1.2f);
                    g.drawEllipse(pR.x - r - 2, pR.y - r - 2, 2*r + 4, 2*r + 4, 1.2f);
                }
                if (!frames[i].label.empty() || sel) {
                    g.setColour(juce::Colours::white.withAlpha(0.85f));
                    g.setFont(10.0f);
                    juce::String txt = frames[i].label.empty()
                                       ? juce::String("#" + juce::String(i + 1))
                                       : juce::String(frames[i].label);
                    auto mid = (pL + pR) * 0.5f;
                    g.drawText(txt, (int)mid.x - 30, (int)mid.y + (int)r + 2,
                               60, 12, juce::Justification::centred);
                }
            } else {
                float r = sel ? 8.0f : 6.0f;
                g.setColour(base.withAlpha(sel ? 1.0f : 0.85f));
                g.fillEllipse(pp.x - r, pp.y - r, 2*r, 2*r);
                g.setColour(juce::Colours::white.withAlpha(sel ? 1.0f : 0.5f));
                g.drawEllipse(pp.x - r, pp.y - r, 2*r, 2*r, sel ? 1.6f : 1.0f);

                g.setColour(juce::Colours::white.withAlpha(0.9f));
                g.setFont(10.5f);
                juce::String txt = frames[i].label.empty()
                                   ? juce::String("#" + juce::String(i + 1))
                                   : juce::String(frames[i].label);
                g.drawText(txt, (int)pp.x - 40, (int)pp.y + (int)r + 1,
                           80, 12, juce::Justification::centred);
            }
        }

        // Position cursor
        if ((int)owner.currentPosition.size() >= owner.wave.scatterDims) {
            auto pp = projectPoint(owner.currentPosition, area);
            if (anaglyph3D) {
                auto pL = applyParallax(pp, -1);
                auto pR = applyParallax(pp, +1);
                g.setColour(juce::Colour::fromRGBA(255, 0, 0, 230));
                drawCrosshair(g, pL, 9.0f);
                g.setColour(juce::Colour::fromRGBA(0, 255, 255, 230));
                drawCrosshair(g, pR, 9.0f);
            } else {
                g.setColour(juce::Colours::yellow);
                drawCrosshair(g, { pp.x, pp.y }, 10.0f);
            }
        }
    }

    static void drawCrosshair(juce::Graphics& g, juce::Point<float> p, float r) {
        g.drawEllipse(p.x - r, p.y - r, 2*r, 2*r, 1.5f);
        g.drawLine(p.x - r - 3, p.y, p.x + r + 3, p.y, 1.0f);
        g.drawLine(p.x, p.y - r - 3, p.x, p.y + r + 3, 1.0f);
    }

    void drawSquare2D(juce::Graphics& g, juce::Rectangle<float> area) {
        // unit-square bounds (axis range [0,1])
        std::vector<float> p00 = {0,0}; std::vector<float> p10 = {1,0};
        std::vector<float> p11 = {1,1}; std::vector<float> p01 = {0,1};
        // pad with 0.5 for unused dims
        int nd = std::max(2, owner.wave.scatterDims);
        for (int i = 2; i < nd; ++i) {
            p00.push_back(0.5f); p10.push_back(0.5f);
            p11.push_back(0.5f); p01.push_back(0.5f);
        }
        g.setColour(juce::Colour(80, 80, 100));
        auto a = projectToScreen(p00, area);
        auto b = projectToScreen(p10, area);
        auto c = projectToScreen(p11, area);
        auto d = projectToScreen(p01, area);
        g.drawLine(a.x, a.y, b.x, b.y, 1.0f);
        g.drawLine(b.x, b.y, c.x, c.y, 1.0f);
        g.drawLine(c.x, c.y, d.x, d.y, 1.0f);
        g.drawLine(d.x, d.y, a.x, a.y, 1.0f);
    }

    void drawCube3D(juce::Graphics& g, juce::Rectangle<float> area) {
        int nd = std::max(3, owner.wave.scatterDims);
        auto pad = [&](std::vector<float>&& v) {
            while ((int)v.size() < nd) v.push_back(0.5f);
            return v;
        };
        std::vector<std::vector<float>> verts = {
            pad({0,0,0}), pad({1,0,0}), pad({1,1,0}), pad({0,1,0}),
            pad({0,0,1}), pad({1,0,1}), pad({1,1,1}), pad({0,1,1}),
        };
        std::vector<juce::Point<float>> sp(8);
        for (int i = 0; i < 8; ++i) sp[i] = projectToScreen(verts[i], area);
        int edges[12][2] = {
            {0,1},{1,2},{2,3},{3,0},
            {4,5},{5,6},{6,7},{7,4},
            {0,4},{1,5},{2,6},{3,7}
        };
        g.setColour(juce::Colour(70, 70, 90));
        for (auto& e : edges)
            g.drawLine(sp[e[0]].x, sp[e[0]].y, sp[e[1]].x, sp[e[1]].y, 1.0f);
    }

    // ---- Mouse interaction ----
    int dragFrameIdx = -1;
    bool dragCursor = false;

    int hitTestFrame(juce::Point<float> p, juce::Rectangle<float> area) const {
        if (owner.wave.mode != WavetableMode::Scatter) return -1;
        const auto& frames = owner.wave.scatterFrames;
        int best = -1; float bestD2 = 14.0f * 14.0f;
        for (int i = 0; i < (int)frames.size(); ++i) {
            auto sp = projectToScreen(frames[i].position, area);
            float dx = sp.x - p.x, dy = sp.y - p.y;
            float d2 = dx*dx + dy*dy;
            if (d2 < bestD2) { bestD2 = d2; best = i; }
        }
        return best;
    }

    // Inverse projection: convert a screen point back into the projected
    // axes (axisX, axisY) of the N-D position. Other axes are left at the
    // current value of `current`. Only meaningful in 2D mode (anaglyph 3D
    // can't be unprojected unambiguously, so dragging is locked to the 2D
    // projected axes there too — Z stays at the current cursor value).
    std::vector<float> screenToPosition(juce::Point<float> p,
                                        juce::Rectangle<float> area,
                                        const std::vector<float>& current) const
    {
        std::vector<float> out = current;
        while ((int)out.size() < owner.wave.scatterDims) out.push_back(0.5f);
        // Undo the centring + scale used in projectToScreen for the 2D path.
        // (For 3D drag we use the same 2D mapping — close enough.)
        float cx = area.getCentreX();
        float cyc = area.getCentreY();
        float scale = std::min(area.getWidth(), area.getHeight()) * 0.42f;
        if (scale < 1e-3f) return out;
        float nx = (p.x - cx) / scale + 0.5f;
        float ny = (p.y - cyc) / scale + 0.5f;
        nx = juce::jlimit(0.0f, 1.0f, nx);
        ny = juce::jlimit(0.0f, 1.0f, ny);
        if (axisX >= 0 && axisX < (int)out.size()) out[axisX] = nx;
        if (axisY >= 0 && axisY < (int)out.size()) out[axisY] = ny;
        return out;
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (owner.wave.mode != WavetableMode::Scatter) return;
        auto area = getLocalBounds().toFloat().reduced(2.0f);
        int hit = hitTestFrame(e.position, area);

        if (e.mods.isShiftDown() && hit >= 0) {
            // Shift-click frame to delete (keep at least one).
            if ((int)owner.wave.scatterFrames.size() > 1) {
                owner.wave.scatterFrames.erase(owner.wave.scatterFrames.begin() + hit);
                if (owner.currentFrameIdx >= (int)owner.wave.scatterFrames.size())
                    owner.currentFrameIdx = (int)owner.wave.scatterFrames.size() - 1;
                owner.rebuildRows();
                owner.onLayerChanged();
                repaint();
            }
            return;
        }

        if (hit >= 0) {
            // Select + start drag
            owner.currentFrameIdx = hit;
            owner.rebuildRows();
            owner.refreshPreview();
            owner.syncCoordRowFromSelection();
            dragFrameIdx = hit;
            dragCursor = false;
            repaint();
            return;
        }

        if (e.mods.isAltDown()) {
            // Alt-click empty space: move Position cursor here
            owner.currentPosition = screenToPosition(e.position, area, owner.currentPosition);
            dragCursor = true;
            owner.onLayerChanged();
            repaint();
            return;
        }

        // Click empty space: add a new frame at this projected location.
        // Other (non-projected) axes default to 0.5.
        ScatterFrame sf;
        sf.wave = LayeredWaveform::defaultSine();
        sf.position.assign(owner.wave.scatterDims, 0.5f);
        sf.position = screenToPosition(e.position, area, sf.position);
        owner.wave.scatterFrames.push_back(std::move(sf));
        owner.currentFrameIdx = (int)owner.wave.scatterFrames.size() - 1;
        owner.rebuildRows();
        owner.syncCoordRowFromSelection();
        owner.onLayerChanged();
        dragFrameIdx = owner.currentFrameIdx;
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (owner.wave.mode != WavetableMode::Scatter) return;
        auto area = getLocalBounds().toFloat().reduced(2.0f);
        if (dragFrameIdx >= 0 && dragFrameIdx < (int)owner.wave.scatterFrames.size()) {
            auto& sf = owner.wave.scatterFrames[dragFrameIdx];
            sf.position = screenToPosition(e.position, area, sf.position);
            owner.syncCoordRowFromSelection();
            owner.onLayerChanged();
            repaint();
        } else if (dragCursor) {
            owner.currentPosition = screenToPosition(e.position, area, owner.currentPosition);
            owner.onLayerChanged();
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent&) override {
        dragFrameIdx = -1;
        dragCursor = false;
    }

private:
    LayeredWaveEditorComponent& owner;
};

// ==============================================================================
// LayeredWaveEditorComponent
// ==============================================================================

LayeredWaveEditorComponent::LayeredWaveEditorComponent(NodeGraph& g, int nid, std::function<void()> apply)
    : graph(g), nodeId(nid), onApply(std::move(apply))
{
    // Decode existing state. Try wavetable first, then fall back to single
    // layered waveform (wrapped as a 1-frame wavetable), then default sine.
    auto* nd = graph.findNode(nodeId);
    std::string script = nd ? nd->script : "";
    if (!wave.decode(script)) {
        LayeredWaveform single;
        if (single.decode(script)) {
            wave.tableSize = single.tableSize;
            wave.frames.push_back(single);
        } else {
            wave = WavetableDoc::defaultSingleSine();
        }
    }
    if (wave.mode == WavetableMode::Grid && wave.frames.empty())
        wave.frames.push_back(LayeredWaveform::defaultSine());
    if (wave.mode == WavetableMode::Scatter && wave.scatterFrames.empty()) {
        // Seed with two frames so the user can immediately see the morph.
        ScatterFrame a; a.wave = LayeredWaveform::defaultSine();
        a.position.assign(wave.scatterDims, 0.5f);
        a.position[0] = 0.25f;
        ScatterFrame b; b.wave = LayeredWaveform::defaultSine();
        b.position.assign(wave.scatterDims, 0.5f);
        b.position[0] = 0.75f;
        wave.scatterFrames.push_back(std::move(a));
        wave.scatterFrames.push_back(std::move(b));
    }
    currentFrameIdx = 0;
    currentPosition.assign(std::max(1, wave.scatterDims), 0.5f);

    addAndMakeVisible(addLayerBtn);
    addLayerBtn.setTooltip("Add a new harmonic layer to the current waveform frame. "
                           "Each layer is a sine, saw, square, triangle, noise, or drawn shape "
                           "that gets summed into the final waveform.");
    addLayerBtn.onClick = [this]() {
        auto& layers = currentLayers();
        WaveLayer l;
        l.shape = WaveLayer::Sine;
        l.ratio = (int)layers.size() + 1; // each new layer defaults to next harmonic
        l.phase = 0.0f;
        l.amp = 0.5f;
        layers.push_back(l);
        rebuildRows();
        onLayerChanged();
    };

    addAndMakeVisible(addFrameBtn);
    addFrameBtn.setTooltip("Add a new frame to the wavetable. The synth crossfades between frames "
                           "as you sweep the Position parameter, letting you morph between different "
                           "waveform shapes during playback.");
    addFrameBtn.onClick = [this]() {
        // Duplicate the current frame so the user has a starting point.
        if (wave.mode == WavetableMode::Grid) {
            if (currentFrameIdx >= 0 && currentFrameIdx < (int)wave.frames.size())
                wave.frames.insert(wave.frames.begin() + currentFrameIdx + 1,
                                   wave.frames[currentFrameIdx]);
            else
                wave.frames.push_back(LayeredWaveform::defaultSine());
            currentFrameIdx = std::min((int)wave.frames.size() - 1, currentFrameIdx + 1);
            rebuildFrameTabs();
        } else {
            ScatterFrame sf;
            if (currentFrameIdx >= 0 && currentFrameIdx < (int)wave.scatterFrames.size()) {
                sf = wave.scatterFrames[currentFrameIdx];
                // Offset slightly so the new dot doesn't sit exactly on top.
                for (auto& v : sf.position) v = juce::jlimit(0.0f, 1.0f, v + 0.05f);
            } else {
                sf.wave = LayeredWaveform::defaultSine();
                sf.position.assign(wave.scatterDims, 0.5f);
            }
            wave.scatterFrames.push_back(std::move(sf));
            currentFrameIdx = (int)wave.scatterFrames.size() - 1;
            if (scatterView) scatterView->repaint();
        }
        rebuildRows();
        onLayerChanged();
    };

    // Mode toggle: switch between Grid and Scatter authoring.
    addAndMakeVisible(modeToggleBtn);
    modeToggleBtn.setTooltip("Toggle between Grid mode (frames laid out on a regular N-D grid) and "
                             "Scatter mode (frames placed at arbitrary N-D positions, blended via radial basis functions)");
    modeToggleBtn.onClick = [this]() { toggleMode(); };

    // Scatter dimensions: + / - to add/remove a Position axis.
    addAndMakeVisible(addDimBtn);
    addDimBtn.setTooltip("Add a new Position axis (dimension) to the wavetable. Each axis adds a "
                         "Position knob on the synth node that morphs through the frames along that axis.");
    addDimBtn.onClick = [this]() {
        if (wave.mode == WavetableMode::Scatter) {
            if (wave.scatterDims < 8) wave.scatterDims++;
            for (auto& sf : wave.scatterFrames)
                while ((int)sf.position.size() < wave.scatterDims)
                    sf.position.push_back(0.5f);
            while ((int)currentPosition.size() < wave.scatterDims)
                currentPosition.push_back(0.5f);
        } else {
            // Grid mode: add a new dimension of size 1
            if ((int)wave.gridDims.size() < 8) wave.gridDims.push_back(1);
        }
        rebuildScatterUI();
        syncPositionParams();
        onLayerChanged();
    };
    addAndMakeVisible(removeDimBtn);
    removeDimBtn.setTooltip("Remove the last Position axis from the wavetable");
    removeDimBtn.onClick = [this]() {
        if (wave.mode == WavetableMode::Scatter) {
            if (wave.scatterDims > 1) {
                wave.scatterDims--;
                for (auto& sf : wave.scatterFrames)
                    if ((int)sf.position.size() > wave.scatterDims)
                        sf.position.resize(wave.scatterDims);
                if ((int)currentPosition.size() > wave.scatterDims)
                    currentPosition.resize(wave.scatterDims);
            }
        } else {
            if (wave.gridDims.size() > 1) wave.gridDims.pop_back();
        }
        rebuildScatterUI();
        syncPositionParams();
        onLayerChanged();
    };

    addAndMakeVisible(projectionCombo);
    projectionCombo.setTooltip("Choose which axes are shown in the 2D/3D viewer when the wavetable has more than 2-3 dimensions. "
                                "Other axes get fixed to a single value (controlled by the per-axis sliders below).");
    projectionCombo.onChange = [this]() { onProjectionChanged(); };

    addAndMakeVisible(anaglyph3DBtn);
    anaglyph3DBtn.setTooltip("Toggle red/cyan anaglyph 3D rendering. Put on red/cyan glasses to see the scatter "
                             "frames in stereoscopic 3D — useful for visualizing 3+ dimensional wavetables.");
    anaglyph3DBtn.setClickingTogglesState(true);
    anaglyph3DBtn.onClick = [this]() {
        if (scatterView) scatterView->anaglyph3D = anaglyph3DBtn.getToggleState();
        rebuildScatterUI();
    };

    scatterView = std::make_unique<ScatterView>(*this);
    addAndMakeVisible(scatterView.get());

    // Auto-detect screen DPI from JUCE so the parallax math is calibrated
    // to the user's actual display.
    {
        auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* main = displays.getPrimaryDisplay();
        if (main && main->dpi > 1.0)
            scatterView->dpi = (float)main->dpi;
    }

    // Stereo settings sliders — only visible when 3D anaglyph mode is on.
    addAndMakeVisible(stereoRow);
    auto setupStereoSlider = [this](juce::Slider& s, juce::Label& l, const char* name,
                                    double lo, double hi, double def, const char* suffix) {
        stereoRow.addAndMakeVisible(l);
        l.setText(name, juce::dontSendNotification);
        l.setFont(11.0f);
        stereoRow.addAndMakeVisible(s);
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 16);
        s.setRange(lo, hi, 0.5);
        s.setValue(def, juce::dontSendNotification);
        s.setTextValueSuffix(suffix);
        s.onValueChange = [this]() { pushStereoSettingsToView(); };
    };
    setupStereoSlider(ipdSlider,   ipdLabel,   "IPD",       50.0, 75.0,  63.0, " mm");
    setupStereoSlider(distSlider,  distLabel,  "View dist", 30.0, 120.0, 60.0, " cm");
    setupStereoSlider(depthSlider, depthLabel, "Depth",     10.0, 200.0, 60.0, " mm");
    ipdSlider.setTooltip("Interpupillary distance (mm) — the distance between your eyes. "
                         "Affects the apparent depth of the 3D anaglyph rendering. Adult average is ~63mm.");
    distSlider.setTooltip("Viewing distance (cm) — how far your eyes are from the screen. "
                          "Combines with IPD to scale the parallax for accurate 3D.");
    depthSlider.setTooltip("Depth scale (mm) — how much the 3D effect protrudes from the screen. "
                           "Increase for a more dramatic effect, decrease for subtler depth.");
    stereoRow.addAndMakeVisible(dpiLabel);
    dpiLabel.setText("DPI " + juce::String((int)scatterView->dpi), juce::dontSendNotification);
    dpiLabel.setFont(11.0f);

    // Numeric coordinate entry for the selected scatter frame.
    addAndMakeVisible(coordRow);
    coordRow.addAndMakeVisible(coordRowTitle);
    coordRowTitle.setText("Frame:", juce::dontSendNotification);
    coordRowTitle.setFont(11.0f);

    addAndMakeVisible(nonProjAxisRow);
    addAndMakeVisible(frameTabsRow);

    addAndMakeVisible(helpBtn);
    helpBtn.setTooltip("Open the wavetable / layered waveform docs");
    helpBtn.onClick = []() { openHelpDocFile("wavetables.html"); };

    addAndMakeVisible(applyBtn);
    applyBtn.setButtonText("Apply");
    applyBtn.setTooltip("Save the current waveform edits to the synth without closing this editor");
    applyBtn.onClick = [this]() {
        commitToNode();
        if (onApply) onApply();
    };

    addAndMakeVisible(closeBtn);
    closeBtn.setButtonText("Close");
    closeBtn.onClick = [this]() {
        // Commit on close too so work isn't lost by accident.
        commitToNode();
        if (onApply) onApply();
        // Properly destroy the dialog window. exitModalState() alone would
        // leave the window visible and intercepting clicks. Defer the delete
        // via callAsync so we don't tear ourselves down mid-callback.
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            juce::Component::SafePointer<juce::DialogWindow> safe(dw);
            juce::MessageManager::callAsync([safe]() {
                if (safe) delete safe.getComponent();
            });
        }
    };

    addAndMakeVisible(layersViewport);
    layersViewport.setViewedComponent(&layersContainer, false);
    layersViewport.setScrollBarsShown(true, false);

    modeToggleBtn.setButtonText(wave.mode == WavetableMode::Grid ? "Mode: Grid" : "Mode: Scatter");
    rebuildFrameTabs();
    rebuildScatterUI();
    rebuildRows();
    refreshPreview();
    syncPositionParams();
    setSize(900, 820);
}

void LayeredWaveEditorComponent::toggleMode() {
    if (wave.mode == WavetableMode::Grid) {
        // Convert grid frames to scatter frames preserving the N-D grid layout.
        // Each grid frame's flat index decomposes into per-axis grid coords;
        // those coords map to [0,1] scatter positions on each axis.
        wave.mode = WavetableMode::Scatter;
        int gridN = std::max(1, (int)wave.gridDims.size());
        wave.scatterDims = std::min(8, gridN);

        std::vector<int> dims = wave.gridDims;
        if (dims.empty()) dims.push_back((int)wave.frames.size());
        for (auto& d : dims) d = std::max(1, d);

        // Walk frames in row-major order matching gridToFlat's layout.
        int total = 1;
        for (int d : dims) total *= d;
        int nFrames = (int)wave.frames.size();

        wave.scatterFrames.clear();
        for (int f = 0; f < nFrames && f < total; ++f) {
            // Decompose flat index f into per-dim coords (row-major: last dim
            // varies fastest — matches WavetableDoc::gridToFlat).
            std::vector<int> idx(dims.size(), 0);
            int rem = f;
            for (int d = (int)dims.size() - 1; d >= 0; --d) {
                idx[d] = rem % dims[d];
                rem /= dims[d];
            }
            ScatterFrame sf;
            sf.wave = wave.frames[f];
            sf.position.assign(wave.scatterDims, 0.5f);
            for (int d = 0; d < (int)dims.size() && d < wave.scatterDims; ++d) {
                sf.position[d] = (dims[d] > 1)
                    ? (float)idx[d] / (float)(dims[d] - 1)
                    : 0.5f;
            }
            wave.scatterFrames.push_back(std::move(sf));
        }
        if (wave.scatterFrames.empty()) {
            ScatterFrame sf;
            sf.wave = LayeredWaveform::defaultSine();
            sf.position.assign(wave.scatterDims, 0.5f);
            wave.scatterFrames.push_back(std::move(sf));
        }
        currentPosition.assign(wave.scatterDims, 0.5f);
    } else {
        // Convert scatter frames back to grid by sorting along axis 0.
        wave.mode = WavetableMode::Grid;
        std::vector<ScatterFrame> sorted = wave.scatterFrames;
        std::sort(sorted.begin(), sorted.end(),
                  [](const ScatterFrame& a, const ScatterFrame& b) {
                      float av = a.position.empty() ? 0.0f : a.position[0];
                      float bv = b.position.empty() ? 0.0f : b.position[0];
                      return av < bv;
                  });
        wave.frames.clear();
        for (auto& sf : sorted) wave.frames.push_back(sf.wave);
        if (wave.frames.empty()) wave.frames.push_back(LayeredWaveform::defaultSine());
        wave.gridDims = { (int)wave.frames.size() };
    }
    currentFrameIdx = 0;
    modeToggleBtn.setButtonText(wave.mode == WavetableMode::Grid ? "Mode: Grid" : "Mode: Scatter");
    rebuildFrameTabs();
    rebuildScatterUI();
    rebuildRows();
    syncPositionParams();
    onLayerChanged();
    resized();
}

void LayeredWaveEditorComponent::syncPositionParams() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    int n = wave.numDimensions();

    // Remove existing Position params (any param starting with "Position").
    auto isPosName = [](const std::string& s) {
        return s.rfind("Position", 0) == 0;
    };
    nd->params.erase(std::remove_if(nd->params.begin(), nd->params.end(),
        [&](const Param& p) { return isPosName(p.name); }), nd->params.end());

    // Add fresh Position params (1D = "Position", N-D = "Position 1".."Position N")
    for (int i = 0; i < n; ++i) {
        Param p;
        p.name = (n == 1) ? "Position" : ("Position " + std::to_string(i + 1));
        p.value = 0.5f;
        p.minVal = 0.0f;
        p.maxVal = 1.0f;
        nd->params.push_back(std::move(p));
    }
}

void LayeredWaveEditorComponent::rebuildScatterUI() {
    // Populate the projection combo with all C(N,2) axis pairs (or C(N,3)
    // triples if 3D mode is on). When there's only one possible projection
    // (N==2 in 2D, or N<=3 in 3D where rotation already covers it) the
    // combo is hidden — it would just show one item and be confusing.
    projectionCombo.clear(juce::dontSendNotification);
    int n = wave.scatterDims;
    bool is3D = anaglyph3DBtn.getToggleState();
    int id = 1;
    auto axName = [](int i) {
        const char* names[] = {"X","Y","Z","W","V","U","T","S"};
        return juce::String((i >= 0 && i < 8) ? names[i] : "?");
    };
    if (!is3D) {
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                projectionCombo.addItem(axName(i) + " x " + axName(j), id++);
    } else {
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                for (int k = j + 1; k < n; ++k)
                    projectionCombo.addItem(axName(i) + " x " + axName(j) + " x " + axName(k), id++);
    }

    // Hide the combo when there's nothing meaningful to pick. With N==2 in
    // 2D mode the only pair is (X,Y); with N<=3 in 3D mode the only triple
    // is (X,Y,Z) and the rotation handles all viewing angles.
    bool projPickerUseful = (!is3D && n >= 3) || (is3D && n >= 4);
    projectionCombo.setVisible(projPickerUseful);

    if (projectionCombo.getNumItems() > 0) {
        projectionCombo.setSelectedId(1, juce::dontSendNotification);
        if (scatterView) scatterView->setProjection(0, 1, 2);
    }

    // Build sliders for axes not currently projected so the user can move
    // along them independently.
    nonProjAxisSliders.clear();
    nonProjAxisLabels.clear();
    nonProjAxisRow.removeAllChildren();

    if (wave.mode == WavetableMode::Scatter && scatterView) {
        std::vector<int> shown;
        shown.push_back(scatterView->axisX);
        shown.push_back(scatterView->axisY);
        if (is3D) shown.push_back(scatterView->axisZ);
        for (int d = 0; d < n; ++d) {
            if (std::find(shown.begin(), shown.end(), d) != shown.end()) continue;
            auto sl = std::make_unique<juce::Slider>();
            sl->setSliderStyle(juce::Slider::LinearHorizontal);
            sl->setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 16);
            sl->setRange(0.0, 1.0, 0.01);
            sl->setValue(d < (int)currentPosition.size() ? currentPosition[d] : 0.5f,
                         juce::dontSendNotification);
            int axis = d;
            juce::Slider* rawSl = sl.get();
            sl->onValueChange = [this, axis, rawSl]() {
                while ((int)currentPosition.size() <= axis)
                    currentPosition.push_back(0.5f);
                currentPosition[axis] = (float)rawSl->getValue();
                if (scatterView) scatterView->repaint();
                onLayerChanged();
            };
            nonProjAxisRow.addAndMakeVisible(sl.get());

            const char* names[] = {"X","Y","Z","W","V","U","T","S"};
            auto lab = std::make_unique<juce::Label>();
            lab->setText(juce::String(names[axis < 8 ? axis : 0]) + ":",
                         juce::dontSendNotification);
            lab->setFont(11.0f);
            nonProjAxisRow.addAndMakeVisible(lab.get());

            nonProjAxisSliders.push_back(std::move(sl));
            nonProjAxisLabels.push_back(std::move(lab));
        }
    }

    rebuildCoordRow();

    // Stereo settings row visible only when 3D anaglyph is on (and we're in
    // scatter mode — otherwise the rest of the editor doesn't use it).
    bool show3DSettings = (wave.mode == WavetableMode::Scatter)
                        && anaglyph3DBtn.getToggleState();
    stereoRow.setVisible(show3DSettings);
    if (show3DSettings) pushStereoSettingsToView();

    resized();
}

void LayeredWaveEditorComponent::rebuildCoordRow() {
    coordSliders.clear();
    coordLabels.clear();
    coordRow.removeAllChildren();
    coordRow.addAndMakeVisible(coordRowTitle);

    if (wave.mode != WavetableMode::Scatter) {
        coordRow.setVisible(false);
        return;
    }
    coordRow.setVisible(true);

    int n = wave.scatterDims;
    const char* names[] = { "X","Y","Z","W","V","U","T","S" };
    for (int i = 0; i < n; ++i) {
        auto lab = std::make_unique<juce::Label>();
        lab->setText(juce::String(i < 8 ? names[i] : "?") + ":",
                     juce::dontSendNotification);
        lab->setFont(11.0f);
        coordRow.addAndMakeVisible(lab.get());

        auto sl = std::make_unique<juce::Slider>();
        sl->setSliderStyle(juce::Slider::LinearHorizontal);
        sl->setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 16);
        sl->setRange(0.0, 1.0, 0.001);
        sl->setValue(0.5, juce::dontSendNotification);
        int axis = i;
        juce::Slider* raw = sl.get();
        sl->onValueChange = [this, axis, raw]() {
            if (currentFrameIdx < 0
                || currentFrameIdx >= (int)wave.scatterFrames.size())
                return;
            auto& sf = wave.scatterFrames[currentFrameIdx];
            while ((int)sf.position.size() <= axis) sf.position.push_back(0.5f);
            sf.position[axis] = (float)raw->getValue();
            if (scatterView) scatterView->repaint();
            onLayerChanged();
        };
        coordRow.addAndMakeVisible(sl.get());

        coordLabels.push_back(std::move(lab));
        coordSliders.push_back(std::move(sl));
    }
    syncCoordRowFromSelection();
    resized();
}

void LayeredWaveEditorComponent::syncCoordRowFromSelection() {
    if (wave.mode != WavetableMode::Scatter) return;
    if (currentFrameIdx < 0 || currentFrameIdx >= (int)wave.scatterFrames.size()) return;
    const auto& sf = wave.scatterFrames[currentFrameIdx];
    for (int i = 0; i < (int)coordSliders.size(); ++i) {
        float v = (i < (int)sf.position.size()) ? sf.position[i] : 0.5f;
        coordSliders[i]->setValue(v, juce::dontSendNotification);
    }
    coordRowTitle.setText("Frame #" + juce::String(currentFrameIdx + 1) + ":",
                          juce::dontSendNotification);
}

void LayeredWaveEditorComponent::pushStereoSettingsToView() {
    if (!scatterView) return;
    scatterView->ipdMm         = (float)ipdSlider.getValue();
    scatterView->viewingDistMm = (float)distSlider.getValue() * 10.0f; // cm → mm
    scatterView->sceneDepthMm  = (float)depthSlider.getValue();
    scatterView->repaint();
}

void LayeredWaveEditorComponent::onProjectionChanged() {
    if (!scatterView) return;
    int sel = projectionCombo.getSelectedId() - 1;
    if (sel < 0) return;
    int n = wave.scatterDims;
    bool is3D = anaglyph3DBtn.getToggleState();
    int idx = 0;
    if (!is3D) {
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                if (idx == sel) { scatterView->setProjection(i, j, -1); rebuildScatterUI(); return; }
                idx++;
            }
    } else {
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                for (int k = j + 1; k < n; ++k) {
                    if (idx == sel) { scatterView->setProjection(i, j, k); rebuildScatterUI(); return; }
                    idx++;
                }
    }
}

std::vector<WaveLayer>& LayeredWaveEditorComponent::currentLayers() {
    if (auto* f = wave.frameAt(currentFrameIdx)) return f->layers;
    static std::vector<WaveLayer> empty;
    return empty;
}

const std::vector<WaveLayer>& LayeredWaveEditorComponent::currentLayers() const {
    if (auto* f = wave.frameAt(currentFrameIdx)) return f->layers;
    static const std::vector<WaveLayer> empty;
    return empty;
}

void LayeredWaveEditorComponent::switchToFrame(int idx) {
    int n = wave.activeFrameCount();
    if (idx < 0 || idx >= n) return;
    if (idx == currentFrameIdx) return;
    currentFrameIdx = idx;
    rebuildFrameTabs();
    rebuildRows();
    refreshPreview();
}

void LayeredWaveEditorComponent::rebuildFrameTabs() {
    frameTabs.clear();
    frameTabsRow.removeAllChildren();
    // Frame tabs are only used in Grid mode. In Scatter mode the user picks
    // frames by clicking dots in the viewport.
    if (wave.mode != WavetableMode::Grid) {
        frameTabsRow.repaint();
        return;
    }
    for (int i = 0; i < (int)wave.frames.size(); ++i) {
        FrameTab ft;

        ft.selectBtn = std::make_unique<juce::TextButton>();
        ft.selectBtn->setButtonText(juce::String(i + 1));
        ft.selectBtn->setClickingTogglesState(true);
        ft.selectBtn->setToggleState(i == currentFrameIdx, juce::dontSendNotification);
        ft.selectBtn->onClick = [this, i]() { switchToFrame(i); };
        frameTabsRow.addAndMakeVisible(ft.selectBtn.get());

        ft.deleteBtn = std::make_unique<juce::TextButton>();
        ft.deleteBtn->setButtonText("x");
        ft.deleteBtn->onClick = [this, i]() {
            if ((int)wave.frames.size() <= 1) return; // keep at least one
            wave.frames.erase(wave.frames.begin() + i);
            currentFrameIdx = std::min(currentFrameIdx, (int)wave.frames.size() - 1);
            rebuildFrameTabs();
            rebuildRows();
            onLayerChanged();
        };
        frameTabsRow.addAndMakeVisible(ft.deleteBtn.get());

        frameTabs.push_back(std::move(ft));
    }
    resized(); // trigger tab layout
    frameTabsRow.repaint();
}

LayeredWaveEditorComponent::~LayeredWaveEditorComponent() {
    // If there's a pending debounced apply, flush it now so the audio
    // engine picks up the last edits even if the editor closes quickly.
    if (isTimerRunning()) {
        stopTimer();
        if (onApply) onApply();
    }
}

void LayeredWaveEditorComponent::rebuildRows() {
    rows.clear();
    layersContainer.removeAllChildren();

    int y = 0;
    int rh = LayerRow::rowHeight();
    int vw = std::max(layersViewport.getWidth(), 500);
    const auto& layers = currentLayers();
    for (int i = 0; i < (int)layers.size(); ++i) {
        auto row = std::make_unique<LayerRow>(*this, i);
        row->setBounds(0, y, vw, rh);
        row->syncFromModel();
        layersContainer.addAndMakeVisible(row.get());
        rows.push_back(std::move(row));
        y += rh + 4;
    }
    layersContainer.setSize(vw, std::max(y, 10));
}

void LayeredWaveEditorComponent::refreshPreview() {
    // Render the currently-edited frame for the combined preview.
    if (auto* f = wave.frameAt(currentFrameIdx)) {
        f->tableSize = wave.tableSize;
        f->render(previewSamples);
    } else {
        previewSamples.clear();
    }
    repaint();
}

void LayeredWaveEditorComponent::commitToNode() {
    if (auto* nd = graph.findNode(nodeId))
        nd->script = wave.encode();
}

void LayeredWaveEditorComponent::onLayerChanged() {
    // Live visual preview every tick; audio rebuild is debounced so we don't
    // race JUCE's AudioProcessorGraph async rebuild (which crashes on rapid
    // concurrent rebuilds).
    refreshPreview();
    commitToNode();
    // (Re)start the debounce: fire 150ms after the last change.
    startTimer(150);
}

void LayeredWaveEditorComponent::timerCallback() {
    stopTimer();
    if (onApply) onApply();
}

void LayeredWaveEditorComponent::resized() {
    auto a = getLocalBounds().reduced(8);

    // Top row: add-layer / add-frame / mode toggle / +dim / -dim / projection / 3D / apply / close
    auto top = a.removeFromTop(28);
    addLayerBtn  .setBounds(top.removeFromLeft(100));
    top.removeFromLeft(4);
    addFrameBtn  .setBounds(top.removeFromLeft(70));
    top.removeFromLeft(4);
    modeToggleBtn.setBounds(top.removeFromLeft(110));
    top.removeFromLeft(4);
    addDimBtn    .setBounds(top.removeFromLeft(48));
    removeDimBtn .setBounds(top.removeFromLeft(48));
    top.removeFromLeft(6);
    closeBtn  .setBounds(top.removeFromRight(60));
    top.removeFromRight(4);
    applyBtn  .setBounds(top.removeFromRight(60));
    top.removeFromRight(4);
    helpBtn   .setBounds(top.removeFromRight(26));
    top.removeFromRight(6);
    anaglyph3DBtn.setBounds(top.removeFromRight(40));
    top.removeFromRight(4);
    projectionCombo.setBounds(top);

    a.removeFromTop(6);

    // Frame tab row (only shown in Grid mode; tabs render here)
    if (wave.mode == WavetableMode::Grid) {
        auto tabRow = a.removeFromTop(28);
        frameTabsRow.setBounds(tabRow);
        {
            int bw = 32, xw = 16, gap = 3;
            int x = 0;
            for (int i = 0; i < (int)frameTabs.size(); ++i) {
                int h = tabRow.getHeight() - 4;
                frameTabs[i].selectBtn->setBounds(x, 2, bw, h);
                x += bw;
                frameTabs[i].deleteBtn->setBounds(x, 2, xw, h);
                x += xw + gap;
            }
        }
        // Hide scatter view
        if (scatterView) scatterView->setVisible(false);
        nonProjAxisRow.setVisible(false);
    } else {
        frameTabsRow.setBounds({});
        // Scatter mode: viewport + coord entry + non-projected sliders + (3D settings)
        if (scatterView) scatterView->setVisible(true);
        nonProjAxisRow.setVisible(true);

        int viewH = 260;
        auto viewArea = a.removeFromTop(viewH);
        if (scatterView) scatterView->setBounds(viewArea);

        a.removeFromTop(4);

        // Coordinate entry row for the selected frame
        int coordRowH = coordSliders.empty() ? 0 : 22;
        auto coordArea = a.removeFromTop(coordRowH);
        coordRow.setBounds(coordArea);
        {
            int x = 0;
            int titleW = 56;
            coordRowTitle.setBounds(x, 2, titleW, coordRowH - 4);
            x += titleW + 4;
            int n = (int)coordSliders.size();
            int rowW = std::max(0, coordArea.getWidth() - x);
            int slotW = (n > 0) ? rowW / n : 0;
            for (int i = 0; i < n; ++i) {
                int lw = 16;
                coordLabels[i]->setBounds(x, 2, lw, coordRowH - 4);
                coordSliders[i]->setBounds(x + lw, 2, slotW - lw - 4, coordRowH - 4);
                x += slotW;
            }
        }

        a.removeFromTop(2);

        // Non-projected axis sliders (Position cursor along hidden axes)
        int axisRowH = (int)nonProjAxisSliders.size() > 0 ? 22 : 0;
        auto axisArea = a.removeFromTop(axisRowH);
        nonProjAxisRow.setBounds(axisArea);
        {
            int x = 0;
            int rowW = axisArea.getWidth();
            int n = (int)nonProjAxisSliders.size();
            int slotW = (n > 0) ? rowW / n : 0;
            for (int i = 0; i < n; ++i) {
                int lw = 18;
                nonProjAxisLabels[i]->setBounds(x, 2, lw, axisRowH - 4);
                nonProjAxisSliders[i]->setBounds(x + lw, 2, slotW - lw - 4, axisRowH - 4);
                x += slotW;
            }
        }

        a.removeFromTop(2);

        // 3D anaglyph stereo geometry sliders (only when 3D toggle is on)
        int stereoH = stereoRow.isVisible() ? 22 : 0;
        auto stereoArea = a.removeFromTop(stereoH);
        if (stereoH > 0) {
            stereoRow.setBounds(stereoArea);
            int x = 0;
            int totalW = stereoArea.getWidth();
            // 3 slider slots + a small DPI label on the right
            int dpiW = 70;
            int slotW = (totalW - dpiW) / 3;
            auto layoutSlot = [&](juce::Label& l, juce::Slider& s) {
                int lw = 60;
                l.setBounds(x, 2, lw, stereoH - 4);
                s.setBounds(x + lw, 2, slotW - lw - 4, stereoH - 4);
                x += slotW;
            };
            layoutSlot(ipdLabel,   ipdSlider);
            layoutSlot(distLabel,  distSlider);
            layoutSlot(depthLabel, depthSlider);
            dpiLabel.setBounds(x, 2, dpiW, stereoH - 4);
        }
    }

    a.removeFromTop(4);

    // Preview at bottom
    int previewH = 170;
    auto previewArea = a.removeFromBottom(previewH);
    (void)previewArea; // painted in paint()

    a.removeFromBottom(6);

    // Layers viewport in the middle
    layersViewport.setBounds(a);
    int vw = layersViewport.getWidth();
    int rh = LayerRow::rowHeight();
    int y = 0;
    for (auto& row : rows) {
        row->setBounds(0, y, vw, rh);
        y += rh + 4;
    }
    layersContainer.setSize(vw, std::max(y, 10));
}

void LayeredWaveEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));

    // Preview area rectangle — preview always sits at the bottom, so we
    // just carve from the bottom regardless of what's above.
    auto a = getLocalBounds().reduced(8);
    int previewH = 170;
    auto previewArea = a.removeFromBottom(previewH).toFloat();

    g.setColour(juce::Colour(30, 30, 38));
    g.fillRoundedRectangle(previewArea, 4.0f);
    g.setColour(juce::Colour(70, 70, 90));
    g.drawRoundedRectangle(previewArea, 4.0f, 1.0f);

    // Title
    g.setColour(juce::Colours::grey);
    g.setFont(11.0f);
    int total = wave.activeFrameCount();
    juce::String title = (wave.mode == WavetableMode::Grid ? "Grid frame " : "Scatter frame ")
                       + juce::String(currentFrameIdx + 1) + "/" + juce::String(total)
                       + "  —  baked on edit; Position morphs the active blend";
    g.drawText(title,
               previewArea.reduced(6, 2).toNearestInt(),
               juce::Justification::topLeft, true);

    // Waveform curve
    if (!previewSamples.empty()) {
        float cx = previewArea.getX() + 4;
        float cy = previewArea.getCentreY();
        float w  = previewArea.getWidth() - 8;
        float h  = previewArea.getHeight() - 8;

        g.setColour(juce::Colours::grey.withAlpha(0.3f));
        g.drawHorizontalLine((int)cy, cx, cx + w);

        juce::Path p;
        int n = (int)previewSamples.size();
        for (int i = 0; i < n; ++i) {
            float x = cx + (float)i / (float)(n - 1) * w;
            float y = cy - previewSamples[i] * h * 0.45f;
            if (i == 0) p.startNewSubPath(x, y);
            else p.lineTo(x, y);
        }
        g.setColour(juce::Colours::cornflowerblue);
        g.strokePath(p, juce::PathStrokeType(1.5f));
    }
}

} // namespace SoundShop
