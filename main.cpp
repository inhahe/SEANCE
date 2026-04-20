// 4D Julia Set Explorer — single-file C++/OpenGL application
// Renders 2D slices (escape-time) and 3D volumes (ray-marched) of
// 4-dimensional Julia sets with interactive ImGui controls.

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <array>
#include <algorithm>

// =========================================================================
// Shader sources
// =========================================================================

static const char* kVertSrc = R"glsl(
#version 430 core
layout(location=0) in vec2 aPos;
out vec2 vNdc;
void main() {
    vNdc = aPos;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)glsl";

static const char* kFrag2dSrc = R"glsl(
#version 430 core
in vec2 vNdc;
out vec4 FragColor;

uniform vec4  u_origin;
uniform vec4  u_u;
uniform vec4  u_v;
uniform vec2  u_resolution;
uniform float u_zoom;
uniform int   u_maxIter;
uniform int   u_palette;
uniform int   u_fracType;  // 0 = complex, 1 = quaternion
uniform vec4  u_qc;        // quaternion c constant

vec3 pal(float t, vec3 a, vec3 b, vec3 c, vec3 d) {
    return a + b * cos(6.28318 * (c*t + d));
}

vec3 getColor(float t) {
    if (u_palette == 0) return pal(t, vec3(.5), vec3(.5), vec3(1), vec3(0,.1,.2));
    if (u_palette == 1) return pal(t, vec3(.5,.1,0), vec3(.5,.4,.3), vec3(1,.7,.4), vec3(0,.15,.2));
    if (u_palette == 2) return pal(t, vec3(0,.15,.25), vec3(.35,.45,.55), vec3(1), vec3(0,.15,.25));
    if (u_palette == 3) return pal(t, vec3(.5), vec3(.5), vec3(1,1,1), vec3(0,.33,.67));
    return vec3(.5 + .5*cos(6.28318*t));
}

void main() {
    vec2 uv = (gl_FragCoord.xy - 0.5 * u_resolution) / min(u_resolution.x, u_resolution.y) * 2.0 / u_zoom;
    vec4 p4 = u_origin + uv.x * u_u + uv.y * u_v;

    float a = p4.x, b = p4.y, c = p4.z, d = p4.w;

    // For complex mode: z = (a,b), c_const = (c,d) — standard Julia
    // For quaternion mode: q = (a,b,c,d), c_const = u_qc — quaternion Julia
    int n = 0;
    if (u_fracType == 0) {
        // Complex: z -> z^2 + c
        float zr = a, zi = b, cr = c, ci = d;
        for (; n < u_maxIter; n++) {
            if (zr*zr + zi*zi > 256.0) break;
            float t = zr*zr - zi*zi + cr;
            zi = 2.0*zr*zi + ci;
            zr = t;
        }
        a = zr; b = zi;
    } else {
        // Quaternion: q -> q^2 + c
        // q^2 = (a^2-b^2-c^2-d^2, 2ab, 2ac, 2ad)
        for (; n < u_maxIter; n++) {
            if (a*a+b*b+c*c+d*d > 256.0) break;
            float na = a*a - b*b - c*c - d*d + u_qc.x;
            float nb = 2.0*a*b + u_qc.y;
            float nc = 2.0*a*c + u_qc.z;
            float nd = 2.0*a*d + u_qc.w;
            a=na; b=nb; c=nc; d=nd;
        }
    }

    if (n == u_maxIter) {
        FragColor = vec4(0,0,0,1);
    } else {
        float mag2 = a*a + b*b + c*c + d*d;
        float mu = float(n) + 1.0 - log2(max(log2(mag2) * 0.5, 1e-6));
        vec3 col = getColor(mu * 0.042);
        FragColor = vec4(col, 1.0);
    }
}
)glsl";

static const char* kFrag3dSrc = R"glsl(
#version 430 core
in vec2 vNdc;
out vec4 FragColor;

uniform mat4  u_invPV;
uniform vec3  u_camPos;
uniform vec4  u_origin;
uniform vec4  u_u, u_v, u_w;
uniform int   u_maxIter;
uniform int   u_numSteps;
uniform int   u_palette;
uniform int   u_fracType;   // 0 = complex, 1 = quaternion
uniform vec4  u_qc;         // quaternion c constant
uniform float u_threshold;  // controls opacity ramp width

// ── Escape time (for opacity — reliable with fixed steps) ──
float juliaEscape(vec3 pos) {
    vec4 p4 = u_origin + pos.x*u_u + pos.y*u_v + pos.z*u_w;
    int n = 0;

    if (u_fracType == 0) {
        // Complex: z -> z^2 + c, where z=(x,y), c=(z,w)
        float zr = p4.x, zi = p4.y, cr = p4.z, ci = p4.w;
        for (; n < u_maxIter; n++) {
            if (zr*zr + zi*zi > 4.0) break;
            float t = zr*zr - zi*zi + cr;
            zi = 2.0*zr*zi + ci;
            zr = t;
        }
    } else {
        // Quaternion: q -> q^2 + c, where q=(a,b,c,d), c=u_qc
        float a=p4.x, b=p4.y, c=p4.z, d=p4.w;
        for (; n < u_maxIter; n++) {
            if (a*a+b*b+c*c+d*d > 4.0) break;
            float na = a*a-b*b-c*c-d*d + u_qc.x;
            float nb = 2.0*a*b + u_qc.y;
            float nc = 2.0*a*c + u_qc.z;
            float nd = 2.0*a*d + u_qc.w;
            a=na; b=nb; c=nc; d=nd;
        }
    }
    return float(n) / float(u_maxIter);
}

// ── Distance estimator (for smooth normals + AO — NOT for marching) ──
float juliaDist(vec3 pos) {
    vec4 p4 = u_origin + pos.x*u_u + pos.y*u_v + pos.z*u_w;

    if (u_fracType == 0) {
        // Complex distance estimator
        float zr = p4.x, zi = p4.y, cr = p4.z, ci = p4.w;
        float dzr = 1.0, dzi = 0.0;
        for (int n = 0; n < u_maxIter; n++) {
            float r2 = zr*zr + zi*zi;
            if (r2 > 256.0) {
                float r = sqrt(r2);
                return 0.5 * r * log(r) / max(sqrt(dzr*dzr+dzi*dzi), 1e-20);
            }
            float nd = 2.0*(zr*dzr - zi*dzi);
            dzi = 2.0*(zr*dzi + zi*dzr);
            dzr = nd;
            float nz = zr*zr - zi*zi + cr;
            zi = 2.0*zr*zi + ci;
            zr = nz;
        }
        return 0.0;
    } else {
        // Quaternion distance estimator
        // Derivative: dq' = 2*q*dq (quaternion multiply), dq_0 = (1,0,0,0)
        float a=p4.x, b=p4.y, c=p4.z, d=p4.w;
        float da=1.0, db=0.0, dc=0.0, dd=0.0;
        for (int n = 0; n < u_maxIter; n++) {
            float r2 = a*a+b*b+c*c+d*d;
            if (r2 > 256.0) {
                float r = sqrt(r2);
                float dr = sqrt(da*da+db*db+dc*dc+dd*dd);
                return 0.5 * r * log(r) / max(dr, 1e-20);
            }
            // dq = 2 * q * dq (quaternion product)
            float nda = 2.0*(a*da - b*db - c*dc - d*dd);
            float ndb = 2.0*(a*db + b*da + c*dd - d*dc);
            float ndc = 2.0*(a*dc - b*dd + c*da + d*db);
            float ndd = 2.0*(a*dd + b*dc - c*db + d*da);
            da=nda; db=ndb; dc=ndc; dd=ndd;
            // q = q^2 + c
            float na = a*a-b*b-c*c-d*d + u_qc.x;
            float nb = 2.0*a*b + u_qc.y;
            float nc = 2.0*a*c + u_qc.z;
            float nd = 2.0*a*d + u_qc.w;
            a=na; b=nb; c=nc; d=nd;
        }
        return 0.0;
    }
}

vec2 boxHit(vec3 ro, vec3 rd) {
    vec3 inv = 1.0/rd;
    vec3 t1 = (-2.0 - ro)*inv, t2 = (2.0 - ro)*inv;
    vec3 mn = min(t1,t2), mx = max(t1,t2);
    return vec2(max(max(mn.x,mn.y),mn.z), min(min(mx.x,mx.y),mx.z));
}

// Normal from distance field gradient (smooth, no fractal noise).
// Larger epsilon smooths out fine "gill" striations so larger-scale
// topology reads clearly (like the sculpted-clay look in the reference).
vec3 distNormal(vec3 p) {
    float e = 0.004;
    return normalize(vec3(
        juliaDist(p+vec3(e,0,0)) - juliaDist(p-vec3(e,0,0)),
        juliaDist(p+vec3(0,e,0)) - juliaDist(p-vec3(0,e,0)),
        juliaDist(p+vec3(0,0,e)) - juliaDist(p-vec3(0,0,e))
    ));
}

// AO from distance field
float distAO(vec3 p, vec3 n) {
    float ao = 0.0, w = 1.0;
    for (int i = 1; i <= 5; i++) {
        float d = 0.012 * float(i);
        ao += w * max(d - juliaDist(p + n*d), 0.0);
        w *= 0.55;
    }
    return clamp(1.0 - 4.5*ao, 0.0, 1.0);
}

void main() {
    vec4 n4 = u_invPV * vec4(vNdc, -1.0, 1.0);
    vec4 f4 = u_invPV * vec4(vNdc,  1.0, 1.0);
    vec3 ro = u_camPos;
    vec3 rd = normalize(f4.xyz/f4.w - n4.xyz/n4.w);

    vec2 tt = boxHit(ro, rd);
    tt.x = max(tt.x, 0.0);
    if (tt.x >= tt.y) { FragColor = vec4(0,0,0,1); return; }

    // ── Material ──
    vec3 baseCol;
    if      (u_palette == 0) baseCol = vec3(0.30, 0.70, 0.78);  // teal jade
    else if (u_palette == 1) baseCol = vec3(0.82, 0.58, 0.48);  // rose gold
    else if (u_palette == 2) baseCol = vec3(0.45, 0.58, 0.88);  // steel blue
    else if (u_palette == 3) baseCol = vec3(0.85, 0.78, 0.68);  // ivory
    else                     baseCol = vec3(0.72);               // silver

    // ── Fixed-step volume march ──
    // Volume compositing finds the surface reliably (no distance-estimate
    // issues with c-varying directions) and naturally anti-aliases.
    float dt = (tt.y - tt.x) / float(u_numSteps);
    vec4 accum = vec4(0.0);
    bool surfaceFound = false;
    vec3 litColor = vec3(0.0);

    for (int i = 0; i < u_numSteps; i++) {
        float t = tt.x + (float(i) + 0.5) * dt;
        vec3 pos = ro + t * rd;

        float fi = juliaEscape(pos);

        // Opacity: exterior transparent, ramp up at boundary, solid interior
        float alpha = smoothstep(0.08, 0.25 + u_threshold * 0.15, fi);
        alpha *= dt * 20.0;
        alpha = min(alpha, 1.0);

        if (alpha > 0.003) {
            // Compute lighting ONCE at first surface contact.
            // Distance-field normal is smooth (no fractal noise).
            // Volume compositing handles anti-aliasing.
            if (!surfaceFound) {
                surfaceFound = true;
                vec3 N = distNormal(pos);
                float ao = distAO(pos, N);

                // Two-light warm/cool shading
                vec3 keyDir  = normalize(vec3(0.5, 0.85, 0.5));
                vec3 keyCol  = vec3(1.0, 0.88, 0.75);
                vec3 fillDir = normalize(vec3(-0.4, -0.2, -0.6));
                vec3 fillCol = vec3(0.42, 0.36, 0.48);  // warmer fill

                float diff1 = max(dot(N, keyDir), 0.0);
                float diff2 = max(dot(N, fillDir), 0.0) * 0.35;

                vec3  H     = normalize(keyDir - rd);
                // Dual-lobe specular: tight highlight + soft sheen
                float spec1 = pow(max(dot(N, H), 0.0), 48.0) * 0.40;
                float spec2 = pow(max(dot(N, H), 0.0), 8.0)  * 0.18;
                float fres  = pow(1.0 - max(dot(N, -rd), 0.0), 4.0) * 0.12;

                // Ground-bounce rim (subtle warm light from below)
                float bounce = max(dot(N, vec3(0, -1, 0)), 0.0) * 0.06;

                litColor = baseCol * (0.12 + diff1*keyCol + diff2*fillCol
                                          + bounce*vec3(0.8, 0.6, 0.5))
                         + keyCol * (spec1 + spec2)
                         + vec3(0.50, 0.45, 0.55) * fres;
                litColor *= ao;
            }

            accum.rgb += (1.0 - accum.a) * alpha * litColor;
            accum.a   += (1.0 - accum.a) * alpha;
        }

        if (accum.a > 0.98) break;
    }

    if (!surfaceFound) { FragColor = vec4(0,0,0,1); return; }

    accum.rgb += (1.0 - accum.a) * vec3(0.0);
    FragColor = vec4(accum.rgb, 1.0);
}
)glsl";

// =========================================================================
// Constants
// =========================================================================

struct RotPlane { int a, b; const char* name; const char* desc; };
static constexpr RotPlane kPlanes[6] = {
    {0, 1, "X-Y", "Screen rotation"},
    {0, 2, "X-R", "z_re <-> c_re"},
    {0, 3, "X-I", "z_re <-> c_im"},
    {1, 2, "Y-R", "z_im <-> c_re"},
    {1, 3, "Y-I", "z_im <-> c_im"},
    {2, 3, "R-I", "c-plane rotation"},
};

struct Preset {
    const char* name;
    float o[4];
    bool customBasis;
    float b[4][4];
};

static const Preset kPresets[] = {
    {"Dendrite",   {0, 0, 0, 1},           false, {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}},
    {"Rabbit",     {0, 0,-0.1228f,0.7449f}, false, {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}},
    {"Spiral",     {0, 0,-0.7269f,0.1889f}, false, {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}},
    {"Siegel",     {0, 0,-0.3912f,0.5877f}, false, {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}},
    {"Lightning",  {0, 0,-0.0180f,0.8832f}, false, {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}},
    {"Galaxy",     {0, 0, 0.2850f,0.0100f}, false, {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}},
    {"Mandelbrot", {0, 0, 0, 0},           true,  {{0,0,1,0},{0,0,0,1},{1,0,0,0},{0,1,0,0}}},
    {"Sculpture",  {0, 0,-0.77f,0.22f},   true,  {{0.4226f,0,0.9063f,0},{0,0.9659f,0,0.2588f},{-0.9063f,0,0.4226f,0},{0,-0.2588f,0,0.9659f}}},
};
static constexpr int kNumPresets = sizeof(kPresets) / sizeof(kPresets[0]);

static const char* kPalNames[] = {"Ocean", "Ember", "Ice", "Rainbow", "Mono"};
static constexpr int kNumPalettes = 5;

// =========================================================================
// Application state
// =========================================================================

struct AppState {
    float origin[4]   = {0, 0, -0.7269f, 0.1889f};
    float basis[4][4] = {
        {1,0,0,0},
        {0,1,0,0},
        {0,0,1,0},
        {0,0,0,1}
    };
    float zoom    = 0.8f;
    int   maxIter = 256;
    int   mode    = 0;   // 0 = 2D, 1 = 3D
    int   plane   = 0;
    int   palette = 0;

    // Accumulated rotation per plane (radians, for display)
    float rotAngles[6] = {0,0,0,0,0,0};

    // 3D camera (orbit)
    float camTheta     = 0.4f;
    float camPhi       = 0.3f;
    float camDist      = 6.0f;
    float camTarget[3] = {0, 0, 0};

    // 3D quality
    int   numSteps    = 128;
    int   maxIter3d   = 64;
    float threshold   = 0.5f;
    float renderScale = 0.5f;
    int   fracType    = 0;   // 0 = Complex (4D z+c), 1 = Quaternion
    float qc[4]      = {-0.2f, 0.8f, 0.0f, 0.0f};  // quaternion c constant

    float fps = 0.0f;
};

// =========================================================================
// Mouse state (file-scope)
// =========================================================================

static bool   g_mouseDown = false;
static double g_lastMx    = 0.0;
static double g_lastMy    = 0.0;

// Forward-declared so callbacks can reach it
static AppState* g_state = nullptr;

// =========================================================================
// 4D math
// =========================================================================

static void rotate4(float basis[4][4], int a, int b, float angle) {
    float c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < 4; i++) {
        float va = basis[i][a];
        float vb = basis[i][b];
        basis[i][a] = c * va - s * vb;
        basis[i][b] = s * va + c * vb;
    }
}

// Rotate AND track the accumulated angle for display
static void doRotate(AppState& s, int planeIdx, float angle) {
    rotate4(s.basis, kPlanes[planeIdx].a, kPlanes[planeIdx].b, angle);
    s.rotAngles[planeIdx] += angle;
}

static void translate4(float origin[4], float basis[4][4], int axis, float amt, float zoom) {
    float step = amt / zoom;
    for (int i = 0; i < 4; i++)
        origin[i] += step * basis[axis][i];
}

static void resetState(AppState& s) {
    s = AppState{};
}

// =========================================================================
// Camera / matrix math (column-major: m[col*4 + row])
// =========================================================================

static void mat4Identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4Perspective(float m[16], float fovY, float aspect, float zNear, float zFar) {
    memset(m, 0, 16 * sizeof(float));
    float tanHalf = tanf(fovY * 0.5f);
    m[0]  = 1.0f / (aspect * tanHalf);
    m[5]  = 1.0f / tanHalf;
    m[10] = -(zFar + zNear) / (zFar - zNear);
    m[11] = -1.0f;
    m[14] = -2.0f * zFar * zNear / (zFar - zNear);
}

static void mat4LookAt(float m[16], const float eye[3], const float center[3], const float up[3]) {
    float fx = center[0] - eye[0];
    float fy = center[1] - eye[1];
    float fz = center[2] - eye[2];
    float flen = sqrtf(fx*fx + fy*fy + fz*fz);
    fx /= flen; fy /= flen; fz /= flen;

    // s = f x up
    float sx = fy * up[2] - fz * up[1];
    float sy = fz * up[0] - fx * up[2];
    float sz = fx * up[1] - fy * up[0];
    float slen = sqrtf(sx*sx + sy*sy + sz*sz);
    sx /= slen; sy /= slen; sz /= slen;

    // u = s x f
    float ux = sy * fz - sz * fy;
    float uy = sz * fx - sx * fz;
    float uz = sx * fy - sy * fx;

    // column-major
    m[0]  = sx;  m[1]  = ux;  m[2]  = -fx;  m[3]  = 0.0f;
    m[4]  = sy;  m[5]  = uy;  m[6]  = -fy;  m[7]  = 0.0f;
    m[8]  = sz;  m[9]  = uz;  m[10] = -fz;  m[11] = 0.0f;
    m[12] = -(sx*eye[0] + sy*eye[1] + sz*eye[2]);
    m[13] = -(ux*eye[0] + uy*eye[1] + uz*eye[2]);
    m[14] = (fx*eye[0] + fy*eye[1] + fz*eye[2]);
    m[15] = 1.0f;
}

static void mat4Multiply(float out[16], const float a[16], const float b[16]) {
    float tmp[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += a[k*4 + r] * b[c*4 + k];
            tmp[c*4 + r] = sum;
        }
    }
    memcpy(out, tmp, 16 * sizeof(float));
}

static bool mat4Inverse(float out[16], const float m[16]) {
    // Standard cofactor-based 4x4 inverse (column-major)
    float s[6], c[6];
    s[0] = m[0]*m[5]  - m[4]*m[1];
    s[1] = m[0]*m[9]  - m[8]*m[1];
    s[2] = m[0]*m[13] - m[12]*m[1];
    s[3] = m[4]*m[9]  - m[8]*m[5];
    s[4] = m[4]*m[13] - m[12]*m[5];
    s[5] = m[8]*m[13] - m[12]*m[9];

    c[0] = m[2]*m[7]  - m[6]*m[3];
    c[1] = m[2]*m[11] - m[10]*m[3];
    c[2] = m[2]*m[15] - m[14]*m[3];
    c[3] = m[6]*m[11] - m[10]*m[7];
    c[4] = m[6]*m[15] - m[14]*m[7];
    c[5] = m[10]*m[15] - m[14]*m[11];

    float det = s[0]*c[5] - s[1]*c[4] + s[2]*c[3] + s[3]*c[2] - s[4]*c[1] + s[5]*c[0];
    if (fabsf(det) < 1e-12f) return false;
    float inv = 1.0f / det;

    out[0]  = ( m[5]*c[5] - m[9]*c[4]  + m[13]*c[3]) * inv;
    out[1]  = (-m[1]*c[5] + m[9]*c[2]  - m[13]*c[1]) * inv;
    out[2]  = ( m[1]*c[4] - m[5]*c[2]  + m[13]*c[0]) * inv;
    out[3]  = (-m[1]*c[3] + m[5]*c[1]  - m[9]*c[0])  * inv;

    out[4]  = (-m[4]*c[5] + m[8]*c[4]  - m[12]*c[3]) * inv;
    out[5]  = ( m[0]*c[5] - m[8]*c[2]  + m[12]*c[1]) * inv;
    out[6]  = (-m[0]*c[4] + m[4]*c[2]  - m[12]*c[0]) * inv;
    out[7]  = ( m[0]*c[3] - m[4]*c[1]  + m[8]*c[0])  * inv;

    out[8]  = ( m[7]*s[5] - m[11]*s[4] + m[15]*s[3]) * inv;
    out[9]  = (-m[3]*s[5] + m[11]*s[2] - m[15]*s[1]) * inv;
    out[10] = ( m[3]*s[4] - m[7]*s[2]  + m[15]*s[0]) * inv;
    out[11] = (-m[3]*s[3] + m[7]*s[1]  - m[11]*s[0]) * inv;

    out[12] = (-m[6]*s[5] + m[10]*s[4] - m[14]*s[3]) * inv;
    out[13] = ( m[2]*s[5] - m[10]*s[2] + m[14]*s[1]) * inv;
    out[14] = (-m[2]*s[4] + m[6]*s[2]  - m[14]*s[0]) * inv;
    out[15] = ( m[2]*s[3] - m[6]*s[1]  + m[10]*s[0]) * inv;

    return true;
}

// =========================================================================
// OpenGL helpers
// =========================================================================

static GLuint compileShader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader compile error:\n%s\n", log);
    }
    return sh;
}

static GLuint linkProgram(GLuint vs, GLuint fs) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "Program link error:\n%s\n", log);
    }
    return prog;
}

// =========================================================================
// BMP save (no external library needed)
// =========================================================================

static void saveBMP(const char* path, int w, int h, const unsigned char* bgr) {
    int pad = (4 - (w * 3) % 4) % 4;
    int dataSize = (w * 3 + pad) * h;
    int fileSize = 54 + dataSize;

    unsigned char hdr[54] = {};
    hdr[0]='B'; hdr[1]='M';
    memcpy(hdr+2,  &fileSize, 4);
    int off=54;   memcpy(hdr+10, &off, 4);
    int ihsz=40;  memcpy(hdr+14, &ihsz, 4);
    memcpy(hdr+18, &w, 4);
    memcpy(hdr+22, &h, 4);
    short p1=1;   memcpy(hdr+26, &p1, 2);
    short bpp=24; memcpy(hdr+28, &bpp, 2);
    memcpy(hdr+34, &dataSize, 4);

    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot write %s\n", path); return; }
    fwrite(hdr, 1, 54, f);
    unsigned char padBytes[3] = {};
    for (int y = 0; y < h; y++) {
        fwrite(bgr + y * w * 3, 1, w * 3, f);
        if (pad) fwrite(padBytes, 1, pad, f);
    }
    fclose(f);
    printf("Saved %s (%dx%d)\n", path, w, h);
}

static GLuint createQuadVAO() {
    float verts[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);
    return vao;
}

// =========================================================================
// FBO for 3D resolution scaling
// =========================================================================

struct FboState {
    GLuint fbo   = 0;
    GLuint tex   = 0;
    GLuint rbo   = 0;
    int    width  = 0;
    int    height = 0;
};

static void ensureFbo(FboState& fb, int w, int h) {
    if (w == fb.width && h == fb.height && fb.fbo != 0)
        return;

    if (fb.fbo) {
        glDeleteFramebuffers(1, &fb.fbo);
        glDeleteTextures(1, &fb.tex);
        glDeleteRenderbuffers(1, &fb.rbo);
    }

    fb.width  = w;
    fb.height = h;

    glGenFramebuffers(1, &fb.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

    glGenTextures(1, &fb.tex);
    glBindTexture(GL_TEXTURE_2D, fb.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb.tex, 0);

    glGenRenderbuffers(1, &fb.rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, fb.rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fb.rbo);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// =========================================================================
// ImGui control panel
// =========================================================================

static void drawUI(AppState& s, int winW, int winH) {
    (void)winH;
    ImGui::SetNextWindowPos(ImVec2((float)winW - 250.0f, 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(240.0f, 0.0f));
    ImGui::Begin("Controls", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    // --- VIEW MODE ---
    ImGui::SeparatorText("View Mode");
    ImGui::RadioButton("2D Slice",  &s.mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("3D Volume", &s.mode, 1);

    // --- FRACTAL TYPE ---
    ImGui::SeparatorText("Fractal Type");
    ImGui::RadioButton("Complex z\xc2\xb2+c", &s.fracType, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Quat q\xc2\xb2+c",  &s.fracType, 1);
    if (s.fracType == 1) {
        ImGui::TextColored(ImVec4(0.5f,0.5f,0.6f,1), "q\xc2\xb2+c, c constant:");
        ImGui::PushItemWidth(-1);
        ImGui::DragFloat4("##qc", s.qc, 0.005f, -2.0f, 2.0f, "%.3f");
        ImGui::PopItemWidth();
    }

    // --- 4D ROTATION ---
    ImGui::SeparatorText("4D Rotation");

    // Bright selection highlight so active plane is unmistakable
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.0f, 0.45f, 0.65f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.0f, 0.55f, 0.75f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.0f, 0.60f, 0.85f, 1.00f));

    float bw = 60.0f;
    for (int i = 0; i < 6; i++) {
        bool selected = (s.plane == i);
        if (ImGui::Selectable(kPlanes[i].name, selected, 0, ImVec2(bw, 0)))
            s.plane = i;
        if ((i % 3) != 2) ImGui::SameLine();
    }
    ImGui::PopStyleColor(3);

    ImGui::TextWrapped("%s", kPlanes[s.plane].desc);

    ImGui::PushButtonRepeat(true);
    if (ImGui::Button("Rotate <<"))
        doRotate(s, s.plane, -0.04f);
    ImGui::SameLine();
    if (ImGui::Button("Rotate >>"))
        doRotate(s, s.plane,  0.04f);
    ImGui::PopButtonRepeat();

    // Rotation angle readout — show accumulated degrees per plane
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.65f, 1.0f));
    for (int i = 0; i < 6; i++) {
        float deg = s.rotAngles[i] * (180.0f / 3.14159265f);
        // Normalize to [-180, 180]
        deg = fmodf(deg + 180.0f, 360.0f) - 180.0f;
        ImGui::Text("%-3s %+7.1f\xc2\xb0", kPlanes[i].name, deg);
        if ((i % 2) == 0) ImGui::SameLine(105.0f);
    }
    ImGui::PopStyleColor();

    // --- MOVE (SCREEN) ---
    ImGui::SeparatorText("Move (Screen)");
    ImGui::PushButtonRepeat(true);
    float halfW = ImGui::GetContentRegionAvail().x * 0.5f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + halfW - 20.0f);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up))
        translate4(s.origin, s.basis, 1, 0.05f, s.zoom);
    if (ImGui::ArrowButton("##left", ImGuiDir_Left))
        translate4(s.origin, s.basis, 0, -0.05f, s.zoom);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##down", ImGuiDir_Down))
        translate4(s.origin, s.basis, 1, -0.05f, s.zoom);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##right", ImGuiDir_Right))
        translate4(s.origin, s.basis, 0, 0.05f, s.zoom);
    ImGui::PopButtonRepeat();

    // --- MOVE (DEPTH) ---
    ImGui::SeparatorText("Move (Depth)");
    ImGui::PushButtonRepeat(true);
    if (ImGui::Button("In (W)"))
        translate4(s.origin, s.basis, 2, 0.05f, s.zoom);
    ImGui::SameLine();
    if (ImGui::Button("Out (S)"))
        translate4(s.origin, s.basis, 2, -0.05f, s.zoom);
    if (ImGui::Button("4th+ (E)"))
        translate4(s.origin, s.basis, 3, 0.05f, s.zoom);
    ImGui::SameLine();
    if (ImGui::Button("4th- (Q)"))
        translate4(s.origin, s.basis, 3, -0.05f, s.zoom);
    ImGui::PopButtonRepeat();

    // --- VIEW CONTROLS ---
    ImGui::SeparatorText("View Controls");
    ImGui::SliderFloat("Zoom", &s.zoom, 0.01f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);

    if (s.mode == 0) {
        ImGui::SliderInt("Iterations", &s.maxIter, 16, 2048);
    } else {
        ImGui::SliderInt("3D Iter", &s.maxIter3d, 16, 128);
        ImGui::SliderInt("Steps", &s.numSteps, 32, 300);
        ImGui::SliderFloat("Surface", &s.threshold, 0.1f, 0.95f, "%.2f");
        ImGui::SliderFloat("Scale", &s.renderScale, 0.2f, 1.0f);
    }

    ImGui::Combo("Palette", &s.palette, kPalNames, kNumPalettes);

    // --- PRESETS ---
    ImGui::SeparatorText("Presets");
    for (int i = 0; i < kNumPresets; i++) {
        if (i > 0 && (i % 3) != 0) ImGui::SameLine();
        if (ImGui::Button(kPresets[i].name)) {
            int prevMode = s.mode;
            resetState(s);
            s.mode = prevMode;
            memcpy(s.origin, kPresets[i].o, sizeof(s.origin));
            if (kPresets[i].customBasis)
                memcpy(s.basis, kPresets[i].b, sizeof(s.basis));
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Reset"))
        resetState(s);

    ImGui::Separator();
    ImGui::Text("FPS: %.1f", s.fps);

    ImGui::End();
}

// =========================================================================
// GLFW callbacks
// =========================================================================

static void mouseButtonCallback(GLFWwindow* win, int button, int action, int /*mods*/) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            g_mouseDown = true;
            glfwGetCursorPos(win, &g_lastMx, &g_lastMy);
        } else {
            g_mouseDown = false;
        }
    }
}

static void cursorPosCallback(GLFWwindow* win, double mx, double my) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    if (!g_mouseDown) return;
    AppState& s = *g_state;

    double dx = mx - g_lastMx;
    double dy = my - g_lastMy;
    g_lastMx = mx;
    g_lastMy = my;

    if (s.mode == 0) {
        // 2D pan: translate origin in screen plane
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        float minDim = (float)std::min(w, h);
        float scale = 2.0f / (minDim * s.zoom);
        translate4(s.origin, s.basis, 0, (float)(-dx) * scale * s.zoom, s.zoom);
        translate4(s.origin, s.basis, 1, (float)( dy) * scale * s.zoom, s.zoom);
    } else {
        // 3D orbit
        s.camTheta -= (float)dx * 0.005f;
        s.camPhi   += (float)dy * 0.005f;
        float limit = 1.5f;
        s.camPhi = std::clamp(s.camPhi, -limit, limit);
    }
}

static void scrollCallback(GLFWwindow* win, double /*xoff*/, double yoff) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    AppState& s = *g_state;

    if (s.mode == 0) {
        // Zoom at cursor position
        double mx, my;
        glfwGetCursorPos(win, &mx, &my);
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        float minDim = (float)std::min(w, h);

        // NDC of cursor
        float cx = ((float)mx - 0.5f * w) / minDim * 2.0f;
        float cy = (0.5f * h - (float)my) / minDim * 2.0f;

        // World-space point under cursor before zoom
        // uv = cursorNDC / zoom
        float ux0 = cx / s.zoom;
        float uy0 = cy / s.zoom;

        float factor = (yoff > 0) ? 1.15f : 1.0f / 1.15f;
        s.zoom *= factor;
        s.zoom = std::clamp(s.zoom, 0.01f, 1000.0f);

        // After zoom
        float ux1 = cx / s.zoom;
        float uy1 = cy / s.zoom;

        // Adjust origin so point stays fixed
        float ddx = ux0 - ux1;
        float ddy = uy0 - uy1;
        for (int i = 0; i < 4; i++)
            s.origin[i] += ddx * s.basis[0][i] + ddy * s.basis[1][i];
    } else {
        // 3D: change camDist
        float factor = (yoff > 0) ? 0.9f : 1.1f;
        s.camDist *= factor;
        s.camDist = std::clamp(s.camDist, 1.0f, 30.0f);
    }
}

static void keyCallback(GLFWwindow* win, int key, int /*scancode*/, int action, int /*mods*/) {
    if (ImGui::GetIO().WantCaptureKeyboard) return;
    if (action != GLFW_PRESS) return;
    AppState& s = *g_state;

    switch (key) {
        case GLFW_KEY_TAB:   s.mode = 1 - s.mode; break;
        case GLFW_KEY_R:     resetState(s); break;
        case GLFW_KEY_C:     s.palette = (s.palette + 1) % kNumPalettes; break;
        case GLFW_KEY_I:
            if (s.mode == 0) s.maxIter = std::min(s.maxIter + 32, 2048);
            else             s.maxIter3d = std::min(s.maxIter3d + 8, 128);
            break;
        case GLFW_KEY_K:
            if (s.mode == 0) s.maxIter = std::max(s.maxIter - 32, 16);
            else             s.maxIter3d = std::max(s.maxIter3d - 8, 16);
            break;
        case GLFW_KEY_H: break;
        case GLFW_KEY_ESCAPE: glfwSetWindowShouldClose(win, GLFW_TRUE); break;
        default: break;
    }
}

// =========================================================================
// main
// =========================================================================

int main(int argc, char** argv) {

    // ----- CLI parsing -----
    AppState cliState;
    const char* saveFile = nullptr;
    int saveW = 800, saveH = 800;
    bool cliMode = false;

    // Rotation angles to apply (degrees), in order: XY XR XI YR YI RI
    float cliRot[6] = {0,0,0,0,0,0};
    bool hasRot = false;

    for (int i = 1; i < argc; i++) {
        auto match = [&](const char* flag) { return strcmp(argv[i], flag) == 0 && i+1 < argc; };
        if (match("--save"))      { saveFile = argv[++i]; cliMode = true; }
        else if (match("--width"))  saveW = atoi(argv[++i]);
        else if (match("--height")) saveH = atoi(argv[++i]);
        else if (match("--c")) {
            float cr, ci;
            if (sscanf(argv[++i], "%f,%f", &cr, &ci) == 2) {
                cliState.origin[2] = cr; cliState.origin[3] = ci;
            }
        }
        else if (match("--origin")) {
            sscanf(argv[++i], "%f,%f,%f,%f",
                   &cliState.origin[0], &cliState.origin[1],
                   &cliState.origin[2], &cliState.origin[3]);
        }
        else if (match("--rot-xy")) { cliRot[0] = (float)atof(argv[++i]); hasRot = true; }
        else if (match("--rot-xr")) { cliRot[1] = (float)atof(argv[++i]); hasRot = true; }
        else if (match("--rot-xi")) { cliRot[2] = (float)atof(argv[++i]); hasRot = true; }
        else if (match("--rot-yr")) { cliRot[3] = (float)atof(argv[++i]); hasRot = true; }
        else if (match("--rot-yi")) { cliRot[4] = (float)atof(argv[++i]); hasRot = true; }
        else if (match("--rot-ri")) { cliRot[5] = (float)atof(argv[++i]); hasRot = true; }
        else if (match("--palette")) cliState.palette = atoi(argv[++i]);
        else if (match("--iter"))    cliState.maxIter3d = atoi(argv[++i]);
        else if (match("--steps"))   cliState.numSteps = atoi(argv[++i]);
        else if (match("--cam-dist")) cliState.camDist = (float)atof(argv[++i]);
        else if (match("--cam-theta")) cliState.camTheta = (float)atof(argv[++i]);
        else if (match("--cam-phi"))   cliState.camPhi = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--quat") == 0) cliState.fracType = 1;
        else if (match("--qc")) {
            sscanf(argv[++i], "%f,%f,%f,%f",
                   &cliState.qc[0], &cliState.qc[1], &cliState.qc[2], &cliState.qc[3]);
        }
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: julia4d [options]\n"
                   "  --save <file.bmp>     Render and save (headless)\n"
                   "  --width <n>           Image width  (default 800)\n"
                   "  --height <n>          Image height (default 800)\n"
                   "  --c <r>,<i>           Julia constant c = r + i*i\n"
                   "  --origin <x>,<y>,<r>,<i>  Full 4D origin\n"
                   "  --rot-xy <deg>        Rotation in X-Y plane\n"
                   "  --rot-xr <deg>        Rotation in X-R plane\n"
                   "  --rot-xi <deg>        Rotation in X-I plane\n"
                   "  --rot-yr <deg>        Rotation in Y-R plane\n"
                   "  --rot-yi <deg>        Rotation in Y-I plane\n"
                   "  --rot-ri <deg>        Rotation in R-I plane\n"
                   "  --palette <n>         Color palette (0-4)\n"
                   "  --iter <n>            Max iterations\n"
                   "  --steps <n>           Ray march steps\n"
                   "  --cam-dist <f>        Camera distance\n"
                   "  --cam-theta <f>       Camera azimuth (rad)\n"
                   "  --cam-phi <f>         Camera elevation (rad)\n");
            return 0;
        }
    }

    // Apply rotations (in order: XY, XR, XI, YR, YI, RI)
    if (hasRot) {
        for (int p = 0; p < 6; p++) {
            if (cliRot[p] != 0.0f) {
                float rad = cliRot[p] * 3.14159265f / 180.0f;
                rotate4(cliState.basis, kPlanes[p].a, kPlanes[p].b, rad);
                cliState.rotAngles[p] = rad;
            }
        }
    }

    // ----- GLFW init -----
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    if (cliMode) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(
        cliMode ? saveW : 1600,
        cliMode ? saveH : 900,
        "4D Julia Set Explorer", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    if (!cliMode) glfwSwapInterval(1);

    // ----- GLAD -----
    if (!gladLoadGL(glfwGetProcAddress)) {
        fprintf(stderr, "Failed to initialize GLAD\n");
        glfwTerminate();
        return 1;
    }

    // ----- ImGui -----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    auto& colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg]       = ImVec4(0.04f, 0.04f, 0.10f, 0.95f);
    colors[ImGuiCol_FrameBg]        = ImVec4(0.08f, 0.08f, 0.18f, 1.00f);
    colors[ImGuiCol_Button]         = ImVec4(0.10f, 0.10f, 0.22f, 1.00f);
    colors[ImGuiCol_ButtonHovered]  = ImVec4(0.15f, 0.15f, 0.35f, 1.00f);
    colors[ImGuiCol_CheckMark]      = ImVec4(0.00f, 0.85f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]     = ImVec4(0.00f, 0.70f, 0.90f, 1.00f);
    colors[ImGuiCol_Header]         = ImVec4(0.10f, 0.10f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderHovered]  = ImVec4(0.15f, 0.15f, 0.40f, 1.00f);

    // ----- GLFW callbacks -----
    // MUST be set BEFORE ImGui_ImplGlfw_InitForOpenGL with install_callbacks=true.
    // ImGui saves these as "previous" callbacks and chains to them, so both
    // ImGui and our app receive events. If set AFTER, ImGui's callbacks get
    // overwritten and ImGui never sees mouse/keyboard input.
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetScrollCallback(window, scrollCallback);
    glfwSetKeyCallback(window, keyCallback);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 430");

    // ----- Compile shaders -----
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertSrc);

    GLuint fs2d = compileShader(GL_FRAGMENT_SHADER, kFrag2dSrc);
    GLuint prog2d = linkProgram(vs, fs2d);

    GLuint fs3d = compileShader(GL_FRAGMENT_SHADER, kFrag3dSrc);
    GLuint prog3d = linkProgram(vs, fs3d);

    glDeleteShader(vs);
    glDeleteShader(fs2d);
    glDeleteShader(fs3d);

    // ----- Cache uniform locations (2D) -----
    GLint u2d_origin    = glGetUniformLocation(prog2d, "u_origin");
    GLint u2d_u         = glGetUniformLocation(prog2d, "u_u");
    GLint u2d_v         = glGetUniformLocation(prog2d, "u_v");
    GLint u2d_resolution = glGetUniformLocation(prog2d, "u_resolution");
    GLint u2d_zoom      = glGetUniformLocation(prog2d, "u_zoom");
    GLint u2d_maxIter   = glGetUniformLocation(prog2d, "u_maxIter");
    GLint u2d_palette   = glGetUniformLocation(prog2d, "u_palette");
    GLint u2d_fracType  = glGetUniformLocation(prog2d, "u_fracType");
    GLint u2d_qc        = glGetUniformLocation(prog2d, "u_qc");

    // ----- Cache uniform locations (3D) -----
    GLint u3d_invPV     = glGetUniformLocation(prog3d, "u_invPV");
    GLint u3d_camPos    = glGetUniformLocation(prog3d, "u_camPos");
    GLint u3d_origin    = glGetUniformLocation(prog3d, "u_origin");
    GLint u3d_u         = glGetUniformLocation(prog3d, "u_u");
    GLint u3d_v         = glGetUniformLocation(prog3d, "u_v");
    GLint u3d_w         = glGetUniformLocation(prog3d, "u_w");
    GLint u3d_maxIter   = glGetUniformLocation(prog3d, "u_maxIter");
    GLint u3d_numSteps  = glGetUniformLocation(prog3d, "u_numSteps");
    GLint u3d_palette   = glGetUniformLocation(prog3d, "u_palette");
    GLint u3d_threshold = glGetUniformLocation(prog3d, "u_threshold");
    GLint u3d_fracType  = glGetUniformLocation(prog3d, "u_fracType");
    GLint u3d_qc        = glGetUniformLocation(prog3d, "u_qc");

    // ----- Geometry -----
    GLuint quadVAO = createQuadVAO();

    // ----- FBO for 3D -----
    FboState fbo;

    // ----- App state -----
    AppState state;
    if (cliMode || hasRot) state = cliState;
    state.mode = cliMode ? 1 : state.mode;  // 3D for headless
    g_state = &state;

    // ----- Headless render + save -----
    if (cliMode && saveFile) {
        glBindVertexArray(quadVAO);
        glViewport(0, 0, saveW, saveH);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        // Compute camera
        float eye[3] = {
            state.camTarget[0] + state.camDist * cosf(state.camPhi) * sinf(state.camTheta),
            state.camTarget[1] + state.camDist * sinf(state.camPhi),
            state.camTarget[2] + state.camDist * cosf(state.camPhi) * cosf(state.camTheta),
        };
        float up[3] = {0,1,0};
        float proj[16], view[16], pv[16], invPV[16];
        mat4Perspective(proj, 0.8f, (float)saveW/(float)saveH, 0.1f, 100.0f);
        mat4LookAt(view, eye, state.camTarget, up);
        mat4Multiply(pv, proj, view);
        mat4Inverse(invPV, pv);

        glUseProgram(prog3d);
        glUniformMatrix4fv(u3d_invPV, 1, GL_FALSE, invPV);
        glUniform3fv(u3d_camPos, 1, eye);
        glUniform4fv(u3d_origin, 1, state.origin);
        glUniform4fv(u3d_u, 1, state.basis[0]);
        glUniform4fv(u3d_v, 1, state.basis[1]);
        glUniform4fv(u3d_w, 1, state.basis[2]);
        glUniform1i(u3d_maxIter, state.maxIter3d);
        glUniform1i(u3d_numSteps, state.numSteps);
        glUniform1i(u3d_palette, state.palette);
        glUniform1f(u3d_threshold, state.threshold);
        glUniform1i(u3d_fracType, state.fracType);
        glUniform4fv(u3d_qc, 1, state.qc);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();

        // Read pixels (GL gives bottom-up BGR with GL_BGR)
        unsigned char* pixels = new unsigned char[saveW * saveH * 3];
        glReadPixels(0, 0, saveW, saveH, GL_BGR, GL_UNSIGNED_BYTE, pixels);
        saveBMP(saveFile, saveW, saveH, pixels);
        delete[] pixels;

        glDeleteProgram(prog2d);
        glDeleteProgram(prog3d);
        glDeleteVertexArrays(1, &quadVAO);
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ----- FPS tracking -----
    double lastTime = glfwGetTime();
    int    frameCount = 0;

    // ----- Main loop -----
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // --- Poll held keys ---
        if (!ImGui::GetIO().WantCaptureKeyboard) {
            float moveAmt = 0.05f;
            float rotAmt  = 0.04f;

            if (glfwGetKey(window, GLFW_KEY_LEFT)  == GLFW_PRESS)
                translate4(state.origin, state.basis, 0, -moveAmt, state.zoom);
            if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
                translate4(state.origin, state.basis, 0,  moveAmt, state.zoom);
            if (glfwGetKey(window, GLFW_KEY_UP)    == GLFW_PRESS)
                translate4(state.origin, state.basis, 1,  moveAmt, state.zoom);
            if (glfwGetKey(window, GLFW_KEY_DOWN)  == GLFW_PRESS)
                translate4(state.origin, state.basis, 1, -moveAmt, state.zoom);

            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
                translate4(state.origin, state.basis, 2,  moveAmt, state.zoom);
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
                translate4(state.origin, state.basis, 2, -moveAmt, state.zoom);
            if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
                translate4(state.origin, state.basis, 3,  moveAmt, state.zoom);
            if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
                translate4(state.origin, state.basis, 3, -moveAmt, state.zoom);

            if (glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_KP_ADD) == GLFW_PRESS) {
                state.zoom *= 1.02f;
                state.zoom = std::min(state.zoom, 1000.0f);
            }
            if (glfwGetKey(window, GLFW_KEY_MINUS) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS) {
                state.zoom /= 1.02f;
                state.zoom = std::max(state.zoom, 0.01f);
            }

            if (glfwGetKey(window, GLFW_KEY_COMMA)  == GLFW_PRESS)
                doRotate(state, state.plane, -rotAmt);
            if (glfwGetKey(window, GLFW_KEY_PERIOD) == GLFW_PRESS)
                doRotate(state, state.plane,  rotAmt);

            // 1-6 select rotation plane
            for (int k = 0; k < 6; k++) {
                if (glfwGetKey(window, GLFW_KEY_1 + k) == GLFW_PRESS)
                    state.plane = k;
            }
        }

        // --- ImGui frame ---
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);

        drawUI(state, fbW, fbH);

        // --- Render ---
        glBindVertexArray(quadVAO);

        if (state.mode == 0) {
            // 2D mode: render directly to default framebuffer
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, fbW, fbH);
            glClearColor(0.02f, 0.02f, 0.05f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            glUseProgram(prog2d);
            glUniform4fv(u2d_origin, 1, state.origin);
            glUniform4fv(u2d_u,      1, state.basis[0]);
            glUniform4fv(u2d_v,      1, state.basis[1]);
            glUniform2f(u2d_resolution, (float)fbW, (float)fbH);
            glUniform1f(u2d_zoom,    state.zoom);
            glUniform1i(u2d_maxIter, state.maxIter);
            glUniform1i(u2d_palette, state.palette);
            glUniform1i(u2d_fracType, state.fracType);
            glUniform4fv(u2d_qc, 1, state.qc);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        } else {
            // 3D mode: render to FBO, then blit
            int rw = std::max(1, (int)(fbW * state.renderScale));
            int rh = std::max(1, (int)(fbH * state.renderScale));
            ensureFbo(fbo, rw, rh);

            // Compute camera matrices
            float eye[3] = {
                state.camTarget[0] + state.camDist * cosf(state.camPhi) * sinf(state.camTheta),
                state.camTarget[1] + state.camDist * sinf(state.camPhi),
                state.camTarget[2] + state.camDist * cosf(state.camPhi) * cosf(state.camTheta),
            };
            float up[3] = {0.0f, 1.0f, 0.0f};

            float proj[16], view[16], pv[16], invPV[16];
            float aspect = (float)rw / (float)rh;
            mat4Perspective(proj, 0.8f, aspect, 0.1f, 100.0f);
            mat4LookAt(view, eye, state.camTarget, up);
            mat4Multiply(pv, proj, view);
            mat4Inverse(invPV, pv);

            // Render to FBO
            glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
            glViewport(0, 0, rw, rh);
            glClearColor(0.02f, 0.02f, 0.05f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            glUseProgram(prog3d);
            glUniformMatrix4fv(u3d_invPV, 1, GL_FALSE, invPV);
            glUniform3fv(u3d_camPos, 1, eye);
            glUniform4fv(u3d_origin, 1, state.origin);
            glUniform4fv(u3d_u,      1, state.basis[0]);
            glUniform4fv(u3d_v,      1, state.basis[1]);
            glUniform4fv(u3d_w,      1, state.basis[2]);
            glUniform1i(u3d_maxIter, state.maxIter3d);
            glUniform1i(u3d_numSteps, state.numSteps);
            glUniform1i(u3d_palette,  state.palette);
            glUniform1f(u3d_threshold, state.threshold);
            glUniform1i(u3d_fracType, state.fracType);
            glUniform4fv(u3d_qc, 1, state.qc);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            // Blit FBO to default framebuffer
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, rw, rh, 0, 0, fbW, fbH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        glBindVertexArray(0);

        // --- ImGui render ---
        ImGui::Render();
        glViewport(0, 0, fbW, fbH);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);

        // --- FPS ---
        frameCount++;
        double now = glfwGetTime();
        if (now - lastTime >= 1.0) {
            state.fps = (float)(frameCount / (now - lastTime));
            frameCount = 0;
            lastTime = now;
        }
    }

    // ----- Cleanup -----
    if (fbo.fbo) {
        glDeleteFramebuffers(1, &fbo.fbo);
        glDeleteTextures(1, &fbo.tex);
        glDeleteRenderbuffers(1, &fbo.rbo);
    }
    glDeleteProgram(prog2d);
    glDeleteProgram(prog3d);
    glDeleteVertexArrays(1, &quadVAO);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
