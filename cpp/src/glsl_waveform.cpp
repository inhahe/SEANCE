#include "glsl_waveform.h"
#include "glsl_compute.h"        // glslSetCachedBuffer
#include "waveform_bank.h"       // factory waveform bank

#include <string>

namespace SoundShop {

// Stable cached-SSBO id for the factory waveform bank (binding 2). Shared by
// every GLSL bake path that wants waveform(); the bank is uploaded at most once
// per session (idempotent per id).
constexpr unsigned long long kWaveformBankBufferId = 0x53534257u; // 'SSBW'

// Build the GLSL source for the waveform() function (and its SSBO declaration),
// indexing the full globally-uploaded bank. bankCount<=0 (bank empty/missing)
// yields a stub that always returns 0 and declares no SSBO, so the shader still
// links and waveform() calls read silence.
static std::string buildGlslWaveformFn(int bankCount) {
    const std::string N = std::to_string(WaveformBank::kSampleCount);
    if (bankCount <= 0) {
        return "float waveform(int id, float phase) { return 0.0; }\n";
    }
    return
        "layout(std430, binding = 2) readonly buffer WfBank { float wfData[]; };\n"
        "const int WF_COUNT = " + std::to_string(bankCount) + ";\n"
        "const int WF_LEN = " + N + ";\n"
        "float waveform(int id, float phase) {\n"
        "  if (id < 0 || id >= WF_COUNT) return 0.0;\n"
        "  float p = phase - floor(phase);\n"           // wrap to [0,1)
        "  float fp = p * float(WF_LEN);\n"
        "  int i0 = int(fp); if (i0 >= WF_LEN) i0 = WF_LEN - 1;\n"
        "  int i1 = i0 + 1; if (i1 >= WF_LEN) i1 = 0;\n"
        "  float frac = fp - float(i0);\n"
        "  int base = id * WF_LEN;\n"
        "  float a = wfData[base + i0];\n"
        "  float b = wfData[base + i1];\n"
        "  return a + (b - a) * frac;\n"
        "}\n";
}

bool glslWaveformFn(const std::string& userBody, std::string& outFn,
                    std::string* err) {
    outFn.clear();
    // If the body doesn't mention "waveform" at all (cheap substring check, not
    // a parse), emit nothing and upload nothing — the common bake pays nothing.
    if (userBody.find("waveform") == std::string::npos) return true;

    auto& bank = WaveformBank::get();
    bank.ensureLoaded();
    const int bankCount = bank.numEntries();
    if (bankCount > 0) {
        std::string upErr;
        if (!glslSetCachedBuffer(2, kWaveformBankBufferId, bank.allSampleData(),
                                 bank.totalSamples(), &upErr)) {
            if (err) *err = "could not upload factory waveform bank to GPU: " + upErr;
            return false;
        }
    }
    outFn = buildGlslWaveformFn(bankCount);
    return true;
}

} // namespace SoundShop
