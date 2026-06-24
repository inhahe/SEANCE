#pragma once

// ---------------------------------------------------------------------------
// glsl_compute — headless OpenGL 4.3 compute-shader dispatch for offline
// terrain baking.
//
// SEANCE's UI is 100% JUCE software rendering: there is NO OpenGL context
// anywhere in the running app (see CLAUDE.md "Tech Stack" note). To run GLSL
// compute shaders as a terrain *generation language* (alongside Builtin/Lua/
// Python) we stand up our OWN offscreen GL 4.3-core context on demand, dispatch
// a compute shader that writes a flat std430 SSBO, read the buffer back to the
// CPU, and tear nothing down (the context is cached for reuse). This is a
// bake-time-only facility — nothing is ever displayed or played from the GPU.
//
// The context is thread-affine: every call below must happen on the same thread
// (in practice the JUCE message thread, where terrain bakes run synchronously,
// and the self-test main thread). Creation is lazy on first use.
//
// Currently Windows-only (WGL bootstrap). On other platforms — or when no
// GPU/driver can provide a 4.3 core context — glslComputeAvailable() returns
// false and callers fall back to another generation language.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

namespace SoundShop {

// One scalar (values.size()==1) or array (>1) int uniform to set on the program
// before dispatch. Used to feed grid rank/dims/total into generated shaders.
struct GlslUniformInt {
    std::string name;
    std::vector<int> values;
};

struct GlslDispatchResult {
    bool ok = false;
    std::string error;        // compile/link/runtime error, or unavailability reason
    std::vector<float> data;  // output SSBO (binding 0) contents: totalCells floats
};

// True if a headless OpenGL 4.3+ core compute context is available on this
// machine. Caches the result (including the failure reason) after the first
// call. If non-null, *whyNot receives a human-readable reason on failure.
bool glslComputeAvailable(std::string* whyNot = nullptr);

// Upload `count` floats as a process-static, read-only SSBO that is cached in
// the GL context and re-bound at `binding` before EVERY subsequent dispatch
// (single-pass and ping-pong alike). Idempotent per `id`: the first call with a
// given non-zero id uploads the data and remembers it; later calls with the same
// id are a cheap no-op (the data is assumed immutable). Used to hand the entire
// factory waveform bank (~8 MB, globally indexed) to shaders that call
// waveform() — it uploads ONCE per session instead of once per bake, and no
// source rewriting is needed because the shader indexes the bank by stable entry
// index. Pass data==nullptr / count==0 to clear any cached buffer. Must run on
// the GL thread. Returns false + *err on unavailability / GL / allocation error.
bool glslSetCachedBuffer(int binding, unsigned long long id,
                         const float* data, size_t count, std::string* err = nullptr);

// Compile `computeSource` (a complete "#version 430" compute shader), allocate
// an std430 float output buffer of `totalCells` elements bound at binding=0,
// set the given int uniforms, dispatch ceil(totalCells/64) 64-wide workgroups,
// memory-barrier + finish, and read the buffer back. Returns ok=false with a
// populated `error` on any compile/link/GL failure or when GL is unavailable.
GlslDispatchResult glslDispatchCompute(const std::string& computeSource,
                                       int totalCells,
                                       const std::vector<GlslUniformInt>& intUniforms = {});

// Multi-pass ("ping-pong") variant for iterative whole-grid programs that need
// to read neighbouring cells from a STABLE snapshot — convolution/blur, cellular
// automata, diffusion/erosion, etc. Two std430 float buffers of `totalCells` are
// allocated and zero-initialised; on each pass the shader writes the "current"
// buffer (binding=0, `data[]`) while reading the "previous" buffer (binding=1,
// `prev[]`). The two buffers swap roles after every pass, so a pass never reads a
// half-updated buffer. The integer uniform `uPass` (current pass index, 0-based)
// is set automatically before each dispatch; the caller supplies constants like
// `uNumPasses` via `intUniforms`. After `numPasses` passes the most-recently
// written buffer is read back. `numPasses` must be >= 1 (1 == a single pass with
// an all-zero `prev[]`, i.e. equivalent to single-buffer dispatch). Same failure
// semantics as glslDispatchCompute.
GlslDispatchResult glslDispatchComputePingPong(const std::string& computeSource,
                                               int totalCells,
                                               int numPasses,
                                               const std::vector<GlslUniformInt>& intUniforms = {});

} // namespace SoundShop
