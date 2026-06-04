#define _USE_MATH_DEFINES
#include "spatializer_3d.h"
#include "hrtf_data.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace SoundShop {

static float paramByName_(const Node& node, const char* name, float def) {
    for (auto& p : node.params)
        if (p.name == name) return p.value;
    return def;
}

Spatializer3DProcessor::Spatializer3DProcessor(Node& n) : node(n) {}

void Spatializer3DProcessor::prepareToPlay(double sr, int) {
    sampleRate = sr;
    std::memset(delayBufL, 0, sizeof(delayBufL));
    std::memset(delayBufR, 0, sizeof(delayBufR));
    std::memset(pinnaDelayL, 0, sizeof(pinnaDelayL));
    std::memset(pinnaDelayR, 0, sizeof(pinnaDelayR));
    delayWritePos = 0;
    pinnaWritePos = 0;
    filterStateL = filterStateR = 0;

    // Initialize convolution history
    std::memset(convHistoryL, 0, sizeof(convHistoryL));
    std::memset(convHistoryR, 0, sizeof(convHistoryR));
    convWritePos = 0;

    // Load initial HRTF
    auto& table = HRTFTable::instance();
    table.lookup(0, 0, currentIR_L, currentIR_R);
}

void Spatializer3DProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) {
    if (buf.getNumChannels() < 2) return;

    float azimuth  = paramByName_(node, "Azimuth", 0.0f);
    float elevation = paramByName_(node, "Elevation", 0.0f);
    float distance  = paramByName_(node, "Distance", 0.5f);

    // Smooth parameter changes
    float smoothRate = 0.05f;
    prevAzimuth   += (azimuth - prevAzimuth) * smoothRate;
    prevElevation += (elevation - prevElevation) * smoothRate;
    prevDistance   += (distance - prevDistance) * smoothRate;

    // Look up the HRTF for the current direction
    float targetIR_L[HRTF_IR_LENGTH], targetIR_R[HRTF_IR_LENGTH];
    HRTFTable::instance().lookup(prevAzimuth, prevElevation, targetIR_L, targetIR_R);

    // Crossfade between current and target IR to avoid clicks when direction changes
    float crossfadeRate = 0.1f;
    for (int i = 0; i < HRTF_IR_LENGTH; ++i) {
        currentIR_L[i] += (targetIR_L[i] - currentIR_L[i]) * crossfadeRate;
        currentIR_R[i] += (targetIR_R[i] - currentIR_R[i]) * crossfadeRate;
    }

    // Distance attenuation
    float distGain = 1.0f / (1.0f + prevDistance * 3.0f);

    int numSamples = buf.getNumSamples();
    auto* inL = buf.getReadPointer(0);
    auto* inR = buf.getNumChannels() > 1 ? buf.getReadPointer(1) : inL;

    auto* outL = buf.getWritePointer(0);
    auto* outR = buf.getWritePointer(1);

    for (int s = 0; s < numSamples; ++s) {
        // Mix to mono
        float mono = (inL[s] + inR[s]) * 0.5f;

        // Write to convolution history
        convHistoryL[convWritePos] = mono;
        convHistoryR[convWritePos] = mono;

        // Convolve with HRTF IR (direct form, short IR so it's efficient)
        float sumL = 0, sumR = 0;
        for (int k = 0; k < HRTF_IR_LENGTH; ++k) {
            int idx = (convWritePos - k + CONV_HISTORY_SIZE) % CONV_HISTORY_SIZE;
            sumL += convHistoryL[idx] * currentIR_L[k];
            sumR += convHistoryR[idx] * currentIR_R[k];
        }

        convWritePos = (convWritePos + 1) % CONV_HISTORY_SIZE;

        outL[s] = juce::jlimit(-1.0f, 1.0f, sumL * distGain);
        outR[s] = juce::jlimit(-1.0f, 1.0f, sumR * distGain);
    }
}

int Spatializer3DProcessor::loadHrtfDirectory(const std::string& dirPath) {
    measuredHrtfs.clear();
    hasMeasuredHrtf = false;

    auto dir = juce::File(dirPath);
    if (!dir.isDirectory()) return 0;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    // Scan for files matching hrtf_az*_el*_L.wav
    auto files = dir.findChildFiles(juce::File::findFiles, false, "hrtf_az*_el*_L.wav");
    for (auto& leftFile : files) {
        auto name = leftFile.getFileNameWithoutExtension().toStdString();
        // Parse azimuth and elevation from filename.
        int az = 0, el = 0;
        if (std::sscanf(name.c_str(), "hrtf_az%d_el%d_L", &az, &el) != 2) continue;

        auto rightFile = leftFile.getSiblingFile(
            leftFile.getFileNameWithoutExtension().replace("_L", "_R") + ".wav");
        if (!rightFile.existsAsFile()) continue;

        HrtfEntry entry;
        entry.azimuth = (float)az;
        entry.elevation = (float)el;

        // Load left IR.
        std::unique_ptr<juce::AudioFormatReader> lr(fm.createReaderFor(leftFile));
        if (lr) {
            int len = std::min((int)lr->lengthInSamples, HRTF_IR_LENGTH);
            juce::AudioBuffer<float> b(1, len);
            lr->read(&b, 0, len, 0, true, false);
            entry.irL.assign(b.getReadPointer(0), b.getReadPointer(0) + len);
            entry.irL.resize(HRTF_IR_LENGTH, 0.0f);
        }
        // Load right IR.
        std::unique_ptr<juce::AudioFormatReader> rr(fm.createReaderFor(rightFile));
        if (rr) {
            int len = std::min((int)rr->lengthInSamples, HRTF_IR_LENGTH);
            juce::AudioBuffer<float> b(1, len);
            rr->read(&b, 0, len, 0, true, false);
            entry.irR.assign(b.getReadPointer(0), b.getReadPointer(0) + len);
            entry.irR.resize(HRTF_IR_LENGTH, 0.0f);
        }

        if (!entry.irL.empty() && !entry.irR.empty())
            measuredHrtfs.push_back(std::move(entry));
    }

    hasMeasuredHrtf = !measuredHrtfs.empty();
    fprintf(stderr, "Loaded %d HRTF measurements from %s\n",
            (int)measuredHrtfs.size(), dirPath.c_str());
    return (int)measuredHrtfs.size();
}

void Spatializer3DProcessor::selectMeasuredHrtf(float azimuth, float elevation) {
    if (!hasMeasuredHrtf || measuredHrtfs.empty()) return;

    // Find nearest entry by angular distance.
    int bestIdx = 0;
    float bestDist = 1e9f;
    for (int i = 0; i < (int)measuredHrtfs.size(); ++i) {
        float daz = measuredHrtfs[i].azimuth - azimuth;
        float del = measuredHrtfs[i].elevation - elevation;
        float d = daz * daz + del * del;
        if (d < bestDist) { bestDist = d; bestIdx = i; }
    }

    auto& entry = measuredHrtfs[bestIdx];
    for (int k = 0; k < HRTF_IR_LENGTH; ++k) {
        currentIR_L[k] = (k < (int)entry.irL.size()) ? entry.irL[k] : 0.0f;
        currentIR_R[k] = (k < (int)entry.irR.size()) ? entry.irR[k] : 0.0f;
    }
}

} // namespace SoundShop
