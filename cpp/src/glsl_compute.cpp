#include "glsl_compute.h"

#include <mutex>
#include <cstdio>

#if defined(_WIN32)

// Pull in Windows + WGL before the JUCE GL bindings. JUCE's juce_opengl module
// provides the core GL 4.x entry points in namespace juce::gl, loaded from the
// driver after a context is current via juce::gl::loadFunctions().
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <juce_opengl/juce_opengl.h>

// wglCreateContextAttribsARB is an extension entry point — declare its type and
// the attribute enums ourselves so we don't depend on a particular GL loader.
#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB  0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif
typedef HGLRC(WINAPI* PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int*);

namespace SoundShop {
namespace {

// ---------------------------------------------------------------------------
// Lazily-created, cached offscreen GL 4.3 core context.
// ---------------------------------------------------------------------------
struct GlContext {
    HWND  hwnd = nullptr;
    HDC   hdc  = nullptr;
    HGLRC hglrc = nullptr;
    bool  tried = false;     // creation attempted (success or fail)
    bool  ok = false;        // context live + compute entry points loaded
    std::string error;       // why creation failed (if !ok)

    // Cached process-static read-only SSBO (the factory waveform bank). Uploaded
    // once via glslSetCachedBuffer and re-bound before every dispatch. 0 = none.
    unsigned int       cachedBuf = 0;
    unsigned long long cachedId = 0;   // dataset id (0 = nothing cached)
    int                cachedBinding = 2;
};

std::mutex g_mutex;
GlContext g_ctx;

const wchar_t* kWndClass = L"SeanceHeadlessGLClass";

// Register the (hidden) window class once. CS_OWNDC gives the window a private
// device context, required for a stable GL pixel format.
bool ensureWindowClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClass;
    wc.style = CS_OWNDC;
    if (!RegisterClassW(&wc)) {
        // 1410 == ERROR_CLASS_ALREADY_EXISTS — fine, someone beat us to it.
        if (GetLastError() != 1410) return false;
    }
    registered = true;
    return true;
}

// Attempt to build the context. Fills g_ctx. Must hold g_mutex.
void createContextLocked() {
    if (g_ctx.tried) return;
    g_ctx.tried = true;

    auto fail = [&](const std::string& msg) {
        g_ctx.error = msg;
        g_ctx.ok = false;
        // Best-effort cleanup of partial state.
        if (g_ctx.hglrc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(g_ctx.hglrc); g_ctx.hglrc = nullptr; }
        if (g_ctx.hdc && g_ctx.hwnd) { ReleaseDC(g_ctx.hwnd, g_ctx.hdc); g_ctx.hdc = nullptr; }
        if (g_ctx.hwnd) { DestroyWindow(g_ctx.hwnd); g_ctx.hwnd = nullptr; }
    };

    if (!ensureWindowClass()) { fail("could not register window class"); return; }

    // Hidden window — never shown; only exists to own a device context.
    g_ctx.hwnd = CreateWindowExW(0, kWndClass, L"SeanceHeadlessGL",
                                 WS_OVERLAPPEDWINDOW, 0, 0, 16, 16,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_ctx.hwnd) { fail("could not create hidden window"); return; }

    g_ctx.hdc = GetDC(g_ctx.hwnd);
    if (!g_ctx.hdc) { fail("could not get device context"); return; }

    // A minimal pixel format. Compute doesn't render to the default framebuffer,
    // but WGL still requires a pixel format to create any context.
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    int pf = ChoosePixelFormat(g_ctx.hdc, &pfd);
    if (pf == 0 || !SetPixelFormat(g_ctx.hdc, pf, &pfd)) {
        fail("could not set pixel format (no GL-capable display?)"); return;
    }

    // Bootstrap a legacy context so we can query wglCreateContextAttribsARB.
    HGLRC legacy = wglCreateContext(g_ctx.hdc);
    if (!legacy) { fail("wglCreateContext failed"); return; }
    if (!wglMakeCurrent(g_ctx.hdc, legacy)) {
        wglDeleteContext(legacy); fail("wglMakeCurrent(legacy) failed"); return;
    }

    auto wglCreateContextAttribsARB =
        (PFN_wglCreateContextAttribsARB) wglGetProcAddress("wglCreateContextAttribsARB");
    if (!wglCreateContextAttribsARB) {
        wglMakeCurrent(nullptr, nullptr); wglDeleteContext(legacy);
        fail("wglCreateContextAttribsARB unavailable (driver too old for modern GL)");
        return;
    }

    const int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };
    g_ctx.hglrc = wglCreateContextAttribsARB(g_ctx.hdc, nullptr, attribs);
    // Done with the legacy context regardless of outcome.
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(legacy);
    if (!g_ctx.hglrc) { fail("could not create GL 4.3 core context (GPU/driver lacks 4.3?)"); return; }

    if (!wglMakeCurrent(g_ctx.hdc, g_ctx.hglrc)) { fail("wglMakeCurrent(4.3) failed"); return; }

    juce::gl::loadFunctions();
    // The compute pipeline entry points must have resolved.
    if (juce::gl::glDispatchCompute == nullptr || juce::gl::glCreateShader == nullptr
        || juce::gl::glBindBufferBase == nullptr || juce::gl::glGetBufferSubData == nullptr
        || juce::gl::glClearBufferData == nullptr) {
        fail("GL 4.3 compute entry points failed to load");
        return;
    }

    g_ctx.ok = true;
}

// Ensure our context is current on the calling thread. Returns false if the
// context can't be created/made current; *err gets the reason.
bool makeCurrent(std::string* err) {
    createContextLocked();
    if (!g_ctx.ok) { if (err) *err = g_ctx.error; return false; }
    if (!wglMakeCurrent(g_ctx.hdc, g_ctx.hglrc)) {
        if (err) *err = "wglMakeCurrent failed on dispatch thread";
        return false;
    }
    return true;
}

std::string shaderInfoLog(unsigned int sh) {
    using namespace juce::gl;
    GLint len = 0;
    glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
    if (len <= 0) return {};
    std::string log((size_t) len, '\0');
    glGetShaderInfoLog(sh, len, nullptr, log.data());
    // Trim trailing NULs/newlines.
    while (!log.empty() && (log.back() == '\0' || log.back() == '\n')) log.pop_back();
    return log;
}

std::string programInfoLog(unsigned int prog) {
    using namespace juce::gl;
    GLint len = 0;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
    if (len <= 0) return {};
    std::string log((size_t) len, '\0');
    glGetProgramInfoLog(prog, len, nullptr, log.data());
    while (!log.empty() && (log.back() == '\0' || log.back() == '\n')) log.pop_back();
    return log;
}

// Compile + link a complete compute shader. Returns the linked program (0 on
// failure, with *error populated). The shader object is deleted after linking
// (the program retains it). Shared by single-pass and ping-pong dispatch.
unsigned int compileComputeProgram(const std::string& computeSource, std::string& error) {
    using namespace juce::gl;
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    const char* src = computeSource.c_str();
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        error = "compile error:\n" + shaderInfoLog(sh);
        glDeleteShader(sh);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(sh);    // flagged for deletion; freed once detached from prog
    if (!ok) {
        error = "link error:\n" + programInfoLog(prog);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// Set int (scalar or array) uniforms on the currently-bound program. Locations
// that don't exist (optimised out) are skipped — not an error.
void setIntUniforms(unsigned int prog, const std::vector<GlslUniformInt>& intUniforms) {
    using namespace juce::gl;
    for (const auto& u : intUniforms) {
        GLint loc = glGetUniformLocation(prog, u.name.c_str());
        if (loc < 0) continue;
        if (u.values.size() == 1) glUniform1i(loc, u.values[0]);
        else if (!u.values.empty())
            glUniform1iv(loc, (GLsizei) u.values.size(), u.values.data());
    }
}

// Re-bind the cached process-static read-only SSBO (the waveform bank) at its
// binding point, if one has been uploaded. Cheap (just a bind); the buffer is
// never re-uploaded or deleted. Must hold g_mutex with the context current.
void bindCachedBufferLocked() {
    using namespace juce::gl;
    if (g_ctx.cachedBuf != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
                         (GLuint) g_ctx.cachedBinding, g_ctx.cachedBuf);
}

// Format a GL error enum as "0x%04X" for messages.
std::string glErrString(unsigned int gle) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%04X", gle);
    return buf;
}

} // namespace

bool glslComputeAvailable(std::string* whyNot) {
    std::lock_guard<std::mutex> lk(g_mutex);
    createContextLocked();
    if (!g_ctx.ok && whyNot) *whyNot = g_ctx.error;
    return g_ctx.ok;
}

bool glslSetCachedBuffer(int binding, unsigned long long id,
                         const float* data, size_t count, std::string* err) {
    using namespace juce::gl;
    std::lock_guard<std::mutex> lk(g_mutex);
    std::string e;
    if (!makeCurrent(&e)) { if (err) *err = "GL unavailable: " + e; return false; }

    // Same dataset already resident — nothing to do (the common per-bake case).
    if (g_ctx.cachedBuf != 0 && g_ctx.cachedId == id && id != 0) return true;

    while (glGetError() != GL_NO_ERROR) {}

    // Replacing or clearing: drop any existing cached buffer first.
    if (g_ctx.cachedBuf != 0) {
        glDeleteBuffers(1, &g_ctx.cachedBuf);
        g_ctx.cachedBuf = 0;
        g_ctx.cachedId = 0;
    }
    if (!data || count == 0) return true;   // clear request

    GLuint buf = 0;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 (GLsizeiptr) (sizeof(float) * count), data, GL_STATIC_DRAW);
    if (glGetError() == GL_OUT_OF_MEMORY) {
        glDeleteBuffers(1, &buf);
        if (err) *err = "out of GPU memory uploading cached buffer";
        return false;
    }
    g_ctx.cachedBuf = buf;
    g_ctx.cachedId = id;
    g_ctx.cachedBinding = binding;
    return true;
}

GlslDispatchResult glslDispatchCompute(const std::string& computeSource,
                                       int totalCells,
                                       const std::vector<GlslUniformInt>& intUniforms) {
    using namespace juce::gl;
    GlslDispatchResult res;
    if (totalCells <= 0) { res.error = "totalCells must be > 0"; return res; }

    std::lock_guard<std::mutex> lk(g_mutex);
    std::string err;
    if (!makeCurrent(&err)) { res.error = "GL unavailable: " + err; return res; }

    // Drain any pre-existing error so our checks are meaningful.
    while (glGetError() != GL_NO_ERROR) {}

    std::string cerr;
    GLuint prog = compileComputeProgram(computeSource, cerr);
    if (prog == 0) { res.error = cerr; return res; }
    glUseProgram(prog);
    setIntUniforms(prog, intUniforms);

    // Output SSBO at binding=0.
    GLuint ssbo = 0;
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 (GLsizeiptr) (sizeof(float) * (size_t) totalCells),
                 nullptr, GL_DYNAMIC_READ);
    if (glGetError() == GL_OUT_OF_MEMORY) {
        res.error = "out of GPU memory allocating output buffer (grid too large)";
        glDeleteBuffers(1, &ssbo);
        glDeleteProgram(prog);
        return res;
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);

    // Cached read-only aux buffer (e.g. the waveform bank at binding 2).
    bindCachedBufferLocked();

    const GLuint groups = (GLuint) ((totalCells + 63) / 64);
    glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glFinish();

    GLenum gle = glGetError();
    if (gle != GL_NO_ERROR) {
        res.error = "GL error during dispatch: " + glErrString((unsigned) gle);
        glDeleteBuffers(1, &ssbo);
        glDeleteProgram(prog);
        return res;
    }

    res.data.resize((size_t) totalCells);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                       (GLsizeiptr) (sizeof(float) * (size_t) totalCells),
                       res.data.data());

    glDeleteBuffers(1, &ssbo);
    glDeleteProgram(prog);

    res.ok = true;
    return res;
}

GlslDispatchResult glslDispatchComputePingPong(const std::string& computeSource,
                                               int totalCells,
                                               int numPasses,
                                               const std::vector<GlslUniformInt>& intUniforms) {
    using namespace juce::gl;
    GlslDispatchResult res;
    if (totalCells <= 0) { res.error = "totalCells must be > 0"; return res; }
    if (numPasses < 1)  { res.error = "numPasses must be >= 1"; return res; }

    std::lock_guard<std::mutex> lk(g_mutex);
    std::string err;
    if (!makeCurrent(&err)) { res.error = "GL unavailable: " + err; return res; }

    while (glGetError() != GL_NO_ERROR) {}

    std::string cerr;
    GLuint prog = compileComputeProgram(computeSource, cerr);
    if (prog == 0) { res.error = cerr; return res; }
    glUseProgram(prog);
    setIntUniforms(prog, intUniforms);
    const GLint passLoc = glGetUniformLocation(prog, "uPass");

    // Two ping-pong SSBOs, both zero-initialised so the very first pass reads a
    // defined (all-zero) prev[]. glClearBufferData (core 4.3) zero-fills on the
    // GPU without a CPU-side allocation.
    const GLsizeiptr bytes = (GLsizeiptr) (sizeof(float) * (size_t) totalCells);
    GLuint buf[2] = { 0, 0 };
    glGenBuffers(2, buf);
    for (int i = 0; i < 2; ++i) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, nullptr, GL_DYNAMIC_READ);
        if (glGetError() == GL_OUT_OF_MEMORY) {
            res.error = "out of GPU memory allocating ping-pong buffers (grid too large)";
            glDeleteBuffers(2, buf);
            glDeleteProgram(prog);
            return res;
        }
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32F, GL_RED, GL_FLOAT, nullptr);
    }

    // Cached read-only aux buffer (e.g. the waveform bank at binding 2). It never
    // changes between passes, so bind it once before the loop and leave it bound.
    bindCachedBufferLocked();

    const GLuint groups = (GLuint) ((totalCells + 63) / 64);
    int writeIdx = 0;
    for (int pass = 0; pass < numPasses; ++pass) {
        writeIdx = pass & 1;                 // alternate write target
        const int readIdx = writeIdx ^ 1;    // read the OTHER buffer (prev snapshot)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf[writeIdx]);  // data[] (write)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buf[readIdx]);   // prev[] (read)
        if (passLoc >= 0) glUniform1i(passLoc, pass);
        glDispatchCompute(groups, 1, 1);
        // Make this pass's writes visible to the next pass's prev[] reads.
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
    glFinish();

    GLenum gle = glGetError();
    if (gle != GL_NO_ERROR) {
        res.error = "GL error during ping-pong dispatch: " + glErrString((unsigned) gle);
        glDeleteBuffers(2, buf);
        glDeleteProgram(prog);
        return res;
    }

    // The last pass wrote buf[writeIdx]; read that one back.
    res.data.resize((size_t) totalCells);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[writeIdx]);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, bytes, res.data.data());

    glDeleteBuffers(2, buf);
    glDeleteProgram(prog);

    res.ok = true;
    return res;
}

} // namespace SoundShop

#else // non-Windows: no headless GL backend yet.

namespace SoundShop {

bool glslComputeAvailable(std::string* whyNot) {
    if (whyNot) *whyNot = "GLSL compute is only implemented on Windows in this build";
    return false;
}

bool glslSetCachedBuffer(int, unsigned long long, const float*, size_t, std::string* err) {
    if (err) *err = "GLSL compute is only implemented on Windows in this build";
    return false;
}

GlslDispatchResult glslDispatchCompute(const std::string&, int,
                                       const std::vector<GlslUniformInt>&) {
    GlslDispatchResult res;
    res.error = "GLSL compute is only implemented on Windows in this build";
    return res;
}

GlslDispatchResult glslDispatchComputePingPong(const std::string&, int, int,
                                               const std::vector<GlslUniformInt>&) {
    GlslDispatchResult res;
    res.error = "GLSL compute is only implemented on Windows in this build";
    return res;
}

} // namespace SoundShop

#endif
