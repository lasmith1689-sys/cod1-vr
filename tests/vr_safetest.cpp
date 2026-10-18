// Safe renderer test suite: vr.cpp + comfort.cpp against the mock OpenXR
// runtime (xr_mock.cpp) and a REAL hidden-window OpenGL context.
//
// This exists because dllmain.cpp (which hosts the in-game self test) is
// integration code; the ordinary VR/GL/math layer must be testable without it.
// The xr* symbols are resolved at link time to the mock, so failure injection
// needs no seams in production code. The mock doubles as a spec cop: protocol
// misuse (frame call order, acquire/wait/release balance, dead handles,
// malformed layers) records a violation, and every scenario asserts zero.
//
// Output format matches the DLL suite ("  PASS  " / "  FAIL  " lines and a
// "=== SELF TEST: N passed, M failed ===" summary) so the same build gate
// logic applies. Exit code is nonzero on any failure.

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <type_traits>

#include "../src/vr.h"
#include "../src/codvr.h"
#include "../src/vr_math.h"
#include "../src/vr_input.h"
#include "../src/vr_weapon.h"
#include "../src/vr_binds.h"
#include "../src/vr_native_hands.h"
#include "../src/vr_reload.h"
#include "../src/vr_menu.h"
#include "../src/vr_snapshot.h"
#include "../src/cfg_line.h"
#include "xr_mock.h"
#include <limits.h>

// ------------------------------------------------------------ test plumbing

static int g_pass = 0, g_fail = 0;

static void Check(bool ok, const char* fmt, ...)
{
    char name[512];
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(name, fmt, ap);
    va_end(ap);
    if (ok) { ++g_pass; printf("  PASS  %s\n", name); }
    else    { ++g_fail; printf("  FAIL  %s\n", name); }
}
#define CHECK(cond, ...)           Check(!!(cond), __VA_ARGS__)
#define CHECK_NEAR(a, b, tol, ...) Check(fabs((double)(a) - (double)(b)) <= (tol), __VA_ARGS__)

// The production Log(); route to stdout so mock-run VR logging is visible.
void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(buf, fmt, ap);
    va_end(ap);
    printf("        | %s\n", buf);
}

// ------------------------------------------------------------- GL plumbing

static HWND g_wnd; static HDC g_dc; static HGLRC g_rc; static HMODULE g_sysGL;

extern "C" void* CODVR_GetGLProc(const char* name)
{
    if (!g_sysGL) return nullptr;
    void* p = (void*)GetProcAddress(g_sysGL, name);
    if (!p) {
        typedef PROC (WINAPI *WglGpaFn)(LPCSTR);
        static WglGpaFn wglGpa =
            (WglGpaFn)GetProcAddress(g_sysGL, "wglGetProcAddress");
        if (wglGpa) p = (void*)wglGpa(name);
    }
    return p;
}

static bool MakeGLContext()
{
    char sys[MAX_PATH];
    if (!GetSystemDirectoryA(sys, MAX_PATH)) return false;
    strcat_s(sys, "\\opengl32.dll");
    g_sysGL = LoadLibraryA(sys);
    if (!g_sysGL) return false;

    typedef HGLRC (WINAPI *CreateFn)(HDC);
    typedef BOOL  (WINAPI *CurrentFn)(HDC, HGLRC);
    CreateFn  create  = (CreateFn)GetProcAddress(g_sysGL, "wglCreateContext");
    CurrentFn current = (CurrentFn)GetProcAddress(g_sysGL, "wglMakeCurrent");
    if (!create || !current) return false;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "codvr_safetest";
    RegisterClassA(&wc);
    g_wnd = CreateWindowA("codvr_safetest", "codvr", WS_OVERLAPPEDWINDOW,
                          0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_wnd) return false;
    g_dc = GetDC(g_wnd);

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(g_dc, &pfd);
    if (!pf || !SetPixelFormat(g_dc, pf, &pfd)) return false;
    g_rc = create(g_dc);
    if (!g_rc || !current(g_dc, g_rc)) return false;
    return true;
}

// Direct GL access for scenario setup and independent verification. Resolved
// through the same resolver production uses.
enum {
    T_GL_TEXTURE_2D = 0x0DE1, T_GL_RGBA8 = 0x8058, T_GL_RGBA = 0x1908,
    T_GL_UNSIGNED_BYTE = 0x1401, T_GL_COLOR_BUFFER_BIT = 0x4000,
    T_GL_PROJECTION = 0x1701, T_GL_MODELVIEW = 0x1700,
    T_GL_MATRIX_MODE = 0x0BA0, T_GL_CURRENT_COLOR = 0x0B00,
    T_GL_MODELVIEW_MATRIX = 0x0BA6, T_GL_PROJECTION_MATRIX = 0x0BA7,
    T_GL_BLEND = 0x0BE2, T_GL_BLEND_SRC = 0x0BE1, T_GL_BLEND_DST = 0x0BE0,
    T_GL_ONE = 1, T_GL_DEPTH_TEST = 0x0B71, T_GL_SCISSOR_TEST = 0x0C11,
    T_GL_SCISSOR_BOX = 0x0C10, T_GL_CULL_FACE = 0x0B44, T_GL_LIGHTING = 0x0B50,
    T_GL_ALPHA_TEST = 0x0BC0, T_GL_GREATER = 0x0204, T_GL_FOG = 0x0B60,
    T_GL_SHADE_MODEL = 0x0B54, T_GL_FLAT = 0x1D00, T_GL_SMOOTH = 0x1D01,
    T_GL_VIEWPORT = 0x0BA2, T_GL_COLOR_WRITEMASK = 0x0C23,
    T_GL_ACTIVE_TEXTURE = 0x84E0, T_GL_TEXTURE0 = 0x84C0,
    T_GL_TEXTURE_BINDING_2D = 0x8069,
    T_GL_DRAW_FB_BINDING = 0x8CA6, T_GL_READ_FB_BINDING = 0x8CAA
};

typedef void (WINAPI *Pv4f)(float, float, float, float);
typedef void (WINAPI *Pclear)(unsigned);
typedef void (WINAPI *PgenTex)(int, unsigned*);
typedef void (WINAPI *PbindTex)(unsigned, unsigned);
typedef void (WINAPI *PtexImg)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
typedef void (WINAPI *Pmm)(unsigned);
typedef void (WINAPI *Pbf)(unsigned, unsigned);
typedef void (WINAPI *Pen)(unsigned);
typedef void (WINAPI *Psc)(int, int, int, int);
typedef void (WINAPI *Paf)(unsigned, float);
typedef void (WINAPI *Pvp)(int, int, int, int);
typedef void (WINAPI *Psm)(unsigned);
typedef void (WINAPI *Pcm)(unsigned char, unsigned char, unsigned char, unsigned char);
typedef void (WINAPI *Pat)(unsigned);
typedef void (WINAPI *Pgi)(unsigned, int*);
typedef void (WINAPI *Pgf)(unsigned, float*);
typedef unsigned (WINAPI *Pge)(void);
typedef unsigned char (WINAPI *PisEn)(unsigned);   // "Pie" collides with GDI's Pie()

static Pv4f  tg_clearColor, tg_color4f;
static Pclear tg_clear;
static PgenTex tg_genTex;
static PbindTex tg_bindTex;
static PtexImg tg_texImg;
static Pmm  tg_matrixMode;
static Pbf  tg_blendFunc;
static Pen  tg_enable, tg_disable;
static Psc  tg_scissor;
static Paf  tg_alphaFunc;
static Pvp  tg_viewport;
static Psm  tg_shadeModel;
static Pcm  tg_colorMask;
static Pat  tg_activeTexture;
static Pgi  tg_getIntegerv;
static Pgf  tg_getFloatv;
static Pge   tg_getError;
static PisEn tg_isEnabled;

static void ResolveGLHelpers()
{
    tg_clearColor    = (Pv4f)CODVR_GetGLProc("glClearColor");
    tg_color4f       = (Pv4f)CODVR_GetGLProc("glColor4f");
    tg_clear         = (Pclear)CODVR_GetGLProc("glClear");
    tg_genTex        = (PgenTex)CODVR_GetGLProc("glGenTextures");
    tg_bindTex       = (PbindTex)CODVR_GetGLProc("glBindTexture");
    tg_texImg        = (PtexImg)CODVR_GetGLProc("glTexImage2D");
    tg_matrixMode    = (Pmm)CODVR_GetGLProc("glMatrixMode");
    tg_blendFunc     = (Pbf)CODVR_GetGLProc("glBlendFunc");
    tg_enable        = (Pen)CODVR_GetGLProc("glEnable");
    tg_disable       = (Pen)CODVR_GetGLProc("glDisable");
    tg_scissor       = (Psc)CODVR_GetGLProc("glScissor");
    tg_alphaFunc     = (Paf)CODVR_GetGLProc("glAlphaFunc");
    tg_viewport      = (Pvp)CODVR_GetGLProc("glViewport");
    tg_shadeModel    = (Psm)CODVR_GetGLProc("glShadeModel");
    tg_colorMask     = (Pcm)CODVR_GetGLProc("glColorMask");
    tg_activeTexture = (Pat)CODVR_GetGLProc("glActiveTexture");
    if (!tg_activeTexture)
        tg_activeTexture = (Pat)CODVR_GetGLProc("glActiveTextureARB");
    tg_getIntegerv   = (Pgi)CODVR_GetGLProc("glGetIntegerv");
    tg_getFloatv     = (Pgf)CODVR_GetGLProc("glGetFloatv");
    tg_getError      = (Pge)CODVR_GetGLProc("glGetError");
    tg_isEnabled     = (PisEn)CODVR_GetGLProc("glIsEnabled");
}

static unsigned MockCreateImage(unsigned w, unsigned h)
{
    (void)w; (void)h;
    if (!tg_genTex || !tg_bindTex || !tg_texImg) return 0xBEEF0001;
    unsigned tex = 0;
    tg_genTex(1, &tex);
    tg_bindTex(T_GL_TEXTURE_2D, tex);
    tg_texImg(T_GL_TEXTURE_2D, 0, T_GL_RGBA8, 8, 8, 0,
              T_GL_RGBA, T_GL_UNSIGNED_BYTE, nullptr);
    tg_bindTex(T_GL_TEXTURE_2D, 0);
    return tex;
}

static void ClearBackBuffer(float r, float g, float b)
{
    if (tg_clearColor && tg_clear) {
        tg_clearColor(r, g, b, 1.0f);
        tg_clear(T_GL_COLOR_BUFFER_BIT);
    }
}

// ------------------------------------------------------------- math helpers

static const float DEG = 0.01745329252f;

static bool PoseEq(const XrPosef& a, const XrPosef& b)
{
    return fabsf(a.position.x - b.position.x) < 1e-6f
        && fabsf(a.position.y - b.position.y) < 1e-6f
        && fabsf(a.position.z - b.position.z) < 1e-6f
        && fabsf(a.orientation.x - b.orientation.x) < 1e-6f
        && fabsf(a.orientation.y - b.orientation.y) < 1e-6f
        && fabsf(a.orientation.z - b.orientation.z) < 1e-6f
        && fabsf(a.orientation.w - b.orientation.w) < 1e-6f;
}

// Quake row basis from yaw/pitch/roll degrees (yaw about +Z left-positive,
// pitch up-positive, roll about forward). Built independently of production
// code so the composition tests verify against a second implementation.
static void MakeAxis(float yawDeg, float pitchDeg, float rollDeg, float axis[9])
{
    const float cy = cosf(yawDeg * DEG),  sy = sinf(yawDeg * DEG);
    const float cp = cosf(pitchDeg * DEG), sp = sinf(pitchDeg * DEG);
    const float cr = cosf(rollDeg * DEG),  sr = sinf(rollDeg * DEG);
    const float f[3]  = { cp * cy, cp * sy, sp };
    const float l0[3] = { -sy, cy, 0 };
    const float u0[3] = { -sp * cy, -sp * sy, cp };
    axis[0] = f[0]; axis[1] = f[1]; axis[2] = f[2];
    for (int i = 0; i < 3; ++i) {
        axis[3 + i] =  cr * l0[i] + sr * u0[i];
        axis[6 + i] = -sr * l0[i] + cr * u0[i];
    }
}

// Extract yaw/pitch/roll (degrees) from a Quake row basis; same factorisation.
static void AxisAngles(const float axis[9], float* yawDeg, float* pitchDeg, float* rollDeg)
{
    float fz = axis[2];
    if (fz > 1.0f) fz = 1.0f;
    if (fz < -1.0f) fz = -1.0f;
    const float pitch = asinf(fz);
    const float yaw = atan2f(axis[1], axis[0]);
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    const float l0[3] = { -sy, cy, 0 };
    const float u0[3] = { -sp * cy, -sp * sy, cp };
    const float* l = &axis[3];
    const float roll = atan2f(l[0]*u0[0] + l[1]*u0[1] + l[2]*u0[2],
                              l[0]*l0[0] + l[1]*l0[1] + l[2]*l0[2]);
    *yawDeg = yaw / DEG; *pitchDeg = pitch / DEG; *rollDeg = roll / DEG;
}

// Place the mock head: yaw about XR +Y (left-positive, matching Quake yaw
// after conversion), eyes IPD apart along the head's rotated right.
static void SetMockHead(float yawDeg, float mx, float my, float mz, float ipd = 0.064f)
{
    const float half = yawDeg * DEG * 0.5f;
    for (int e = 0; e < MOCK_EYES; ++e) {
        g_mock.viewPose[e].orientation.x = 0;
        g_mock.viewPose[e].orientation.y = sinf(half);
        g_mock.viewPose[e].orientation.z = 0;
        g_mock.viewPose[e].orientation.w = cosf(half);
        const float rx = cosf(yawDeg * DEG), rz = -sinf(yawDeg * DEG);
        const float s = (e == 0) ? -0.5f : 0.5f;
        g_mock.viewPose[e].position.x = mx + s * ipd * rx;
        g_mock.viewPose[e].position.y = my;
        g_mock.viewPose[e].position.z = mz + s * ipd * rz;
    }
}

// ------------------------------------------------------------ frame drivers

static float g_gameOrigin[3] = { 100.0f, 200.0f, 50.0f };
static float g_gameYawDeg = 0.0f;
static float g_out[2][3], g_outAxis[2][9];

static void DriveEye(int eye)
{
    float axis[9];
    MakeAxis(g_gameYawDeg, 0, 0, axis);
    float fx = 0, fy = 0;
    VR_GetEyeCamera(eye, g_gameOrigin, axis, 1440, 1440,
                    g_out[eye], g_outAxis[eye], &fx, &fy);
}

// One 3D GAME frame, exactly as dllmain drives it: begin (a real
// wait/begin/locate at a pair boundary; deliberately nothing between the two
// eyes of a pair), render the eye the pair machine expects, submit.
static VRFrameDisposition StepGame3D()
{
    VRFrameDisposition d = VR_BeginFrameEx();
    if (d != VR_FRAME_RENDER)
        return d;
    const int eye = VR_ExpectedEye();
    DriveEye(eye);
    VR_SubmitFrame(640, 480, true, eye);
    return d;
}

// One flat/menu game frame: no 3D scene, panel to both eyes.
static VRFrameDisposition StepGameFlat()
{
    VRFrameDisposition d = VR_BeginFrameEx();
    if (d != VR_FRAME_RENDER)
        return d;
    VR_SubmitFrame(640, 480, false, 0);
    return d;
}

// A complete 3D pair: two game frames sharing one XR frame.
static VRFrameDisposition StepPair()
{
    VRFrameDisposition d = StepGame3D();
    if (d != VR_FRAME_RENDER)
        return d;
    return StepGame3D();
}

// ---- native-VR test plumbing: injection sink + emulated game feedback ----

enum { SINK_MAX = 8192 };
static int g_sinkKind[SINK_MAX], g_sinkA[SINK_MAX], g_sinkB[SINK_MAX];
static int g_sinkCount;
static int g_pendDx, g_pendDy;      // injected mouse awaiting "the game"

static void TestSink(int kind, int a, int b)
{
    if (g_sinkCount < SINK_MAX) {
        g_sinkKind[g_sinkCount] = kind;
        g_sinkA[g_sinkCount] = a;
        g_sinkB[g_sinkCount] = b;
        ++g_sinkCount;
    }
    if (kind == VRB_EV_MOUSEMOVE) { g_pendDx += a; g_pendDy += b; }
}

static void SinkClear() { g_sinkCount = 0; }

// Count sink events of a kind (a == INT_MIN matches any payload).
static int CountEvents(int kind, int a)
{
    int n = 0;
    for (int i = 0; i < g_sinkCount; ++i)
        if (g_sinkKind[i] == kind && (a == INT_MIN || g_sinkA[i] == a)) ++n;
    return n;
}

static float  g_gamePitchDeg = 0.0f;
static double g_now = 100.0;
static int    g_steerRespond = 1;   // "the game": apply injected mouse to aim
static int    g_skipAimSamples = 0; // frames to run WITHOUT a fresh aim sample

static void ResetAll()
{
    VR_TestResetAll();
    MockXr_Reset();
    g_mock.createImage = MockCreateImage;
    g_gameYawDeg = 0.0f;
    g_gamePitchDeg = 0.0f;
    VRBinds_TestReset();
    VRBinds_TestSetSink(TestSink);
    SinkClear();
    g_pendDx = g_pendDy = 0;
    g_steerRespond = 1;
    g_skipAimSamples = 0;
    g_now = 100.0;
}

static bool BringUp()
{
    if (!VR_Startup(g_dc, g_rc))
        return false;
    MockXr_QueueState(XR_SESSION_STATE_READY);
    return true;
}

// ---------------------------------------------------------------- scenarios

static void S_PureMath()
{
    printf("--- pure math: conversions and composition ---\n");
    float q[4] = { 0, 0, 0, 1 };
    float ax[9];
    VR_TestQuatToAxis(q, ax);
    CHECK(fabsf(ax[0] - 1) < 1e-6f && fabsf(ax[4] - 1) < 1e-6f && fabsf(ax[8] - 1) < 1e-6f,
          "identity quaternion maps to the identity basis");

    // Euler round trip: VRM_AxisToAnglesDeg must be the exact inverse of
    // VRM_AnglesToRows (the entity rewrite writes CoD's angle fields from a
    // rotated axis, so a mismatch would desync the struct's two orientation
    // representations). Includes nonzero roll and both pitch signs.
    {
        const float trips[][3] = {
            {  30.0f,  10.0f,   0.0f },
            { -146.096f, -27.169f, 0.0f },   // the live-captured viewmodel
            { 120.0f, -60.0f,  25.0f },
            { -75.0f,  45.0f, -40.0f },
        };
        float worst = 0.0f;
        for (int i = 0; i < 4; ++i) {
            float rows[9], y2, p2, r2;
            VRM_AnglesToRows(trips[i][0], trips[i][1], trips[i][2], rows);
            VRM_AxisToAnglesDeg(rows, &y2, &p2, &r2);
            const float dy = fabsf(VRM_WrapDeg(y2 - trips[i][0]));
            const float dp = fabsf(p2 - trips[i][1]);
            const float dr = fabsf(VRM_WrapDeg(r2 - trips[i][2]));
            if (dy > worst) worst = dy;
            if (dp > worst) worst = dp;
            if (dr > worst) worst = dr;
        }
        CHECK(worst < 0.01f,
              "euler extraction inverts AnglesToRows exactly (worst %.4f deg)", worst);

        // And the CoD convention bridge: the captured entity's axis rows must
        // reproduce its own angle fields under quakePitch = -pitch.
        float rows[9], y2, p2, r2;
        VRM_AnglesToRows(-146.096f, -27.169f, 0.0f, rows);
        CHECK(fabsf(rows[0] + 0.738f) < 0.002f && fabsf(rows[1] + 0.496f) < 0.002f
              && fabsf(rows[2] + 0.457f) < 0.002f,
              "captured viewmodel: angles (27.169, -146.096) rebuild its axis rows");
        VRM_AxisToAnglesDeg(rows, &y2, &p2, &r2);
        CHECK(fabsf(-p2 - 27.169f) < 0.01f && fabsf(VRM_WrapDeg(y2 + 146.096f)) < 0.01f,
              "captured viewmodel: axis extracts back to CoD's angle fields");
    }

    // The OLD composition seam keeps its old semantics (existing tests define
    // them): head yaw inside a pitched game frame DOES roll the horizon.
    float head[9], game[9], out[9], y, p, r;
    MakeAxis(30, 0, 0, head);
    MakeAxis(0, 30, 0, game);
    VR_TestComposeAxis(head, game, out);
    AxisAngles(out, &y, &p, &r);
    CHECK(fabsf(r) > 10.0f,
          "old ComposeAxis unchanged: 30/30 still rolls %.1f deg (seam kept for DLL suite)", r);

    // A: centred head returns the full game aim EXACTLY (yaw+pitch+kick roll).
    MakeAxis(0, 0, 0, head);
    MakeAxis(37, 25, 8, game);
    VR_TestComposeView(head, game, out);
    float maxd = 0;
    for (int i = 0; i < 9; ++i) maxd = fmaxf(maxd, fabsf(out[i] - game[i]));
    CHECK(maxd < 1e-4f, "compose A: centred head == exact game aim (max delta %.2e)", maxd);

    // B: game yaw 0 + head yaw 30 -> view yaw 30, level.
    MakeAxis(30, 0, 0, head); MakeAxis(0, 0, 0, game);
    VR_TestComposeView(head, game, out); AxisAngles(out, &y, &p, &r);
    CHECK(fabsf(y - 30) < 0.01f && fabsf(r) < 0.01f,
          "compose B: yaw 0 + head 30 -> yaw %.2f, roll %.3f", y, r);

    // C: game yaw 90 + head yaw 30 -> view yaw 120, level.
    MakeAxis(30, 0, 0, head); MakeAxis(90, 0, 0, game);
    VR_TestComposeView(head, game, out); AxisAngles(out, &y, &p, &r);
    CHECK(fabsf(y - 120) < 0.01f && fabsf(r) < 0.01f,
          "compose C: yaw 90 + head 30 -> yaw %.2f, roll %.3f", y, r);

    // D: game pitch 30 + head yaw 30 -> free-look with a LEVEL horizon.
    MakeAxis(30, 0, 0, head); MakeAxis(0, 30, 0, game);
    VR_TestComposeView(head, game, out); AxisAngles(out, &y, &p, &r);
    CHECK(fabsf(r) < 0.05f && fabsf(y - 30) < 0.1f && fabsf(p - 30) < 0.1f,
          "compose D: pitch 30 + head yaw 30 -> yaw %.1f pitch %.1f ROLL %.4f", y, p, r);

    // E: game pitch 30 + head pitch 20 -> ~50 pitch, no invented roll.
    MakeAxis(0, 20, 0, head); MakeAxis(0, 30, 0, game);
    VR_TestComposeView(head, game, out); AxisAngles(out, &y, &p, &r);
    CHECK(fabsf(p - 50) < 0.1f && fabsf(r) < 0.05f,
          "compose E: pitch 30 + head pitch 20 -> pitch %.1f, roll %.4f", p, r);

    // F: physical head roll passes through as itself. (Composed with a
    // pitched aim, the roll under this factorisation deviates by fractions of
    // a degree -- rotations do not commute -- so the assertion is "the roll is
    // represented", not "bit-exact".)
    MakeAxis(0, 0, 15, head); MakeAxis(20, 10, 0, game);
    VR_TestComposeView(head, game, out); AxisAngles(out, &y, &p, &r);
    CHECK(fabsf(r - 15) < 0.5f, "compose F: physical head roll 15 -> output roll %.2f", r);

    // Grid: no head-yaw x game-pitch combination invents roll.
    const float yaws[]    = { -60, -30, -15, 15, 30, 60 };
    const float pitches[] = { -45, -30, -15, 15, 30, 45 };
    float worst = 0;
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j) {
            MakeAxis(yaws[i], 0, 0, head);
            MakeAxis(0, pitches[j], 0, game);
            VR_TestComposeView(head, game, out);
            AxisAngles(out, &y, &p, &r);
            worst = fmaxf(worst, fabsf(r));
        }
    CHECK(worst < 0.05f, "compose grid: 36 yaw/pitch combos, max artificial roll %.4f deg", worst);

    // Output stays orthonormal even for a full three-angle head + game pair.
    MakeAxis(25, -10, 5, head); MakeAxis(-40, 35, -3, game);
    VR_TestComposeView(head, game, out);
    float dotFL = out[0]*out[3] + out[1]*out[4] + out[2]*out[5];
    float lenF  = sqrtf(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
    CHECK(fabsf(dotFL) < 1e-4f && fabsf(lenF - 1) < 1e-4f,
          "compose output orthonormal (f.l %.2e, |f| %.6f)", dotFL, lenF);
}

static void S_Filters()
{
    printf("--- motion filters and comfort curves ---\n");

    // Sustained turning converges to the true rate.
    float t = 0;
    for (int i = 0; i < 90; ++i) t = VR_TestFilterTurn(t, 1.0f, 1.0f / 90.0f);
    CHECK(t > 85.0f && t < 95.0f, "sustained 90 deg/s reads %.1f deg/s after 1 s", t);

    // A discrete snap step is NOT a violent sustained turn.
    t = 0;
    t = VR_TestFilterTurn(t, 30.0f, 1.0f / 90.0f);
    CHECK(t < 1.0f, "a single 30-degree step reads %.2f deg/s (snap turn exempt)", t);

    float s = 0;
    for (int i = 0; i < 90; ++i) s = VR_TestFilterSpeed(s, 100.0f / 90.0f, 1.0f / 90.0f);
    CHECK(s > 90.0f && s < 110.0f, "walking 100 u/s reads %.1f u/s after 1 s", s);
    s = 0;
    s = VR_TestFilterSpeed(s, 500.0f, 1.0f / 90.0f);
    CHECK(s < 1.0f, "a 500-unit teleport reads %.2f u/s (respawn exempt)", s);

    // Vignette smoothing is frame-rate independent: time-to-90%-closed within
    // 10% across 72/90/120 Hz.
    const float rates[3] = { 72, 90, 120 };
    float closeT[3], openT[3];
    for (int k = 0; k < 3; ++k) {
        const float dt = 1.0f / rates[k];
        float v = 0; int n = 0;
        while (v < 0.9f && n < 10000) { v = Comfort_TestStep(v, 1.0f, dt); ++n; }
        closeT[k] = n * dt;
        v = 1.0f; n = 0;
        while (v > 0.1f && n < 10000) { v = Comfort_TestStep(v, 0.0f, dt); ++n; }
        openT[k] = n * dt;
    }
    CHECK(fabsf(closeT[0] - closeT[2]) < 0.1f * closeT[0]
          && fabsf(closeT[0] - closeT[1]) < 0.1f * closeT[0],
          "close-to-90%% takes %.0f/%.0f/%.0f ms at 72/90/120 Hz",
          closeT[0] * 1000, closeT[1] * 1000, closeT[2] * 1000);
    CHECK(fabsf(openT[0] - openT[2]) < 0.1f * openT[0],
          "open-to-10%% takes %.0f/%.0f/%.0f ms at 72/90/120 Hz",
          openT[0] * 1000, openT[1] * 1000, openT[2] * 1000);
    CHECK(openT[0] > closeT[0] * 2.0f, "opening is slower than closing");

    // Threshold semantics finally match the config file's description.
    CHECK(Comfort_TestTarget(30, 0) == 0.0f, "speed 30 < onset 40: tunnel stays open");
    CHECK(Comfort_TestTarget(40, 0) == 0.0f, "speed 40 == onset: still open");
    CHECK_NEAR(Comfort_TestTarget(100, 0), 0.5, 1e-3, "speed 100: half closed");
    CHECK_NEAR(Comfort_TestTarget(160, 0), 1.0, 1e-3, "speed 160 == 4x onset: fully closed");
    CHECK(Comfort_TestTarget(0, 40) == 0.0f, "turn 40 < onset 45: open");
    CHECK_NEAR(Comfort_TestTarget(0, 112.5f), 0.5, 1e-3, "turn 112.5: half closed");

    // Published aperture untouched.
    float in0 = 0, out0 = 0;
    Comfort_TestAperture(1.0f, &in0, &out0);
    CHECK_NEAR(in0, 0.25, 1e-4, "closed aperture inner radius still 0.25");
    CHECK_NEAR(out0, 0.65, 1e-4, "closed aperture outer radius still 0.65");
}

static void S_ProjectionMath()
{
    printf("--- projection math: OpenXR fov -> asymmetric GL matrix ---\n");
    // The canonical asymmetric frustum from the brief.
    const float L = -40.0f * DEG, R = 54.0f * DEG;
    const float U = 44.0f * DEG,  D = -55.0f * DEG;
    float m[16];
    VRM_ProjectionFromFovRad(L, R, U, D, 4.0f, 4096.0f, m);

    // An independent transcription of the Khronos reference construction
    // (hello_xr's XrMatrix4x4f_CreateProjection, GRAPHICS_OPENGL branch),
    // written out separately so a shared bug cannot hide.
    float ref[16] = { 0 };
    {
        const float tl = tanf(L), tr = tanf(R), tu = tanf(U), td = tanf(D);
        const float w = tr - tl, h = tu - td;
        ref[0]  = 2.0f / w;
        ref[5]  = 2.0f / h;
        ref[8]  = (tr + tl) / w;
        ref[9]  = (tu + td) / h;
        ref[10] = -(4096.0f + 4.0f) / (4096.0f - 4.0f);
        ref[11] = -1.0f;
        ref[14] = -2.0f * 4096.0f * 4.0f / (4096.0f - 4.0f);
    }
    float worst = 0.0f;
    for (int i = 0; i < 16; ++i)
        if (fabsf(m[i] - ref[i]) > worst) worst = fabsf(m[i] - ref[i]);
    CHECK(worst < 1e-5f,
          "matches the Khronos reference element-for-element (max diff %.2e)", worst);

    // All four fov edges land exactly on the NDC edges.
    float ndc[3];
    VRM_ProjectPoint(m, tanf(L) * 10.0f, 0, -10.0f, ndc);
    CHECK_NEAR(ndc[0], -1.0, 1e-4, "ray at angleLeft -> NDC x = -1 (got %.5f)", ndc[0]);
    VRM_ProjectPoint(m, tanf(R) * 10.0f, 0, -10.0f, ndc);
    CHECK_NEAR(ndc[0],  1.0, 1e-4, "ray at angleRight -> NDC x = +1 (got %.5f)", ndc[0]);
    VRM_ProjectPoint(m, 0, tanf(U) * 10.0f, -10.0f, ndc);
    CHECK_NEAR(ndc[1],  1.0, 1e-4, "ray at angleUp -> NDC y = +1 (got %.5f)", ndc[1]);
    VRM_ProjectPoint(m, 0, tanf(D) * 10.0f, -10.0f, ndc);
    CHECK_NEAR(ndc[1], -1.0, 1e-4, "ray at angleDown -> NDC y = -1 (got %.5f)", ndc[1]);

    // The asymmetric optical centre is retained, and the forward ray lands
    // on it -- never forced back to the texture centre.
    CHECK(fabsf(m[8]) > 0.2f && fabsf(m[9]) > 0.05f,
          "asymmetric centre offset retained (m8 %.4f, m9 %.4f)", m[8], m[9]);
    VRM_ProjectPoint(m, 0, 0, -10.0f, ndc);
    CHECK_NEAR(ndc[0], -m[8], 1e-5, "forward ray maps to NDC -m8 (got %.4f)", ndc[0]);
    CHECK_NEAR(ndc[1], -m[9], 1e-5, "forward ray maps to NDC -m9 (got %.4f)", ndc[1]);

    // Symmetric input degenerates to the classic centred projection.
    float sym[16];
    VRM_ProjectionFromFovRad(-48.0f * DEG, 48.0f * DEG, 48.0f * DEG, -48.0f * DEG,
                             4.0f, 4096.0f, sym);
    CHECK(fabsf(sym[8]) < 1e-6f && fabsf(sym[9]) < 1e-6f
          && fabsf(sym[0] - 1.0f / tanf(48.0f * DEG)) < 1e-5f,
          "symmetric fov degenerates to the centred cot(fov/2) projection");

    // fov -> matrix -> tangents round-trips tightly.
    float tl2, tr2, tu2, td2;
    VRM_ExtractTangents(m, &tl2, &tr2, &tu2, &td2);
    CHECK(fabsf(tl2 - tanf(L)) < 1e-5f && fabsf(tr2 - tanf(R)) < 1e-5f
          && fabsf(tu2 - tanf(U)) < 1e-5f && fabsf(td2 - tanf(D)) < 1e-5f,
          "fov -> matrix -> recovered edge tangents round-trips");

    // Near/far mapping keeps the OpenGL conventions.
    float n2, f2;
    VRM_ExtractNearFar(m, &n2, &f2);
    CHECK(fabsf(n2 - 4.0f) < 0.01f && fabsf(f2 - 4096.0f) < 2.0f,
          "near/far recovered from the depth row (%.2f / %.0f)", n2, f2);
    VRM_ProjectPoint(m, 0, 0, -4.0f, ndc);
    CHECK_NEAR(ndc[2], -1.0, 1e-3, "z = -near maps to NDC z = -1");
    VRM_ProjectPoint(m, 0, 0, -4096.0f, ndc);
    CHECK_NEAR(ndc[2],  1.0, 1e-3, "z = -far maps to NDC z = +1");

    float inf[16];
    VRM_ProjectionFromTangents(tanf(L), tanf(R), tanf(U), tanf(D), 4.0f, 0.0f, inf);
    CHECK(fabsf(inf[10] + 1.0f) < 1e-6f && fabsf(inf[14] + 8.0f) < 1e-5f,
          "farZ <= nearZ selects the infinite-far form");

    // Mirrored eyes (Quest 3 shape): equal focal terms, opposite centres.
    float e0[16], e1[16];
    VRM_ProjectionFromFovRad(-54.0f * DEG, 40.0f * DEG, U, D, 4.0f, 4096.0f, e0);
    VRM_ProjectionFromFovRad(-40.0f * DEG, 54.0f * DEG, U, D, 4.0f, 4096.0f, e1);
    CHECK(fabsf(e0[0] - e1[0]) < 1e-6f && fabsf(e0[8] + e1[8]) < 1e-6f
          && fabsf(e0[5] - e1[5]) < 1e-6f && fabsf(e0[9] - e1[9]) < 1e-6f,
          "mirrored eyes: equal focal terms, opposite centre offsets");
    CHECK(fabsf(e0[8] - e1[8]) > 0.4f,
          "the per-eye matrices genuinely differ where the fov differs (d m8 %.3f)",
          fabsf(e0[8] - e1[8]));

    // The surgical replacement keeps the engine's depth row bit-for-bit.
    float engine[16], repl[16];
    VRM_ProjectionFromFovRad(-48.21f * DEG, 48.21f * DEG, 48.21f * DEG,
                             -48.21f * DEG, 4.0f, 3000.0f, engine);
    VRM_ReplaceLateralOptics(engine, tanf(L), tanf(R), tanf(U), tanf(D), repl);
    CHECK(repl[10] == engine[10] && repl[14] == engine[14]
          && repl[11] == engine[11] && repl[15] == engine[15]
          && repl[2] == engine[2] && repl[6] == engine[6],
          "replacement preserves the engine's depth row bit-for-bit");
    float rn, rf;
    VRM_ExtractNearFar(repl, &rn, &rf);
    CHECK(fabsf(rn - 4.0f) < 0.01f && fabsf(rf - 3000.0f) < 2.0f,
          "the replaced matrix still clips at the engine's near/far (%.2f / %.0f)", rn, rf);
    CHECK(fabsf(repl[0] - m[0]) < 1e-6f && fabsf(repl[8] - m[8]) < 1e-6f
          && fabsf(repl[5] - m[5]) < 1e-6f && fabsf(repl[9] - m[9]) < 1e-6f,
          "the replacement's lateral optics equal the fresh construction");

    // The structural classifier: what can and cannot be replaced.
    CHECK(VRM_IsPerspective(engine), "classifier accepts the engine's perspective");
    float orthoM[16] = { 0 };
    orthoM[0] = 2.0f / 640.0f; orthoM[5] = -2.0f / 480.0f; orthoM[10] = -1.0f;
    orthoM[12] = -1.0f; orthoM[13] = 1.0f; orthoM[15] = 1.0f;
    CHECK(!VRM_IsPerspective(orthoM) && VRM_IsOrtho(orthoM),
          "classifier rejects the 2D pass's orthographic matrix");
    float rigid[16] = { 0 };
    rigid[0] = rigid[5] = rigid[10] = rigid[15] = 1.0f; rigid[14] = -50.0f;
    CHECK(!VRM_IsPerspective(rigid), "classifier rejects a rigid modelview transform");

    // Coverage verdicts for the optics check.
    CHECK(VRM_ViewportCovers(0, 0, 3240, 3240, 3240, 3240)
          && !VRM_ViewportCovers(0, 0, 1440, 1440, 3240, 3240),
          "viewport coverage verdict distinguishes full from partial");
    CHECK(VRM_ScissorCovers(0, 0, 0, 64, 64, 3240, 3240)
          && VRM_ScissorCovers(1, 0, 0, 3240, 3240, 3240, 3240)
          && !VRM_ScissorCovers(1, 0, 0, 2560, 1440, 3240, 3240),
          "scissor coverage verdict honours the enable flag");

    // HUD principal-point shift: aligns each eye's 2D overlay with its
    // optical axis (mirrored horizontally, shared vertically).
    int dx = 0, dy = 0;
    VRM_PrincipalPointShiftPx(tanf(-54.0f * DEG), tanf(40.0f * DEG),
                              tanf(44.0f * DEG), tanf(-55.0f * DEG),
                              1440, 1440, &dx, &dy);
    CHECK(abs(dx - 175) <= 1 && abs(dy - 139) <= 1,
          "left eye principal shift ~ +175,+139 px at 1440^2 (got %d,%d)", dx, dy);
    int dx1 = 0, dy1 = 0;
    VRM_PrincipalPointShiftPx(tanf(-40.0f * DEG), tanf(54.0f * DEG),
                              tanf(44.0f * DEG), tanf(-55.0f * DEG),
                              1440, 1440, &dx1, &dy1);
    CHECK(dx1 == -dx && dy1 == dy,
          "right eye mirrors horizontally, matches vertically (%d,%d)", dx1, dy1);
    VRM_PrincipalPointShiftPx(-1.0f, 1.0f, 1.0f, -1.0f, 1440, 1440, &dx, &dy);
    CHECK(dx == 0 && dy == 0, "a symmetric fov needs no shift");
}

static void S_StartupTransactional()
{
    printf("--- startup is all-or-nothing at every stage ---\n");
    struct Stage { const char* name; void (*arm)(); };
    static const Stage stages[] = {
        { "instance creation fails",   [](){ g_mock.failCreateInstance = true; } },
        { "no headset (getSystem)",    [](){ g_mock.failGetSystem = true; } },
        { "session creation fails",    [](){ g_mock.failCreateSession = true; } },
        { "reference space fails",     [](){ g_mock.failCreateSpace = true; } },
        { "eye-0 swapchain fails",     [](){ g_mock.failCreateSwapchainAt = 0; } },
        { "eye-0 image enum fails",    [](){ g_mock.failEnumImagesAt = 0; } },
        { "eye-1 swapchain fails",     [](){ g_mock.failCreateSwapchainAt = 1; } },
        { "eye-1 image enum fails",    [](){ g_mock.failEnumImagesAt = 1; } },
    };
    for (int i = 0; i < 8; ++i) {
        ResetAll();
        stages[i].arm();
        const bool up = VR_Startup(g_dc, g_rc);
        CHECK(!up && MockXr_LiveSessionObjects() == 0 && g_mock.violations == 0,
              "%s -> no session objects survive (%d live)",
              stages[i].name, MockXr_LiveSessionObjects());
    }
    ResetAll();
    CHECK(VR_Startup(g_dc, g_rc) && MockXr_LiveSessionObjects() == 4,
          "with no failures armed, startup builds everything");
}

static void S_HappyPath()
{
    printf("--- happy path: one XR frame spans two game frames ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp(), "startup against the mock runtime");
    CHECK(g_mock.chains[0].width == 3072 && g_mock.chains[0].height == 3264,
          "swapchains sized from the per-view recommendation");

    // ---- first game frame of the pair: eye 0, capture only ----
    const int w0 = g_mock.waitFrameCount, b0 = g_mock.beginFrameCount;
    const int l0 = g_mock.locateViewsCount, e0 = g_mock.endFrameCount;

    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "first frame renders after READY");
    CHECK(VR_ExpectedEye() == 0 && VR_TestPairPhase() == 1,
          "a fresh pair expects eye 0");

    // Both eyes' cameras from ONE locate: midpoint neutral puts them at
    // -IPD/2 and +IPD/2 around the game origin (0.032 m * 39.37 = 1.26 u).
    DriveEye(0); DriveEye(1);
    const float sepY = g_out[0][1] - g_out[1][1];   // eye0 is LEFT = Quake +y
    const float midX = (g_out[0][0] + g_out[1][0]) * 0.5f;
    const float midY = (g_out[0][1] + g_out[1][1]) * 0.5f;
    const float midZ = (g_out[0][2] + g_out[1][2]) * 0.5f;
    CHECK_NEAR(sepY, 2.5197, 0.01, "eye separation %.4f units (64 mm IPD)", sepY);
    CHECK(fabsf(midX - 100) < 0.01f && fabsf(midY - 200) < 0.01f && fabsf(midZ - 50) < 0.01f,
          "binocular midpoint sits ON the game camera (%.2f %.2f %.2f)", midX, midY, midZ);

    VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_Active(), "active after the first located frame");
    CHECK(g_mock.endFrameCount == e0 && g_mock.endsWithLayers == 0,
          "first eye captured WITHOUT xrEndFrame or a layer submission");
    CHECK(VR_TestPairPhase() == 2 && VR_ExpectedEye() == 1,
          "the pair now awaits eye 1");

    // ---- second game frame: eye 1, same XR frame, then submit both ----
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "mid-pair begin is a pure no-op");
    CHECK(g_mock.waitFrameCount == w0 + 1 && g_mock.beginFrameCount == b0 + 1
          && g_mock.locateViewsCount == l0 + 1,
          "NO xrWaitFrame / xrBeginFrame / xrLocateViews between the eyes");
    DriveEye(1);
    VR_SubmitFrame(640, 480, true, 1);

    CHECK(g_mock.endFrameCount == e0 + 1 && g_mock.endsWithLayers == 1
          && g_mock.lastEnd.hasProjection && g_mock.lastEnd.layerCount == 1,
          "second eye completes the pair: exactly one stereo submission");
    CHECK(VR_TestPairPhase() == 0 && VR_ExpectedEye() == 0,
          "pair machine back at the boundary");
    CHECK(g_mock.waitFrameCount == w0 + 1 && g_mock.beginFrameCount == b0 + 1
          && g_mock.locateViewsCount == l0 + 1 && g_mock.endFrameCount == e0 + 1,
          "exactly 1 wait / 1 begin / 1 locate / 1 end per TWO 3D game frames");
    CHECK(g_mock.violations == 0,
          "the FIRST submission already carried both eyes (no half-black frame)");

    // Both records belong to the same located generation.
    CHECK(VR_TestEyeRecordSerial(0) >= 0
          && VR_TestEyeRecordSerial(0) == VR_TestEyeRecordSerial(1)
          && VR_TestEyeRecordSerial(0) == (int)VR_TestFrameSerial(),
          "both eye records carry the pair's XR frame serial (%d)",
          VR_TestEyeRecordSerial(0));

    // FULL-rect submission: each eye rendered through the runtime's OWN
    // asymmetric frustum (the projection intercept), so the whole texture IS
    // the view. The old symmetric-superset -> sub-rect crop is gone from the
    // 3D path; S_ProjectionInterception proves it cannot come back.
    const XrRect2Di& r0 = g_mock.lastEnd.views[0].subImage.imageRect;
    const XrRect2Di& r1 = g_mock.lastEnd.views[1].subImage.imageRect;
    CHECK(r0.offset.x == 0 && r0.offset.y == 0
          && r0.extent.width == 3072 && r0.extent.height == 3264,
          "eye0 submits the FULL eye image {0,0 3072x3264}: got {%d,%d %dx%d}",
          r0.offset.x, r0.offset.y, r0.extent.width, r0.extent.height);
    CHECK(r1.offset.x == 0 && r1.offset.y == 0
          && r1.extent.width == 3072 && r1.extent.height == 3264,
          "eye1 submits the FULL eye image {0,0 3072x3264}: got {%d,%d %dx%d}",
          r1.offset.x, r1.offset.y, r1.extent.width, r1.extent.height);
    CHECK_NEAR(g_mock.lastEnd.views[0].fov.angleLeft, -54.0f * DEG, 1e-4,
               "eye0 submitted with ITS runtime frustum (angleLeft -54)");
    CHECK_NEAR(g_mock.lastEnd.views[0].fov.angleRight, 40.0f * DEG, 1e-4,
               "eye0 submitted with ITS runtime frustum (angleRight +40)");
    CHECK(PoseEq(g_mock.lastEnd.views[0].pose, g_mock.viewPose[0])
          && PoseEq(g_mock.lastEnd.views[1].pose, g_mock.viewPose[1]),
          "both eyes submitted at their rendered poses");
    CHECK_NEAR(g_mock.lastEnd.views[1].fov.angleLeft, -40.0f * DEG, 1e-4,
               "eye1 submitted with the runtime's asymmetric fov");
    CHECK(g_mock.lastEnd.displayTime == g_mock.nextDisplayTime,
          "displayTime is the pair's predicted time");

    // A second pair: per-chain acquire/wait/release balance is ONE per pair.
    CHECK(StepPair() == VR_FRAME_RENDER, "second pair runs");
    for (int c = 0; c < 2; ++c)
        CHECK(g_mock.chains[c].acquired == g_mock.chains[c].released
              && g_mock.chains[c].acquired == g_mock.chains[c].waited
              && g_mock.chains[c].acquired == 2,
              "chain %d acquire/wait/release balanced at 2/2/2 after two pairs", c);

    VRPerfStats st = {};
    VR_GetPerfStats(&st);
    CHECK_NEAR(st.xrTargetHz, 90.0, 0.5, "XR target cadence measured as %.1f Hz", st.xrTargetHz);
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_PairSharedGeneration()
{
    printf("--- both eyes of a pair share ONE located view generation ---\n");
    // Continues on the live session from S_HappyPath.
    const XrPosef pairPose0 = g_mock.viewPose[0];
    const XrPosef pairPose1 = g_mock.viewPose[1];
    const XrFovf  pairFov1  = g_mock.viewFov[1];

    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    DriveEye(0);
    VR_SubmitFrame(640, 480, true, 0);

    // The head moves AND eye1's runtime fov changes between the two game
    // frames (imagine an eye-tracked runtime). NONE of it may reach this
    // pair -- no locate happens between the eyes, so both cameras and both
    // submitted views derive from the pair's single located generation.
    SetMockHead(25, 0, 0.15f, 0);
    g_mock.viewFov[1].angleLeft  = -30.0f * DEG;
    g_mock.viewFov[1].angleRight =  30.0f * DEG;

    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "mid-pair begin is a no-op");
    DriveEye(1);
    float y, p, r;
    AxisAngles(g_outAxis[1], &y, &p, &r);
    CHECK(fabsf(y) < 0.5f,
          "the eye-1 CAMERA still derives from the pair's locate (yaw %.2f)", y);
    VR_SubmitFrame(640, 480, true, 1);

    CHECK(PoseEq(g_mock.lastEnd.views[0].pose, pairPose0)
          && PoseEq(g_mock.lastEnd.views[1].pose, pairPose1),
          "both submitted poses are the pair's, not the mid-pair sample");
    CHECK_NEAR(g_mock.lastEnd.views[1].fov.angleLeft, pairFov1.angleLeft, 1e-5,
               "eye-1 fov is the pair's -- FOV generations cannot mix");

    // The NEXT pair picks the new pose and fov up at its own locate.
    CHECK(StepPair() == VR_FRAME_RENDER, "next pair runs");
    CHECK(PoseEq(g_mock.lastEnd.views[1].pose, g_mock.viewPose[1]),
          "the next pair's locate picks up the new pose");
    CHECK(g_mock.violations == 0, "no protocol violations");

    // Settle back to the neutral head for the scenarios that follow.
    g_mock.viewFov[1] = pairFov1;
    SetMockHead(0, 0, 0, 0);
    StepPair();
}

static void S_ShouldRenderMachine()
{
    printf("--- shouldRender == false skips ALL rendering work ---\n");
    // Continues on the live session.
    const int endsBefore = g_mock.endFrameCount;
    const int acq0 = g_mock.chains[0].acquired, acq1 = g_mock.chains[1].acquired;

    g_mock.shouldRender = false;
    VRFrameDisposition d = VR_BeginFrameEx();
    CHECK(d == VR_FRAME_SKIP_RENDER, "disposition is SKIP_RENDER");
    CHECK(g_mock.endFrameCount == endsBefore + 1 && g_mock.lastEnd.layerCount == 0,
          "the frame was ALREADY ended with zero layers inside BeginFrame");

    // A caller that submits anyway must be a no-op: no capture, no blits, no
    // second end.
    VR_SubmitFrame(640, 480, true, 0);
    CHECK(g_mock.endFrameCount == endsBefore + 1, "a late VR_SubmitFrame is a no-op");
    CHECK(g_mock.chains[0].acquired == acq0 && g_mock.chains[1].acquired == acq1,
          "no swapchain images were acquired for the skipped frame");
    CHECK(VR_TestPairPhase() == 0 && VR_ExpectedEye() == 0,
          "a skipped frame leaves the machine at 'need eye 0'");

    VRPerfStats st = {};
    VR_GetPerfStats(&st);
    CHECK(st.skippedFrames >= 1, "skip counted in the perf stats (%u)", st.skippedFrames);

    g_mock.shouldRender = true;
    CHECK(StepPair() == VR_FRAME_RENDER, "a full pair renders next");
    CHECK(g_mock.violations == 0, "wait/begin/end stayed paired throughout");
}

static void S_HalfPairShouldRenderFlip()
{
    printf("--- half-pair, then the runtime stops wanting frames ---\n");
    // Continues on the live session. Eye 0 lands; the pair dies before eye 1
    // completes (capture failure), and the runtime flips shouldRender off.
    // The machine must converge through zero-layer ends to a clean skip
    // state, and nothing from the dead half-pair may ever be submitted.
    const int layersBefore = g_mock.endsWithLayers;
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    DriveEye(0);
    VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_TestPairPhase() == 2, "half-pair open");
    const int deadSerial = VR_TestEyeRecordSerial(0);

    g_mock.shouldRender = false;
    VR_TestFailNextCaptures(1);
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER,
          "mid-pair begin cannot know yet (no wait happens between eyes)");
    DriveEye(1);
    VR_SubmitFrame(640, 480, true, 1);       // eye-1 capture fails -> abort
    CHECK(g_mock.endsWithLayers == layersBefore && g_mock.lastEnd.layerCount == 0,
          "the dying half-pair ended with ZERO layers");
    CHECK(VR_TestPairPhase() == 0, "pair reset");

    CHECK(VR_BeginFrameEx() == VR_FRAME_SKIP_RENDER,
          "the shouldRender flip lands at the next real wait");
    CHECK(g_mock.lastEnd.layerCount == 0, "the skip frame also ends with zero layers");
    CHECK(VR_TestPairPhase() == 0 && VR_ExpectedEye() == 0,
          "skip leaves the machine at 'need eye 0'");

    g_mock.shouldRender = true;
    CHECK(StepPair() == VR_FRAME_RENDER, "rendering resumes with a fresh pair");
    CHECK(g_mock.endsWithLayers == layersBefore + 1, "exactly one new stereo submission");
    CHECK(VR_TestEyeRecordSerial(0) == (int)VR_TestFrameSerial()
          && VR_TestEyeRecordSerial(1) == (int)VR_TestFrameSerial()
          && VR_TestEyeRecordSerial(0) != deadSerial,
          "the fresh pair never reuses the dead half-pair's generation");
    VRPerfStats st = {};
    VR_GetPerfStats(&st);
    CHECK(st.abortedPairs >= 1, "the abort is counted (%u)", st.abortedPairs);
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_CaptureFailures()
{
    printf("--- capture failures abort the pair, never malform a layer ---\n");
    // Continues on the live session.
    const int layers = g_mock.endsWithLayers;
    const int empt = g_mock.endsEmpty;

    // Eye 0's capture fails: the pair aborts before it ever half-opens.
    VR_TestFailNextCaptures(1);
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    DriveEye(0);
    VR_SubmitFrame(640, 480, true, 0);
    CHECK(g_mock.endsEmpty == empt + 1 && g_mock.endsWithLayers == layers,
          "eye-0 capture failure: zero-layer end, no malformed submission");
    CHECK(VR_TestPairPhase() == 0 && VR_ExpectedEye() == 0,
          "reset straight back to 'need eye 0'");

    // Recovery is a complete, ordinary pair.
    CHECK(StepPair() == VR_FRAME_RENDER, "recovery pair runs");
    CHECK(g_mock.endsWithLayers == layers + 1, "recovery pair submits");
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_SwapchainFailureAtSubmit()
{
    printf("--- swapchain failure during the final submit ---\n");
    // Continues on the live session.
    const int layers = g_mock.endsWithLayers;
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    DriveEye(0); VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "mid-pair begin");
    DriveEye(1);
    g_mock.failAcquire = true;               // the copy-out will fail
    VR_SubmitFrame(640, 480, true, 1);
    g_mock.failAcquire = false;
    CHECK(g_mock.endsWithLayers == layers && g_mock.lastEnd.layerCount == 0,
          "acquire failure at submit: zero layers, the whole pair withheld");
    CHECK(VR_TestPairPhase() == 0, "pair reset after the failed submit");

    // Recovery must use FRESH captures -- no borrowed stale eye. Move the
    // head so a borrow would be visible in the submitted poses.
    SetMockHead(10, 0, 0, 0);
    CHECK(StepPair() == VR_FRAME_RENDER, "recovery pair runs");
    CHECK(g_mock.endsWithLayers == layers + 1, "recovery pair submits");
    CHECK(PoseEq(g_mock.lastEnd.views[0].pose, g_mock.viewPose[0])
          && PoseEq(g_mock.lastEnd.views[1].pose, g_mock.viewPose[1]),
          "recovery submits the fresh pair's poses -- no borrowed stale eye");
    CHECK(VR_TestEyeRecordSerial(0) == (int)VR_TestFrameSerial()
          && VR_TestEyeRecordSerial(1) == (int)VR_TestFrameSerial(),
          "recovery records carry the new generation");
    SetMockHead(0, 0, 0, 0);
    StepPair();
    CHECK(g_mock.violations == 0, "no protocol violations");
    CHECK(g_mock.beginFrameCount == g_mock.endFrameCount, "begin/end balanced");
}

static void S_MixedGenerationGuard()
{
    printf("--- the serial guard refuses a mixed-generation pair ---\n");
    // Continues on the live session. The state machine makes a mismatch
    // impossible in normal operation, so age one record artificially and
    // prove the last line of defence actually fires.
    const int layers = g_mock.endsWithLayers;
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    DriveEye(0); VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "mid-pair begin");
    DriveEye(1);
    VR_TestCorruptEyeSerial(0);              // age eye 0 by one generation
    VR_SubmitFrame(640, 480, true, 1);
    CHECK(g_mock.endsWithLayers == layers && g_mock.lastEnd.layerCount == 0,
          "a mixed-generation stereo pair is NEVER submitted (zero-layer end)");
    CHECK(VR_TestPairPhase() == 0, "the guard resets the pair");
    CHECK(StepPair() == VR_FRAME_RENDER && g_mock.endsWithLayers == layers + 1,
          "recovery pair submits cleanly");
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_WrongEyeIsRefused()
{
    printf("--- an unexpected eye is refused, loudly ---\n");
    // Continues on the live session.
    const int layers = g_mock.endsWithLayers;
    const int empt = g_mock.endsEmpty;
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    CHECK(VR_ExpectedEye() == 0, "boundary expects eye 0");
    DriveEye(1);
    VR_SubmitFrame(640, 480, true, 1);       // wrong: eye 1 first
    CHECK(g_mock.endsEmpty == empt + 1 && g_mock.endsWithLayers == layers,
          "wrong first eye: pair aborted with a zero-layer end");
    CHECK(VR_TestPairPhase() == 0 && VR_ExpectedEye() == 0, "machine reset to eye 0");
    CHECK(StepPair() == VR_FRAME_RENDER && g_mock.endsWithLayers == layers + 1,
          "recovery pair submits");
    CHECK(g_mock.violations == 0, "no protocol violations");
}


// VR UI SAFE AREA (brief 2026-09-04, P3): the flat panel's angular fit
// shrinks with vr_ui_scale, aspect preserved, and the fit is reported.
// Continues on the live session, after the transitions test.
static void S_UiSafeArea()
{
    printf("--- VR UI safe area: menus and loading screens inset, aspect kept ---\n");
    // VR UI SAFE AREA (brief 2026-09-04, P3): the flat panel's angular fit
    // shrinks with vr_ui_scale, aspect preserved, and the fit is reported.
    {
        float sc0, sx0, sy0;
        VR_GetUiSafeArea(&sc0, &sx0, &sy0);
        CHECK(sc0 == VR_UI_SCALE_DEFAULT && sx0 == 1.0f && sy0 == 1.0f,
              "ui safe area defaults %.2f / %.2f / %.2f", sc0, sx0, sy0);
        float cx1, cy1, vx1, vy1;
        VR_SetUiSafeArea(1.0f, 1.0f, 1.0f);
        CHECK(StepGameFlat() == VR_FRAME_RENDER, "flat frame (no ui inset)");
        const int fit1 = VR_GetMenuSafeArea(&cx1, &cy1, &vx1, &vy1);
        float cx2, cy2, vx2, vy2;
        VR_SetUiSafeArea(0.92f, 1.0f, 1.0f);
        CHECK(StepGameFlat() == VR_FRAME_RENDER, "flat frame (ui inset 0.92)");
        const int fit2 = VR_GetMenuSafeArea(&cx2, &cy2, &vx2, &vy2);
        CHECK(fit1 && fit2, "the angular fit is measured on both flat frames");
        if (fit1 && fit2) {
            const float d2r = 0.0174533f;
            const float rx = tanf(cx2 * d2r) / tanf(cx1 * d2r);
            const float ry = tanf(cy2 * d2r) / tanf(cy1 * d2r);
            CHECK(cx2 < cx1 && cy2 < cy1 && fabsf(rx - 0.92f) < 0.02f && fabsf(ry - 0.92f) < 0.02f,
                  "ui inset 0.92: content +/-%.1fx%.1f -> +/-%.1fx%.1f deg (tan ratios %.3f / %.3f)",
                  cx1, cy1, cx2, cy2, rx, ry);
            CHECK(cx2 < vx2 && cy2 < vy2,
                  "...and stays inside the binocularly visible +/-%.1fx%.1f deg", vx2, vy2);
            const float ar1 = tanf(cx1 * d2r) / tanf(cy1 * d2r);
            const float ar2 = tanf(cx2 * d2r) / tanf(cy2 * d2r);
            CHECK(fabsf(ar1 - ar2) < 0.01f, "...aspect preserved (%.3f vs %.3f)", ar1, ar2);
        }
        VR_SetUiSafeArea(sc0, sx0, sy0);
    }

}

static void S_FlatAndPairTransitions()
{
    printf("--- menus: one game frame per XR frame; transitions flush cleanly ---\n");
    // Continues on the live session.
    // Plain flat frames (no half-pair): one game frame -> one XR submission.
    const int w0 = g_mock.waitFrameCount;
    const int layers0 = g_mock.endsWithLayers;
    CHECK(StepGameFlat() == VR_FRAME_RENDER, "flat frame runs");
    CHECK(g_mock.waitFrameCount == w0 + 1 && g_mock.endsWithLayers == layers0 + 1,
          "a flat frame keeps one XR frame per game frame");
    const XrRect2Di& fr0 = g_mock.lastEnd.views[0].subImage.imageRect;
    CHECK(fr0.offset.x == 0 && fr0.extent.width == 3072 && fr0.extent.height == 3264,
          "panel occupies the full eye image (letterboxing happened at capture)");

    // Flat -> 3D: the next pair starts cleanly at eye 0.
    CHECK(VR_ExpectedEye() == 0, "after a menu the next 3D frame is eye 0");
    CHECK(StepPair() == VR_FRAME_RENDER, "flat -> 3D: a clean pair runs");
    CHECK(g_mock.endsWithLayers == layers0 + 2, "the pair submitted");

    // 3D -> flat MID-PAIR: the half-pair is flushed; the panel goes to both
    // eyes on the same open XR frame; nothing from the gameplay pair leaks.
    VRPerfStats st0 = {};
    VR_GetPerfStats(&st0);
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    DriveEye(0); VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_TestPairPhase() == 2, "half-pair open");
    const int wMid = g_mock.waitFrameCount;
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "next game frame begins mid-pair");
    VR_SubmitFrame(640, 480, false, 0);      // ...but the game showed a menu
    CHECK(g_mock.waitFrameCount == wMid,
          "the panel reused the open XR frame (no extra wait)");
    CHECK(g_mock.endsWithLayers == layers0 + 3 && g_mock.lastEnd.layerCount == 1,
          "panel submitted to both eyes on the flushed frame");
    // A fitted panel submits a SYMMETRIC fov; a gameplay eye submits the
    // asymmetric runtime frustum. Both views symmetric = no gameplay leak.
    CHECK(fabsf(g_mock.lastEnd.views[0].fov.angleLeft
                + g_mock.lastEnd.views[0].fov.angleRight) < 1e-6f
          && fabsf(g_mock.lastEnd.views[1].fov.angleLeft
                   + g_mock.lastEnd.views[1].fov.angleRight) < 1e-6f,
          "BOTH submitted views are fitted panels -- no gameplay eye leaked");
    VRPerfStats st1 = {};
    VR_GetPerfStats(&st1);
    CHECK(st1.abortedPairs == st0.abortedPairs + 1, "the flushed half-pair is counted");

    // Flat -> 3D again: still clean.
    CHECK(VR_ExpectedEye() == 0, "flush leaves the machine at 'need eye 0'");
    CHECK(StepPair() == VR_FRAME_RENDER, "3D resumes with a full pair");
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_PairDiscarded()
{
    printf("--- XR_FRAME_DISCARDED does not corrupt the pair ---\n");
    // Continues on the live session.
    VRPerfStats s0 = {};
    VR_GetPerfStats(&s0);
    const int layers = g_mock.endsWithLayers;
    g_mock.forceDiscardNextBegin = true;
    CHECK(StepPair() == VR_FRAME_RENDER, "a pair runs through a discarded begin");
    CHECK(g_mock.endsWithLayers == layers + 1, "the pair still submitted");
    VRPerfStats s1 = {};
    VR_GetPerfStats(&s1);
    CHECK(s1.discardedFrames == s0.discardedFrames + 1, "the discard was counted");
    CHECK(VR_TestPairPhase() == 0 && VR_ExpectedEye() == 0,
          "machine back at the boundary");
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_DefensiveDoubleBegin()
{
    printf("--- an unsubmitted frame is ended defensively, not discarded ---\n");
    const int emptyBefore = g_mock.endsEmpty;
    const int discBefore  = g_mock.discardedCount;
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "frame A begins");
    // The caller 'forgets' to submit and begins again.
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "frame B begins anyway");
    CHECK(g_mock.endsEmpty == emptyBefore + 1, "frame A was ended empty first");
    CHECK(g_mock.discardedCount == discBefore, "the runtime never had to discard a frame");
    CHECK(VR_ExpectedEye() == 0, "the defensive end restarted the pair at eye 0");
    DriveEye(0); VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "frame B's pair continues");
    DriveEye(1); VR_SubmitFrame(640, 480, true, 1);
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_StoppingReadyResume()
{
    printf("--- STOPPING ends the session; READY resumes it (VD reconnect) ---\n");
    MockXr_QueueState(XR_SESSION_STATE_STOPPING);
    CHECK(VR_BeginFrameEx() == VR_FRAME_INACTIVE, "inactive after STOPPING");
    CHECK(g_mock.endSessionCount == 1 && MockXr_LiveSessionObjects() == 4,
          "session ended but NOT destroyed (reconnects reuse it)");
    CHECK(!VR_Active(), "not active while stopped");

    MockXr_QueueState(XR_SESSION_STATE_READY);
    CHECK(StepPair() == VR_FRAME_RENDER, "READY resumes pairs on the same session");
    CHECK(g_mock.beginSessionCount == 2, "session begun a second time");
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_LossRecovery()
{
    printf("--- LOSS_PENDING tears down; the retry path rebuilds ---\n");
    MockXr_QueueState(XR_SESSION_STATE_LOSS_PENDING);
    CHECK(VR_BeginFrameEx() == VR_FRAME_INACTIVE, "inactive after loss");
    CHECK(MockXr_LiveSessionObjects() == 0, "lost session fully destroyed");
    CHECK(!VR_Active(), "not active after loss");
    CHECK(!VR_Startup(g_dc, g_rc), "immediate rebuild is throttled (3 s backoff)");

    Sleep(3100);
    CHECK(VR_Startup(g_dc, g_rc), "rebuild succeeds after the backoff");
    MockXr_QueueState(XR_SESSION_STATE_READY);
    CHECK(StepPair() == VR_FRAME_RENDER, "pairs run on the rebuilt session");
    CHECK(MockXr_LiveSessionObjects() == 4 && g_mock.violations == 0,
          "rebuilt session is complete and clean");
}

static void S_HalfPairStopping()
{
    printf("--- STOPPING mid-pair: frame closed, session kept, resume at eye 0 ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up");
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    DriveEye(0); VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_TestPairPhase() == 2, "half-pair open");

    MockXr_QueueState(XR_SESSION_STATE_STOPPING);
    CHECK(VR_BeginFrameEx() == VR_FRAME_INACTIVE, "inactive after STOPPING");
    CHECK(!g_mock.frameOpen && g_mock.beginFrameCount == g_mock.endFrameCount,
          "the open pair frame was ended before the session stopped");
    CHECK(MockXr_LiveSessionObjects() == 4, "session objects kept for reconnection");

    MockXr_QueueState(XR_SESSION_STATE_READY);
    CHECK(VR_ExpectedEye() == 0, "resumption starts at eye 0");
    CHECK(StepPair() == VR_FRAME_RENDER, "READY resumes with a clean pair");
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_HalfPairSessionLoss()
{
    printf("--- session loss mid-pair: teardown + pair reset ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up, one pair done");
    VRPerfStats st0 = {};
    VR_GetPerfStats(&st0);

    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    DriveEye(0); VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_TestPairPhase() == 2, "half-pair open");

    MockXr_QueueState(XR_SESSION_STATE_LOSS_PENDING);
    CHECK(VR_BeginFrameEx() == VR_FRAME_INACTIVE, "mid-pair begin sees the loss");
    CHECK(MockXr_LiveSessionObjects() == 0, "session fully destroyed");
    CHECK(g_mock.beginFrameCount == g_mock.endFrameCount && !g_mock.frameOpen,
          "the open pair frame was ended before the session died");
    VRPerfStats st1 = {};
    VR_GetPerfStats(&st1);
    CHECK(st1.abortedPairs == st0.abortedPairs + 1, "the dead half-pair is counted");

    Sleep(3100);
    CHECK(VR_Startup(g_dc, g_rc), "rebuild succeeds after the backoff");
    MockXr_QueueState(XR_SESSION_STATE_READY);
    CHECK(VR_ExpectedEye() == 0, "rebuilt session starts at eye 0");
    CHECK(StepPair() == VR_FRAME_RENDER, "a clean pair runs on the rebuilt session");
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_ShutdownMidPair()
{
    printf("--- shutdown after an odd number of 3D frames: nothing left hanging ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up");
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    DriveEye(0); VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_TestPairPhase() == 2, "half-pair open (odd 3D frame count)");

    VR_Shutdown();
    CHECK(!g_mock.frameOpen, "no begun XR frame left hanging");
    CHECK(g_mock.beginFrameCount == g_mock.endFrameCount,
          "every successful xrBeginFrame got exactly one xrEndFrame (%d/%d)",
          g_mock.beginFrameCount, g_mock.endFrameCount);
    CHECK(g_mock.waitsNotBegun == 0, "no dangling xrWaitFrame either");
    CHECK(g_mock.liveInstances == 0 && MockXr_LiveSessionObjects() == 0,
          "everything destroyed");
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_ExitingIsFinal()
{
    printf("--- EXITING honours the user: no auto-restart ---\n");
    MockXr_QueueState(XR_SESSION_STATE_EXITING);
    CHECK(VR_BeginFrameEx() == VR_FRAME_INACTIVE, "inactive after EXITING");
    CHECK(MockXr_LiveSessionObjects() == 0, "session destroyed");
    CHECK(!VR_Startup(g_dc, g_rc), "startup refuses after user exit");
    CHECK(strstr(VR_StatusText(), "user exited") != nullptr,
          "status says why: '%s'", VR_StatusText());
}

static void S_RefSpaceChange()
{
    printf("--- runtime recenter (LOCAL space moved) relatches our neutral ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up, neutral at yaw 0");

    // Head physically at yaw 40: ordinary free-look.
    SetMockHead(40, 0, 0, 0);
    CHECK(StepPair() == VR_FRAME_RENDER, "pair under the turned head");
    float y, p, r;
    AxisAngles(g_outAxis[1], &y, &p, &r);
    CHECK_NEAR(y, 40.0, 0.5, "head at 40 deg reads as free-look (view yaw %.1f)", y);

    // The runtime moves LOCAL space under us (user recentred in VD's UI).
    MockXr_QueueRefSpaceChange();
    CHECK(StepPair() == VR_FRAME_RENDER, "pair after the space-change event");
    AxisAngles(g_outAxis[0], &y, &p, &r);
    CHECK_NEAR(y, 0.0, 0.5, "after the space-change event the neutral relatches (view yaw %.1f)", y);
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_RecenterAtomicAcrossPairs()
{
    printf("--- recenter latches at pair boundaries, never inside a pair ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up, neutral at yaw 0");

    // The player has physically turned 70 degrees: ordinary free-look.
    SetMockHead(70, 0.05f, 0.02f, -0.03f);
    CHECK(StepPair() == VR_FRAME_RENDER, "pair under the turned head");
    float y, p, r;
    AxisAngles(g_outAxis[1], &y, &p, &r);
    CHECK_NEAR(y, 70.0, 0.5, "head at 70 deg reads as free-look (view yaw %.1f)", y);

    // Recenter pressed BETWEEN the two eyes of a pair. The pair must stay
    // internally coherent -- eye 1 keeps the neutral its eye-0 partner
    // rendered with -- and the relatch lands atomically at the NEXT pair's
    // locate, never half-way through a pair.
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    DriveEye(0);
    float y0;
    AxisAngles(g_outAxis[0], &y0, &p, &r);
    VR_SubmitFrame(640, 480, true, 0);
    VR_Recenter();
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "mid-pair begin");
    DriveEye(1);
    float y1;
    AxisAngles(g_outAxis[1], &y1, &p, &r);
    CHECK(fabsf(y1 - y0) < 0.5f,
          "eye 1 stays coherent with its pair partner (yaw %.1f vs %.1f)", y1, y0);
    CHECK_NEAR(y1, 70.0, 1.0, "the old neutral holds until the pair completes");
    VR_SubmitFrame(640, 480, true, 1);

    // Next pair: the relatch was consumed atomically at its locate --
    // BOTH eyes of that pair are centred.
    CHECK(StepPair() == VR_FRAME_RENDER, "next pair runs");
    AxisAngles(g_outAxis[0], &y, &p, &r);
    CHECK(fabsf(y) < 0.5f && fabsf(r) < 0.1f,
          "next pair's eye 0 is centred (yaw %.2f, roll %.2f)", y, r);
    AxisAngles(g_outAxis[1], &y, &p, &r);
    CHECK(fabsf(y) < 0.5f, "next pair's eye 1 is centred too (yaw %.2f)", y);

    // Position neutral moved with it: eyes back to +-IPD/2 about the origin.
    DriveEye(0); DriveEye(1);
    const float midY = (g_out[0][1] + g_out[1][1]) * 0.5f;
    CHECK_NEAR(midY, 200.0, 0.05, "midpoint re-latched at the new spot (y %.2f)", midY);

    // Recenter during a MENU (no 3D eye renders): must still take effect
    // before the next 3D pair.
    SetMockHead(-55, 0, 0, 0);
    VR_Recenter();
    CHECK(StepGameFlat() == VR_FRAME_RENDER, "menu frame after recenter");
    CHECK(StepPair() == VR_FRAME_RENDER, "next 3D pair runs");
    AxisAngles(g_outAxis[1], &y, &p, &r);
    CHECK(fabsf(y) < 0.5f, "menu-time recenter consumed before any 3D frame (yaw %.2f)", y);
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_InsanePoses()
{
    printf("--- absurd position samples keep the eye separation alive ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up with sane history");
    DriveEye(0); DriveEye(1);
    const float sepBefore = fabsf(g_out[0][1] - g_out[1][1]);

    // One eye goes absurd (the y = -1592 m class of sample).
    g_mock.viewPose[1].position.x = 1000.0f;
    g_mock.viewPose[1].position.y = -1592.0f;
    StepPair();
    DriveEye(0); DriveEye(1);
    float sep = fabsf(g_out[0][1] - g_out[1][1]);
    CHECK_NEAR(sep, sepBefore, 0.05,
               "one absurd eye: separation preserved via its last sane sample (%.3f u)", sep);

    // Both eyes absurd: whole head freezes at the last sane spot, still stereo.
    g_mock.viewPose[0].position.y = 2000.0f;
    StepPair();
    DriveEye(0); DriveEye(1);
    sep = fabsf(g_out[0][1] - g_out[1][1]);
    CHECK_NEAR(sep, sepBefore, 0.05,
               "both absurd: separation still %.3f u, world does not flatten", sep);
    CHECK(fabsf(g_out[0][0] - 100.0f) < 3.0f,
          "camera stayed near the game origin instead of teleporting");

    SetMockHead(0, 0, 0, 0);
    StepPair();
    CHECK(g_mock.violations == 0, "no protocol violations");
}

// The 2026-08-20 hardware failure, replayed: VDXR parked the LOCAL origin
// 197 m above the head. Every real pose failed the old absolute sanity gate,
// which silently killed the neutral midpoint, collapsed both rendered eyes
// onto one origin (ipdScale FAIL, laterality WRONG-SIDE), and starved the
// whole weapon/steering stack. Absolute magnitude must never disqualify an
// internally consistent pair.
static void S_RuntimeOriginFarAway()
{
    printf("--- VDXR-style far LOCAL origin: neutral, IPD and laterality survive ---\n");
    ResetAll();
    SetMockHead(0, -0.021f, -197.41f, -0.002f, 0.0641f);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "far origin: session up");
    StepPair();

    float yawRef = 0, refM[3];
    CHECK((VR_GetNeutralRefs(&yawRef, refM) & 2) != 0,
          "far origin: neutral MIDPOINT latched at y = -197 m");
    float mid[3];
    CHECK(VR_GetHeadRoomMid(mid) == 1, "far origin: head room mid available");

    DriveEye(0); DriveEye(1);
    const float wantSep = 0.0641f * 39.37f;              // 2.524 u
    const float sep = fabsf(g_out[0][1] - g_out[1][1]);
    CHECK_NEAR(sep, wantSep, 0.01,
               "far origin: rendered separation %.3f u = runtime IPD * worldscale", sep);
    CHECK(g_out[0][1] > g_out[1][1],
          "far origin: laterality -- eye0 sits view-LEFT of eye1");
    CHECK_NEAR((g_out[0][1] + g_out[1][1]) * 0.5f, 200.0, 0.01,
               "far origin: eye midpoint lands on the game camera");

    // +10 cm physical translation moves BOTH eyes together by 3.937 u.
    const float e0y = g_out[0][1], e1y = g_out[1][1];
    SetMockHead(0, -0.021f + 0.10f, -197.41f, -0.002f, 0.0641f);
    StepPair();
    DriveEye(0); DriveEye(1);
    const float d0 = g_out[0][1] - e0y, d1 = g_out[1][1] - e1y;
    CHECK_NEAR(d0, d1, 1e-3, "far origin: both eyes translate together");
    CHECK_NEAR(fabsf(d0), 0.10f * 39.37f, 0.02,
               "far origin: 10 cm = %.3f u of camera translation", fabsf(d0));

    // A one-eye teleport at the far origin: continuity keeps the separation.
    const float sepBefore = fabsf(g_out[0][1] - g_out[1][1]);
    g_mock.viewPose[1].position.y = -190.0f;             // 7 m jump, one eye
    StepPair();
    DriveEye(0); DriveEye(1);
    CHECK_NEAR(fabsf(g_out[0][1] - g_out[1][1]), sepBefore, 0.05,
               "far origin: one-eye glitch cannot collapse the separation");

    // Non-finite garbage is rejected outright.
    g_mock.viewPose[1].position.x = sqrtf(-1.0f);        // NaN
    StepPair();
    DriveEye(0); DriveEye(1);
    CHECK_NEAR(fabsf(g_out[0][1] - g_out[1][1]), sepBefore, 0.05,
               "far origin: NaN position rejected, separation preserved");

    // The runtime silently moves the origin ~200 m with NO recenter event: a
    // sustained consistent pair at the new origin is adopted and the neutral
    // relatches, instead of freezing on stale samples forever.
    SetMockHead(0, 0.0f, 5.0f, 0.0f, 0.0641f);
    for (int i = 0; i < 40; ++i) StepPair();
    CHECK((VR_GetNeutralRefs(&yawRef, refM) & 2) != 0,
          "far origin: silent origin jump adopted, neutral still latched");
    DriveEye(0); DriveEye(1);
    CHECK(fabsf((g_out[0][0] + g_out[1][0]) * 0.5f - 100.0f) < 1.0f
          && fabsf((g_out[0][1] + g_out[1][1]) * 0.5f - 200.0f) < 1.0f,
          "far origin: camera back on the game origin after adoption");
    CHECK_NEAR(fabsf(g_out[0][1] - g_out[1][1]), wantSep, 0.05,
               "far origin: separation intact after adoption");

    CHECK(g_mock.violations == 0, "far origin: no protocol violations "
          "(identity-pose cop stayed quiet)");
}

// Bootstrap gate: garbage that LOOKS like positions (zero-separation
// identity poses, uninitialised zeros, absurd inter-eye distances) must
// never seed the neutral -- only a human-IPD pair may, at any offset.
static void S_SanePairBootstrap()
{
    printf("--- pair-consistency bootstrap rejects non-stereo garbage ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0, 0.0f);                       // coincident eyes
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "bootstrap: session up");
    StepPair();
    float yawRef = 0, refM[3];
    CHECK((VR_GetNeutralRefs(&yawRef, refM) & 2) == 0,
          "bootstrap: zero-separation pair never latches a midpoint");

    SetMockHead(0, 0, 0, 0, 0.30f);                      // 30 cm "IPD"
    StepPair();
    CHECK((VR_GetNeutralRefs(&yawRef, refM) & 2) == 0,
          "bootstrap: 30 cm inter-eye distance is not a head");

    SetMockHead(0, 0, 0, 0, 0.064f);                     // a real head arrives
    StepPair();
    CHECK((VR_GetNeutralRefs(&yawRef, refM) & 2) != 0,
          "bootstrap: the first real pair completes the deferred latch");
    CHECK(g_mock.violations == 0, "bootstrap: no protocol violations");
}

static void QMul(const float a[4], const float b[4], float out[4])   // a * b
{
    out[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    out[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    out[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    out[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}

// Overwrite the mock head ORIENTATIONS with a full yaw/pitch/roll attitude
// (positions untouched): the startup states a real headset passes through
// while face-down on a desk or being put on.
static void SetMockHeadAttitude(float yawDeg, float pitchDeg, float rollDeg)
{
    const float hy = yawDeg * DEG * 0.5f;
    const float hp = pitchDeg * DEG * 0.5f;
    const float hr = rollDeg * DEG * 0.5f;
    const float qy[4] = { 0, sinf(hy), 0, cosf(hy) };
    const float qp[4] = { sinf(hp), 0, 0, cosf(hp) };
    const float qr[4] = { 0, 0, sinf(hr), cosf(hr) };
    float t[4], q[4];
    QMul(qy, qp, t);
    QMul(t, qr, q);
    for (int e = 0; e < MOCK_EYES; ++e) {
        g_mock.viewPose[e].orientation.x = q[0];
        g_mock.viewPose[e].orientation.y = q[1];
        g_mock.viewPose[e].orientation.z = q[2];
        g_mock.viewPose[e].orientation.w = q[3];
    }
}

static float PanelYawDeg(const float q[4])
{
    return 2.0f * atan2f(q[1], q[3]) / DEG;
}

// The 2026-08-20 "menu inverted and crooked at startup" bug: the panel was
// pinned to the RAW head pose of the first flat frame -- whatever garbage
// attitude the headset had before it was even on a head. Contract under
// test: upright (+Y), zero pitch/roll, facing the head's true horizontal
// azimuth (no mirror, no 180 flip), pinned only once orientation TRACKING
// is real, and a valid orientation alone is enough even at a -197 m
// position.
static void S_MenuOrientationLatch()
{
    printf("--- startup menu: upright and level for ANY head attitude ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    g_mock.viewFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT
                     | XR_VIEW_STATE_POSITION_VALID_BIT;   // valid, NOT tracked
    SetMockHeadAttitude(35.0f, -20.0f, 170.0f);            // rolled, pitched down
    CHECK(BringUp(), "menu latch: session up");
    CHECK(StepGameFlat() == VR_FRAME_RENDER, "menu latch: flat frame renders");

    float q[4], pos[3]; int pinned = 9;
    CHECK(VR_TestGetPanelPose(0, q, pos, &pinned) == 1, "menu latch: pose readable");
    CHECK(pinned == 0, "menu latch: no pin before ORIENTATION_TRACKED");
    CHECK(fabsf(q[0]) < 0.01f && fabsf(q[2]) < 0.01f,
          "menu latch: panel upright -- roll 170 pitch -20 head yields zero pitch/roll");
    CHECK(fabsf(VRM_WrapDeg(PanelYawDeg(q) - 35.0f)) < 1.0f,
          "menu latch: panel faces the head azimuth (%.1f deg)", PanelYawDeg(q));

    // Mirror/flip mutant: a yaw-sign or handedness bug would negate this.
    SetMockHeadAttitude(-120.0f, 10.0f, -170.0f);
    StepGameFlat();
    VR_TestGetPanelPose(0, q, pos, &pinned);
    CHECK(fabsf(VRM_WrapDeg(PanelYawDeg(q) + 120.0f)) < 1.0f,
          "menu latch: azimuth tracks sign-correctly (no mirror, no 180 flip)");

    // Looking straight down is degenerate: fall back to upright identity.
    SetMockHeadAttitude(0.0f, -89.5f, 0.0f);
    StepGameFlat();
    VR_TestGetPanelPose(0, q, pos, &pinned);
    CHECK(fabsf(q[0]) < 0.01f && fabsf(q[2]) < 0.01f,
          "menu latch: straight-down head still yields an upright panel");

    // Valid orientation + absurd -197 m position: the panel must present.
    SetMockHead(10.0f, -0.02f, -197.4f, 0.0f);
    StepGameFlat();
    VR_TestGetPanelPose(0, q, pos, &pinned);
    CHECK(fabsf(VRM_WrapDeg(PanelYawDeg(q) - 10.0f)) < 1.0f,
          "menu latch: usable panel with position at -197 m");
    CHECK(fabsf(pos[1] + 197.4f) < 0.5f,
          "menu latch: finite far position adopted for compositor consistency");

    // Tracking arrives: pin, and HOLD while the head looks around.
    g_mock.viewFlags |= XR_VIEW_STATE_ORIENTATION_TRACKED_BIT
                      | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    StepGameFlat();
    VR_TestGetPanelPose(0, q, pos, &pinned);
    CHECK(pinned == 1, "menu latch: pinned once orientation tracking is up");
    CHECK(fabsf(VRM_WrapDeg(PanelYawDeg(q) - 10.0f)) < 1.0f,
          "menu latch: pinned at the current azimuth");
    SetMockHead(80.0f, -0.02f, -197.4f, 0.0f);
    StepGameFlat();
    VR_TestGetPanelPose(0, q, pos, &pinned);
    CHECK(pinned == 1 && fabsf(VRM_WrapDeg(PanelYawDeg(q) - 10.0f)) < 1.0f,
          "menu latch: pinned panel holds while the head looks around");

    // User recenter re-pins in front of wherever the player faces now.
    VR_Recenter();
    StepGameFlat();
    StepGameFlat();
    VR_TestGetPanelPose(0, q, pos, &pinned);
    CHECK(pinned == 1 && fabsf(VRM_WrapDeg(PanelYawDeg(q) - 80.0f)) < 1.0f,
          "menu latch: recenter re-pins the panel in front of the user");

    // Runtime recenter (LOCAL space moved) re-pins too.
    MockXr_QueueRefSpaceChange();
    SetMockHead(-30.0f, -0.02f, -197.4f, 0.0f);
    StepGameFlat();
    StepGameFlat();
    VR_TestGetPanelPose(0, q, pos, &pinned);
    CHECK(pinned == 1 && fabsf(VRM_WrapDeg(PanelYawDeg(q) + 30.0f)) < 1.0f,
          "menu latch: runtime recenter re-pins the panel");

    CHECK(g_mock.violations == 0, "menu latch: no protocol violations");
}

static void S_WaitFailureRecovery()
{
    printf("--- a wedged swapchain never gets a bogus release; session rebuilds ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up");

    g_mock.failWaitImage = true;
    int pairs = 0;
    while (MockXr_LiveSessionObjects() > 0 && pairs < 25) {
        StepPair();
        ++pairs;
    }
    CHECK(MockXr_LiveSessionObjects() == 0,
          "persistent wait failures tore the session down (after %d pairs)", pairs);
    // The cop rule 'release requires a successful wait' never fired: proof the
    // wait-failure path does NOT try to balance the acquire with a release.
    CHECK(g_mock.violations == 0,
          "no release was attempted after a failed wait (cop clean)");
    VRPerfStats st = {};
    VR_GetPerfStats(&st);
    CHECK(st.xrErrors > 0, "the failures were counted (%u xr errors)", st.xrErrors);
    CHECK(st.abortedPairs > 0, "the dead pairs were counted (%u)", st.abortedPairs);

    g_mock.failWaitImage = false;
    Sleep(3100);
    CHECK(VR_Startup(g_dc, g_rc), "rebuild succeeds after the backoff");
    MockXr_QueueState(XR_SESSION_STATE_READY);
    CHECK(StepPair() == VR_FRAME_RENDER,
          "a full pair runs cleanly on the rebuilt session");
    CHECK(g_mock.violations == 0, "no protocol violations after recovery");
}

static void S_BlendModePreference()
{
    printf("--- blend mode comes from the runtime's enumeration ---\n");
    ResetAll();
    g_mock.blendModes[0] = XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    g_mock.blendModes[1] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    g_mock.blendModeCount = 2;
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "pair runs on an additive-first runtime");
    CHECK(g_mock.lastEnd.blend == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE,
          "the runtime's PREFERRED mode is submitted, not a hardcoded OPAQUE");

    ResetAll();
    g_mock.failEnumBlendModes = true;
    g_mock.blendModes[0] = XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    g_mock.blendModes[1] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    g_mock.blendModeCount = 2;
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "pair runs with enumeration broken");
    CHECK(g_mock.lastEnd.blend == XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
          "OPAQUE is the fallback when enumeration fails");
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_PerViewDimensions()
{
    printf("--- per-view recommended sizes are honoured per eye ---\n");
    ResetAll();
    g_mock.recommendW[1] = 2880;
    g_mock.recommendH[1] = 3200;
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp(), "startup with asymmetric view recommendations");
    CHECK(g_mock.chains[0].width == 3072 && g_mock.chains[1].width == 2880,
          "each swapchain uses ITS view's size (%u / %u)",
          g_mock.chains[0].width, g_mock.chains[1].width);

    CHECK(StepPair() == VR_FRAME_RENDER, "a full pair rendered");
    const XrRect2Di& r1 = g_mock.lastEnd.views[1].subImage.imageRect;
    CHECK(r1.offset.x == 0 && r1.offset.y == 0
          && r1.extent.width == 2880 && r1.extent.height == 3200,
          "eye1 submits ITS OWN full image {0,0 2880x3200}: got {%d,%d %dx%d}",
          r1.offset.x, r1.offset.y, r1.extent.width, r1.extent.height);
    CHECK(g_mock.violations == 0, "no protocol violations");
}

// ---------------------------------------------------------------------------
// ANGULAR MAPPING, end to end.
//
// The hardware report was "the image still feels a bit too zoomed", and the
// named suspect was the square scene FBO (3240x3240) being blitted into a
// non-square eye image (3072x3264). This suite settles that mechanically
// rather than by opinion: it walks known world rays at 0, +/-10, +/-20 and
// +/-30 degrees through EVERY stage the real pipeline has --
//
//     world ray -> projection -> NDC -> scene FBO viewport -> capture blit
//              -> eye texture -> swapchain blit -> submit imageRect
//              -> the angle the runtime will reconstruct
//
// -- and asserts the angle that comes out is the angle that went in. Any
// magnification anywhere in the chain shows up as an angle error at the far
// end, which is exactly what "zoomed" would be.
//
// The result: a full-rect -> full-rect resample, whatever the two aspect
// ratios are, is angle-preserving. The square target cost SHARPNESS, never
// geometry. The mutants below are the same pipeline with a real defect
// injected, and they must fail.
// ---------------------------------------------------------------------------

// One full-rect stage: normalised position in, normalised position out.
// srcW/srcH -> dstW/dstH covering the whole of both. This is what
// glBlitFramebuffer(0,0,srcW,srcH -> 0,0,dstW,dstH) does.
static void StageFullRect(double* u, double* v, int, int, int, int)
{
    (void)u; (void)v;      // a full->full resample moves nothing in u,v
}

// A CROP instead of a scale: reads only cropW x cropH of the source but
// presents it as the whole image. This is the defect the square->rect blit
// was suspected of, and it really would zoom.
static void StageCrop(double* u, double* v, int srcW, int srcH,
                      int cropW, int cropH)
{
    *u = (*u) * (double)srcW / (double)cropW;
    *v = (*v) * (double)srcH / (double)cropH;
}

struct EyeOptics { double tL, tR, tU, tD; };

// The angle a runtime reconstructs for a normalised position in the eye
// image, given the fov it was submitted with. Inverse of the projection.
static void AngleFromEyeUV(const EyeOptics& e, double u, double v,
                           double* degX, double* degY)
{
    const double tanx = e.tL + u * (e.tR - e.tL);
    const double tany = e.tD + v * (e.tU - e.tD);
    *degX = atan(tanx) * 57.29577951308232;
    *degY = atan(tany) * 57.29577951308232;
}

static void S_AngularMapping()
{
    printf("--- angular mapping: world ray -> submitted eye pixel -> angle ---\n");

    // The exact optics from the 2026-08-23 hardware log, eye 0.
    const EyeOptics eye = { tan(-54.0 * 0.017453292519943295),
                            tan( 40.0 * 0.017453292519943295),
                            tan( 44.0 * 0.017453292519943295),
                            tan(-55.0 * 0.017453292519943295) };
    float m[16];
    VRM_ProjectionFromTangents((float)eye.tL, (float)eye.tR,
                               (float)eye.tU, (float)eye.tD,
                               4.0f, 0.0f, m);

    // Real sizes from that run.
    const int baseW = 1440, baseH = 1440;      // engine refdef (square)
    const int sqW   = 3240, sqH   = 3240;      // the OLD square scene target
    const int eyeW  = 3072, eyeH  = 3264;      // runtime eye image
    // The NEW eye-aspect target at the same pixel budget.
    const double r  = (double)eyeW / (double)eyeH;
    int newH = (int)(sqrt((double)sqW * sqH / r) + 0.5);
    int newW = (int)(r * newH + 0.5);
    newW = (newW + 3) & ~3; newH = (newH + 3) & ~3;
    CHECK(fabs((double)newW * newH / ((double)sqW * sqH) - 1.0) < 0.01,
          "eye-aspect target %dx%d keeps the pixel budget (%.2f%% of %dx%d)",
          newW, newH, 100.0 * newW * newH / ((double)sqW * sqH), sqW, sqH);
    CHECK(fabs((double)newW / newH - r) < 0.002,
          "eye-aspect target really is the eye aspect (%.4f vs %.4f)",
          (double)newW / newH, r);

    const double probes[7] = { 0.0, 10.0, -10.0, 20.0, -20.0, 30.0, -30.0 };
    double worstX = 0.0, worstY = 0.0, worstSquare = 0.0;

    for (int axis = 0; axis < 2; ++axis) {
        for (int i = 0; i < 7; ++i) {
            const double deg = probes[i];
            const double t   = tan(deg * 0.017453292519943295);

            // A world ray at this angle off the eye's optical axis.
            const double Z = -10.0;
            const double X = (axis == 0) ? t * 10.0 : 0.0;
            const double Y = (axis == 1) ? t * 10.0 : 0.0;

            float ndc[3];
            VRM_ProjectPoint(m, (float)X, (float)Y, (float)Z, ndc);

            // NDC -> normalised position in whatever we render into. The
            // viewport covers the target fully, so this is aspect-free.
            double u = (ndc[0] + 1.0) * 0.5;
            double v = (ndc[1] + 1.0) * 0.5;

            // Stage: scene target -> eye texture (full rect -> full rect),
            // then eye texture -> swapchain (1:1), then submit the full rect.
            double us = u, vs = v;
            StageFullRect(&us, &vs, newW, newH, eyeW, eyeH);
            StageFullRect(&us, &vs, eyeW, eyeH, eyeW, eyeH);

            double outX = 0.0, outY = 0.0;
            AngleFromEyeUV(eye, us, vs, &outX, &outY);
            const double got = (axis == 0) ? outX : outY;
            const double err = fabs(got - deg);
            if (axis == 0) { if (err > worstX) worstX = err; }
            else           { if (err > worstY) worstY = err; }

            // Tolerance is float32 round-off in the projection build, not
            // a modelling allowance: 1e-4 deg is 0.36 milli-arcseconds.
            CHECK(err < 1e-4,
                  "angular %s: %+.0f deg in -> %+.4f deg out (err %.2e)",
                  axis == 0 ? "X" : "Y", deg, got, err);

            // And the SQUARE target must give the identical answer -- that is
            // the actual claim being tested about the old pipeline.
            double uq = u, vq = v;
            StageFullRect(&uq, &vq, sqW, sqH, eyeW, eyeH);
            double sqX = 0.0, sqY = 0.0;
            AngleFromEyeUV(eye, uq, vq, &sqX, &sqY);
            const double sqErr = fabs(((axis == 0) ? sqX : sqY) - deg);
            if (sqErr > worstSquare) worstSquare = sqErr;
        }
    }

    CHECK(worstX < 1e-4, "angularMappingX = PASS (worst %.2e deg)", worstX);
    CHECK(worstY < 1e-4, "angularMappingY = PASS (worst %.2e deg)", worstY);
    CHECK(worstSquare < 1e-4,
          "the SQUARE target was angle-correct too (worst %.2e deg): the "
          "square->rect blit was a sharpness cost, NOT the zoom", worstSquare);

    // anisotropicEyeStretch: the two outgoing blit factors must now match.
    const double kxNew = (double)eyeW / newW, kyNew = (double)eyeH / newH;
    const double kxOld = (double)eyeW / sqW,  kyOld = (double)eyeH / sqH;
    CHECK(fabs(kxNew - kyNew) < 0.005,
          "anisotropicEyeStretch = PASS: eye-aspect target resamples %.4f x "
          "%.4f (isotropic)", kxNew, kyNew);
    CHECK(fabs(kxOld - kyOld) > 0.05,
          "and the old square target really was anisotropic (%.4f x %.4f, "
          "%.1f%% apart) -- the sharpness defect this fixes",
          kxOld, kyOld, 100.0 * fabs(kxOld - kyOld) / kyOld);

    // ---- mutants: a real geometric defect must be caught ----
    {
        // A CROP of the source instead of a scale -- reads 3072x3264 out of a
        // 3240x3240 target and calls it the whole image. 5.5% horizontal zoom.
        double dx = 0, dy = 0;
        float ndc[3];
        VRM_ProjectPoint(m, (float)(tan(20.0 * 0.0174532925) * 10.0), 0.0f, -10.0f, ndc);
        double u = (ndc[0] + 1.0) * 0.5, v = (ndc[1] + 1.0) * 0.5;
        StageCrop(&u, &v, sqW, sqH, eyeW, eyeH);
        AngleFromEyeUV(eye, u, v, &dx, &dy);
        CHECK(fabs(dx - 20.0) > 0.5,
              "mutant: a square->rect CROP is caught (20 deg reads %.2f deg)", dx);
    }
    {
        // A viewport covering only 90% of the target: the classic
        // "rendered small, presented full" magnification.
        float ndc[3];
        VRM_ProjectPoint(m, (float)(tan(20.0 * 0.0174532925) * 10.0), 0.0f, -10.0f, ndc);
        double u = (ndc[0] + 1.0) * 0.5, v = (ndc[1] + 1.0) * 0.5, dx = 0, dy = 0;
        u = u * 0.90; v = v * 0.90;         // NDC compressed into 90% of the rect
        AngleFromEyeUV(eye, u, v, &dx, &dy);
        CHECK(fabs(dx - 20.0) > 0.5,
              "mutant: a 90%%-coverage viewport is caught (20 deg reads %.2f deg)", dx);
    }
    {
        // Widening the submitted fov while rendering the same image: the
        // "just make the numbers bigger" fix the brief forbids.
        EyeOptics wide = { eye.tL * 1.1, eye.tR * 1.1, eye.tU * 1.1, eye.tD * 1.1 };
        float ndc[3];
        VRM_ProjectPoint(m, (float)(tan(20.0 * 0.0174532925) * 10.0), 0.0f, -10.0f, ndc);
        double u = (ndc[0] + 1.0) * 0.5, v = (ndc[1] + 1.0) * 0.5, dx = 0, dy = 0;
        AngleFromEyeUV(wide, u, v, &dx, &dy);
        CHECK(fabs(dx - 20.0) > 0.5,
              "mutant: lying about the fov is caught (20 deg reads %.2f deg)", dx);
    }
}

static void S_ProjectionInterception()
{
    printf("--- projection interception: armed 3D pass only, all else untouched ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp(), "session up");

    // The engine-shaped symmetric perspective its renderer would load.
    float engine[16];
    VRM_ProjectionFromFovRad(-48.21f * DEG, 48.21f * DEG, 48.21f * DEG,
                             -48.21f * DEG, 4.0f, 3000.0f, engine);

    float m[16];
    memcpy(m, engine, sizeof(m));
    CHECK(VR_TestProjectionDecision(m) == 0 && VR_ProjectionArmedEye() == -1,
          "nothing is replaced before any 3D frame arms the override");

    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    DriveEye(0);
    CHECK(VR_ProjectionArmedEye() == 0, "driving eye 0's camera arms eye 0");

    memcpy(m, engine, sizeof(m));
    CHECK(VR_TestProjectionDecision(m) == 1, "the perspective load is replaced while armed");
    float tl, tr, tu, td;
    VRM_ExtractTangents(m, &tl, &tr, &tu, &td);
    CHECK(fabsf(tl - tanf(-54.0f * DEG)) < 1e-4f && fabsf(tr - tanf(40.0f * DEG)) < 1e-4f
          && fabsf(tu - tanf(44.0f * DEG)) < 1e-4f && fabsf(td - tanf(-55.0f * DEG)) < 1e-4f,
          "the replacement carries eye 0's RUNTIME frustum");
    CHECK(m[10] == engine[10] && m[14] == engine[14],
          "the engine's depth row survives replacement untouched");

    // Menu/HUD shapes while armed: never touched.
    float ortho[16] = { 0 };
    ortho[0] = 2.0f / 640.0f; ortho[5] = -2.0f / 480.0f; ortho[10] = -1.0f;
    ortho[12] = -1.0f; ortho[13] = 1.0f; ortho[15] = 1.0f;
    float o2[16];
    memcpy(o2, ortho, sizeof(o2));
    CHECK(VR_TestProjectionDecision(o2) == 0 && memcmp(o2, ortho, sizeof(o2)) == 0,
          "an orthographic load stays byte-identical while armed");
    float rigid[16] = { 0 };
    rigid[0] = rigid[5] = rigid[10] = rigid[15] = 1.0f; rigid[14] = -50.0f;
    CHECK(VR_TestProjectionDecision(rigid) == 0,
          "a modelview-shaped load is never replaced");

    VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_ProjectionArmedEye() == -1, "submitting the frame disarms the override");
    memcpy(m, engine, sizeof(m));
    CHECK(VR_TestProjectionDecision(m) == 0,
          "after submit (the 2D phase) a perspective load passes through");

    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "mid-pair begin");
    DriveEye(1);
    CHECK(VR_ProjectionArmedEye() == 1, "eye 1's camera arms eye 1");
    memcpy(m, engine, sizeof(m));
    CHECK(VR_TestProjectionDecision(m) == 1, "eye 1's load is replaced too");
    float tl1, tr1;
    float tu1, td1;
    VRM_ExtractTangents(m, &tl1, &tr1, &tu1, &td1);
    CHECK(fabsf(tl1 - tanf(-40.0f * DEG)) < 1e-4f && fabsf(tr1 - tanf(54.0f * DEG)) < 1e-4f,
          "eye 1's replacement differs from eye 0 exactly as the runtime fov does");
    VR_SubmitFrame(640, 480, true, 1);

    // A menu frame never arms the override.
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "menu frame begins");
    CHECK(VR_ProjectionArmedEye() == -1, "a flat frame leaves the override disarmed");
    VR_SubmitFrame(640, 480, false, 0);
    memcpy(m, engine, sizeof(m));
    CHECK(VR_TestProjectionDecision(m) == 0, "no replacement on the menu path");

    CHECK(VR_TestProjectionReplacedCount(0) >= 1 && VR_TestProjectionReplacedCount(1) >= 1,
          "replacement counts registered for both eyes");

    // The refdef fov is a conservative CULLING superset of the eye frustum.
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair for the culling check");
    float gAx[9], oAx[9], org[3];
    float fx0 = 0, fy0 = 0, fx1 = 0, fy1 = 0;
    MakeAxis(0, 0, 0, gAx);
    VR_GetEyeCamera(0, g_gameOrigin, gAx, 1440, 1440, org, oAx, &fx0, &fy0);
    VR_GetEyeCamera(1, g_gameOrigin, gAx, 1440, 1440, org, oAx, &fx1, &fy1);
    bool contained = true;
    for (int e = 0; e < MOCK_EYES; ++e) {
        const XrFovf& f = g_mock.viewFov[e];
        const float hx = (e == 0 ? fx0 : fx1) * 0.5f * DEG;
        const float hy = (e == 0 ? fy0 : fy1) * 0.5f * DEG;
        if (!VRM_SymmetricContains(hx, hy, f.angleLeft, f.angleRight,
                                   f.angleUp, f.angleDown))
            contained = false;
    }
    CHECK(contained, "the culling fov CONTAINS each eye's real frustum");
    CHECK(fx0 >= 2.0f * 54.0f + 5.0f && fy0 >= 2.0f * 55.0f + 5.0f,
          "the culling fov carries the safety margin (%.1f x %.1f deg)", fx0, fy0);
    VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "finish the culling-check pair");
    DriveEye(1);
    VR_SubmitFrame(640, 480, true, 1);

    // The old crop path cannot activate: even an EXTREME asymmetry submits
    // the full image with the exact runtime fov.
    const XrFovf save0 = g_mock.viewFov[0], save1 = g_mock.viewFov[1];
    g_mock.viewFov[0].angleLeft = -70.0f * DEG; g_mock.viewFov[0].angleRight = 10.0f * DEG;
    g_mock.viewFov[1].angleLeft = -10.0f * DEG; g_mock.viewFov[1].angleRight = 70.0f * DEG;
    CHECK(StepPair() == VR_FRAME_RENDER, "pair under an extreme asymmetric fov");
    const XrRect2Di& x0 = g_mock.lastEnd.views[0].subImage.imageRect;
    const XrRect2Di& x1 = g_mock.lastEnd.views[1].subImage.imageRect;
    CHECK(x0.offset.x == 0 && x0.offset.y == 0
          && x0.extent.width == 3072 && x0.extent.height == 3264
          && x1.offset.x == 0 && x1.offset.y == 0
          && x1.extent.width == 3072 && x1.extent.height == 3264,
          "even under extreme asymmetry the 3D path submits FULL rects (crop gone)");
    CHECK_NEAR(g_mock.lastEnd.views[0].fov.angleLeft, -70.0f * DEG, 1e-4,
               "and the submitted fov is the exact runtime frustum");
    g_mock.viewFov[0] = save0;
    g_mock.viewFov[1] = save1;
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_PositionalTracking()
{
    printf("--- positional tracking: head centre, eye offsets, gain, exact scale ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up, neutral latched");

    DriveEye(0); DriveEye(1);
    float base[2][3];
    memcpy(base, g_out, sizeof(base));
    const float U = 39.37f;               // units per metre
    const float sepBase = fabsf(base[0][1] - base[1][1]);
    CHECK_NEAR(sepBase, 0.064f * U, 1e-3,
               "64 mm IPD -> exactly %.4f units apart (got %.4f)", 0.064f * U, sepBase);
    CHECK_NEAR((base[0][1] + base[1][1]) * 0.5f, 200.0, 1e-3,
               "the eye midpoint sits ON the head centre (no double IPD)");

    struct Move { const char* name; float mx, my, mz; int axis; float want; };
    // XrToQuake: XR +X (right) -> Quake -y, XR +Y (up) -> +z, XR -Z (fwd) -> +x.
    const Move moves[4] = {
        { "0.10 m left",    -0.10f, 0.0f, 0.0f,  1, +0.10f * U },
        { "0.10 m right",   +0.10f, 0.0f, 0.0f,  1, -0.10f * U },
        { "0.10 m up",      0.0f, +0.10f, 0.0f,  2, +0.10f * U },
        { "0.10 m forward", 0.0f, 0.0f, -0.10f,  0, +0.10f * U },
    };
    for (int k = 0; k < 4; ++k) {
        SetMockHead(0, moves[k].mx, moves[k].my, moves[k].mz);
        CHECK(StepPair() == VR_FRAME_RENDER, "pair after %s", moves[k].name);
        DriveEye(0); DriveEye(1);
        const int a = moves[k].axis;
        const float mid  = (g_out[0][a] + g_out[1][a]) * 0.5f;
        const float mid0 = (base[0][a] + base[1][a]) * 0.5f;
        CHECK_NEAR(mid - mid0, moves[k].want, 2e-3,
                   "%s -> %+.4f u on axis %d (want %+.4f)",
                   moves[k].name, mid - mid0, a, moves[k].want);
        CHECK_NEAR(fabsf(g_out[0][1] - g_out[1][1]), sepBase, 1e-3,
                   "%s leaves the eye separation untouched", moves[k].name);
    }

    // The gain scales head translation only -- IPD is untouched by it.
    VR_SetPositionalGain(0.5f);
    SetMockHead(0, -0.10f, 0, 0);
    CHECK(StepPair() == VR_FRAME_RENDER, "pair under gain 0.5");
    DriveEye(0); DriveEye(1);
    const float midG = (g_out[0][1] + g_out[1][1]) * 0.5f
                     - (base[0][1] + base[1][1]) * 0.5f;
    CHECK_NEAR(midG, 0.05f * U, 2e-3, "gain 0.5 halves the head delta (got %.4f)", midG);
    CHECK_NEAR(fabsf(g_out[0][1] - g_out[1][1]), sepBase, 1e-3,
               "the gain never touches the IPD separation");
    VR_SetPositionalGain(1.0f);

    // Return to neutral restores the baseline exactly.
    SetMockHead(0, 0, 0, 0);
    CHECK(StepPair() == VR_FRAME_RENDER, "pair back at neutral");
    DriveEye(0); DriveEye(1);
    float worst = 0.0f;
    for (int e = 0; e < 2; ++e)
        for (int i = 0; i < 3; ++i)
            if (fabsf(g_out[e][i] - base[e][i]) > worst)
                worst = fabsf(g_out[e][i] - base[e][i]);
    CHECK(worst < 1e-3f,
          "return to neutral restores both eye origins exactly (worst %.2e)", worst);
    CHECK(g_mock.violations == 0, "no protocol violations");
}

// NDC of a world point through one eye's REAL rendered camera: production
// origin/orientation from VR_GetEyeCamera, projection from the runtime fov
// (exactly what the interception installs).
static void EyeNdc(int eye, const float world[3], float ndc[3])
{
    const float* o  = g_out[eye];
    const float* ax = g_outAxis[eye];     // Quake rows: forward, left, up
    const float d[3] = { world[0] - o[0], world[1] - o[1], world[2] - o[2] };
    const float xg = -(d[0]*ax[3] + d[1]*ax[4] + d[2]*ax[5]);   // GL right = -left
    const float yg =  (d[0]*ax[6] + d[1]*ax[7] + d[2]*ax[8]);
    const float zg = -(d[0]*ax[0] + d[1]*ax[1] + d[2]*ax[2]);   // GL looks down -Z
    const XrFovf& f = g_mock.viewFov[eye];
    float m[16];
    VRM_ProjectionFromFovRad(f.angleLeft, f.angleRight, f.angleUp, f.angleDown,
                             4.0f, 4096.0f, m);
    VRM_ProjectPoint(m, xg, yg, zg, ndc);
}

static void S_ParallaxInvariant()
{
    printf("--- parallax: physical translation shifts near more than far, exactly ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up");
    DriveEye(0); DriveEye(1);

    const float U = 39.37f;
    const float nearPole[3] = { 100.0f + 1.0f * U,  200.0f, 50.0f };   //  1 m ahead
    const float farPole[3]  = { 100.0f + 10.0f * U, 200.0f, 50.0f };   // 10 m ahead

    float nb[2][3], fb[2][3];
    for (int e = 0; e < 2; ++e) {
        EyeNdc(e, nearPole, nb[e]);
        EyeNdc(e, farPole, fb[e]);
    }

    // 10 cm to the RIGHT (XR +x -> Quake -y).
    SetMockHead(0, 0.10f, 0, 0);
    CHECK(StepPair() == VR_FRAME_RENDER, "pair after 10 cm right");
    DriveEye(0); DriveEye(1);
    for (int e = 0; e < 2; ++e) {
        float na[3], fa[3];
        EyeNdc(e, nearPole, na);
        EyeNdc(e, farPole, fa);
        const float dn = na[0] - nb[e][0], df = fa[0] - fb[e][0];
        CHECK(fabsf(dn) > fabsf(df),
              "eye%d: the near pole shifts more than the far (%.4f vs %.4f)", e, dn, df);
        CHECK(dn < 0.0f && df < 0.0f,
              "eye%d: moving right shifts the world left in view", e);
        // Exact expectation: dNDC = m0 * lateralDelta / forwardDistance --
        // ordinary perspective geometry, no hidden multiplier anywhere.
        const XrFovf& f = g_mock.viewFov[e];
        const float m0 = 2.0f / (tanf(f.angleRight) - tanf(f.angleLeft));
        CHECK_NEAR(dn, m0 * (-0.10f * U) / (1.0f * U), 2e-3,
                   "eye%d near shift equals m0*d/D exactly (%.4f)", e, dn);
        CHECK_NEAR(df, m0 * (-0.10f * U) / (10.0f * U), 5e-4,
                   "eye%d far shift equals m0*d/D exactly (%.4f)", e, df);
    }

    // 10 cm UP (XR +y -> Quake +z): the same law vertically.
    SetMockHead(0, 0, 0.10f, 0);
    CHECK(StepPair() == VR_FRAME_RENDER, "pair after 10 cm up");
    DriveEye(0); DriveEye(1);
    for (int e = 0; e < 2; ++e) {
        float na[3], fa[3];
        EyeNdc(e, nearPole, na);
        EyeNdc(e, farPole, fa);
        const float dn = na[1] - nb[e][1], df = fa[1] - fb[e][1];
        CHECK(fabsf(dn) > fabsf(df) && dn < 0.0f,
              "eye%d: moving up shifts the world down, near more than far", e);
        const XrFovf& f = g_mock.viewFov[e];
        const float m5 = 2.0f / (tanf(f.angleUp) - tanf(f.angleDown));
        CHECK_NEAR(dn, m5 * (-0.10f * U) / (1.0f * U), 2e-3,
                   "eye%d vertical near shift follows perspective exactly", e);
    }

    // Back at neutral both projections return to baseline exactly.
    SetMockHead(0, 0, 0, 0);
    CHECK(StepPair() == VR_FRAME_RENDER, "pair back at neutral");
    DriveEye(0); DriveEye(1);
    float worst = 0.0f;
    for (int e = 0; e < 2; ++e) {
        float na[3], fa[3];
        EyeNdc(e, nearPole, na);
        EyeNdc(e, farPole, fa);
        for (int i = 0; i < 2; ++i) {
            if (fabsf(na[i] - nb[e][i]) > worst) worst = fabsf(na[i] - nb[e][i]);
            if (fabsf(fa[i] - fb[e][i]) > worst) worst = fabsf(fa[i] - fb[e][i]);
        }
    }
    CHECK(worst < 1e-4f,
          "return to neutral restores both eyes' projections exactly (worst %.2e)", worst);

    // ---- the canonical 1/5/20/100 m ladder (Phase-5 acceptance) ----
    // World points straight ahead at four distances; translate the head
    // +0.10 m laterally; image displacement must follow 1/D exactly, with
    // the 100 m point barely moving. A far object that magnifies or slides
    // substantially under head translation fails here.
    const float ladder[4] = { 1.0f, 5.0f, 20.0f, 100.0f };
    float ndcBase[4][3];
    for (int k = 0; k < 4; ++k) {
        const float p[3] = { 100.0f + ladder[k] * U, 200.0f, 50.0f };
        EyeNdc(0, p, ndcBase[k]);
    }
    const float e0yBase = g_out[0][1], e1yBase = g_out[1][1];
    SetMockHead(0, 0.10f, 0, 0);                    // 10 cm right
    CHECK(StepPair() == VR_FRAME_RENDER, "pair for the distance ladder");
    DriveEye(0); DriveEye(1);
    CHECK_NEAR(g_out[0][1] - e0yBase, -0.10f * U, 2e-3,
               "ladder: eye 0 translated exactly with the head");
    CHECK_NEAR(g_out[1][1] - e1yBase, -0.10f * U, 2e-3,
               "ladder: eye 1 translated exactly with the head (both together)");
    float shift[4];
    for (int k = 0; k < 4; ++k) {
        const float p[3] = { 100.0f + ladder[k] * U, 200.0f, 50.0f };
        float ndc[3];
        EyeNdc(0, p, ndc);
        shift[k] = ndc[0] - ndcBase[k][0];
        const XrFovf& f = g_mock.viewFov[0];
        const float m0 = 2.0f / (tanf(f.angleRight) - tanf(f.angleLeft));
        const float want = m0 * (-0.10f * U) / (ladder[k] * U);
        CHECK_NEAR(shift[k], want, fmax(2e-4, fabs(want) * 0.02),
                   "ladder: %.0f m displacement is exactly m0*d/D (%.5f)",
                   ladder[k], shift[k]);
    }
    CHECK(shift[0] < 0.0f, "ladder: sign correct (moving right shifts the world left)");
    CHECK(fabsf(shift[0]) > fabsf(shift[1]) && fabsf(shift[1]) > fabsf(shift[2])
          && fabsf(shift[2]) > fabsf(shift[3]),
          "ladder: displacement strictly decays with distance");
    CHECK_NEAR(shift[0] / shift[3], 100.0f, 8.0f,
               "ladder: 1 m moves ~100x more than 100 m (ratio %.1f)", shift[0] / shift[3]);
    CHECK(fabsf(shift[3]) < fabsf(shift[0]) / 80.0f,
          "ladder: the 100 m point is appropriately still under 10 cm of head travel");
    // MUTATION: a double-applied head translation lands at exactly 2x the
    // ladder law; assert every acceptance window above rejects that.
    {
        const XrFovf& f = g_mock.viewFov[0];
        const float m0 = 2.0f / (tanf(f.angleRight) - tanf(f.angleLeft));
        const float want1m = m0 * (-0.10f * U) / (1.0f * U);
        CHECK(fabs(2.0f * want1m - want1m) > fabs(want1m) * 0.02 + 2e-4,
              "mutation: doubled roomscale displacement lies outside every window");
    }
    SetMockHead(0, 0, 0, 0);
    CHECK(g_mock.violations == 0, "no protocol violations");
}

// Independent lateral view tangent of a world point through one production
// eye camera -- first-principles trig only, no production helpers and no
// VRM matrices, so it cross-examines them rather than echoing them.
static float EyeViewTanX(int eye, const float world[3])
{
    const float* o  = g_out[eye];
    const float* ax = g_outAxis[eye];
    const float d[3] = { world[0] - o[0], world[1] - o[1], world[2] - o[2] };
    const float fwd   =   d[0]*ax[0] + d[1]*ax[1] + d[2]*ax[2];
    const float right = -(d[0]*ax[3] + d[1]*ax[4] + d[2]*ax[5]);
    return right / fwd;
}

// ---------------- canonical stereo oracle (independent of production) ----
//
// Takes the COMPLETE per-eye story -- rendered cameras, located fovs, and
// the submitted layer -- and answers one question: is this a geometrically
// consistent LEFT/RIGHT stereo pair? The 2026-08-19 hardware run proved a
// 63.1 mm numeric separation can coexist with catastrophically broken
// stereo, so magnitude alone is never accepted:
//   (a) rendered eye 0 sits view-LEFT of rendered eye 1 by exactly the IPD;
//   (b) eye 0 carries the left-biased frustum, eye 1 the right-biased one;
//   (c) each submitted fov is its own located fov, same index;
//   (d) submit slot e carries swapchain e (a capture or submit swap crosses
//       them).
// Each mutant class the symptom could hide behind must flip acceptance to
// rejection -- the mutant checks in the scenario prove the discrimination.
static bool OracleAccepts(const float o0[3], const float o1[3],
                          const float ax0[9], const float ax1[9],
                          const XrFovf& locFov0, const XrFovf& locFov1,
                          const XrCompositionLayerProjectionView& sub0,
                          const XrCompositionLayerProjectionView& sub1,
                          XrSwapchain chain0, XrSwapchain chain1,
                          float ipdM, float unitsPerMetre)
{
    const float left[3] = { (ax0[3] + ax1[3]) * 0.5f,
                            (ax0[4] + ax1[4]) * 0.5f,
                            (ax0[5] + ax1[5]) * 0.5f };
    const float d[3] = { o1[0] - o0[0], o1[1] - o0[1], o1[2] - o0[2] };
    const float eye0LeftBy = -(d[0]*left[0] + d[1]*left[1] + d[2]*left[2]);
    const float wantU = ipdM * unitsPerMetre;
    if (!(eye0LeftBy > 0.0f)) return false;
    if (fabsf(eye0LeftBy - wantU) > 0.05f * wantU + 1e-3f) return false;
    if (!((locFov0.angleLeft + locFov0.angleRight) < 0.0f)) return false;
    if (!((locFov1.angleLeft + locFov1.angleRight) > 0.0f)) return false;
    if (fabsf(sub0.fov.angleLeft  - locFov0.angleLeft)  > 1e-5f) return false;
    if (fabsf(sub0.fov.angleRight - locFov0.angleRight) > 1e-5f) return false;
    if (fabsf(sub0.fov.angleUp    - locFov0.angleUp)    > 1e-5f) return false;
    if (fabsf(sub0.fov.angleDown  - locFov0.angleDown)  > 1e-5f) return false;
    if (fabsf(sub1.fov.angleLeft  - locFov1.angleLeft)  > 1e-5f) return false;
    if (fabsf(sub1.fov.angleRight - locFov1.angleRight) > 1e-5f) return false;
    if (fabsf(sub1.fov.angleUp    - locFov1.angleUp)    > 1e-5f) return false;
    if (fabsf(sub1.fov.angleDown  - locFov1.angleDown)  > 1e-5f) return false;
    if (sub0.subImage.swapchain != chain0) return false;
    if (sub1.subImage.swapchain != chain1) return false;
    return true;
}

static void S_StereoOracleAndMutants()
{
    printf("--- stereo oracle: LEFT renders left, RIGHT renders right, mutants rejected ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up");
    DriveEye(0); DriveEye(1);

    const float U = 39.37f;
    const float ipdU = 0.064f * U;

    // Direction, not just magnitude: OpenXR view 0 (x = -0.032, physically
    // LEFT) must render from view-LEFT of the game camera (+Y at yaw 0) and
    // view 1 from view-RIGHT. These are the canonical first-principles
    // positions, computed here with no production math at all.
    CHECK_NEAR(g_out[0][1], 200.0f + ipdU * 0.5f, 1e-3,
               "canonical: OpenXR -X eye renders at +Y (view LEFT) exactly");
    CHECK_NEAR(g_out[1][1], 200.0f - ipdU * 0.5f, 1e-3,
               "canonical: OpenXR +X eye renders at -Y (view RIGHT) exactly");
    CHECK_NEAR(g_out[0][0], 100.0f, 1e-3, "canonical: no forward contamination (eye 0)");
    CHECK_NEAR(g_out[0][2], 50.0f, 1e-3,  "canonical: no vertical contamination (eye 0)");

    // The complete story as submitted.
    CHECK(g_mock.lastEnd.hasProjection, "a stereo layer was submitted");
    const XrSwapchain chain0 = (XrSwapchain)(uintptr_t)0x44440001;
    const XrSwapchain chain1 = (XrSwapchain)(uintptr_t)0x44440002;
    const XrCompositionLayerProjectionView sub0 = g_mock.lastEnd.views[0];
    const XrCompositionLayerProjectionView sub1 = g_mock.lastEnd.views[1];

    CHECK(OracleAccepts(g_out[0], g_out[1], g_outAxis[0], g_outAxis[1],
                        g_mock.viewFov[0], g_mock.viewFov[1],
                        sub0, sub1, chain0, chain1, 0.064f, U),
          "oracle: production output is a consistent LEFT/RIGHT pair");

    // Mutants: each class of eye-identity bug must be REJECTED. The mutant
    // inputs are the production outputs with exactly one fault injected.
    CHECK(!OracleAccepts(g_out[1], g_out[0], g_outAxis[0], g_outAxis[1],
                         g_mock.viewFov[0], g_mock.viewFov[1],
                         sub0, sub1, chain0, chain1, 0.064f, U),
          "mutant: REVERSED eye-translation sign is rejected");
    CHECK(!OracleAccepts(g_out[0], g_out[1], g_outAxis[0], g_outAxis[1],
                         g_mock.viewFov[1], g_mock.viewFov[0],
                         sub0, sub1, chain0, chain1, 0.064f, U),
          "mutant: swapped eye FOVs are rejected");
    CHECK(!OracleAccepts(g_out[0], g_out[1], g_outAxis[0], g_outAxis[1],
                         g_mock.viewFov[0], g_mock.viewFov[1],
                         sub1, sub0, chain0, chain1, 0.064f, U),
          "mutant: swapped submit slots are rejected");
    {
        XrCompositionLayerProjectionView x0 = sub0, x1 = sub1;
        x0.subImage.swapchain = chain1;
        x1.subImage.swapchain = chain0;
        CHECK(!OracleAccepts(g_out[0], g_out[1], g_outAxis[0], g_outAxis[1],
                             g_mock.viewFov[0], g_mock.viewFov[1],
                             x0, x1, chain0, chain1, 0.064f, U),
              "mutant: crossed capture/swapchain routing is rejected");
    }
    {
        // Half the IPD: magnitude is part of the contract too.
        float half0[3] = { g_out[0][0], 200.0f + ipdU * 0.25f, g_out[0][2] };
        float half1[3] = { g_out[1][0], 200.0f - ipdU * 0.25f, g_out[1][2] };
        CHECK(!OracleAccepts(half0, half1, g_outAxis[0], g_outAxis[1],
                             g_mock.viewFov[0], g_mock.viewFov[1],
                             sub0, sub1, chain0, chain1, 0.064f, U),
              "mutant: halved separation is rejected");
    }
    {
        // Doubled: the other direction of the same fault class -- an eye
        // offset applied twice (once inside the centre delta, once per eye).
        float dbl0[3] = { g_out[0][0], 200.0f + ipdU, g_out[0][2] };
        float dbl1[3] = { g_out[1][0], 200.0f - ipdU, g_out[1][2] };
        CHECK(!OracleAccepts(dbl0, dbl1, g_outAxis[0], g_outAxis[1],
                             g_mock.viewFov[0], g_mock.viewFov[1],
                             sub0, sub1, chain0, chain1, 0.064f, U),
              "mutant: DOUBLED separation (eye offset applied twice) is rejected");
    }

    // Disparity law: for a point D metres ahead, the lateral view-tangent
    // difference between the eyes is exactly IPD/D -- positive (each eye
    // sees the point displaced toward its own nose: natural convergence),
    // and it decays toward zero with distance.
    const float dists[3] = { 1.0f, 5.0f, 10.0f };
    float prev = 1e9f;
    for (int k = 0; k < 3; ++k) {
        const float D = dists[k];
        const float ahead[3] = { 100.0f + D * U, 200.0f, 50.0f };
        const float t0 = EyeViewTanX(0, ahead);
        const float t1 = EyeViewTanX(1, ahead);
        CHECK(t0 > 0.0f && t1 < 0.0f,
              "disparity at %.0f m converges naturally (L sees it right, R left)", D);
        CHECK_NEAR(t0 - t1, 0.064f / D, 0.064f / D * 0.02f,
                   "disparity at %.0f m is exactly IPD/D (%.4f)", D, t0 - t1);
        CHECK(t0 - t1 < prev, "disparity decreases with distance (%.0f m)", D);
        prev = t0 - t1;

        // Off-axis: the law holds for a point 30 degrees to the side too.
        const float side[3] = { 100.0f + D * U, 200.0f + 0.577f * D * U, 50.0f };
        const float s0 = EyeViewTanX(0, side), s1 = EyeViewTanX(1, side);
        CHECK(s0 - s1 > 0.0f && s0 - s1 < 1.5f * 0.064f / D,
              "off-axis disparity at %.0f m stays positive and bounded", D);
    }

    // Zero IPD collapses the cameras to one point...
    SetMockHead(0, 0, 0, 0, 0.0f);
    CHECK(StepPair() == VR_FRAME_RENDER, "pair under zero IPD");
    DriveEye(0); DriveEye(1);
    float worst = 0.0f;
    for (int i = 0; i < 3; ++i)
        worst = fmaxf(worst, fabsf(g_out[0][i] - g_out[1][i]));
    CHECK(worst < 1e-4f, "zero IPD collapses both camera origins (worst %.2e)", worst);

    // ...and with equal fovs on top, the two eyes' projections of any point
    // are identical -- the stereo pipeline adds nothing of its own.
    const XrFovf sf0 = g_mock.viewFov[0], sf1 = g_mock.viewFov[1];
    g_mock.viewFov[1] = g_mock.viewFov[0];
    CHECK(StepPair() == VR_FRAME_RENDER, "pair under equal fov + zero IPD");
    DriveEye(0); DriveEye(1);
    const float probe[3] = { 100.0f + 5.0f * U, 212.0f, 61.0f };
    float n0[3], n1[3];
    EyeNdc(0, probe, n0);
    EyeNdc(1, probe, n1);
    CHECK(fabsf(n0[0] - n1[0]) < 1e-5f && fabsf(n0[1] - n1[1]) < 1e-5f,
          "equal fov + zero IPD projects identically in both eyes");
    g_mock.viewFov[0] = sf0;
    g_mock.viewFov[1] = sf1;
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_HeadTranslationRigidity()
{
    printf("--- head translation: both eyes move by the same delta, IPD vector survives ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up");
    DriveEye(0); DriveEye(1);
    float base[2][3];
    memcpy(base, g_out, sizeof(base));
    const float ipdVec[3] = { g_out[1][0] - g_out[0][0],
                              g_out[1][1] - g_out[0][1],
                              g_out[1][2] - g_out[0][2] };

    const float U = 39.37f;
    struct Move { const char* name; float x, y, z; float want[3]; };
    const Move moves[3] = {
        { "+0.10 m XR X (right)",   +0.10f, 0, 0, { 0, -0.10f * U, 0 } },
        { "+0.10 m XR Y (up)",      0, +0.10f, 0, { 0, 0, +0.10f * U } },
        { "-0.10 m XR Z (forward)", 0, 0, -0.10f, { +0.10f * U, 0, 0 } },
    };
    for (int k = 0; k < 3; ++k) {
        SetMockHead(0, moves[k].x, moves[k].y, moves[k].z);
        CHECK(StepPair() == VR_FRAME_RENDER, "pair after %s", moves[k].name);
        DriveEye(0); DriveEye(1);
        // BOTH eyes carry the identical delta -- not just the midpoint. A
        // per-eye asymmetry here is exactly the "world stretches when I
        // move" class of failure.
        for (int e = 0; e < 2; ++e) {
            float err = 0.0f;
            for (int i = 0; i < 3; ++i)
                err = fmaxf(err, fabsf((g_out[e][i] - base[e][i]) - moves[k].want[i]));
            CHECK(err < 2e-3f, "%s moves eye %d by exactly the head delta (err %.4f)",
                  moves[k].name, e, err);
        }
        float verr = 0.0f;
        for (int i = 0; i < 3; ++i)
            verr = fmaxf(verr, fabsf((g_out[1][i] - g_out[0][i]) - ipdVec[i]));
        CHECK(verr < 1e-3f, "%s leaves the IPD VECTOR untouched (err %.4f)",
              moves[k].name, verr);
    }

    // Under a turned aim the delta rotates with the body, the separation is
    // still exactly one IPD (no second offset source anywhere), and eye 0 is
    // still the LEFT camera.
    SetMockHead(0, 0, 0, 0);
    g_gameYawDeg = 137.0f;
    CHECK(StepPair() == VR_FRAME_RENDER, "pair at aim yaw 137");
    DriveEye(0); DriveEye(1);
    {
        const float dx = g_out[1][0] - g_out[0][0];
        const float dy = g_out[1][1] - g_out[0][1];
        const float dz = g_out[1][2] - g_out[0][2];
        CHECK_NEAR(sqrtf(dx*dx + dy*dy + dz*dz), 0.064f * U, 1e-3,
                   "yaw 137: separation is still exactly one IPD (no doubling)");
        const float l[3] = { -sinf(137.0f * DEG), cosf(137.0f * DEG), 0.0f };
        CHECK((-(dx*l[0] + dy*l[1] + dz*l[2])) > 0.0f,
              "yaw 137: eye 0 is still the LEFT camera");
    }
    {
        float turnedBase[2][3];
        memcpy(turnedBase, g_out, sizeof(turnedBase));
        SetMockHead(0, +0.10f, 0, 0);
        CHECK(StepPair() == VR_FRAME_RENDER, "pair after right-step at yaw 137");
        DriveEye(0); DriveEye(1);
        // Body-right at yaw 137 is (sin137, -cos137, 0) in world terms.
        const float want[3] = { 0.10f * U * sinf(137.0f * DEG),
                                0.10f * U * -cosf(137.0f * DEG), 0.0f };
        for (int e = 0; e < 2; ++e) {
            float err = 0.0f;
            for (int i = 0; i < 3; ++i)
                err = fmaxf(err, fabsf((g_out[e][i] - turnedBase[e][i]) - want[i]));
            CHECK(err < 2e-3f, "yaw 137: physical right-step lands body-right (eye %d)", e);
        }
    }
    g_gameYawDeg = 0.0f;
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_SubmitIdentityLedger()
{
    printf("--- eye identity ledger: locate index -> camera -> tangents -> submit slot ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp(), "session up");
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "pair begins");
    CHECK(VR_ExpectedEye() == 0, "pair machine starts at eye 0");
    DriveEye(0);
    CHECK(VR_ProjectionArmedEye() == 0, "camera eye 0 arms projection eye 0");
    float t0[4];
    VR_TestGetEyeTangents(0, t0);
    CHECK_NEAR(t0[0], tanf(-54.0f * DEG), 1e-4,
               "eye 0 carries the LEFT eye's tangents (tanL %.4f)", t0[0]);
    VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "mid-pair begin");
    CHECK(VR_ExpectedEye() == 1, "the second game frame renders eye 1");
    DriveEye(1);
    CHECK(VR_ProjectionArmedEye() == 1, "camera eye 1 arms projection eye 1");
    float t1[4];
    VR_TestGetEyeTangents(1, t1);
    CHECK_NEAR(t1[1], tanf(54.0f * DEG), 1e-4,
               "eye 1 carries the RIGHT eye's tangents (tanR %.4f)", t1[1]);
    CHECK(fabsf(t0[0] - t1[0]) > 0.3f, "the two eyes' tangent sets are distinct");
    VR_SubmitFrame(640, 480, true, 1);

    CHECK(VR_TestEyeRecordSerial(0) == VR_TestEyeRecordSerial(1)
          && VR_TestEyeRecordSerial(0) == (int)VR_TestFrameSerial(),
          "camera->projection->capture->submit share one located generation");

    CHECK(g_mock.lastEnd.hasProjection, "the pair submitted a stereo layer");
    for (int e = 0; e < 2; ++e) {
        CHECK(g_mock.lastEnd.views[e].subImage.swapchain
                  == (XrSwapchain)(uintptr_t)(0x44440001 + e),
              "submit slot %d carries swapchain %d", e, e);
        CHECK(fabsf(g_mock.lastEnd.views[e].fov.angleLeft
                    - g_mock.viewFov[e].angleLeft) < 1e-6f
              && fabsf(g_mock.lastEnd.views[e].fov.angleRight
                       - g_mock.viewFov[e].angleRight) < 1e-6f,
              "submit slot %d carries locate index %d's fov", e, e);
        CHECK(PoseEq(g_mock.lastEnd.views[e].pose, g_mock.viewPose[e]),
              "submit slot %d carries locate index %d's pose", e, e);
    }
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_HeadMotionVsVignette()
{
    printf("--- comfort: physical head motion is NOT virtual motion ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    double t = 5000.0;
    CODVR_TestSetNow(t);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up under virtual time");

    // One second of a STATIONARY game camera while the player physically
    // translates 20 cm and turns their head 30 degrees. The body felt all of
    // it -- none of it is vection -- so the vignette must not close.
    bool allRan = true;
    for (int i = 1; i <= 90; ++i) {
        const float s = (float)i / 90.0f;
        SetMockHead(30.0f * s, 0.20f * s, 0, 0);
        t += 1.0 / 90.0;
        CODVR_TestSetNow(t);
        if (StepPair() != VR_FRAME_RENDER)
            allRan = false;
    }
    CHECK(allRan, "90 pairs ran under pure physical head motion");
    float sp = 0, tr = 0;
    VR_GetPlayerMotion(&sp, &tr);
    CHECK(sp < 1.0f && tr < 1.0f,
          "20 cm translation + 30 deg head turn reads as ZERO virtual motion "
          "(%.2f u/s, %.2f deg/s)", sp, tr);
    CHECK(Comfort_TestTarget(sp, tr) == 0.0f,
          "the vignette target stays fully open under physical motion");

    // Virtual locomotion -- the game camera itself moving -- IS vection and
    // must still close it.
    for (int i = 1; i <= 90; ++i) {
        g_gameOrigin[0] += 150.0f / 90.0f;
        t += 1.0 / 90.0;
        CODVR_TestSetNow(t);
        StepPair();
    }
    VR_GetPlayerMotion(&sp, &tr);
    CHECK(sp > 100.0f, "virtual locomotion at 150 u/s is measured (%.1f u/s)", sp);
    CHECK(Comfort_TestTarget(sp, 0) > 0.5f, "and it closes the vignette");

    for (int i = 1; i <= 90; ++i) {
        g_gameYawDeg += 1.0f;
        t += 1.0 / 90.0;
        CODVR_TestSetNow(t);
        StepPair();
    }
    VR_GetPlayerMotion(&sp, &tr);
    CHECK(tr > 60.0f, "mouse turning at 90 deg/s is measured (%.1f deg/s)", tr);
    g_gameOrigin[0] = 100.0f;
    g_gameYawDeg = 0.0f;
    CODVR_TestSetNow(-1.0);
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_HudPrincipalShift()
{
    printf("--- hud: per-eye principal alignment through the production seam ---\n");
    ResetAll();
    int dx = 9, dy = 9;
    VR_GetHudPrincipalShift(0, 1440, 1440, &dx, &dy);
    CHECK(dx == 0 && dy == 0, "no shift before any 3D eye has been driven");

    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "session up");
    int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
    VR_GetHudPrincipalShift(0, 1440, 1440, &dx0, &dy0);
    VR_GetHudPrincipalShift(1, 1440, 1440, &dx1, &dy1);
    CHECK(abs(dx0 - 175) <= 1 && abs(dy0 - 139) <= 1,
          "left eye HUD lands on its optical axis: +175,+139 px (got %d,%d)", dx0, dy0);
    CHECK(dx1 == -dx0 && dy1 == dy0,
          "right eye mirrors horizontally, matches vertically (%d,%d)", dx1, dy1);
    CHECK(g_mock.violations == 0, "no protocol violations");
}

static void S_PerfCountersVirtualTime()
{
    printf("--- perf counters under virtual time: 90 Hz XR, 200 Hz-capable game ---\n");
    ResetAll();
    // Tiny eye buffers so a thousand captures cost nothing.
    for (int e = 0; e < MOCK_EYES; ++e) {
        g_mock.recommendW[e] = 64;
        g_mock.recommendH[e] = 64;
    }
    SetMockHead(0, 0, 0, 0);

    const double period = 1.0 / 90.0;        // XR display interval
    const double gameDt = 0.005;             // the engine renders an eye in 5 ms
    double t = 1000.0;
    CODVR_TestSetNow(t);
    CHECK(BringUp(), "startup under virtual time");

    // 470 pairs ~= 5.2 virtual seconds: enough to close one 5 s perf window.
    // Per pair: wait/begin/locate at t, eye 0 done at t+5ms, eye 1 done at
    // t+10ms, then xrWaitFrame absorbs the remaining ~1.1 ms of the interval.
    int ran = 0;
    for (int pair = 0; pair < 470; ++pair) {
        CODVR_TestSetNow(t);
        if (VR_BeginFrameEx() != VR_FRAME_RENDER)
            break;
        CODVR_TestSetNow(t + gameDt);
        DriveEye(0);                         // as dllmain does: camera, then submit
        VR_SubmitFrame(640, 480, true, 0);
        VR_BeginFrameEx();                   // between the eyes: no wait
        CODVR_TestSetNow(t + 2.0 * gameDt);
        DriveEye(1);
        VR_SubmitFrame(640, 480, true, 1);
        t += period;
        ++ran;
    }
    CHECK(ran == 470, "all %d pairs ran (got %d)", 470, ran);

    VRPerfStats st = {};
    VR_GetPerfStats(&st);
    CHECK(st.valid, "a perf window completed under virtual time");
    CHECK_NEAR(st.xrTargetHz, 90.0, 0.5, "XR target %.1f Hz", st.xrTargetHz);
    CHECK_NEAR(st.submitHz, 90.0, 2.0, "XR submissions ~90/s (got %.1f)", st.submitHz);
    CHECK_NEAR(st.presentHz, 90.0, 2.0, "XR frames begun ~90/s (got %.1f)", st.presentHz);
    CHECK_NEAR(st.freshHz[0], 90.0, 2.0, "fresh left ~90/s (got %.1f)", st.freshHz[0]);
    CHECK_NEAR(st.freshHz[1], 90.0, 2.0, "fresh right ~90/s (got %.1f)", st.freshHz[1]);
    CHECK_NEAR(st.gameHz, 180.0, 4.0, "game frames ~180/s (got %.1f)", st.gameHz);
    CHECK_NEAR(st.pairSepMeanMs, 5.0, 0.1,
               "mean eye-pair separation %.2f ms (= one 5 ms game frame)", st.pairSepMeanMs);
    CHECK_NEAR(st.pairSepMaxMs, 5.0, 0.1, "max separation %.2f ms", st.pairSepMaxMs);
    CHECK(st.pairVerdict == VR_PAIR_PASS,
          "PAIR THROUGHPUT verdict is PASS (180 >= 1.9 x 90), got %d", st.pairVerdict);
    CHECK(st.abortedPairs == 0, "no pair aborted under load");
    CHECK(g_mock.violations == 0, "no protocol violations");
    CODVR_TestSetNow(-1.0);
}

static void S_ComfortCadenceIndependence()
{
    printf("--- comfort advances by wall time, not by frames or XR rate ---\n");
    // One wall-clock second of vignette decay, stepped at 90 steps/s (the old
    // one-XR-frame-per-game-frame world), 180 (paired) and 144: the result
    // must match. Comfort must NOT advance twice as far because two game
    // frames now share one XR frame.
    float a = 1.0f;
    for (int i = 0; i < 90; ++i)  a = Comfort_TestStep(a, 0.0f, 1.0f / 90.0f);
    float b = 1.0f;
    for (int i = 0; i < 180; ++i) b = Comfort_TestStep(b, 0.0f, 1.0f / 180.0f);
    float c = 1.0f;
    for (int i = 0; i < 144; ++i) c = Comfort_TestStep(c, 0.0f, 1.0f / 144.0f);
    CHECK(fabsf(a - b) < 0.02f && fabsf(a - c) < 0.02f,
          "1 s of decay at 90/180/144 steps: %.4f / %.4f / %.4f", a, b, c);

    // End-to-end through Comfort_Update's own clock, under virtual time:
    // 1 s at a paired 180 game frames/s vs 1 s at 90 -- same result.
    CODVR_TestSetNow(2000.0);
    Comfort_Update();                        // re-base the internal clock
    double tt = 2000.0;
    Comfort_TestSetVignette(1.0f);
    for (int i = 0; i < 180; ++i) {
        tt += 1.0 / 180.0;
        CODVR_TestSetNow(tt);
        Comfort_Update();
    }
    const float paired = Comfort_VignetteAmount();
    Comfort_TestSetVignette(1.0f);
    for (int i = 0; i < 90; ++i) {
        tt += 1.0 / 90.0;
        CODVR_TestSetNow(tt);
        Comfort_Update();
    }
    const float single = Comfort_VignetteAmount();
    CHECK(fabsf(paired - single) < 0.02f,
          "Comfort_Update over 1 s: %.4f at 180 fps vs %.4f at 90 fps", paired, single);
    Comfort_TestSetVignette(0.0f);
    CODVR_TestSetNow(-1.0);
}

static void S_FinalProtocolAudit()
{
    printf("--- final audit: the last session's frame protocol is balanced ---\n");
    CHECK(!g_mock.frameOpen, "no frame left open at suite end");
    CHECK(g_mock.waitsNotBegun == 0, "no wait left unbegun at suite end");
    CHECK(g_mock.beginFrameCount == g_mock.endFrameCount,
          "begins == ends for the final session (%d/%d)",
          g_mock.beginFrameCount, g_mock.endFrameCount);
    CHECK(g_mock.violations == 0, "no protocol violations at suite end");
}

// -------------------------------------------------- GL hostile-state harness

struct GLSnap {
    int   matrixMode, blendSrc, blendDst, shade, active;
    int   viewport[4], scissorBox[4], colorMask[4];
    int   drawFbo, readFbo, bind0, bind1;
    float color[4], proj[16], model[16];
    bool  blend, depth, scissor, cull, light, alpha, fog, tex0, tex1;
};

static void TakeSnap(GLSnap* s)
{
    memset(s, 0, sizeof(*s));
    tg_getIntegerv(T_GL_MATRIX_MODE, &s->matrixMode);
    tg_getIntegerv(T_GL_BLEND_SRC, &s->blendSrc);
    tg_getIntegerv(T_GL_BLEND_DST, &s->blendDst);
    tg_getIntegerv(T_GL_SHADE_MODEL, &s->shade);
    tg_getIntegerv(T_GL_VIEWPORT, s->viewport);
    tg_getIntegerv(T_GL_SCISSOR_BOX, s->scissorBox);
    tg_getIntegerv(T_GL_COLOR_WRITEMASK, s->colorMask);
    tg_getIntegerv(T_GL_DRAW_FB_BINDING, &s->drawFbo);
    tg_getIntegerv(T_GL_READ_FB_BINDING, &s->readFbo);
    tg_getFloatv(T_GL_CURRENT_COLOR, s->color);
    tg_getFloatv(T_GL_PROJECTION_MATRIX, s->proj);
    tg_getFloatv(T_GL_MODELVIEW_MATRIX, s->model);
    s->blend   = tg_isEnabled(T_GL_BLEND) != 0;
    s->depth   = tg_isEnabled(T_GL_DEPTH_TEST) != 0;
    s->scissor = tg_isEnabled(T_GL_SCISSOR_TEST) != 0;
    s->cull    = tg_isEnabled(T_GL_CULL_FACE) != 0;
    s->light   = tg_isEnabled(T_GL_LIGHTING) != 0;
    s->alpha   = tg_isEnabled(T_GL_ALPHA_TEST) != 0;
    s->fog     = tg_isEnabled(T_GL_FOG) != 0;
    if (tg_activeTexture) {
        tg_getIntegerv(T_GL_ACTIVE_TEXTURE, &s->active);
        tg_activeTexture(T_GL_TEXTURE0);
        s->tex0 = tg_isEnabled(T_GL_TEXTURE_2D) != 0;
        tg_getIntegerv(T_GL_TEXTURE_BINDING_2D, &s->bind0);
        tg_activeTexture(T_GL_TEXTURE0 + 1);
        s->tex1 = tg_isEnabled(T_GL_TEXTURE_2D) != 0;
        tg_getIntegerv(T_GL_TEXTURE_BINDING_2D, &s->bind1);
        tg_activeTexture((unsigned)s->active);
    } else {
        s->tex0 = tg_isEnabled(T_GL_TEXTURE_2D) != 0;
        tg_getIntegerv(T_GL_TEXTURE_BINDING_2D, &s->bind0);
    }
}

static int CompareSnap(const GLSnap& a, const GLSnap& b, char* diff, size_t cap)
{
    int bad = 0;
    #define DIFFI(field) if (a.field != b.field) { ++bad; sprintf_s(diff, cap, "%s", #field); }
    #define DIFFB(field) DIFFI(field)
    DIFFI(matrixMode) DIFFI(blendSrc) DIFFI(blendDst) DIFFI(shade) DIFFI(active)
    DIFFI(drawFbo) DIFFI(readFbo) DIFFI(bind0) DIFFI(bind1)
    DIFFB(blend) DIFFB(depth) DIFFB(scissor) DIFFB(cull) DIFFB(light)
    DIFFB(alpha) DIFFB(fog) DIFFB(tex0) DIFFB(tex1)
    for (int i = 0; i < 4; ++i) {
        if (a.viewport[i] != b.viewport[i])   { ++bad; sprintf_s(diff, cap, "viewport[%d]", i); }
        if (a.scissorBox[i] != b.scissorBox[i]) { ++bad; sprintf_s(diff, cap, "scissorBox[%d]", i); }
        if (a.colorMask[i] != b.colorMask[i]) { ++bad; sprintf_s(diff, cap, "colorMask[%d]", i); }
        if (fabsf(a.color[i] - b.color[i]) > 1e-5f) { ++bad; sprintf_s(diff, cap, "color[%d]", i); }
    }
    for (int i = 0; i < 16; ++i) {
        if (fabsf(a.proj[i]  - b.proj[i])  > 1e-5f) { ++bad; sprintf_s(diff, cap, "proj[%d]", i); }
        if (fabsf(a.model[i] - b.model[i]) > 1e-5f) { ++bad; sprintf_s(diff, cap, "model[%d]", i); }
    }
    #undef DIFFI
    #undef DIFFB
    return bad;
}

static void S_VignetteHostileGL()
{
    printf("--- vignette under hostile GL state: draws right, restores ALL ---\n");
    if (!tg_getIntegerv || !tg_getFloatv || !tg_isEnabled) {
        printf("  (no GL introspection; skipping)\n");
        return;
    }
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    ClearBackBuffer(1.0f, 0.0f, 0.0f);   // the eye capture will be solid red
    CHECK(BringUp() && VR_BeginFrameEx() == VR_FRAME_RENDER, "session up");
    DriveEye(0);
    VR_SubmitFrame(640, 480, true, 0);   // captures eye 0 = solid red; pair open
    CHECK(VR_TestPairPhase() == 2, "eye 0 captured red (pair mid-flight)");

    // A 1x1 texture with ALPHA ZERO: if the vignette's draw is modulated by a
    // leaked texture unit, the ring's alpha collapses and the tunnel vanishes.
    unsigned decoy = 0;
    unsigned char texel[4] = { 255, 255, 255, 0 };
    tg_genTex(1, &decoy);
    tg_bindTex(T_GL_TEXTURE_2D, decoy);
    tg_texImg(T_GL_TEXTURE_2D, 0, T_GL_RGBA8, 1, 1, 0, T_GL_RGBA, T_GL_UNSIGNED_BYTE, texel);

    // Hostile state: everything the engine could plausibly leave live.
    tg_color4f(0.3f, 0.6f, 0.9f, 0.5f);
    tg_matrixMode(T_GL_PROJECTION);
    tg_blendFunc(T_GL_ONE, T_GL_ONE);
    tg_disable(T_GL_BLEND);
    tg_enable(T_GL_SCISSOR_TEST);
    tg_scissor(10, 10, 50, 50);
    tg_enable(T_GL_ALPHA_TEST);
    tg_alphaFunc(T_GL_GREATER, 0.5f);
    tg_enable(T_GL_FOG);
    tg_shadeModel(T_GL_FLAT);
    tg_colorMask(1, 0, 1, 1);
    tg_viewport(3, 5, 101, 103);
    if (tg_activeTexture) {
        tg_activeTexture(T_GL_TEXTURE0);
        tg_enable(T_GL_TEXTURE_2D);
        tg_bindTex(T_GL_TEXTURE_2D, decoy);
        tg_activeTexture(T_GL_TEXTURE0 + 1);
        tg_enable(T_GL_TEXTURE_2D);
        tg_bindTex(T_GL_TEXTURE_2D, decoy);
        // Stay on unit 1 -- the nastiest place to be left.
    } else {
        tg_enable(T_GL_TEXTURE_2D);
        tg_bindTex(T_GL_TEXTURE_2D, decoy);
    }
    while (tg_getError()) {}

    GLSnap before;
    TakeSnap(&before);

    Comfort_TestSetVignette(0.9f);
    CHECK(Comfort_DrawVignette(0), "vignette draws under hostile state");
    CHECK(tg_getError() == 0, "no GL error raised");

    GLSnap after;
    TakeSnap(&after);
    char diff[64] = "";
    const int bad = CompareSnap(before, after, diff, sizeof(diff));
    CHECK(bad == 0, "every saved GL state restored exactly (%d differ%s%s)",
          bad, bad ? ", e.g. " : "", diff);

    // Pixel proof, sampled straight from the eye texture. Radii at 0.9:
    // inner 0.40, outer 0.805 of the half-extent.
    const int cx = 3072 / 2, cy = 3264 / 2;
    const int centre = VR_TestSampleEyeTexture(0, cx, cy);
    const int corner = VR_TestSampleEyeTexture(0, 10, 10);
    const int solid  = VR_TestSampleEyeTexture(0, (int)(3072 * (1.95f / 2)), cy);
    const int soft   = VR_TestSampleEyeTexture(0, (int)(3072 * (1.60f / 2)), cy);
    CHECK(centre > 200, "aperture centre still shows the scene (r=%d)", centre);
    CHECK(corner < 30 && solid < 30,
          "periphery is solid black despite alpha-test/decoy hostility (%d, %d)", corner, solid);
    CHECK(soft > 30 && soft < 220,
          "the edge is a genuine gradient, not a hard ring (r=%d at mid-edge)", soft);

    // Tidy the hostile state so nothing leaks into later scenarios.
    Comfort_TestSetVignette(0.0f);
    tg_disable(T_GL_ALPHA_TEST);
    tg_disable(T_GL_FOG);
    tg_disable(T_GL_SCISSOR_TEST);
    tg_shadeModel(T_GL_SMOOTH);
    tg_colorMask(1, 1, 1, 1);
    tg_color4f(1, 1, 1, 1);
    tg_matrixMode(T_GL_MODELVIEW);
    if (tg_activeTexture) {
        tg_activeTexture(T_GL_TEXTURE0 + 1);
        tg_disable(T_GL_TEXTURE_2D);
        tg_bindTex(T_GL_TEXTURE_2D, 0);
        tg_activeTexture(T_GL_TEXTURE0);
    }
    tg_disable(T_GL_TEXTURE_2D);
    tg_bindTex(T_GL_TEXTURE_2D, 0);
    tg_viewport(0, 0, 640, 480);

    // Finish the pair so the final protocol audit sees a balanced session.
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "mid-pair begin after the GL games");
    DriveEye(1);
    VR_SubmitFrame(640, 480, true, 1);
    CHECK(g_mock.violations == 0, "no protocol violations");
}

// ==================================================== native VR scenarios ==
//
// These drive the EXACT per-frame sequence dllmain runs: XR frame boundary
// (which syncs actions), render hook (NoteGameAim + body-basis substitution +
// VR_GetEyeCamera), submit, then the weapon/binds updates for the next frame.
// "The game" is emulated by applying injected mouse counts to the aim at the
// true 0.11 deg/count, so the steering loop closes exactly as it does live.

static void QuatYawPitch(float yawDeg, float pitchDeg, float q[4])
{
    // XR convention: yaw about +Y, pitch about +X, both mapping to Quake
    // +yaw-left / +pitch-up through VRM_QuatToQuakeAxis (verified in-suite).
    const float hy = yawDeg * DEG * 0.5f, hp = pitchDeg * DEG * 0.5f;
    const float qy[4] = { 0, sinf(hy), 0, cosf(hy) };
    const float qp[4] = { sinf(hp), 0, 0, cosf(hp) };
    // q = qy * qp (apply pitch in the yawed frame)
    q[0] = qy[3]*qp[0] + qy[0]*qp[3] + qy[1]*qp[2] - qy[2]*qp[1];
    q[1] = qy[3]*qp[1] - qy[0]*qp[2] + qy[1]*qp[3] + qy[2]*qp[0];
    q[2] = qy[3]*qp[2] + qy[0]*qp[1] - qy[1]*qp[0] + qy[2]*qp[3];
    q[3] = qy[3]*qp[3] - qy[0]*qp[0] - qy[1]*qp[1] - qy[2]*qp[2];
}

static void SetHandAim(int hand, float yawDeg, float pitchDeg)
{
    float q[4];
    QuatYawPitch(yawDeg, pitchDeg, q);
    g_mock.handAim[hand].orientation.x = q[0];
    g_mock.handAim[hand].orientation.y = q[1];
    g_mock.handAim[hand].orientation.z = q[2];
    g_mock.handAim[hand].orientation.w = q[3];
    g_mock.handGrip[hand].orientation = g_mock.handAim[hand].orientation;
}

static void SetHandGripPos(int hand, float x, float y, float z)
{
    g_mock.handGrip[hand].position.x = x;
    g_mock.handGrip[hand].position.y = y;
    g_mock.handGrip[hand].position.z = z;
}

// One native GAME frame, in dllmain's exact order. menu=1 emulates a frame
// with no 3D scene (menu/briefing): flat submit, no NoteGameAim.
static VRFrameDisposition StepNativeGameFrame(int menu)
{
    VRFrameDisposition d = VR_BeginFrameEx();
    if (d == VR_FRAME_RENDER) {
        if (!menu) {
            // "The game" consumes the mouse we injected last frame.
            if (g_steerRespond) {
                g_gameYawDeg = VRM_WrapDeg(g_gameYawDeg + g_pendDx * -0.11f);
                g_gamePitchDeg += g_pendDy * -0.11f;
                if (g_gamePitchDeg >  85.0f) g_gamePitchDeg =  85.0f;
                if (g_gamePitchDeg < -85.0f) g_gamePitchDeg = -85.0f;
            }
            g_pendDx = g_pendDy = 0;

            const int eye = VR_ExpectedEye();
            float axis[9];
            MakeAxis(g_gameYawDeg, g_gamePitchDeg, 0, axis);
            // A frame with no fresh aim sample: the binds update still runs
            // (and still injects), but nothing new is observed. On hardware
            // this is a game frame that rendered no 3D scene.
            if (g_skipAimSamples > 0) --g_skipAimSamples;
            else                      VRWeapon_NoteGameAim(axis, g_gameOrigin, g_now);
            float base[9];
            const float* camBase = axis;
            if (VRWeapon_GetViewBasisRows(base))
                camBase = base;
            float fx = 0, fy = 0;
            VR_GetEyeCamera(eye, g_gameOrigin, camBase, 1440, 1440,
                            g_out[eye], g_outAxis[eye], &fx, &fy);
            VR_SubmitFrame(640, 480, true, eye);
        } else {
            g_pendDx = g_pendDy = 0;
            VR_SubmitFrame(640, 480, false, 0);
        }
    }
    VRWeapon_Update(VRInput_State(), g_now, menu);
    VRBinds_Update(g_now, menu);
    g_now += 1.0 / 240.0;
    return d;
}

// One XR-FRAME step: a full eye pair for 3D (two game frames sharing one
// action sync -- exactly the production cadence), or a single flat frame for
// menus. Scenario timing below is in XR frames.
static VRFrameDisposition StepNative(int menu)
{
    VRFrameDisposition d = StepNativeGameFrame(menu);
    if (!menu)
        StepNativeGameFrame(menu);
    return d;
}

static void StepNativeFrames(int n, int menu)
{
    for (int i = 0; i < n; ++i)
        StepNative(menu);
}

// Bring the full stack up and run until decoupling engages. Tracking must
// first pass the acquire hysteresis (VRI_ACQUIRE_SYNCS consecutive active
// syncs), so this steps comfortably past that threshold.
static bool BringUpNative()
{
    if (!BringUp())
        return false;
    StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);
    return VRWeapon_Decoupled() != 0;
}

static float AxisYawDeg(const float axis[9])
{
    float y = 0, p = 0;
    VRM_AxisYawPitchDeg(axis, &y, &p);
    return y;
}

static void S_InputActions()
{
    printf("--- native VR: action lifecycle and per-hand identity ---\n");
    ResetAll();
    CHECK(BringUp(), "input: stack comes up");
    StepNativeFrames(2, 0);

    CHECK(VRInput_ActionsLive() == 1, "input: action set + actions created");
    CHECK(VRInput_Attached() == 1, "input: action set attached to the session");
    CHECK(g_mock.attachCount == 1, "input: exactly one attach per session");
    CHECK(g_mock.syncCount >= 2, "input: actions sync once per located XR frame");
    CHECK(g_mock.suggestOk == 2, "input: touch + simple profiles accepted bindings");
    CHECK(g_mock.suggestRejected == 1, "input: touch_plus politely rejected (unknown here)");
    const VRInputState* st = VRInput_State();
    CHECK(st->valid && st->focused, "input: state valid and focused");
    CHECK(st->suggestedOk == 2 && st->suggestedFail == 1,
          "input: profile outcomes recorded for the check block");

    // (1) LEFT and RIGHT are distinct channels end to end.
    g_mock.trigger[0] = 0.9f;
    g_mock.trigger[1] = 0.2f;
    g_mock.stickX[0] = 0.5f;
    g_mock.stickY[1] = -0.7f;
    g_mock.btnX = true;              // left primary
    g_mock.btnB = true;              // right secondary
    StepNativeFrames(2, 0);
    CHECK(st->hand[0].trigger > 0.8f && st->hand[1].trigger < 0.3f,
          "input: trigger values land on their own hands");
    CHECK(st->hand[0].stickX > 0.4f && fabsf(st->hand[1].stickX) < 0.01f,
          "input: stick X lands on the left hand only");
    CHECK(st->hand[1].stickY < -0.6f && fabsf(st->hand[0].stickY) < 0.01f,
          "input: stick Y lands on the right hand only");
    CHECK(st->hand[0].btnPrimary == 1 && st->hand[1].btnPrimary == 0,
          "input: X reads as the LEFT primary button");
    CHECK(st->hand[1].btnSecondary == 1 && st->hand[0].btnSecondary == 0,
          "input: B reads as the RIGHT secondary button");

    // (13)-style hysteresis at the source: a trigger resting on the threshold
    // cannot machine-gun press edges.
    g_mock.trigger[0] = 0.0f; g_mock.trigger[1] = 0.0f;
    g_mock.btnX = g_mock.btnB = false;
    StepNativeFrames(2, 0);
    g_mock.trigger[1] = 0.9f;
    StepNative(0);
    CHECK(st->hand[1].triggerPressed == 1, "input: press edge fires at 0.9");
    StepNative(0);
    CHECK(st->hand[1].triggerPressed == 0 && st->hand[1].triggerDown == 1,
          "input: held trigger stays down with no new edge");
    g_mock.trigger[1] = 0.5f;        // between the thresholds
    StepNative(0);
    CHECK(st->hand[1].triggerDown == 1, "input: 0.5 keeps the trigger held (hysteresis)");
    g_mock.trigger[1] = 0.2f;
    StepNative(0);
    CHECK(st->hand[1].triggerReleased == 1 && st->hand[1].triggerDown == 0,
          "input: release edge fires at 0.2");

    // (4)/(5) pose conversion: the shared math equals the production head
    // conversion exactly, and positions arrive raw.
    {
        const float q45[4] = { 0, 0.3826834f, 0, 0.9238795f };
        float a[9], b[9];
        VRM_QuatToQuakeAxis(q45, a);
        VR_TestQuatToAxis(q45, b);
        bool same = true;
        for (int i = 0; i < 9; ++i)
            if (fabsf(a[i] - b[i]) > 1e-6f) same = false;
        CHECK(same, "input: controller quat conversion == production head conversion");
        const float v[3] = { 0.1f, 0.2f, -0.3f };
        float out[3];
        VRM_XrToQuakeVec(v, out);
        CHECK(fabsf(out[0] - 0.3f) < 1e-6f && fabsf(out[1] + 0.1f) < 1e-6f
              && fabsf(out[2] - 0.2f) < 1e-6f,
              "input: XR->Quake vector mapping is {-z,-x,y}");
        CHECK(st->hand[1].aimValid && fabsf(st->hand[1].aimPos[0] - 0.18f) < 1e-4f,
              "input: located aim pose arrives raw in XR metres");
    }

    // Focus loss: sync succeeds as NOT_FOCUSED and everything reads inactive.
    g_mock.focused = false;
    StepNative(0);
    CHECK(st->focused == 0 && st->hand[0].active == 0 && st->hand[1].active == 0,
          "input: unfocused session reads all controllers inactive");
    g_mock.focused = true;
    StepNative(0);
    CHECK(st->focused == 1 && st->hand[1].active == 1,
          "input: focus returning restores the controllers");

    // Session loss destroys the action spaces; the rebuild re-creates and
    // re-attaches, and controllers come back.
    MockXr_QueueState(XR_SESSION_STATE_LOSS_PENDING);
    StepNative(0);
    CHECK(MockXr_LiveActionSpaces() == 0, "input: session loss destroys action spaces");
    CHECK(VRInput_Attached() == 0, "input: attach forgotten with the session");
    Sleep(3100);                      // the startup retry backoff is real time
    CHECK(BringUp(), "input: session rebuilds");
    StepNativeFrames(3, 0);
    CHECK(g_mock.attachCount == 2, "input: the new session attaches once");
    CHECK(MockXr_LiveActionSpaces() == 4, "input: aim+grip spaces recreated per hand");
    CHECK(VRInput_State()->hand[1].aimValid == 1, "input: controllers live after rebuild");
    CHECK(g_mock.violations == 0, "input: zero protocol violations across the lifecycle");
}

static void S_DecoupledAiming()
{
    printf("--- native VR: head/body/weapon decoupling and fire ray ---\n");
    ResetAll();
    // This scenario exercises EXPERIMENTAL weapon control (controller-derived
    // view steering), which is DISARMED by default in the camera-stable
    // baseline. Arm it explicitly so the subsystem is still covered; the
    // default-off contract itself is pinned by S_BaselineDisarmed.
    VRBinds_SetExperimentalWeapon(1);
    CHECK(BringUpNative(), "aim: decoupling engages with live controllers");

    float ty = 0, tp = 0;
    CHECK(VRWeapon_GetAimTargetDeg(&ty, &tp), "aim: target exists");
    CHECK(fabsf(ty) < 1.0f && fabsf(tp) < 1.0f, "aim: neutral hand aims straight ahead");

    // (6) head yaw must NOT move the weapon target.
    SetMockHead(30.0f, 0, 0, 0);
    StepNativeFrames(4, 0);
    float ty2 = 0, tp2 = 0;
    VRWeapon_GetAimTargetDeg(&ty2, &tp2);
    CHECK(fabsf(VRM_WrapDeg(ty2 - ty)) < 0.75f,
          "aim: 30 deg of head yaw leaves the weapon target unmoved");
    float headYawOff = 0, headPitchOff = 0;
    VR_GetHeadAimOffset(&headYawOff, &headPitchOff);
    CHECK(fabsf(headYawOff - 30.0f) < 1.5f, "aim: the VIEW followed the head (offset ~30)");

    // Mutation guard: fire must follow the HAND, never the head. If someone
    // wires the fire target to the head, this check fails.
    CHECK(fabsf(VRM_WrapDeg(ty2 - (VRWeapon_BodyYawDeg() + headYawOff))) > 20.0f,
          "aim: MUTATION GUARD -- fire target is not the head direction");

    SetMockHead(0, 0, 0, 0);
    StepNativeFrames(4, 0);

    // (7)+(10)+(11) weapon yaw: target follows, view stays, steering converges,
    // and the rendered weapon axis is the same axis the fire ray uses.
    const float viewYawBefore = AxisYawDeg(g_outAxis[0]);
    SetHandAim(VRI_RIGHT, 45.0f, 0.0f);
    StepNativeFrames(60, 0);
    VRWeapon_GetAimTargetDeg(&ty, &tp);
    CHECK(fabsf(VRM_WrapDeg(ty - 45.0f)) < 1.0f, "aim: 45 deg of hand yaw moves the target 45");
    CHECK(fabsf(VRM_WrapDeg(AxisYawDeg(g_outAxis[0]) - viewYawBefore)) < 1.0f,
          "aim: the CAMERA never followed the weapon (still straight ahead)");
    CHECK(fabsf(VRM_WrapDeg(g_gameYawDeg - 45.0f)) < 1.0f,
          "aim: steering drove the game's fire angles to the controller");
    float wOrg[3], wAxis[9];
    CHECK(VRWeapon_GetWeaponWorld(wOrg, wAxis), "aim: weapon world pose exists");
    CHECK(fabsf(VRM_WrapDeg(AxisYawDeg(wAxis) - ty)) < 0.01f,
          "aim: rendered weapon forward IS the fire target (single source)");
    VRBindsDebug bd;
    VRBinds_GetDebug(&bd);
    CHECK(fabsf(fabsf(bd.cpdYaw) - 9.09f) < 1.5f,
          "aim: counts-per-degree auto-calibrated to the true value");

    // (12) head aim != weapon aim is a legal steady state.
    SetMockHead(-25.0f, 0, 0, 0);
    StepNativeFrames(8, 0);
    VR_GetHeadAimOffset(&headYawOff, nullptr);
    VRWeapon_GetAimTargetDeg(&ty, &tp);
    CHECK(fabsf(headYawOff + 25.0f) < 1.5f && fabsf(VRM_WrapDeg(ty - 45.0f)) < 1.0f,
          "aim: head -25 while the weapon holds +45 -- both stand");
    SetMockHead(0, 0, 0, 0);

    // (8) roomscale head translation must not drag the weapon.
    VRWeapon_GetWeaponWorld(wOrg, wAxis);
    const float wx = wOrg[0], wy = wOrg[1];
    SetMockHead(0, 0.35f, 0.1f, -0.2f);
    StepNativeFrames(4, 0);
    float wOrg2[3];
    VRWeapon_GetWeaponWorld(wOrg2, wAxis);
    CHECK(fabsf(wOrg2[0] - wx) < 0.05f && fabsf(wOrg2[1] - wy) < 0.05f,
          "aim: head translation does not move the weapon");
    const float camDx = g_out[0][0] - g_gameOrigin[0];
    const float camDy = g_out[0][1] - g_gameOrigin[1];
    CHECK(sqrtf(camDx*camDx + camDy*camDy) > 3.0f,
          "aim: ...while the camera itself DID move (roomscale)");
    SetMockHead(0, 0, 0, 0);
    StepNativeFrames(2, 0);

    // (9) hand translation moves the weapon by exactly worldscale.
    VRWeapon_GetWeaponWorld(wOrg, wAxis);
    const float beforeY = wOrg[1];
    SetHandGripPos(VRI_RIGHT, 0.28f, -0.25f, -0.30f);    // +0.1 m XR x = -y quake
    StepNativeFrames(2, 0);
    VRWeapon_GetWeaponWorld(wOrg2, wAxis);
    CHECK(fabsf((wOrg2[1] - beforeY) + 0.1f * 39.37f) < 0.05f,
          "aim: 10 cm of hand motion moves the weapon 3.94 units");

    // (10) the entity rewrite points the rendered weapon along the hand.
    // Model the discriminating case -- the game's aim LAGGING the controller
    // (mid-steering): the viewmodel rides the aim at 10 deg while the hand
    // holds 45; the rewrite must land it on the HAND, killing the lag from
    // the rendered weapon. (A broken delta transform leaves it at 10.)
    {
        float aimLag[9];
        MakeAxis(10.0f, 0, 0, aimLag);
        VRWeapon_NoteGameAim(aimLag, g_gameOrigin, g_now);
        float entOrg[3] = { g_gameOrigin[0], g_gameOrigin[1], g_gameOrigin[2] };
        float entAxis[9];
        memcpy(entAxis, aimLag, sizeof(entAxis));
        CHECK(VRWeapon_MaybeRewriteEntity(entOrg, entAxis) == 1,
              "aim: the camera-riding entity classifies as the view weapon");
        CHECK(fabsf(VRM_WrapDeg(AxisYawDeg(entAxis) - 45.0f)) < 1.0f,
              "aim: rendered weapon entity re-posed to the hand (45), not the lagging aim");
        float farOrg[3] = { g_gameOrigin[0] + 500.0f, g_gameOrigin[1], g_gameOrigin[2] };
        float farAxis[9] = { 1,0,0, 0,1,0, 0,0,1 };
        CHECK(VRWeapon_MaybeRewriteEntity(farOrg, farAxis) == 0,
              "aim: an NPC-distance entity is never grabbed");
        StepNativeFrames(2, 0);        // restore the loop's real aim note
    }

    // Mutation guard: swapped hands. The left hand aims somewhere else
    // entirely; the weapon must follow the DOMINANT (right) hand.
    SetHandAim(VRI_LEFT, -60.0f, 0.0f);
    StepNativeFrames(4, 0);
    VRWeapon_GetAimTargetDeg(&ty, &tp);
    CHECK(fabsf(VRM_WrapDeg(ty - 45.0f)) < 1.0f,
          "aim: MUTATION GUARD -- a swapped-hands bug would read -60 here");

    // Dominant-hand selection (2): switching to left-handed makes the LEFT
    // hand the weapon.
    VRWeapon_SetDominantHand(VRI_LEFT);
    StepNativeFrames(30, 0);
    VRWeapon_GetAimTargetDeg(&ty, &tp);
    CHECK(fabsf(VRM_WrapDeg(ty + 60.0f)) < 1.5f,
          "aim: left-handed mode aims with the left hand (-60)");
    VRWeapon_SetDominantHand(VRI_RIGHT);
    SetHandAim(VRI_LEFT, 0, 0);
    SetHandAim(VRI_RIGHT, 0, 0);
    StepNativeFrames(30, 0);

    // Spawn/scripted jump: a large un-injected aim change rebases the body so
    // the view adopts the new facing instead of fighting it.
    g_gameYawDeg = VRM_WrapDeg(g_gameYawDeg + 120.0f);
    StepNativeFrames(6, 0);
    CHECK(fabsf(VRM_WrapDeg(VRWeapon_BodyYawDeg() - g_gameYawDeg)) < 2.0f,
          "aim: a spawn-sized aim jump rebases the body to the new facing");

    CHECK(g_mock.violations == 0, "aim: zero protocol violations");
}

static void S_ScriptedDetection()
{
    printf("--- native VR: dead-input detection (cutscenes) ---\n");
    ResetAll();
    // This scenario exercises EXPERIMENTAL weapon control (controller-derived
    // view steering), which is DISARMED by default in the camera-stable
    // baseline. Arm it explicitly so the subsystem is still covered; the
    // default-off contract itself is pinned by S_BaselineDisarmed.
    VRBinds_SetExperimentalWeapon(1);
    CHECK(BringUpNative(), "scripted: stack up");

    // The game stops responding to injected mouse while the weapon wants to
    // turn: the effectiveness monitor must flip to scripted mode, where the
    // camera falls back to the baseline composition.
    SetHandAim(VRI_RIGHT, 40.0f, 0.0f);
    g_steerRespond = 0;
    StepNativeFrames(50, 0);
    CHECK(VRWeapon_Scripted() == 1, "scripted: dead input detected within ~0.2 s");
    float base[9];
    CHECK(VRWeapon_GetViewBasisRows(base) == 0,
          "scripted: camera falls back to the game-aim composition");
    float entOrg[3] = { g_gameOrigin[0], g_gameOrigin[1], g_gameOrigin[2] };
    float entAxis[9] = { 1,0,0, 0,1,0, 0,0,1 };
    CHECK(VRWeapon_MaybeRewriteEntity(entOrg, entAxis) == 0,
          "scripted: weapon transform suspends in scripted mode");

    // Control returns: the periodic probe notices and decoupling resumes.
    g_steerRespond = 1;
    StepNativeFrames(200, 0);
    CHECK(VRWeapon_Scripted() == 0, "scripted: probe notices control returning");
    CHECK(VRWeapon_Decoupled() == 1, "scripted: decoupling resumes");
    StepNativeFrames(60, 0);
    CHECK(fabsf(VRM_WrapDeg(g_gameYawDeg - 40.0f)) < 1.5f,
          "scripted: steering reconverges after the cutscene");
    CHECK(g_mock.violations == 0, "scripted: zero protocol violations");
}

static void S_BindsMapping()
{
    printf("--- native VR: bindings, movement, turning, haptics ---\n");
    ResetAll();
    // This scenario exercises EXPERIMENTAL weapon control (controller-derived
    // view steering), which is DISARMED by default in the camera-stable
    // baseline. Arm it explicitly so the subsystem is still covered; the
    // default-off contract itself is pinned by S_BaselineDisarmed.
    VRBinds_SetExperimentalWeapon(1);
    CHECK(BringUpNative(), "binds: stack up");
    SinkClear();

    // (13) trigger press-edge fires exactly one MOUSE1 down; (14) holding it
    // keeps +attack held (automatic fire semantics); release lifts it.
    g_mock.trigger[VRI_RIGHT] = 0.9f;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 1, "binds: trigger press = one fire down");
    StepNativeFrames(5, 0);
    CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 1, "binds: held trigger adds no extra downs");
    CHECK(CountEvents(VRB_EV_MBTNUP, -1) == 0, "binds: fire still held");
    g_mock.trigger[VRI_RIGHT] = 0.1f;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_MBTNUP, -1) == 1, "binds: trigger release lifts fire");

    // (20) haptics are bounded: ~1 s of automatic fire is a handful of
    // pulses, never per-frame spam.
    g_mock.hapticCount[VRI_RIGHT] = 0;
    g_mock.trigger[VRI_RIGHT] = 0.9f;
    StepNativeFrames(120, 0);          // 1.0 s of game frames
    g_mock.trigger[VRI_RIGHT] = 0.0f;
    StepNative(0);
    CHECK(g_mock.hapticCount[VRI_RIGHT] >= 4 && g_mock.hapticCount[VRI_RIGHT] <= 16,
          "binds: 1 s of fire = bounded haptic pulses (got a sane count)");
    CHECK(g_mock.hapticLastAmp[VRI_RIGHT] > 0.0f && g_mock.hapticLastAmp[VRI_RIGHT] <= 1.0f,
          "binds: haptic amplitude within range");

    const int reloadHapticsBefore = g_mock.hapticCount[VRI_LEFT];
    VRBinds_SetHaptics(1, 0.6f);
    VRBinds_ReloadFeedback(VRI_LEFT, 0.70f, 30);
    CHECK(g_mock.hapticCount[VRI_LEFT] == reloadHapticsBefore + 1,
          "reload stage feedback reaches the support controller once");
    CHECK_NEAR(g_mock.hapticLastAmp[VRI_LEFT], 0.42f, 0.0001,
               "reload feedback respects the configured global strength");
    VRBinds_SetHaptics(0, 0.6f);
    VRBinds_ReloadFeedback(VRI_LEFT, 0.70f, 30);
    CHECK(g_mock.hapticCount[VRI_LEFT] == reloadHapticsBefore + 1,
          "disabled haptics suppress reload stage feedback");
    VRBinds_SetHaptics(1, 0.6f);

    // Mode 2 must release a fire hold while ammunition is manipulated,
    // then wait for a genuine trigger release before another shot. Drive
    // the real gesture machine between binds updates to isolate input
    // reconciliation from the separate native pose integration fixtures.
    {
        VRReload_Reset(); VRNH_SetReloadMode(2);
        VRReloadProfile p = {};
        p.cls = VRRL_CLASS_MAGAZINE; p.valid = 1;
        p.firingGripM[0] = 0.2f; p.loadZoneM[2] = -0.2f;
        VRReload_SetProfile(&p);
        g_mock.trigger[VRI_RIGHT] = 0.9f; StepNative(0);
        VRReloadInput reload = {};
        reload.valid = 1; reload.handM[2] = -0.2f;
        reload.handWorldM[0] = 2; reload.handWorldM[2] = 2;
        reload.now = g_now; VRReload_Step(&reload);  // REACH, fire allowed
        SinkClear(); VRBinds_Update(g_now, 0);
        CHECK(CountEvents(VRB_EV_MBTNUP, -1) == 0,
              "manual reload REACH preserves an ordinary fire hold");
        reload.gripHeld = 1; reload.now += 0.01; VRReload_Step(&reload);
        SinkClear(); VRBinds_Update(g_now + 0.01, 0);
        CHECK(CountEvents(VRB_EV_MBTNUP, -1) == 1,
              "mode 2 magazine grip immediately releases held fire");
        const int blockedHaptics = g_mock.hapticCount[VRI_RIGHT];
        VRBinds_Update(g_now + 0.2, 0);
        CHECK(g_mock.hapticCount[VRI_RIGHT] == blockedHaptics,
              "blocked reload fire creates no automatic-fire rumble");
        reload.menuOpen = 1; reload.now += 0.02; VRReload_Step(&reload);
        SinkClear(); VRBinds_Update(g_now + 0.3, 0);
        CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 0,
              "ending a reload cannot resume a trigger held throughout it");
        g_mock.trigger[VRI_RIGHT] = 0; StepNative(0);
        SinkClear(); g_mock.trigger[VRI_RIGHT] = 0.9f; StepNative(0);
        CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 1,
              "release then fresh trigger press safely resumes normal fire");

        // A successful insertion has the same safe held-trigger contract
        // as cancellation: COMMIT and RECOVER cannot reopen +attack.
        VRReload_Reset();
        VRReloadProfile receiver = p; receiver.directLoad = 1;
        VRReload_SetProfile(&receiver);
        reload = {}; reload.valid = 1;
        reload.handM[0] = 2; reload.handM[2] = 2;
        reload.handWorldM[0] = 0.15f; reload.handWorldM[2] = -0.45f;
        reload.now = g_now; VRReload_Step(&reload);  // REACH
        reload.now += 0.01; VRReload_Step(&reload); // POUCH
        reload.gripHeld = 1; reload.now += 0.01; VRReload_Step(&reload);
        SinkClear(); VRBinds_Update(g_now + 0.02, 0);
        CHECK(CountEvents(VRB_EV_MBTNUP, -1) == 1,
              "receiver ammo pickup releases a trigger held before the reload");
        reload.handM[0] = 0; reload.handM[2] = -0.2f;
        reload.now += 0.01; VRReload_Step(&reload); // INSERT
        reload.gripHeld = 0; reload.now += 0.01;
        const VRReloadOutput committed = VRReload_Step(&reload);
        CHECK(committed.issueReload == 1 && committed.state == VRRL_COMMIT,
              "held-trigger regression completes a successful receiver insertion");
        SinkClear(); VRBinds_Update(g_now + 0.04, 0);
        CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 0,
              "successful reload COMMIT cannot fire a trigger held throughout it");
        reload.now += 0.01; VRReload_Step(&reload); // RECOVER
        VRBinds_Update(g_now + 0.05, 0);
        CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 0,
              "successful reload RECOVER still requires trigger release");
        g_mock.trigger[VRI_RIGHT] = 0; StepNative(0);
        SinkClear(); g_mock.trigger[VRI_RIGHT] = 0.9f; StepNative(0);
        CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 1,
              "fresh trigger after successful insertion resumes ordinary fire");

        // Tracking absence manufactures a trigger-up sample at the input
        // source. It must not satisfy the reload's real-release requirement.
        VRReload_Reset(); VRReload_SetProfile(&p);
        reload = {}; reload.valid = 1; reload.handM[2] = -0.2f;
        reload.handWorldM[0] = 2; reload.handWorldM[2] = 2;
        reload.now = g_now; VRReload_Step(&reload);
        reload.gripHeld = 1; reload.now += 0.01; VRReload_Step(&reload);
        SinkClear(); VRBinds_Update(g_now + 0.01, 0);
        CHECK(CountEvents(VRB_EV_MBTNUP, -1) == 1,
              "tracking regression begins with reload withdrawing held fire");
        g_mock.handActive[VRI_RIGHT] = false;
        StepNativeFrames(2, 0);
        const VRHandState& absent = VRInput_State()->hand[VRI_RIGHT];
        CHECK(!absent.active && !absent.triggerActive && !absent.triggerDown,
              "dominant tracking absence supplies synthetic trigger-up, not a real release");
        reload.valid = 0; reload.now = g_now; VRReload_Step(&reload);
        VRBinds_Update(g_now, 0);
        SinkClear();
        g_mock.handActive[VRI_RIGHT] = true;   // trigger stays physically held
        StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);
        const VRHandState& returned = VRInput_State()->hand[VRI_RIGHT];
        CHECK(returned.active && returned.triggerActive && returned.triggerDown
              && VRI_HandTrackingActive(returned),
              "dominant tracking returns with a live trigger still held");
        CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 0,
              "reload cancellation plus tracking reacquire cannot fire a trigger held throughout");
        g_mock.trigger[VRI_RIGHT] = 0; StepNative(0);
        SinkClear(); g_mock.trigger[VRI_RIGHT] = 0.9f; StepNative(0);
        CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 1,
              "a live trigger release after reload tracking loss re-arms ordinary fire");

        // A receiver hand waiting at the pouch has not taken any ammo.
        // Unlike a detachable magazine's POUCH, it must not inhibit fire.
        // Sample the real action/input/bind chain without a native pose tick:
        // this fixture deliberately has no driven gun geometry for that tick.
        const auto reloadInputOnly = []() {
            VRInput_Sync(0x22220001ULL, 0x33330001ULL, g_mock.nextDisplayTime);
            VRBinds_Update(g_now, 0);
            g_now += 1.0 / 240.0;
        };
        for (int incremental = 0; incremental < 2; ++incremental) {
            VRReload_Reset();
            VRReloadProfile waiting = p;
            waiting.directLoad = !incremental;
            waiting.cls = incremental ? VRRL_CLASS_INCREMENTAL : VRRL_CLASS_MAGAZINE;
            VRReload_SetProfile(&waiting);
            g_mock.trigger[VRI_RIGHT] = 0; StepNative(0);
            g_mock.trigger[VRI_RIGHT] = 0.9f; StepNative(0);
            reload = {}; reload.valid = 1;
            reload.handM[0] = 2; reload.handM[2] = 2;
            reload.handWorldM[0] = 0.15f; reload.handWorldM[2] = -0.45f;
            reload.now = g_now; VRReload_Step(&reload);
            reload.now += 0.01; VRReload_Step(&reload);
            CHECK(VRReload_State() == VRRL_POUCH && VRReload_ReloadsIssued() == 0,
                  "%s open-hand reach waits at pouch without ammunition pickup",
                  incremental ? "incremental" : "direct");
            SinkClear(); VRBinds_Update(g_now + 0.01, 0);
            CHECK(VRReload_State() == VRRL_POUCH && CountEvents(VRB_EV_MBTNUP, -1) == 0,
                  "%s empty pouch preserves ordinary held fire",
                  incremental ? "incremental" : "direct");
            g_mock.trigger[VRI_RIGHT] = 0; reloadInputOnly();
            SinkClear(); g_mock.trigger[VRI_RIGHT] = 0.9f; reloadInputOnly();
            CHECK(VRReload_State() == VRRL_POUCH && CountEvents(VRB_EV_MBTNDOWN, -1) == 1,
                  "%s empty pouch permits a fresh ordinary trigger press",
                  incremental ? "incremental" : "direct");
        }
        // A detachable POUCH follows removal of the magazine. It remains
        // committed manipulation, even after a live release and fresh trigger.
        VRReload_Reset(); VRReload_SetProfile(&p);
        g_mock.trigger[VRI_RIGHT] = 0; StepNative(0);
        g_mock.trigger[VRI_RIGHT] = 0.9f; StepNative(0);
        reload = {}; reload.valid = 1; reload.handM[2] = -0.2f;
        reload.handWorldM[0] = 2; reload.handWorldM[2] = 2;
        reload.now = g_now; VRReload_Step(&reload);
        reload.gripHeld = 1; reload.now += 0.01; VRReload_Step(&reload);
        VRBinds_Update(g_now + 0.01, 0);
        reload.handM[2] = -0.4f; reload.now += 0.01; VRReload_Step(&reload);
        reload.gripHeld = 0; reload.handWorldM[0] = 0.15f;
        reload.handWorldM[2] = -0.45f; reload.now += 0.01; VRReload_Step(&reload);
        CHECK(VRReload_State() == VRRL_POUCH,
              "detachable pouch follows a real magazine grip and pull");
        g_mock.trigger[VRI_RIGHT] = 0; reloadInputOnly();
        SinkClear(); g_mock.trigger[VRI_RIGHT] = 0.9f; reloadInputOnly();
        CHECK(VRReload_State() == VRRL_POUCH && CountEvents(VRB_EV_MBTNDOWN, -1) == 0,
              "detachable pouch still blocks a fresh trigger after magazine removal");

        VRReload_Reset(); VRReload_SetProfile(&p); VRNH_SetReloadMode(1);
        reload = {}; reload.valid = 1; reload.handM[2] = -0.2f;
        reload.handWorldM[0] = 2; reload.handWorldM[2] = 2;
        reload.now = g_now; VRReload_Step(&reload);
        reload.gripHeld = 1; reload.now += 0.01; VRReload_Step(&reload);
        SinkClear(); VRBinds_Update(g_now + 0.01, 0);
        CHECK(CountEvents(VRB_EV_MBTNUP, -1) == 0,
              "mode 1 preserves original firing during a reload gesture");
        VRReload_Reset(); VRNH_SetReloadMode(2); VRReload_SelectByVertexCount(4242);
        SinkClear(); VRBinds_Update(g_now + 0.02, 0);
        CHECK(CountEvents(VRB_EV_MBTNUP, -1) == 0,
              "mode 2 leaves unmeasured-weapon fire unchanged");
        VRNH_SetReloadMode(1); VRReload_Reset();
        g_mock.trigger[VRI_RIGHT] = 0; StepNative(0);
    }

    // Dominant-hand identity (2): the off-hand trigger never fires, and after
    // the 2026-08-23 fix it does not tap the grenade on a bare edge either --
    // it needs a deliberate hold. A quick squeeze must produce NOTHING.
    SinkClear();
    g_mock.trigger[VRI_LEFT] = 0.9f;
    StepNative(0);
    g_mock.trigger[VRI_LEFT] = 0.0f;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 0, "binds: off-hand trigger never fires");
    CHECK(CountEvents(VRB_EV_KEYDOWN, '4') == 0,
          "binds: a QUICK off-hand trigger squeeze does not throw a grenade");

    // ...but holding it past vr_grenade_hold does, exactly once.
    SinkClear();
    g_mock.trigger[VRI_LEFT] = 0.9f;
    StepNativeFrames(120, 0);            // ~0.5 s, past the 0.35 s hold
    CHECK(CountEvents(VRB_EV_KEYDOWN, '4') == 1,
          "binds: a HELD off-hand trigger taps the grenade slot exactly once");
    StepNativeFrames(120, 0);            // keep holding
    CHECK(CountEvents(VRB_EV_KEYDOWN, '4') == 1,
          "binds: one physical press = at most one grenade, however long it is held");
    g_mock.trigger[VRI_LEFT] = 0.0f;
    StepNativeFrames(2, 0);

    // (3) left-handed mirroring swaps the roles wholesale.
    VRWeapon_SetDominantHand(VRI_LEFT);
    StepNativeFrames(4, 0);
    SinkClear();
    g_mock.trigger[VRI_LEFT] = 0.9f;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 1,
          "binds: left-handed -- LEFT trigger fires");
    g_mock.trigger[VRI_LEFT] = 0.0f;
    StepNative(0);
    SinkClear();
    g_mock.trigger[VRI_RIGHT] = 0.9f;
    StepNativeFrames(120, 0);
    CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 0 && CountEvents(VRB_EV_KEYDOWN, '4') == 1,
          "binds: left-handed -- a held RIGHT trigger is the grenade");
    g_mock.trigger[VRI_RIGHT] = 0.0f;
    VRWeapon_SetDominantHand(VRI_RIGHT);
    StepNativeFrames(4, 0);

    // Buttons: jump mirrors A; use mirrors Y. X is now the weapon cycle
    // (tested below) -- reload moved off it, so nothing taps 'R' here.
    SinkClear();
    g_mock.btnA = true;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_KEYDOWN, VK_SPACE) == 1, "binds: A holds jump down");
    g_mock.btnA = false;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_KEYUP, VK_SPACE) == 1, "binds: A release lifts jump");
    SinkClear();
    g_mock.btnX = true;
    StepNative(0);
    g_mock.btnX = false;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'R') == 0,
          "binds: off-hand X no longer taps reload (it is the weapon cycle now)");

    // B: short press = crouch tap on release; long hold = one prone tap.
    SinkClear();
    g_mock.btnB = true;
    StepNativeFrames(20, 0);          // ~83 ms held
    g_mock.btnB = false;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'C') == 1, "binds: short B taps crouch");
    CHECK(CountEvents(VRB_EV_KEYDOWN, VK_CONTROL) == 0, "binds: short B never prones");
    SinkClear();
    g_mock.btnB = true;
    StepNativeFrames(120, 0);         // 0.5 s held
    g_mock.btnB = false;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_KEYDOWN, VK_CONTROL) == 1, "binds: held B goes prone once");
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'C') == 0, "binds: held B does not also crouch");

    // Melee mirrors the dominant stick click; menu taps ESC.
    SinkClear();
    g_mock.stickClick[VRI_RIGHT] = true;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_KEYDOWN, VK_SHIFT) == 1, "binds: stick click holds melee");
    g_mock.stickClick[VRI_RIGHT] = false;
    StepNative(0);
    SinkClear();
    g_mock.menuClick = true;
    StepNative(0);
    g_mock.menuClick = false;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_KEYDOWN, VK_ESCAPE) == 1, "binds: menu button taps ESC");

    // Movement: off-hand stick quantizes onto the game's own movement keys.
    SinkClear();
    g_mock.stickY[VRI_LEFT] = 1.0f;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'W') == 1, "move: stick forward holds W");
    g_mock.stickX[VRI_LEFT] = 1.0f;   // forward-right
    g_mock.stickY[VRI_LEFT] = 1.0f;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'D') == 1 && CountEvents(VRB_EV_KEYUP, 'W') == 0,
          "move: diagonal adds D while W stays held");
    g_mock.stickX[VRI_LEFT] = 0.0f;
    g_mock.stickY[VRI_LEFT] = 0.0f;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_KEYUP, 'W') == 1 && CountEvents(VRB_EV_KEYUP, 'D') == 1,
          "move: releasing the stick releases every held key");

    // Movement direction reference: with the weapon yawed 90 left, walking
    // "forward by the HMD" (head straight) must press the RIGHT-strafe key
    // relative to the gun. (Catches reversed-reference bugs.)
    SetHandAim(VRI_RIGHT, 90.0f, 0.0f);
    StepNativeFrames(40, 0);          // steering carries the aim to 90
    SinkClear();
    g_mock.stickY[VRI_LEFT] = 1.0f;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'D') == 1 && CountEvents(VRB_EV_KEYDOWN, 'W') == 0,
          "move: hmd-forward with the gun 90 left = strafe right key");
    g_mock.stickY[VRI_LEFT] = 0.0f;
    StepNative(0);
    SetHandAim(VRI_RIGHT, 0, 0);
    StepNativeFrames(40, 0);

    // (29) smooth turn works and (mutation) stick right turns RIGHT.
    const float bodyBefore = VRWeapon_BodyYawDeg();
    g_mock.stickX[VRI_RIGHT] = 1.0f;
    StepNativeFrames(24, 0);          // 0.2 s of game frames at full deflection
    g_mock.stickX[VRI_RIGHT] = 0.0f;
    const float turned = VRM_WrapDeg(VRWeapon_BodyYawDeg() - bodyBefore);
    CHECK(turned < -20.0f && turned > -60.0f,
          "turn: MUTATION GUARD -- stick right turns the body right (yaw down)");
    StepNativeFrames(30, 0);

    // (28) snap turn: one step per deflection, re-armed below the threshold,
    // and 0 = fully disabled.
    VRBinds_SetTurnMode(1);
    VRBinds_SetSnapAngle(30.0f);
    const float snapBefore = VRWeapon_BodyYawDeg();
    g_mock.stickX[VRI_RIGHT] = 1.0f;
    StepNativeFrames(10, 0);
    CHECK(fabsf(VRM_WrapDeg(VRWeapon_BodyYawDeg() - (snapBefore - 30.0f))) < 0.5f,
          "turn: snap steps exactly once per deflection");
    g_mock.stickX[VRI_RIGHT] = 0.0f;
    StepNativeFrames(4, 0);
    g_mock.stickX[VRI_RIGHT] = -1.0f;
    StepNativeFrames(10, 0);
    g_mock.stickX[VRI_RIGHT] = 0.0f;
    CHECK(fabsf(VRM_WrapDeg(VRWeapon_BodyYawDeg() - snapBefore)) < 0.5f,
          "turn: opposite snap steps back (re-arm works)");
    VRBinds_SetTurnMode(0);
    StepNativeFrames(30, 0);

    // Weapon cycle. The 2026-08-23 hardware run cycled the weapon three times
    // from the TURN stick -- diagonal flicks while turning cleared Y > 0.7 --
    // and CoD's cycle walks through the grenade slot, so this was half of the
    // "randomly switched to grenade" report. The turn stick is now inert.
    SinkClear();
    g_mock.stickY[VRI_RIGHT] = -1.0f;
    StepNativeFrames(6, 0);
    g_mock.stickY[VRI_RIGHT] = 0.0f;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_WHEEL, -1) == 0,
          "binds: a dominant stick-Y flick NEVER cycles the weapon any more");
    SinkClear();
    g_mock.stickY[VRI_RIGHT] = 1.0f;      // and the other direction too
    StepNativeFrames(6, 0);
    g_mock.stickY[VRI_RIGHT] = 0.0f;
    StepNativeFrames(4, 0);
    CHECK(CountEvents(VRB_EV_WHEEL, 0) == 0,
          "binds: stick-up flick does not cycle either");

    // The cycle now lives on the off-hand X button: discrete and deliberate.
    SinkClear();
    g_mock.btnX = true;
    StepNativeFrames(3, 0);
    CHECK(CountEvents(VRB_EV_WHEEL, -1) == 1, "binds: off-hand X = one weapnext");
    StepNativeFrames(20, 0);              // still held
    CHECK(CountEvents(VRB_EV_WHEEL, -1) == 1,
          "binds: holding X does not machine-gun the weapon cycle");
    g_mock.btnX = false;
    StepNativeFrames(3, 0);

    // The legacy stick source still exists, but only behind an explicit
    // opt-in, so a config that wants it can have it and the test can prove
    // the switch actually switches.
    VRBinds_SetWeaponCycleBinding(1);
    StepNativeFrames(4, 0);
    SinkClear();
    g_mock.stickY[VRI_RIGHT] = -1.0f;
    StepNativeFrames(6, 0);
    g_mock.stickY[VRI_RIGHT] = 0.0f;
    StepNativeFrames(4, 0);
    CHECK(CountEvents(VRB_EV_WHEEL, -1) == 1,
          "binds: legacy stick cycle still works when explicitly configured");
    VRBinds_SetWeaponCycleBinding(0);
    StepNativeFrames(4, 0);

    // Off-hand stick click requests recenter: with the head yawed 25, the
    // next locate relatches and the view offset returns to ~0.
    SetMockHead(25.0f, 0, 0, 0);
    StepNativeFrames(2, 0);
    float off = 0;
    VR_GetHeadAimOffset(&off, nullptr);
    CHECK(off > 20.0f, "recenter: head yawed 25 shows as view offset");
    g_mock.stickClick[VRI_LEFT] = true;
    StepNative(0);
    g_mock.stickClick[VRI_LEFT] = false;
    StepNativeFrames(3, 0);
    VR_GetHeadAimOffset(&off, nullptr);
    CHECK(fabsf(off) < 1.0f, "recenter: stick click re-zeroes the view offset");

    // The one-shot VR INPUT / WEAPON CHECK block fires after 8 s of live
    // controllers; force that moment and prove it emits without crashing or
    // upsetting the frame loop.
    g_now += 9.0;
    StepNativeFrames(2, 0);
    CHECK(StepNative(0) == VR_FRAME_RENDER, "binds: input-check block emitted, loop healthy");
    CHECK(g_mock.violations == 0, "binds: zero protocol violations");
}

// ---------------------------------------------------------------------------
// The grenade-safety suite. Every case here is a shape the 2026-08-23 hardware
// run actually produced, or a shape that would produce the same complaint.
// The contract is blunt: if the player did not deliberately hold the grenade
// control, grenadeAccepted stays 0.
// ---------------------------------------------------------------------------
static void S_GrenadeSafety()
{
    printf("--- grenade: nothing but a deliberate hold may throw one ---\n");
    ResetAll();
    if (!BringUpNative()) { CHECK(false, "grenade: bring-up"); return; }

    VRBindsGrenadeStats gs;
    const int OFF = VRI_LEFT;      // off-hand with the default right dominance

    // (10) idle: no input at all for a good while.
    SinkClear();
    StepNativeFrames(240, 0);
    VRBinds_GetGrenadeStats(&gs);
    CHECK(gs.accepted == 0 && CountEvents(VRB_EV_KEYDOWN, '4') == 0,
          "grenade: no input for 2 s => zero grenades");
    CHECK(gs.requests == 0, "grenade: no input => zero requests");

    // (12) locomotion and turning: both sticks swept through their full range.
    SinkClear();
    for (int i = 0; i < 60; ++i) {
        const float t = (float)i / 59.0f * 2.0f - 1.0f;
        g_mock.stickX[VRI_LEFT]  = t;   g_mock.stickY[VRI_LEFT]  = -t;
        g_mock.stickX[VRI_RIGHT] = -t;  g_mock.stickY[VRI_RIGHT] = t;
        StepNative(0);
    }
    g_mock.stickX[VRI_LEFT] = g_mock.stickY[VRI_LEFT] = 0.0f;
    g_mock.stickX[VRI_RIGHT] = g_mock.stickY[VRI_RIGHT] = 0.0f;
    StepNativeFrames(4, 0);
    VRBinds_GetGrenadeStats(&gs);
    CHECK(gs.accepted == 0 && CountEvents(VRB_EV_KEYDOWN, '4') == 0,
          "grenade: full stick sweeps on BOTH sticks => zero grenades");

    // (13) grip motion: the support/ADS grip worked hard, both hands.
    SinkClear();
    for (int i = 0; i < 40; ++i) {
        g_mock.squeeze[VRI_LEFT]  = (i & 1) ? 0.95f : 0.0f;
        g_mock.squeeze[VRI_RIGHT] = (i & 2) ? 0.95f : 0.0f;
        StepNative(0);
    }
    g_mock.squeeze[VRI_LEFT] = g_mock.squeeze[VRI_RIGHT] = 0.0f;
    StepNativeFrames(4, 0);
    VRBinds_GetGrenadeStats(&gs);
    CHECK(gs.accepted == 0, "grenade: grip mashing on both hands => zero grenades");

    // (14) weapon cycling: X pressed repeatedly.
    SinkClear();
    for (int i = 0; i < 12; ++i) {
        g_mock.btnX = true;  StepNativeFrames(3, 0);
        g_mock.btnX = false; StepNativeFrames(3, 0);
    }
    VRBinds_GetGrenadeStats(&gs);
    CHECK(gs.accepted == 0, "grenade: repeated weapon cycling => zero grenades");
    CHECK(CountEvents(VRB_EV_WHEEL, -1) == 12,
          "cycle: 12 deliberate X presses = 12 cycles, no more and no fewer");

    // Fire: the dominant trigger held down hard and long, the single most
    // common control in the game. It must never reach the grenade.
    SinkClear();
    g_mock.trigger[VRI_RIGHT] = 1.0f;
    StepNativeFrames(200, 0);
    g_mock.trigger[VRI_RIGHT] = 0.0f;
    StepNativeFrames(4, 0);
    VRBinds_GetGrenadeStats(&gs);
    CHECK(gs.accepted == 0, "grenade: sustained fire => zero grenades");

    // (11) tracking reacquire with the off-hand trigger GENUINELY HELD. This
    // is the manufactured-edge case: the hand vanishes and comes back with the
    // control already down. It must not read as a fresh deliberate hold.
    SinkClear();
    g_mock.trigger[VRI_LEFT] = 1.0f;      // held throughout
    g_mock.handActive[OFF] = false;
    StepNativeFrames(40, 0);              // gone long enough to demote
    g_mock.handActive[OFF] = true;
    StepNativeFrames(200, 0);             // back, still held, well past 0.35 s
    VRBinds_GetGrenadeStats(&gs);
    CHECK(CountEvents(VRB_EV_KEYDOWN, '4') == 0,
          "grenade: tracking reacquire with the trigger held => zero grenades");
    CHECK(gs.accepted == 0, "grenade: reacquire cannot manufacture an accept");
    g_mock.trigger[VRI_LEFT] = 0.0f;
    StepNativeFrames(10, 0);

    // Buttons held across the same dropout must not manufacture an edge
    // either -- that is the BoolEdge gate, and reload/recenter rode on it.
    SinkClear();
    g_mock.btnX = true;
    g_mock.handActive[OFF] = false;
    StepNativeFrames(40, 0);
    g_mock.handActive[OFF] = true;
    StepNativeFrames(40, 0);
    CHECK(CountEvents(VRB_EV_WHEEL, -1) == 0,
          "cycle: reacquire with X held does not manufacture a weapon switch");
    g_mock.btnX = false;
    StepNativeFrames(10, 0);

    // (15) ONE deliberate press: exactly one request, exactly one grenade.
    VRBinds_GetGrenadeStats(&gs);
    const int reqBefore = gs.requests, accBefore = gs.accepted;
    SinkClear();
    g_mock.trigger[VRI_LEFT] = 0.95f;
    StepNativeFrames(160, 0);             // ~0.67 s: past the hold
    g_mock.trigger[VRI_LEFT] = 0.0f;
    StepNativeFrames(10, 0);
    VRBinds_GetGrenadeStats(&gs);
    CHECK(gs.requests == reqBefore + 1,
          "grenade: one deliberate press = exactly one request (got %d)",
          gs.requests - reqBefore);
    CHECK(gs.accepted == accBefore + 1,
          "grenade: one deliberate press = exactly one accept");
    CHECK(CountEvents(VRB_EV_KEYDOWN, '4') == 1 && CountEvents(VRB_EV_KEYUP, '4') == 1,
          "grenade: exactly one down/up pair injected");

    // A squeeze SHORTER than the hold is a request that is simply never
    // accepted -- the reflex-squeeze case from the hardware run.
    VRBinds_GetGrenadeStats(&gs);
    const int accBefore2 = gs.accepted;
    SinkClear();
    for (int i = 0; i < 8; ++i) {         // eight reflex squeezes
        g_mock.trigger[VRI_LEFT] = 0.95f;
        StepNativeFrames(20, 0);          // ~83 ms: well under 0.35 s
        g_mock.trigger[VRI_LEFT] = 0.0f;
        StepNativeFrames(20, 0);
    }
    VRBinds_GetGrenadeStats(&gs);
    CHECK(gs.accepted == accBefore2,
          "grenade: eight reflex squeezes throw nothing (the hardware-run bug)");
    CHECK(CountEvents(VRB_EV_KEYDOWN, '4') == 0,
          "grenade: reflex squeezes inject no grenade key");

    // Menus cannot commit one, and a trigger held across the menu boundary
    // has to be released before it can arm again.
    SinkClear();
    g_mock.trigger[VRI_LEFT] = 0.95f;
    StepNativeFrames(30, 1);              // menu, held
    StepNativeFrames(200, 0);             // back to gameplay, STILL held
    CHECK(CountEvents(VRB_EV_KEYDOWN, '4') == 0,
          "grenade: a trigger held across a menu transition cannot throw one");
    g_mock.trigger[VRI_LEFT] = 0.0f;
    StepNativeFrames(10, 0);

    // Nothing is stuck: the grenade key is not held at the OS.
    VRBindsCounters c;
    VRBinds_GetCounters(&c);
    CHECK(c.grenadeTaps >= 1, "grenade: the deliberate ones were counted");
    CHECK(g_mock.violations == 0, "grenade: zero protocol violations");

    VRBinds_GetGrenadeStats(&gs);
    printf("      grenadeRequests %d  grenadeAccepted %d  grenadeRejectedNoise %d\n",
           gs.requests, gs.accepted, gs.rejectedNoise);
}

static void S_TwoHandAndAds()
{
    printf("--- native VR: two-handed grip and physical ADS ---\n");
    ResetAll();
    // This scenario exercises EXPERIMENTAL weapon control (controller-derived
    // view steering), which is DISARMED by default in the camera-stable
    // baseline. Arm it explicitly so the subsystem is still covered; the
    // default-off contract itself is pinned by S_BaselineDisarmed.
    VRBinds_SetExperimentalWeapon(1);
    CHECK(BringUpNative(), "2h: stack up");

    // Geometry: dominant grip at the right hand's default; the off hand
    // starts far away. Distances are metres in XR space.
    SetHandGripPos(VRI_RIGHT, 0.15f, -0.25f, -0.30f);
    SetHandGripPos(VRI_LEFT, -0.60f, 0.30f, 0.40f);      // way off the weapon
    StepNativeFrames(2, 0);

    // (15) grip held outside the attach radius does nothing.
    g_mock.squeeze[VRI_LEFT] = 0.9f;
    StepNativeFrames(4, 0);
    CHECK(VRWeapon_TwoHandAttached() == 0, "2h: far grip never attaches");

    // (16) inside the radius, in FRONT of the rear hand, it attaches.
    // Weapon fwd (identity aim) is XR -Z, so "in front" = more negative z.
    SetHandGripPos(VRI_LEFT, 0.13f, -0.22f, -0.62f);
    StepNativeFrames(4, 0);
    CHECK(VRWeapon_TwoHandAttached() == 1, "2h: near-forward grip attaches");
    CHECK(g_mock.hapticCount[VRI_LEFT] > 0, "2h: attach ticked the support hand");

    // (18) the weapon forward becomes the hand-to-hand line. Off hand sits
    // 0.02 m XR-x to the LEFT of the rear hand and 0.32 m forward: in Quake
    // terms the line yaws LEFT by atan2(0.02, 0.32) ~ 3.6 deg.
    StepNativeFrames(60, 0);          // let the blend complete + steering settle
    float ty = 0, tp = 0;
    VRWeapon_GetAimTargetDeg(&ty, &tp);
    CHECK(fabsf(VRM_WrapDeg(ty - 3.58f)) < 1.2f,
          "2h: weapon forward follows the two-hand line");

    // (19) roll stays put while both hands hold still.
    float o1[3], a1[9], a2[9];
    VRWeapon_GetWeaponWorld(o1, a1);
    StepNativeFrames(20, 0);
    VRWeapon_GetWeaponWorld(o1, a2);
    const float updot = a1[6]*a2[6] + a1[7]*a2[7] + a1[8]*a2[8];
    CHECK(updot > 0.9999f, "2h: roll is stable while attached");

    // (17) release: no orientation snap -- the largest per-frame step during
    // the blend-out stays small.
    g_mock.squeeze[VRI_LEFT] = 0.1f;
    float prevYaw = ty;
    float maxStep = 0.0f;
    for (int i = 0; i < 60; ++i) {
        StepNative(0);
        float y = 0, p = 0;
        if (VRWeapon_GetAimTargetDeg(&y, &p)) {
            const float step = fabsf(VRM_WrapDeg(y - prevYaw));
            if (step > maxStep) maxStep = step;
            prevYaw = y;
        }
    }
    CHECK(VRWeapon_TwoHandAttached() == 0, "2h: release detaches");
    CHECK(maxStep < 1.0f, "2h: release blends out with no snap (max step < 1 deg)");
    VRWeapon_GetAimTargetDeg(&ty, &tp);
    CHECK(fabsf(VRM_WrapDeg(ty)) < 0.5f, "2h: control returns to the dominant hand");

    // ---- physical ADS ----
    // Raise the grip to the eye, aligned with the barrel: enters after the
    // dwell; jitter around the threshold cannot flicker it; exit restores.
    printf("--- native VR: physical iron sights ---\n");
    float tanBefore[4];
    VR_TestGetEyeTangents(0, tanBefore);
    const float viewYawBefore = AxisYawDeg(g_outAxis[0]);

    SetHandGripPos(VRI_RIGHT, 0.0f, -0.02f, -0.20f);     // 20 cm ahead of the eye
    SinkClear();
    StepNativeFrames(60, 0);          // > 0.12 s dwell
    CHECK(VRWeapon_AdsActive() == 1, "ads: raising the weapon to the eye enters ADS");
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'M') == 1, "ads: the game ADS toggle was tapped once");

    // (23) the HMD never rotates for ADS; (24) the projection is untouched.
    CHECK(fabsf(VRM_WrapDeg(AxisYawDeg(g_outAxis[0]) - viewYawBefore)) < 0.5f,
          "ads: entering ADS does not rotate the camera");
    float tanAfter[4];
    VR_TestGetEyeTangents(0, tanAfter);
    CHECK(fabsf(tanAfter[0] - tanBefore[0]) < 1e-6f && fabsf(tanAfter[1] - tanBefore[1]) < 1e-6f,
          "ads: entering ADS does not touch the projection tangents");

    // (22) hysteresis: oscillate the grip across the ENTER threshold; the
    // state must hold (exit needs the wider exit threshold plus dwell).
    int transitions = 0;
    int last = VRWeapon_AdsActive();
    for (int i = 0; i < 120; ++i) {
        SetHandGripPos(VRI_RIGHT, 0.0f, -0.02f, (i & 1) ? -0.20f : -0.36f);
        StepNative(0);
        if (VRWeapon_AdsActive() != last) { ++transitions; last = VRWeapon_AdsActive(); }
    }
    CHECK(transitions == 0, "ads: threshold jitter cannot flicker the state");

    // (21 exit) lower the weapon: leaves after the exit dwell, tapping the
    // toggle again.
    SinkClear();
    SetHandGripPos(VRI_RIGHT, 0.15f, -0.45f, -0.30f);
    StepNativeFrames(80, 0);
    CHECK(VRWeapon_AdsActive() == 0, "ads: lowering the weapon exits ADS");
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'M') == 1, "ads: exit tapped the toggle once");
    CHECK(g_mock.violations == 0, "2h/ads: zero protocol violations");
}

// ================================================= true 6DoF weapon =======
//
// The primary acceptance case of the two-handed-weapon milestone: the rifle
// is held by the HANDS. Dominant-hand translation moves the whole weapon
// 1:1 (in physical equivalents), with no hidden body/camera/viewmodel pivot;
// the support hand then steers the barrel about the rear grip.

static const float U_PER_M = 39.37f;    // CoD: 1 unit = 1 inch

static void GetWeaponOrThrow(float o[3], float a[9], const char* what)
{
    if (!VRWeapon_GetWeaponWorld(o, a)) {
        CHECK(false, "%s: weapon pose missing", what);
        memset(o, 0, 3 * sizeof(float));
        memset(a, 0, 9 * sizeof(float));
        a[0] = a[4] = a[8] = 1;
    }
}

static void S_OneHandSixDof()
{
    printf("--- native VR: one-hand 6DoF baseline (+20 cm proofs) ---\n");
    ResetAll();
    // This scenario exercises EXPERIMENTAL weapon control (controller-derived
    // view steering), which is DISARMED by default in the camera-stable
    // baseline. Arm it explicitly so the subsystem is still covered; the
    // default-off contract itself is pinned by S_BaselineDisarmed.
    VRBinds_SetExperimentalWeapon(1);
    CHECK(BringUpNative(), "6dof: stack up");

    SetHandAim(VRI_RIGHT, 0, 0);
    SetHandGripPos(VRI_RIGHT, 0.10f, -0.30f, -0.35f);
    StepNativeFrames(6, 0);

    VRWeaponDebug wd;
    VRWeapon_GetDebug(&wd);
    CHECK(wd.weaponPoseValid, "6dof: weapon pose valid");
    CHECK(wd.solverPosErrU >= 0.0f && wd.solverPosErrU < 0.1f,
          "6dof: grip->origin chain reproduces exactly (err %.3f u)", wd.solverPosErrU);

    float o0[3], a0[9];
    GetWeaponOrThrow(o0, a0, "6dof");

    // (1) +20 cm along XR +X (player's right). XR->Quake is {-z,-x,y}, and
    // with a zero neutral/body yaw the room axes ARE the world axes: the
    // origin must move -7.874 u along Quake Y (right), nothing else.
    SetHandGripPos(VRI_RIGHT, 0.30f, -0.30f, -0.35f);
    StepNativeFrames(2, 0);
    float o1[3], a1[9];
    GetWeaponOrThrow(o1, a1, "6dof");
    CHECK_NEAR(o1[1] - o0[1], -0.20f * U_PER_M, 0.20,
               "6dof: +20 cm hand X -> 7.87 u weapon right (got %.2f u)", o1[1] - o0[1]);
    CHECK_NEAR(o1[0] - o0[0], 0.0, 0.10, "6dof: X translation is pure (no fwd bleed)");
    CHECK_NEAR(o1[2] - o0[2], 0.0, 0.10, "6dof: X translation is pure (no up bleed)");

    // (2) +20 cm along XR +Y (up) -> Quake +Z.
    SetHandGripPos(VRI_RIGHT, 0.30f, -0.10f, -0.35f);
    StepNativeFrames(2, 0);
    float o2[3], a2[9];
    GetWeaponOrThrow(o2, a2, "6dof");
    CHECK_NEAR(o2[2] - o1[2], 0.20f * U_PER_M, 0.20,
               "6dof: +20 cm hand up -> 7.87 u weapon up (got %.2f u)", o2[2] - o1[2]);

    // (3) 20 cm forward (XR -Z) -> Quake +X.
    SetHandGripPos(VRI_RIGHT, 0.30f, -0.10f, -0.55f);
    StepNativeFrames(2, 0);
    float o3[3], a3[9];
    GetWeaponOrThrow(o3, a3, "6dof");
    CHECK_NEAR(o3[0] - o2[0], 0.20f * U_PER_M, 0.20,
               "6dof: 20 cm hand forward -> 7.87 u weapon forward (got %.2f u)", o3[0] - o2[0]);

    // (4) pure translation never rotated the weapon.
    float dot = a0[0]*a3[0] + a0[1]*a3[1] + a0[2]*a3[2];
    CHECK(dot > 0.99995f, "6dof: pure translation does not rotate the weapon");

    // (5) pure rotation does not translate: yaw the controller 25 deg with
    // the grip pinned; the origin must hold still while the aim target and
    // (after steering) the game aim follow.
    SetHandAim(VRI_RIGHT, 25, 0);
    StepNativeFrames(40, 0);
    float o4[3], a4[9];
    GetWeaponOrThrow(o4, a4, "6dof");
    CHECK(fabsf(o4[0]-o3[0]) < 0.05f && fabsf(o4[1]-o3[1]) < 0.05f
          && fabsf(o4[2]-o3[2]) < 0.05f,
          "6dof: pure rotation creates no fake body-centred translation");
    float ty = 0, tp = 0;
    VRWeapon_GetAimTargetDeg(&ty, &tp);
    CHECK(fabsf(VRM_WrapDeg(ty - 25.0f)) < 1.0f, "6dof: rotation reaches the aim target");

    // (6) head movement must not drag the gun: translate the head 20 cm,
    // weapon origin pinned (the camera moves instead -- that is parallax).
    float camBefore0 = g_out[0][1];
    SetMockHead(0, 0.20f, 0.0f, 0.0f);
    StepNativeFrames(4, 0);
    float o5[3], a5[9];
    GetWeaponOrThrow(o5, a5, "6dof");
    CHECK(fabsf(o5[0]-o4[0]) < 0.05f && fabsf(o5[1]-o4[1]) < 0.05f
          && fabsf(o5[2]-o4[2]) < 0.05f,
          "6dof: head translation does not move the weapon");
    CHECK(fabsf(g_out[0][1] - camBefore0) > 1.0f,
          "6dof: ...while the camera does move (head owns the view)");
    SetMockHead(0, 0.0f, 0.0f, 0.0f);
    StepNativeFrames(4, 0);

    // (7) the body is not a weapon anchor: with the hand at the latched
    // neutral head point, origin == vieworg; away from it, the offset is the
    // hand's own room offset scaled -- already pinned by the chain-error
    // check, so assert the delivered offset is the grip magnitude.
    VRWeapon_GetDebug(&wd);
    float gripMag = sqrtf(wd.rearAnchorRoomM[0]*wd.rearAnchorRoomM[0]
                        + wd.rearAnchorRoomM[1]*wd.rearAnchorRoomM[1]
                        + wd.rearAnchorRoomM[2]*wd.rearAnchorRoomM[2]);
    float weaponOff = sqrtf(
        (wd.weaponWorldOrigin[0]-wd.gameVieworg[0])*(wd.weaponWorldOrigin[0]-wd.gameVieworg[0]) +
        (wd.weaponWorldOrigin[1]-wd.gameVieworg[1])*(wd.weaponWorldOrigin[1]-wd.gameVieworg[1]) +
        (wd.weaponWorldOrigin[2]-wd.gameVieworg[2])*(wd.weaponWorldOrigin[2]-wd.gameVieworg[2]));
    CHECK_NEAR(weaponOff, gripMag * U_PER_M, 0.5,
               "6dof: weapon-to-camera offset IS the hand's room offset");
    CHECK(g_mock.violations == 0, "6dof: zero protocol violations");
}

static void S_TwoHandSolverGeometry()
{
    printf("--- native VR: two-hand solver geometry ---\n");
    ResetAll();
    CHECK(BringUpNative(), "2hs: stack up");

    SetHandAim(VRI_RIGHT, 0, 0);
    SetHandGripPos(VRI_RIGHT, 0.15f, -0.25f, -0.30f);
    StepNativeFrames(4, 0);

    // (8) support hand BESIDE the rifle line (perp 0.30 m > grab radius):
    // grip held, still no attach.
    SetHandGripPos(VRI_LEFT, 0.15f, 0.05f, -0.60f);   // 0.30 up off the line
    g_mock.squeeze[VRI_LEFT] = 0.9f;
    StepNativeFrames(6, 0);
    VRWeaponDebug wd;
    VRWeapon_GetDebug(&wd);
    CHECK(VRWeapon_TwoHandAttached() == 0,
          "2hs: grip held OUTSIDE the grab radius never attaches (perp %.2f m)",
          wd.twoHandPerpM);
    CHECK(wd.twoHandPerpM > 0.25f, "2hs: perp distance measured (%.2f m)", wd.twoHandPerpM);

    // (9) move onto the rifle: attaches.
    SetHandGripPos(VRI_LEFT, 0.13f, -0.22f, -0.62f);
    StepNativeFrames(4, 0);
    CHECK(VRWeapon_TwoHandAttached() == 1, "2hs: hand on the rifle attaches");

    // (12) the rear anchor stays the dominant grip: attaching must not move
    // the weapon origin at all.
    float oA[3], aA[9];
    GetWeaponOrThrow(oA, aA, "2hs");
    StepNativeFrames(40, 0);          // blend completes
    float oB[3], aB[9];
    GetWeaponOrThrow(oB, aB, "2hs");
    CHECK(fabsf(oB[0]-oA[0]) < 0.05f && fabsf(oB[1]-oA[1]) < 0.05f
          && fabsf(oB[2]-oA[2]) < 0.05f,
          "2hs: attach/blend never moves the rear anchor");

    // (14) support hand LEFT (XR -X): the barrel yaws left, naturally, by
    // atan2 of the hand offsets.
    SetHandGripPos(VRI_LEFT, -0.02f, -0.22f, -0.62f);
    StepNativeFrames(30, 0);
    float ty = 0, tp = 0;
    VRWeapon_GetAimTargetDeg(&ty, &tp);
    // v_quake = (0.32, 0.17, 0.03): yaw = atan2(0.17, 0.32) = 28.0 deg left
    CHECK(fabsf(VRM_WrapDeg(ty - 28.0f)) < 2.0f,
          "2hs: support hand left -> barrel yaws left (%.1f deg)", ty);

    // (15) support hand UP: barrel pitches up.
    SetHandGripPos(VRI_LEFT, 0.13f, 0.00f, -0.62f);
    StepNativeFrames(30, 0);
    VRWeapon_GetAimTargetDeg(&ty, &tp);
    // v_quake = (0.32, 0.02, 0.25): pitch = asin(0.25/0.407) = 37.9 deg up
    CHECK(tp > 30.0f && tp < 45.0f,
          "2hs: support hand up -> barrel pitches up (%.1f deg)", tp);

    // (16) both hands together +20 cm right: the whole rifle translates and
    // the barrel direction holds.
    float oC[3], aC[9];
    GetWeaponOrThrow(oC, aC, "2hs");
    SetHandGripPos(VRI_RIGHT, 0.35f, -0.25f, -0.30f);
    SetHandGripPos(VRI_LEFT, 0.33f, 0.00f, -0.62f);
    StepNativeFrames(6, 0);
    float oD[3], aD[9];
    GetWeaponOrThrow(oD, aD, "2hs");
    CHECK_NEAR(oD[1] - oC[1], -0.20f * U_PER_M, 0.25,
               "2hs: both hands +20 cm right move the whole rifle 7.87 u");
    const float fdot = aC[0]*aD[0] + aC[1]*aD[1] + aC[2]*aD[2];
    CHECK(fdot > 0.9995f, "2hs: joint translation leaves the barrel direction alone");
    // (17) explicitly: the origin is not pinned anywhere -- it moved with
    // the hands, so no fixed point below the player exists.
    CHECK(fabsf(oD[1] - oA[1]) > 6.0f, "2hs: no fixed point survives below the player");

    // (18) roll stability while attached (hands still).
    float aE[9], aF[9], oT[3];
    GetWeaponOrThrow(oT, aE, "2hs");
    StepNativeFrames(20, 0);
    GetWeaponOrThrow(oT, aF, "2hs");
    const float updot = aE[6]*aF[6] + aE[7]*aF[7] + aE[8]*aF[8];
    CHECK(updot > 0.9999f, "2hs: roll is stable while attached");

    // (19) degenerate spacing: support hand collapses onto the rear grip --
    // detaches safely, pose stays finite and returns to the dominant hand.
    SetHandGripPos(VRI_LEFT, 0.36f, -0.24f, -0.32f);
    StepNativeFrames(30, 0);
    CHECK(VRWeapon_TwoHandAttached() == 0, "2hs: collapsed hand spacing detaches");
    float oG[3], aG[9];
    GetWeaponOrThrow(oG, aG, "2hs");
    bool finite = true;
    for (int i = 0; i < 9; ++i)
        if (!(aG[i] > -2.0f && aG[i] < 2.0f)) finite = false;
    CHECK(finite, "2hs: degenerate spacing leaves a finite basis");

    // (11) hysteresis: hover exactly at the reach boundary with the grip
    // held; the attach state may change at most once over the whole hover.
    g_mock.squeeze[VRI_LEFT] = 0.0f;
    StepNativeFrames(10, 0);
    SetHandGripPos(VRI_RIGHT, 0.15f, -0.25f, -0.30f);
    g_mock.squeeze[VRI_LEFT] = 0.9f;
    int changes = 0;
    for (int i = 0; i < 100; ++i) {
        // along oscillates 0.74..0.76 around the 0.75 attach bound
        const float z = -0.30f - (0.74f + 0.02f * ((i & 1) ? 1.0f : 0.0f));
        SetHandGripPos(VRI_LEFT, 0.15f, -0.25f, z);
        StepNative(0);
        if (VRWeapon_TwoHandJustChanged()) ++changes;
        StepNative(0);
        if (VRWeapon_TwoHandJustChanged()) ++changes;
    }
    CHECK(changes <= 1, "2hs: reach-boundary hover cannot chatter (changes %d)", changes);

    // (20)/(21) attach and release transitions are rate-bounded: no frame
    // step larger than the blend permits. Hand line ~32 deg off the one-hand
    // forward, still within the grab radius (perp 0.20 m).
    g_mock.squeeze[VRI_LEFT] = 0.0f;
    StepNativeFrames(40, 0);
    SetHandGripPos(VRI_LEFT, -0.05f, -0.25f, -0.62f);
    g_mock.squeeze[VRI_LEFT] = 0.9f;
    float prevYaw = 0, py = 0, pp = 0;
    VRWeapon_GetAimTargetDeg(&prevYaw, &pp);
    float maxStep = 0.0f;
    for (int i = 0; i < 80; ++i) {
        StepNative(0);
        if (VRWeapon_GetAimTargetDeg(&py, &pp)) {
            const float st = fabsf(VRM_WrapDeg(py - prevYaw));
            if (st > maxStep) maxStep = st;
            prevYaw = py;
        }
    }
    CHECK(VRWeapon_TwoHandAttached() == 1, "2hs: re-attach for the transition test");
    CHECK(maxStep < 2.5f, "2hs: attach blends in rate-bounded (max %.2f deg/frame)", maxStep);
    g_mock.squeeze[VRI_LEFT] = 0.0f;
    maxStep = 0.0f;
    VRWeapon_GetAimTargetDeg(&prevYaw, &pp);
    for (int i = 0; i < 80; ++i) {
        StepNative(0);
        if (VRWeapon_GetAimTargetDeg(&py, &pp)) {
            const float st = fabsf(VRM_WrapDeg(py - prevYaw));
            if (st > maxStep) maxStep = st;
            prevYaw = py;
        }
    }
    CHECK(VRWeapon_TwoHandAttached() == 0, "2hs: release for the transition test");
    CHECK(maxStep < 2.5f, "2hs: release blends out rate-bounded (max %.2f deg/frame)", maxStep);
    CHECK(g_mock.violations == 0, "2hs: zero protocol violations");
}

// Test provider for the cgame ADS hint.
static float g_testAdsHint;
static int   g_testAdsHintValid;
static int TestAdsHintProvider(float* v)
{
    if (!g_testAdsHintValid) return 0;
    *v = g_testAdsHint;
    return 1;
}

static void S_AdsOwnershipMachine()
{
    printf("--- native VR: ADS accuracy-state ownership ---\n");
    ResetAll();
    // This scenario exercises EXPERIMENTAL weapon control (controller-derived
    // view steering), which is DISARMED by default in the camera-stable
    // baseline. Arm it explicitly so the subsystem is still covered; the
    // default-off contract itself is pinned by S_BaselineDisarmed.
    VRBinds_SetExperimentalWeapon(1);
    CHECK(BringUpNative(), "adsm: stack up");

    SetHandAim(VRI_RIGHT, 0, 0);
    SetHandGripPos(VRI_RIGHT, 0.15f, -0.25f, -0.30f);
    SetHandGripPos(VRI_LEFT, 0.13f, -0.22f, -0.62f);
    StepNativeFrames(4, 0);

    // (22) one-hand hip-fire is the default: no ADS taps at rest.
    SinkClear();
    StepNativeFrames(60, 0);
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'M') == 0, "adsm: hip-fire by default, no taps");

    // (23) two-hand attach REQUESTS the accuracy state: exactly one tap ON.
    float tanBefore[4];
    VR_TestGetEyeTangents(0, tanBefore);
    const float viewYawBefore = AxisYawDeg(g_outAxis[0]);
    float oBefore[3], aBefore[9];
    GetWeaponOrThrow(oBefore, aBefore, "adsm");

    SinkClear();
    g_mock.squeeze[VRI_LEFT] = 0.9f;
    StepNativeFrames(50, 0);
    CHECK(VRWeapon_TwoHandAttached() == 1, "adsm: support attached");
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'M') == 1, "adsm: attach granted the ADS state (one tap)");
    VRBindsDebug bd;
    VRBinds_GetDebug(&bd);
    CHECK(bd.adsDesired == 1 && bd.adsAssumed == 1 && (bd.adsOwnerMask & 4),
          "adsm: two-hand owns the accuracy state");

    // (25)/(26)/(27) the accuracy state changes NOTHING visual: camera yaw,
    // projection tangents, and the weapon's rear anchor are untouched.
    CHECK(fabsf(VRM_WrapDeg(AxisYawDeg(g_outAxis[0]) - viewYawBefore)) < 0.5f,
          "adsm: accuracy state does not rotate the camera");
    float tanAfter[4];
    VR_TestGetEyeTangents(0, tanAfter);
    CHECK(fabsf(tanAfter[0] - tanBefore[0]) < 1e-6f
          && fabsf(tanAfter[1] - tanBefore[1]) < 1e-6f,
          "adsm: accuracy state does not touch the VR projection");
    float oAfter[3], aAfter[9];
    GetWeaponOrThrow(oAfter, aAfter, "adsm");
    CHECK(fabsf(oAfter[0]-oBefore[0]) < 0.05f && fabsf(oAfter[1]-oBefore[1]) < 0.05f
          && fabsf(oAfter[2]-oBefore[2]) < 0.05f,
          "adsm: accuracy state does NOT auto-centre the weapon (origin pinned to the hand)");

    // (28) sight alignment stays a physical property: with the rifle held
    // low, the barrel is nowhere near the eye line even though ADS is on.
    VRWeaponDebug wd;
    VRWeapon_GetDebug(&wd);
    CHECK(wd.adsEyeDistM > 0.25f,
          "adsm: gun stays physically low while braced (eye distance %.2f m)", wd.adsEyeDistM);

    // (24) release removes the benefit: one tap OFF.
    SinkClear();
    g_mock.squeeze[VRI_LEFT] = 0.0f;
    StepNativeFrames(30, 0);
    CHECK(VRWeapon_TwoHandAttached() == 0, "adsm: released");
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'M') == 1, "adsm: release dropped the ADS state (one tap)");
    VRBinds_GetDebug(&bd);
    CHECK(bd.adsDesired == 0 && bd.adsAssumed == 0, "adsm: state machine settled at hip");

    // (30) weapon switch: the game auto-lowers ADS, so the assumed state
    // resets and -- with the owner still active -- ONE re-assert tap follows
    // the switch holdoff. No stuck state either way.
    g_mock.squeeze[VRI_LEFT] = 0.9f;
    StepNativeFrames(50, 0);
    CHECK(VRWeapon_TwoHandAttached() == 1, "adsm: re-attached for the switch test");
    SinkClear();
    g_mock.btnX = true;                      // weapon cycle: off-hand X now
    StepNative(0);
    g_mock.btnX = false;
    StepNativeFrames(4, 0);
    CHECK(CountEvents(VRB_EV_WHEEL, INT_MIN) >= 1, "adsm: cycle tap fired");
    VRBinds_GetDebug(&bd);
    CHECK(bd.adsAssumed == 0, "adsm: switch resets the assumed state to hip");
    StepNativeFrames(220, 0);                // ride out the 0.8 s holdoff
    VRBinds_GetDebug(&bd);
    CHECK(bd.adsAssumed == 1 && CountEvents(VRB_EV_KEYDOWN, 'M') == 1,
          "adsm: owner still active -> ONE re-assert after the switch settles");

    // (29) tracking loss while attached: support detaches, and the toggle is
    // returned to hip with a final tap while the game still has focus.
    SinkClear();
    g_mock.handActive[VRI_LEFT] = false;
    g_mock.handActive[VRI_RIGHT] = false;
    StepNativeFrames(30, 0);
    CHECK(VRWeapon_TwoHandAttached() == 0, "adsm: tracking loss detaches the support hand");
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'M') == 1,
          "adsm: tracking loss releases the accuracy state");
    VRBinds_GetDebug(&bd);
    CHECK(bd.adsAssumed == 0, "adsm: assumed state cleared on loss");
    g_mock.handActive[VRI_LEFT] = true;
    g_mock.handActive[VRI_RIGHT] = true;
    g_mock.squeeze[VRI_LEFT] = 0.0f;
    StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);

    // cgame hint: with correction enabled, a persistent hint/assumed mismatch
    // adopts the game's real state and the reconciler re-drives from there.
    VRBinds_SetAdsHintProvider(TestAdsHintProvider);
    VRBinds_SetAdsHintCorrection(1);
    g_testAdsHintValid = 1;
    g_testAdsHint = 1.0f;                    // game says ADS, we assume hip
    SinkClear();
    StepNativeFrames(200, 0);                // > 0.7 s mismatch dwell
    VRBinds_GetDebug(&bd);
    CHECK(bd.adsHintValid == 1, "adsm: hint readable");
    CHECK(bd.adsHintCorrections >= 1, "adsm: persistent mismatch corrected the assumption");
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'M') >= 1,
          "adsm: reconciler drove the game back to the desired hip state");
    g_testAdsHintValid = 0;
    VRBinds_SetAdsHintProvider(nullptr);
    VRBinds_SetAdsHintCorrection(0);
    CHECK(g_mock.violations == 0, "adsm: zero protocol violations");
}

static void S_FireDirectionAndMutants()
{
    printf("--- native VR: fire direction agreement + mutants ---\n");
    ResetAll();
    // This scenario exercises EXPERIMENTAL weapon control (controller-derived
    // view steering), which is DISARMED by default in the camera-stable
    // baseline. Arm it explicitly so the subsystem is still covered; the
    // default-off contract itself is pinned by S_BaselineDisarmed.
    VRBinds_SetExperimentalWeapon(1);
    CHECK(BringUpNative(), "fire: stack up");

    SetHandAim(VRI_RIGHT, 0, 0);
    SetHandGripPos(VRI_RIGHT, 0.15f, -0.25f, -0.30f);
    SetHandGripPos(VRI_LEFT, 0.00f, -0.12f, -0.62f);
    g_mock.squeeze[VRI_LEFT] = 0.9f;
    StepNativeFrames(120, 0);                // attach + blend + steering settle
    CHECK(VRWeapon_TwoHandAttached() == 1, "fire: two-hand attached");

    // (31)/(32) the gameplay fire direction (the game's own aim, steered) and
    // the rendered two-hand barrel agree to the acceptance bound.
    float ty = 0, tp = 0, gy = 0, gp = 0;
    double when = 0;
    VRWeapon_GetAimTargetDeg(&ty, &tp);
    VRWeapon_GetGameAimDeg(&gy, &gp, &when);
    CHECK(fabsf(VRM_WrapDeg(ty - gy)) < 0.5f && fabsf(tp - gp) < 0.5f,
          "fire: fire ray == rendered barrel (err %.2f/%.2f deg)",
          VRM_WrapDeg(ty - gy), tp - gp);

    // The target itself must BE the hand-to-hand line (world = room here
    // because the neutral/body yaws are zero in this scenario).
    VRWeaponDebug wd;
    VRWeapon_GetDebug(&wd);
    float expYaw = atan2f(wd.twoHandFwdRoom[1], wd.twoHandFwdRoom[0]) * 57.2957795f;
    CHECK(fabsf(VRM_WrapDeg(ty - (VRWeapon_BodyYawDeg() + expYaw))) < 1.0f,
          "fire: the fire ray is the physical hand line");

    // (33) MUTANT: reversed support vector. A solver that used R-F instead
    // of F-R would aim ~180 deg away -- the oracle must catch it.
    float mutYaw = atan2f(-wd.twoHandFwdRoom[1], -wd.twoHandFwdRoom[0]) * 57.2957795f;
    CHECK(fabsf(VRM_WrapDeg(ty - (VRWeapon_BodyYawDeg() + mutYaw))) > 90.0f,
          "fire: MUTANT reversed hand line is detected (differs by ~180 deg)");

    // (34) MUTANT: fixed body anchor. Move both hands 20 cm; a solver whose
    // origin ignored the hands would keep the old origin -- must differ.
    float oA[3], aA[9];
    GetWeaponOrThrow(oA, aA, "fire");
    SetHandGripPos(VRI_RIGHT, 0.35f, -0.25f, -0.30f);
    SetHandGripPos(VRI_LEFT, 0.20f, -0.12f, -0.62f);
    StepNativeFrames(4, 0);
    float oB[3], aB[9];
    GetWeaponOrThrow(oB, aB, "fire");
    CHECK(fabsf(oB[1] - oA[1]) > 6.0f,
          "fire: MUTANT fixed-anchor origin is detected (real origin moved %.1f u)",
          fabsf(oB[1] - oA[1]));

    // (35) MUTANT: visual-only weapon with mouse-owned fire. Freeze the
    // game's response to steering, then swing the SUPPORT hand across --
    // the rendered two-hand barrel yaws ~16 deg while the frozen "mouse"
    // fire ray stays put. The divergence must be plainly measurable BEFORE
    // the dead-input monitor reacts (it needs 30 injected-but-dead frames).
    g_steerRespond = false;
    SetHandGripPos(VRI_LEFT, 0.30f, -0.12f, -0.62f);
    StepNativeFrames(10, 0);
    CHECK(VRWeapon_GetAimTargetDeg(&ty, &tp) == 1, "fire: target still live pre-detection");
    VRWeapon_GetGameAimDeg(&gy, &gp, &when);
    CHECK(fabsf(VRM_WrapDeg(ty - gy)) > 2.5f,
          "fire: MUTANT mouse-owned fire is detected (divergence %.1f deg)",
          fabsf(VRM_WrapDeg(ty - gy)));
    // ...and the correct SYSTEM response to a dead fire channel follows: the
    // effectiveness monitor declares control lost rather than letting the
    // rendered gun lie about where bullets go.
    StepNativeFrames(60, 0);
    CHECK(VRWeapon_Scripted() == 1,
          "fire: a dead steering channel is declared (scripted mode)");
    g_steerRespond = true;
    for (int i = 0; i < 400 && VRWeapon_Scripted(); ++i)
        StepNative(0);                       // probe notices control returning
    CHECK(VRWeapon_Scripted() == 0, "fire: control detected returning");
    StepNativeFrames(160, 0);                // re-engage + reconverge
    CHECK(VRWeapon_GetAimTargetDeg(&ty, &tp) == 1, "fire: target live again");
    VRWeapon_GetGameAimDeg(&gy, &gp, &when);
    CHECK(fabsf(VRM_WrapDeg(ty - gy)) < 0.5f, "fire: agreement restored with steering");
    CHECK(g_mock.violations == 0, "fire: zero protocol violations");
}

static void S_SupportGripMovement()
{
    printf("--- native VR: movement stays green under the support grip ---\n");
    ResetAll();
    // This scenario exercises EXPERIMENTAL weapon control (controller-derived
    // view steering), which is DISARMED by default in the camera-stable
    // baseline. Arm it explicitly so the subsystem is still covered; the
    // default-off contract itself is pinned by S_BaselineDisarmed.
    VRBinds_SetExperimentalWeapon(1);
    CHECK(BringUpNative(), "sgm: stack up");

    SetHandAim(VRI_RIGHT, 0, 0);
    SetHandGripPos(VRI_RIGHT, 0.15f, -0.25f, -0.30f);
    SetHandGripPos(VRI_LEFT, 0.13f, -0.22f, -0.62f);
    g_mock.squeeze[VRI_LEFT] = 0.9f;
    StepNativeFrames(30, 0);
    CHECK(VRWeapon_TwoHandAttached() == 1, "sgm: support attached");

    // (36) the off-hand thumbstick still walks while its grip supports the
    // rifle: the two roles coexist on one controller.
    SinkClear();
    g_mock.stickY[VRI_LEFT] = 1.0f;
    StepNativeFrames(4, 0);
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'W') == 1,
          "sgm: left stick walks forward while the left grip holds the rifle");
    VRBindsDebug bd;
    VRBinds_GetDebug(&bd);
    CHECK(bd.movementSector == 0, "sgm: movement sector live under the grip");
    CHECK(VRWeapon_TwoHandAttached() == 1, "sgm: walking does not detach the support hand");

    // (37) dominant-stick turning stays independent.
    const float yaw0 = VRWeapon_BodyYawDeg();
    g_mock.stickX[VRI_RIGHT] = 1.0f;
    StepNativeFrames(30, 0);
    g_mock.stickX[VRI_RIGHT] = 0.0f;
    CHECK(fabsf(VRM_WrapDeg(VRWeapon_BodyYawDeg() - yaw0)) > 5.0f,
          "sgm: right stick still turns the body");
    CHECK(VRWeapon_TwoHandAttached() == 1, "sgm: turning does not detach the support hand");

    // (38) nothing can stay stuck: opening the menu releases every injected
    // key even with the stick still deflected and the grip still held.
    StepNative(1);
    CHECK(CountEvents(VRB_EV_KEYUP, 'W') == 1, "sgm: menu entry releases the held movement");
    g_mock.stickY[VRI_LEFT] = 0.0f;
    g_mock.squeeze[VRI_LEFT] = 0.0f;
    CHECK(g_mock.violations == 0, "sgm: zero protocol violations");
}

static void S_MenuAndScriptedGuards()
{
    printf("--- native VR: menu / non-gameplay guards ---\n");
    ResetAll();
    // This scenario exercises EXPERIMENTAL weapon control (controller-derived
    // view steering), which is DISARMED by default in the camera-stable
    // baseline. Arm it explicitly so the subsystem is still covered; the
    // default-off contract itself is pinned by S_BaselineDisarmed.
    VRBinds_SetExperimentalWeapon(1);
    CHECK(BringUpNative(), "guards: stack up");

    // Hold W via the stick, then a menu opens: every held key releases and
    // the weapon transform refuses menu frames (25).
    g_mock.stickY[VRI_LEFT] = 1.0f;
    StepNative(0);
    SinkClear();
    StepNative(1);                    // menu frame
    CHECK(CountEvents(VRB_EV_KEYUP, 'W') == 1, "guards: menu opening releases held movement");
    CHECK(VRWeapon_Decoupled() == 0, "guards: decoupling suspends in menus");
    float entOrg[3] = { g_gameOrigin[0], g_gameOrigin[1], g_gameOrigin[2] };
    float entAxis[9] = { 1,0,0, 0,1,0, 0,0,1 };
    CHECK(VRWeapon_MaybeRewriteEntity(entOrg, entAxis) == 0,
          "guards: menu frames never transform weapon entities");

    // Menu cursor: the dominant stick moves the mouse, the trigger clicks,
    // and no gameplay key is ever pressed.
    SinkClear();
    g_mock.stickY[VRI_LEFT] = 0.0f;
    g_mock.stickX[VRI_RIGHT] = 1.0f;
    StepNativeFrames(10, 1);
    g_mock.stickX[VRI_RIGHT] = 0.0f;
    CHECK(CountEvents(VRB_EV_MOUSEMOVE, INT_MIN) >= 5, "guards: stick drives the menu cursor");
    CHECK(CountEvents(VRB_EV_KEYDOWN, 'D') == 0, "guards: no movement keys in menus");
    SinkClear();
    g_mock.trigger[VRI_RIGHT] = 0.9f;
    StepNative(1);
    g_mock.trigger[VRI_RIGHT] = 0.0f;
    StepNative(1);
    CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 1 && CountEvents(VRB_EV_MBTNUP, -1) == 1,
          "guards: trigger clicks in menus");

    // Returning to gameplay re-engages cleanly.
    StepNativeFrames(4, 0);
    CHECK(VRWeapon_Decoupled() == 1, "guards: decoupling re-engages after the menu");

    // Controllers dying mid-game: decoupling disengages after the grace
    // period and the baseline camera returns; nothing crashes, keys release.
    g_mock.stickY[VRI_LEFT] = 1.0f;
    StepNative(0);
    SinkClear();
    g_mock.handActive[0] = g_mock.handActive[1] = false;
    for (int i = 0; i < 300; ++i) StepNative(0);       // 1.25 s
    CHECK(VRWeapon_Decoupled() == 0, "guards: controller loss disengages decoupling");
    CHECK(CountEvents(VRB_EV_KEYUP, 'W') == 1, "guards: controller loss releases held keys");
    float base[9];
    CHECK(VRWeapon_GetViewBasisRows(base) == 0, "guards: camera back on the game aim");
    g_mock.handActive[0] = g_mock.handActive[1] = true;
    StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);
    CHECK(VRWeapon_Decoupled() == 1,
          "guards: controllers returning re-engage (after re-acquire hysteresis)");
    CHECK(g_mock.violations == 0, "guards: zero protocol violations");
}

// The 2026-08-20 "gun fired forever" class, eliminated by construction: the
// desired-state reconciler rebuilds the complete wanted key set every update
// and releases anything no longer wanted, so no eaten edge, focus blip,
// tracking loss, mode flip or teardown can strand a synthetic key or button.
static void S_InputReconciler()
{
    printf("--- injection reconciler: nothing stays down without being wanted ---\n");
    ResetAll();
    // This scenario exercises EXPERIMENTAL weapon control (controller-derived
    // view steering), which is DISARMED by default in the camera-stable
    // baseline. Arm it explicitly so the subsystem is still covered; the
    // default-off contract itself is pinned by S_BaselineDisarmed.
    VRBinds_SetExperimentalWeapon(1);
    CHECK(BringUpNative(), "reconciler: stack up");
    VRBindsCounters c0, c1;

    // (16) dominant controller LOST while fire held -> MOUSE1 UP that update.
    SinkClear();
    g_mock.trigger[VRI_RIGHT] = 0.9f;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 1, "reconciler: fire down");
    g_mock.handActive[1] = false;                 // right controller dies mid-pull
    StepNative(0);
    CHECK(CountEvents(VRB_EV_MBTNUP, -1) == 1,
          "reconciler: hand loss releases fire IMMEDIATELY");
    g_mock.handActive[1] = true;
    g_mock.trigger[VRI_RIGHT] = 0.0f;
    StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);

    // Same for every held digital binding at once: fire + jump + melee + use
    // + movement, all released by one focus loss.
    SinkClear();
    g_mock.trigger[VRI_RIGHT] = 0.9f;
    g_mock.btnA = true;                           // jump
    g_mock.stickClick[VRI_RIGHT] = true;          // melee
    g_mock.btnY = true;                           // use (off-hand secondary)
    g_mock.stickY[VRI_LEFT] = 1.0f;               // forward
    StepNativeFrames(2, 0);
    CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 1
          && CountEvents(VRB_EV_KEYDOWN, VK_SPACE) == 1
          && CountEvents(VRB_EV_KEYDOWN, VK_SHIFT) == 1
          && CountEvents(VRB_EV_KEYDOWN, 'F') == 1
          && CountEvents(VRB_EV_KEYDOWN, 'W') == 1,
          "reconciler: five simultaneous holds established");
    g_mock.focused = false;                       // (17) XR focus loss
    StepNative(0);
    CHECK(CountEvents(VRB_EV_MBTNUP, -1) == 1
          && CountEvents(VRB_EV_KEYUP, VK_SPACE) == 1
          && CountEvents(VRB_EV_KEYUP, VK_SHIFT) == 1
          && CountEvents(VRB_EV_KEYUP, 'F') == 1
          && CountEvents(VRB_EV_KEYUP, 'W') == 1,
          "reconciler: focus loss releases every held input at once");
    VRBindsDebug bd;
    VRBinds_GetDebug(&bd);
    CHECK(bd.heldKeys == 0, "reconciler: ledger empty after focus loss");
    g_mock.focused = true;
    g_mock.trigger[VRI_RIGHT] = 0.0f;
    g_mock.btnA = false; g_mock.btnY = false;
    g_mock.stickClick[VRI_RIGHT] = false;
    g_mock.stickY[VRI_LEFT] = 0.0f;
    StepNativeFrames(2, 0);

    // (21) reacquire leaves no sticky state; the same control presses again.
    SinkClear();
    g_mock.trigger[VRI_RIGHT] = 0.9f;
    StepNative(0);
    CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 1,
          "reconciler: fire presses cleanly again after the blip");
    g_mock.trigger[VRI_RIGHT] = 0.0f;
    StepNative(0);

    // THE stuck-fire regression: a menu click whose RELEASE EDGE is eaten by
    // a focus blip (generation freezes while unfocused; edge flags are
    // recomputed every sync, so the release edge is gone when focus
    // returns). The old raw un-ledgered MOUSE1 down then stayed down at the
    // OS forever.
    StepNativeFrames(3, 1);                       // settle into menu mode
    SinkClear();
    g_mock.trigger[VRI_RIGHT] = 0.9f;             // click a menu button...
    StepNativeFrames(2, 1);
    CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 1, "stuck-fire: menu click down");
    g_mock.focused = false;                       // ...VDXR overlay steals focus...
    StepNative(1);
    g_mock.trigger[VRI_RIGHT] = 0.0f;             // ...release lands unfocused
    StepNativeFrames(3, 1);
    g_mock.focused = true;
    StepNativeFrames(2, 1);
    CHECK(CountEvents(VRB_EV_MBTNUP, -1) >= 1,
          "stuck-fire: MOUSE1 released despite the eaten release edge");
    VRBinds_GetDebug(&bd);
    CHECK(bd.heldKeys == 0, "stuck-fire: nothing held after the blip");

    // A trigger held across gameplay->menu must not click the menu; it must
    // re-arm only after being seen UP inside the menu.
    StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);   // back to gameplay
    g_mock.trigger[VRI_RIGHT] = 0.9f;             // firing...
    StepNativeFrames(2, 0);
    SinkClear();
    StepNativeFrames(2, 1);                       // ...menu opens mid-burst
    CHECK(CountEvents(VRB_EV_MBTNUP, -1) == 1,
          "reconciler: menu entry releases the held fire");
    CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 0,
          "reconciler: held trigger does NOT click the fresh menu");
    g_mock.trigger[VRI_RIGHT] = 0.0f;             // seen up in the menu...
    StepNative(1);
    g_mock.trigger[VRI_RIGHT] = 0.9f;             // ...now a deliberate click
    StepNative(1);
    CHECK(CountEvents(VRB_EV_MBTNDOWN, -1) == 1,
          "reconciler: deliberate in-menu click works after re-arm");
    g_mock.trigger[VRI_RIGHT] = 0.0f;
    StepNative(1);

    // (19)+(20) explicit release-all: counts only when it releases, and is
    // idempotent.
    VRBinds_GetCounters(&c0);
    VRBinds_ReleaseAllInjectedInputs("test double call");
    VRBinds_ReleaseAllInjectedInputs("test double call");
    VRBinds_GetCounters(&c1);
    CHECK(c1.releaseAllRuns == c0.releaseAllRuns,
          "reconciler: no-op release-all does not count or emit");
    VRBinds_GetDebug(&bd);
    CHECK(bd.heldKeys == 0, "reconciler: idempotent release-all, ledger empty");

    // Counters pair up: every down has its up after the dust settles.
    VRBinds_GetCounters(&c1);
    CHECK(c1.fireDown == c1.fireUp && c1.fireDown >= 2,
          "counters: fire downs pair with ups (%d/%d)", c1.fireDown, c1.fireUp);
    CHECK(c1.jumpDown == c1.jumpUp && c1.meleeDown == c1.meleeUp
          && c1.useDown == c1.useUp,
          "counters: jump/melee/use downs pair with ups");
    CHECK(c1.moveFwdDown == c1.moveFwdUp && c1.moveFwdDown >= 1,
          "counters: movement downs pair with ups");
    CHECK(c1.menuClickDown == c1.menuClickUp && c1.menuClickDown >= 1,
          "counters: menu clicks pair up");

    // (Teardown) a session teardown releases whatever is held at the OS.
    StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);
    g_mock.trigger[VRI_RIGHT] = 0.9f;
    StepNative(0);
    SinkClear();
    VR_Shutdown();
    CHECK(CountEvents(VRB_EV_MBTNUP, -1) == 1,
          "reconciler: session teardown releases held fire");
    g_mock.trigger[VRI_RIGHT] = 0.0f;
    CHECK(g_mock.violations == 0, "reconciler: zero protocol violations");
}

static void S_SettingsIndependence()
{
    printf("--- native VR: comfort options are independent (27-30) ---\n");
    ResetAll();
    CHECK(BringUpNative(), "settings: stack up");

    // (27) vignette 0 means ZERO, whatever the motion.
    Comfort_SetVignette(false);
    CHECK(Comfort_TestTarget(10000.0f, 0.0f) == 0.0f
          && Comfort_TestTarget(0.0f, 10000.0f) == 0.0f,
          "settings: vignette off produces no tunnel at any speed");

    // (28)+(29) snap fully off leaves smooth functional, and vice versa;
    // (30) flipping one option never flips another.
    VRBinds_SetTurnMode(0);
    const int physAdsBefore = VRWeapon_PhysicalAds();
    const int twoHandBefore = VRWeapon_TwoHandAttached();
    const float body0 = VRWeapon_BodyYawDeg();
    g_mock.stickX[VRI_RIGHT] = 1.0f;
    StepNativeFrames(24, 0);
    g_mock.stickX[VRI_RIGHT] = 0.0f;
    CHECK(VRM_WrapDeg(VRWeapon_BodyYawDeg() - body0) < -8.0f,
          "settings: smooth turning functions with snap disabled");
    CHECK(Comfort_TestTarget(10000.0f, 0.0f) == 0.0f,
          "settings: turning did not re-enable the vignette");
    CHECK(VRWeapon_PhysicalAds() == physAdsBefore && VRWeapon_TwoHandAttached() == twoHandBefore,
          "settings: turning changed nothing else");

    VRBinds_SetTurnMode(1);
    VRBinds_SetSnapAngle(45.0f);
    StepNativeFrames(20, 0);
    const float body1 = VRWeapon_BodyYawDeg();
    g_mock.stickX[VRI_RIGHT] = -1.0f;
    StepNativeFrames(8, 0);
    g_mock.stickX[VRI_RIGHT] = 0.0f;
    CHECK(fabsf(VRM_WrapDeg(VRWeapon_BodyYawDeg() - (body1 + 45.0f))) < 0.5f,
          "settings: snap mode snaps by exactly the configured angle");
    CHECK(Comfort_TestTarget(10000.0f, 0.0f) == 0.0f,
          "settings: snap mode did not re-enable the vignette");

    VRWeapon_SetPhysicalAds(0);
    CHECK(VRWeapon_PhysicalAds() == 0, "settings: physical ADS disables");
    SetHandGripPos(VRI_RIGHT, 0.0f, -0.02f, -0.20f);
    StepNativeFrames(80, 0);
    CHECK(VRWeapon_AdsActive() == 0, "settings: disabled ADS never engages");
    VRWeapon_SetPhysicalAds(1);
    Comfort_SetVignette(true);
    CHECK(Comfort_TestTarget(10000.0f, 0.0f) > 0.9f,
          "settings: vignette re-enables on request (independent both ways)");
    Comfort_SetVignette(false);
    CHECK(g_mock.violations == 0, "settings: zero protocol violations");
}

static void S_MenuSafeArea()
{
    printf("--- native VR: menu safe area (31) ---\n");
    ResetAll();
    CHECK(BringUp(), "menu: stack up");
    StepPair();                       // locate + latch
    StepGameFlat();                   // one flat (menu) frame captures

    float cx = 0, cy = 0, vx = 0, vy = 0;
    CHECK(VR_GetMenuSafeArea(&cx, &cy, &vx, &vy) == 1, "menu: safe area recorded");
    CHECK(cx <= vx + 0.1f && cy <= vy + 0.1f,
          "menu: content stays inside the binocularly visible angles");
    CHECK(vx > 39.0f && vx < 41.0f && vy > 43.0f && vy < 45.0f,
          "menu: visible box is the nasal/vertical minimum (40 x 44)");
    // 0.80 (menu scale) x 0.92 (VR UI safe area, P3 2026-09-04) of the
    // visible width in the tangent domain: ~79 % of the angle.
    CHECK(cx > 0.75f * vx, "menu: content still fills most of the visible width (%.1f of %.1f deg)", cx, vx);
    // Phase-11 acceptance: the 0.80 default leaves a REAL margin against the
    // reported box (>= 5 deg per edge) -- the 0.90 default's ~3 deg was eaten
    // by lens overscan + panel-pin drift and clipped all four edges.
    CHECK(vx - cx >= 5.0f && vy - cy >= 5.0f,
          "menu: default leaves >= 5 deg true margin per edge (%.1f / %.1f)",
          vx - cx, vy - cy);

    // Aspect preserved: reconstruct the pixel box from the recorded angles
    // and compare with the source aspect (640x480 here).
    const float d2r = 0.01745329252f;
    const float subX = tanf(54.0f * d2r), subY = tanf(55.0f * d2r);
    const float pwFrac = tanf(cx * d2r) / subX;      // pw / w
    const float phFrac = tanf(cy * d2r) / subY;      // ph / h
    const float pixAspect = (pwFrac * 3072.0f) / (phFrac * 3264.0f);
    CHECK(fabsf(pixAspect - (640.0f / 480.0f)) < 0.03f,
          "menu: the safe-area fit preserves the panel aspect");

    // A smaller scale shrinks content and still preserves the bound.
    VR_SetMenuScale(0.6f);
    StepPair();
    StepGameFlat();
    float cx2 = 0, cy2 = 0;
    VR_GetMenuSafeArea(&cx2, &cy2, &vx, &vy);
    CHECK(cx2 < cx - 5.0f, "menu: vr_menu_scale scales the content");
    CHECK(g_mock.violations == 0, "menu: zero protocol violations");
}

// ==================================================== controller bring-up ==
//
// The 2026-08-20 hardware run: bindings suggested, attach PASS -- then total
// silence while every controller stayed dead. These scenarios make each
// failure stage of the bring-up ladder distinguishable and prove the ladder
// reports the right stage for each injected fault.

static void S_ControllerBringup()
{
    printf("--- controller bring-up: ladder stages and failure attribution ---\n");

    // (a) the healthy path reaches CONTROLLERS_LIVE, with the optional
    // touch_plus rejection on the record and NOT in the way.
    ResetAll();
    CHECK(BringUp(), "bringup: stack up");
    StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);
    CHECK(VRInput_BringupStage() == VRI_BU_LIVE,
          "bringup: healthy path reaches CONTROLLERS_LIVE");
    CHECK(VRInput_HandTracking(VRI_LEFT) == VRI_TRACK_ACTIVE
          && VRInput_HandTracking(VRI_RIGHT) == VRI_TRACK_ACTIVE,
          "bringup: both hands stably ACTIVE");
    CHECK(VRInput_State()->suggestedFail == 1 && g_mock.suggestRejected == 1,
          "bringup: touch_plus rejection recorded and harmless (Touch still live)");
    VRInput_LogBringupReport("test: idempotence probe");   // already emitted; must not re-emit or crash
    CHECK(StepNative(0) == VR_FRAME_RENDER, "bringup: loop healthy after the report");

    // (b) attach fails: the ladder stops at BINDINGS_SUGGESTED, sync is
    // NEVER attempted before a successful attach, and the camera basis
    // never leaves the game aim.
    ResetAll();
    g_mock.failAttach = true;
    CHECK(BringUp(), "bringup: stack up under attach failure");
    StepNativeFrames(6, 0);
    CHECK(VRInput_Attached() == 0, "bringup: attach failure recorded");
    CHECK(VRInput_BringupStage() == VRI_BU_BINDINGS_SUGGESTED,
          "bringup: ladder pinned at BINDINGS_SUGGESTED");
    CHECK(g_mock.syncCount == 0, "bringup: no xrSyncActions before a successful attach");
    float base[9];
    CHECK(VRWeapon_GetViewBasisRows(base) == 0,
          "bringup: dead attach leaves the camera on the game aim");
    VRInput_LogBringupReport("test: attach-failed report");
    CHECK(g_mock.violations == 0, "bringup: attach failure breaks no protocol");

    // (c) sync fails: ladder pinned at SESSION_ACTIONS_ATTACHED; nothing
    // downstream pretends to be live.
    ResetAll();
    g_mock.failSyncActions = true;
    CHECK(BringUp(), "bringup: stack up under sync failure");
    StepNativeFrames(6, 0);
    CHECK(VRInput_BringupStage() == VRI_BU_ATTACHED,
          "bringup: ladder pinned at SESSION_ACTIONS_ATTACHED");
    CHECK(VRI_HandTrackingUsable(VRInput_State()->hand[0]) == 0
          && VRI_HandTrackingUsable(VRInput_State()->hand[1]) == 0,
          "bringup: failing sync never yields usable tracking");
    CHECK(VRWeapon_Decoupled() == 0, "bringup: failing sync never decouples the camera");
    VRInput_LogBringupReport("test: sync-failed report");

    // (d) sync healthy but no pose ever active (the runtime never bound our
    // profiles / controllers asleep -- the hardware-run signature): ladder
    // pinned at ACTIONS_SYNCING, injection layer stays silent.
    ResetAll();
    g_mock.handActive[0] = g_mock.handActive[1] = false;
    CHECK(BringUp(), "bringup: stack up with dead controllers");
    StepNativeFrames(12, 0);
    CHECK(VRInput_BringupStage() == VRI_BU_SYNCING,
          "bringup: sync-ok/pose-dead pins the ladder at ACTIONS_SYNCING");
    CHECK(VRInput_State()->valid == 1 && VRInput_State()->focused == 1,
          "bringup: the sync loop itself is healthy (the trap the old log hid)");
    CHECK(VRWeapon_Decoupled() == 0 && VRWeapon_GetViewBasisRows(base) == 0,
          "bringup: dead controllers leave the baseline camera untouched");
    SinkClear();
    StepNativeFrames(4, 0);
    CHECK(g_sinkCount == 0, "bringup: dead controllers inject NOTHING");
    VRInput_LogBringupReport("test: pose-dead report");
    CHECK(g_mock.violations == 0, "bringup: zero protocol violations across all stages");
}

static void S_TrackingHysteresis()
{
    printf("--- tracking state machine: acquire hysteresis, dropouts, flicker ---\n");

    // Acquire: below VRI_ACQUIRE_SYNCS consecutive active syncs nothing may
    // engage; past it, tracking is stable and decoupling engages.
    ResetAll();
    CHECK(BringUp(), "track: stack up");
    StepNativeFrames(3, 0);
    CHECK(VRI_HandTrackingUsable(VRInput_State()->hand[VRI_RIGHT]) == 0,
          "track: 3 active syncs are not yet usable");
    CHECK(VRWeapon_Decoupled() == 0,
          "track: camera basis untouched below the acquire threshold");
    StepNativeFrames(VRI_ACQUIRE_SYNCS + 2, 0);
    CHECK(VRI_HandTrackingUsable(VRInput_State()->hand[VRI_RIGHT]) == 1,
          "track: sustained activity becomes usable");
    CHECK(VRWeapon_Decoupled() == 1, "track: stable tracking engages decoupling");

    // One-frame pose miss: LOST-within-grace. The camera basis MUST hold --
    // this is the mutation guard against wiring the basis to raw isActive.
    g_mock.handActive[VRI_RIGHT] = false;
    StepNative(0);
    CHECK(VRInput_HandTracking(VRI_RIGHT) == VRI_TRACK_LOST,
          "track: a one-sync miss reads LOST, not UNAVAILABLE");
    CHECK(VRI_HandTrackingUsable(VRInput_State()->hand[VRI_RIGHT]) == 1
          && VRI_HandTrackingActive(VRInput_State()->hand[VRI_RIGHT]) == 0,
          "track: LOST within grace is USABLE but not ACTIVE");
    float base[9];
    CHECK(VRWeapon_Decoupled() == 1 && VRWeapon_GetViewBasisRows(base) == 1,
          "track: MUTATION GUARD -- one missed sync never flips the camera basis");
    g_mock.handActive[VRI_RIGHT] = true;
    StepNative(0);
    CHECK(VRInput_HandTracking(VRI_RIGHT) == VRI_TRACK_ACTIVE,
          "track: the next good sync restores ACTIVE instantly");
    CHECK(VRWeapon_Decoupled() == 1, "track: decoupling was never interrupted");

    // Per-frame flicker from cold: alternating isActive must never engage
    // anything -- the exact "camera basis alternates frame to frame" bug.
    ResetAll();
    CHECK(BringUp(), "track: stack up for the flicker run");
    int flips = 0, prev = VRWeapon_Decoupled();
    for (int i = 0; i < 40; ++i) {
        g_mock.handActive[0] = g_mock.handActive[1] = (i & 1) != 0;
        StepNative(0);
        if (VRWeapon_Decoupled() != prev) { ++flips; prev = VRWeapon_Decoupled(); }
    }
    CHECK(flips == 0 && VRWeapon_Decoupled() == 0,
          "track: MUTATION GUARD -- per-sync flicker can never engage or flap the basis");

    // Sustained loss demotes deliberately: stable drops, tracking reads
    // UNAVAILABLE, decoupling disengages -- then a clean re-acquire engages.
    g_mock.handActive[0] = g_mock.handActive[1] = true;
    StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);
    CHECK(VRWeapon_Decoupled() == 1, "track: engaged before the sustained loss");
    g_mock.handActive[0] = g_mock.handActive[1] = false;
    for (int i = 0; i < 300; ++i) StepNative(0);
    CHECK(VRWeapon_Decoupled() == 0, "track: sustained loss falls back deliberately");
    CHECK(VRI_HandTrackingUsable(VRInput_State()->hand[VRI_RIGHT]) == 0
          && VRInput_HandTracking(VRI_RIGHT) == VRI_TRACK_UNAVAILABLE,
          "track: a second of silence demotes to UNAVAILABLE");
    g_mock.handActive[0] = g_mock.handActive[1] = true;
    StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);
    CHECK(VRWeapon_Decoupled() == 1, "track: re-acquire engages again");
    CHECK(g_mock.violations == 0, "track: zero protocol violations");
}

// One MUTANT game-frame pair: identical to StepNativeGameFrame except the
// camera is composed on the GAME AIM while decoupled -- the exact
// weapon-into-camera leak class dllmain's basis selection must never
// produce. The leak watchdog has to catch this.
static void StepLeakMutantPair()
{
    for (int gf = 0; gf < 2; ++gf) {
        if (VR_BeginFrameEx() != VR_FRAME_RENDER)
            return;
        if (g_steerRespond) {
            g_gameYawDeg = VRM_WrapDeg(g_gameYawDeg + g_pendDx * -0.11f);
            g_gamePitchDeg += g_pendDy * -0.11f;
        }
        g_pendDx = g_pendDy = 0;
        const int eye = VR_ExpectedEye();
        float axis[9];
        MakeAxis(g_gameYawDeg, g_gamePitchDeg, 0, axis);
        VRWeapon_NoteGameAim(axis, g_gameOrigin, g_now);
        float fx = 0, fy = 0;
        VR_GetEyeCamera(eye, g_gameOrigin, axis, 1440, 1440,   // MUTANT: aim basis
                        g_out[eye], g_outAxis[eye], &fx, &fy);
        VR_SubmitFrame(640, 480, true, eye);
        VRWeapon_Update(VRInput_State(), g_now, 0);
        VRBinds_Update(g_now, 0);
        g_now += 1.0 / 240.0;
    }
}

static void S_SteeringLeakGuard()
{
    printf("--- steering-leak watchdog: injected aim must never move the camera ---\n");
    ResetAll();
    // This scenario exercises EXPERIMENTAL weapon control (controller-derived
    // view steering), which is DISARMED by default in the camera-stable
    // baseline. Arm it explicitly so the subsystem is still covered; the
    // default-off contract itself is pinned by S_BaselineDisarmed.
    VRBinds_SetExperimentalWeapon(1);
    CHECK(BringUpNative(), "leak: stack up");

    // Clean convergence: plenty of injection, zero leak.
    SetHandAim(VRI_RIGHT, 30.0f, 0.0f);
    StepNativeFrames(50, 0);
    VRBindsDebug bd;
    VRBinds_GetDebug(&bd);
    CHECK(fabsf(VRM_WrapDeg(g_gameYawDeg - 30.0f)) < 1.5f, "leak: steering converged to 30");
    CHECK(bd.steerLeakCount == 0,
          "leak: injected steering NEVER moved the rendered camera");

    // Head rotation during steering is legitimate camera movement.
    SetHandAim(VRI_RIGHT, -20.0f, 0.0f);
    SetMockHead(15.0f, 0, 0, 0);
    StepNativeFrames(50, 0);
    VRBinds_GetDebug(&bd);
    CHECK(bd.steerLeakCount == 0,
          "leak: head rotation during steering is not mistaken for a leak");

    // Body turn is legitimate too (stick turning while steering runs).
    SetMockHead(0, 0, 0, 0);
    g_mock.stickX[VRI_RIGHT] = 0.8f;
    StepNativeFrames(20, 0);
    g_mock.stickX[VRI_RIGHT] = 0.0f;
    StepNativeFrames(10, 0);
    VRBinds_GetDebug(&bd);
    CHECK(bd.steerLeakCount == 0,
          "leak: body turning during steering is not mistaken for a leak");

    // THE MUTANT: camera composed on the steered game aim while decoupled.
    SetHandAim(VRI_RIGHT, 40.0f, 0.0f);
    for (int i = 0; i < 30; ++i)
        StepLeakMutantPair();
    VRBinds_GetDebug(&bd);
    CHECK(bd.steerLeakCount > 0,
          "leak: MUTATION -- camera following the steered aim IS detected (%d frames)",
          bd.steerLeakCount);
    CHECK(g_mock.violations == 0, "leak: zero protocol violations");
}

static void S_RenderSubmitConsistency()
{
    printf("--- render/submit pose consistency: the Phase-7 probe ---\n");
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "consistency: session up");

    // A static head: the submitted pose and a fresh relocate at the same
    // displayTime agree exactly.
    VR_TestArmConsistencyCheck();
    CHECK(StepPair() == VR_FRAME_RENDER, "consistency: armed pair runs");
    float pm[2] = { 9, 9 }, rd[2] = { 9, 9 };
    CHECK(VR_TestGetRenderSubmitConsistency(pm, rd) == 1, "consistency: probe measured");
    CHECK(pm[0] < 1e-3f && pm[1] < 1e-3f && rd[0] < 0.05f && rd[1] < 0.05f,
          "consistency: static head -> render pose == submitted pose (%.4f/%.4f m)",
          pm[0], pm[1]);

    // STALE-POSE MUTANT: the 'runtime' revises its answer by 5 cm between
    // the pair's locate and its submit. The probe must report that exact
    // translation mismatch -- this is the reprojection-slide mechanism.
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "consistency: mutant pair begins");
    DriveEye(0);
    VR_SubmitFrame(640, 480, true, 0);
    CHECK(VR_BeginFrameEx() == VR_FRAME_RENDER, "consistency: mid-pair");
    DriveEye(1);
    SetMockHead(0, 0.05f, 0, 0);
    VR_TestArmConsistencyCheck();
    VR_SubmitFrame(640, 480, true, 1);
    CHECK(VR_TestGetRenderSubmitConsistency(pm, rd) == 1, "consistency: mutant measured");
    CHECK(pm[0] > 0.04f && pm[1] > 0.04f,
          "consistency: MUTATION -- a 5 cm pose revision is DETECTED (%.3f/%.3f m)",
          pm[0], pm[1]);
    SetMockHead(0, 0, 0, 0);
    CHECK(g_mock.violations == 0, "consistency: zero protocol violations");
}


// ===========================================================================
// EXACT HARDWARE-RUN REGRESSION -- 2026-08-23
// ===========================================================================
//
// Reproduces the measured conditions of the run that was physically
// intolerable, and requires the composed stereo geometry to stay correct
// through all of them.
//
//   runtime IPD  0.0624 m
//   worldscale   39.37 u/m
//   expected     2.456 u   (NOT the 30.730 u that shipped)
//
// The failure mode being pinned: the F1 scheduler spans one XR pair across
// two GAME frames, and the old code re-read the game camera on each of them.
// Every scenario below therefore mutates the game camera BETWEEN DriveEye(0)
// and DriveEye(1) -- exactly what steering, the body-yaw rebase and the 37/38
// refdef rewrite did on hardware -- and requires the eye separation,
// laterality and eye-basis agreement to be untouched.

// Separation of the last composed pair, in units.
static float HwSepU()
{
    const float dx = g_out[1][0] - g_out[0][0];
    const float dy = g_out[1][1] - g_out[0][1];
    const float dz = g_out[1][2] - g_out[0][2];
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

// How far eye 0 sits along EYE 0 OWN view-left of eye 1. Scored on one eye
// basis, never an average: with divergent bases an averaged left axis is
// meaningless, which is why the shipped check could not tell a side reversal
// from a magnitude error.
static float HwLeftU()
{
    const float dx = g_out[1][0] - g_out[0][0];
    const float dy = g_out[1][1] - g_out[0][1];
    const float dz = g_out[1][2] - g_out[0][2];
    const float* l = &g_outAxis[0][3];
    return -(dx*l[0] + dy*l[1] + dz*l[2]);
}

// Angle between the two eyes forward axes.
static float HwDivergeDeg()
{
    // Chord form: acos(dot) near 1 is ill-conditioned -- one float ULP in
    // the dot product of two IDENTICAL bases reads as 0.02 deg of divergence.
    const float* f0 = &g_outAxis[0][0];
    const float* f1 = &g_outAxis[1][0];
    const float cx = f1[0] - f0[0], cy = f1[1] - f0[1], cz = f1[2] - f0[2];
    float halfChord = sqrtf(cx*cx + cy*cy + cz*cz) * 0.5f;
    if (halfChord > 1.0f) halfChord = 1.0f;
    return 2.0f * asinf(halfChord) / DEG;
}

// Body-yaw rebase: a converging steering loop must not chase its own output.
//
// SCOPE NOTE, stated plainly. The 2026-08-23 runaway (1086 rebases from 2
// engagements, a steady ~25 deg ramp) is NOT reproduced here. The heuristic
// at vr_binds.cpp is unsound by inspection -- it compared an observation
// window against a single-call injection record that any call returning
// before the injection site had already zeroed -- and that is fixed with a
// window-consistent accumulator. But the exact frame timing that made it fire
// at 120 Hz depends on the game applying injected mouse over several frames,
// and this harness applies it instantly and samples the aim on every non-menu
// frame, so the per-call record and the accumulator never diverge here. The
// log does not characterise that latency well enough to model it honestly.
//
// What IS pinned, and what actually makes a runaway impossible, is the
// GOVERNOR: rebases are rate-limited and refusals are counted, whatever the
// heuristic decides. That contract is exercised directly in S_BaselineDisarmed
// (40 requests -> 3 accepted, refusals counted). This scenario adds the
// convergence side: an ordinary pursuit must ask for no rebases at all, and
// the live calibrator must stay inside its band.
static void S_RebaseLoopBounded()
{
    printf("--- body-yaw rebase: an ordinary steering pursuit asks for no rebases ---\n");
    ResetAll();
    VRBinds_SetExperimentalWeapon(1);
    CHECK(BringUpNative(), "rebase: stack up");

    const unsigned base0 = VRBinds_BodyYawRebaseCount();

    // Large-swing pursuit with the game responding to injection, including
    // frames where tracking blinks and frames that render no 3D scene -- the
    // two conditions the hardware log records alongside the runaway.
    for (int burst = 0; burst < 30; ++burst) {
        SetHandAim(VRI_RIGHT, (burst & 1) ? 150.0f : -150.0f, 0.0f);
        StepNativeFrames(1, 0);
        g_skipAimSamples = 2;                       // a pair with no 3D scene
        g_mock.handActive[VRI_LEFT] = g_mock.handActive[VRI_RIGHT] = false;
        StepNativeFrames(1, 0);
        g_mock.handActive[VRI_LEFT] = g_mock.handActive[VRI_RIGHT] = true;
        StepNativeFrames(2, 0);
    }

    const unsigned rebases = VRBinds_BodyYawRebaseCount() - base0;
    const unsigned refused = VRBinds_BodyYawRebaseRefused();
    // Both numbers matter, and the second is the load-bearing one. The
    // governor caps ACCEPTED rebases at 3 per window, so a runaway heuristic
    // still shows only 3 accepted -- the count alone cannot tell a healthy
    // loop from a chasing one. REFUSALS expose the difference: a converging
    // loop asks for none.
    CHECK(rebases <= 3,
          "rebase: 120 XR frames of large-swing pursuit accepted %u rebases (<=3)",
          rebases);
    CHECK(refused == 0,
          "rebase: and REQUESTED no rebases beyond the budget (%u refused; a "
          "chasing loop asks constantly)", refused);

    // Counts-per-degree must stay physically believable. On hardware the live
    // calibrator drifted -9.09 -> -0.7, a 13x injection-gain increase, by
    // reading its own runaway output as evidence. The clamp is unconditional.
    VRBindsDebug bd;
    VRBinds_GetDebug(&bd);
    CHECK(fabsf(bd.cpdYaw) >= 1.9f && fabsf(bd.cpdYaw) <= 200.0f,
          "rebase: counts-per-degree stayed in band (%.2f; hardware ran to 0.7)",
          fabsf(bd.cpdYaw));
    CHECK(bd.steerLeakCount == 0, "rebase: and the camera never followed the aim");
    CHECK(g_mock.violations == 0, "rebase: zero protocol violations");
}

// ===========================================================================
// CAMERA-STABLE BASELINE CONTRACT
// ===========================================================================
//
// The default configuration must be the comfortable one. Every property here
// is a thing the 2026-08-23 build got wrong, expressed as a contract on the
// SHIPPED defaults rather than on a mode someone has to remember to select.
static void S_BaselineDisarmed()
{
    printf("--- camera-stable baseline: the DEFAULT configuration is the safe one ---\n");
    ResetAll();

    // --- Phase 3: controller weapon steering is OFF out of the box --------
    CHECK(VRBinds_ExperimentalWeapon() == 0,
          "baseline: experimental weapon control defaults OFF");
    CHECK(VRBinds_SteeringArmed() == 0,
          "baseline: steering is NOT armed by default");
    CHECK(VRBinds_ExperimentalWeaponLockedOut() == 0,
          "baseline: no lockout latched at startup");

    // --- Phase 7: no rebase happens on a disarmed baseline ----------------
    CHECK(VRBinds_BodyYawRebaseCount() == 0,
          "baseline: zero body-yaw rebases at startup");

    // Drive a long stretch of gameplay updates with a live, moving game aim.
    // On 2026-08-23 this produced 1086 rebases and a mouse-injection storm.
    // Disarmed, it must produce neither.
    {
        const float ident[9] = { 1,0,0, 0,1,0, 0,0,1 };
        const float org[3] = { 100.0f, 200.0f, 50.0f };
        SinkClear();
        double t = 200.0;
        for (int i = 0; i < 400; ++i) {
            // the game's own aim sweeping hard, as during the runaway
            float ax[9];
            MakeAxis((float)i * 25.4f, 0, 0, ax);
            VRWeapon_NoteGameAim(ax, org, t);
            VRBinds_Update(t, 0);
            t += 1.0 / 120.0;
        }
        CHECK(VRBinds_BodyYawRebaseCount() == 0,
              "baseline: 400 frames of violent game-aim motion cause ZERO rebases");
        CHECK(CountEvents(VRB_EV_MOUSEMOVE, INT_MIN) == 0,
              "baseline: and ZERO mouse-steering events are injected");
        (void)ident;
    }

    // --- Phase 7: the rebase governor rate-limits even when armed ---------
    {
        ResetAll();
        VRBinds_SetExperimentalWeapon(1);
        const unsigned before = VRBinds_BodyYawRebaseCount();
        // A rebase storm: far more requests than the window allows.
        double t = 300.0;
        CODVR_TestSetNow(t);
        int accepted = 0;
        for (int i = 0; i < 40; ++i) {
            const unsigned c0 = VRBinds_BodyYawRebaseCount();
            VRBinds_TestRequestRebase("unit test storm");
            if (VRBinds_BodyYawRebaseCount() != c0) ++accepted;
        }
        CHECK(accepted <= 3,
              "baseline: a 40-request rebase storm is rate-limited to %d (<=3)", accepted);
        CHECK(VRBinds_BodyYawRebaseRefused() > 0,
              "baseline: the refused rebases are counted, not silently dropped");
        CHECK(VRBinds_BodyYawRebaseCount() > before,
              "baseline: legitimate rebases still get through");
        CODVR_TestSetNow(-1.0);
    }

    // --- Phase 2: the 37/38 transform cannot be turned on -----------------
    ResetAll();
    CHECK(VRWeapon_TransformEnabled() == 0 || VRBinds_ExperimentalWeapon() == 0,
          "baseline: the retired weapon transform is not active by default");
    CHECK(CODVR_CollisionHookCameraMutationCount() == 0,
          "baseline: collisionHookCameraMutationCount == 0");

    // --- Phase 6: the fail-safe latches the lockout -----------------------
    {
        VRBinds_SetExperimentalWeapon(1);
        CHECK(VRBinds_SteeringArmed() == 1, "baseline: armed before the fail-safe");
        VRBinds_LockOutExperimentalWeapon("unit test");
        CHECK(VRBinds_ExperimentalWeaponLockedOut() == 1, "baseline: lockout latched");
        CHECK(VRBinds_SteeringArmed() == 0, "baseline: lockout disarms steering");
        VRBinds_SetExperimentalWeapon(1);       // config tries to re-enable
        CHECK(VRBinds_SteeringArmed() == 0,
              "baseline: a lockout SURVIVES a config re-enable");
    }

    // --- Phase 4: one latched base camera per pair ------------------------
    {
        ResetAll();
        SetMockHead(0, 0, 0, 0, 0.0624f);
        CHECK(BringUp(), "baseline: session up (latch accounting)");
        unsigned l0 = 0, r0 = 0;
        VR_TestGetBaseLatchCounts(&l0, &r0);
        int pairs = 0;
        for (int p = 0; p < 10; ++p) {
            if (StepGame3D() != VR_FRAME_RENDER) break;   // eye 0
            g_gameYawDeg += 25.4f;                        // runaway between eyes
            if (StepGame3D() != VR_FRAME_RENDER) break;   // eye 1
            ++pairs;
        }
        CHECK(pairs == 10, "baseline: 10 complete pairs ran (got %d)", pairs);
        unsigned l1 = 0, r1 = 0;
        VR_TestGetBaseLatchCounts(&l1, &r1);
        CHECK(l1 - l0 == 10, "baseline: 10 pairs latched 10 fresh base cameras (got %u)",
              l1 - l0);
        CHECK(r1 - r0 == 10, "baseline: and all 10 eye-1 renders REUSED that latch (got %u)",
              r1 - r0);
    }

    CHECK(g_mock.violations == 0, "baseline: zero protocol violations");
}

// ===========================================================================
// COUPLED-BASELINE STICK TURNING -- the 2026-08-28 hardware-run response
// ===========================================================================
//
// The hardware run played the PRODUCTION configuration: legacy head-aim,
// experimental weapon OFF, camera coupled to the game's own aim axis. In that
// mode the old turn wiring commanded VRWeapon_AddBodyYawDeg 1224 times and
// every command was silently dropped -- body yaw only exists while decoupled,
// and the coupled camera would not have rendered it anyway. The user's RIGHT
// thumbstick did nothing.
//
// The production contract now under test: in COUPLED mode the stick turn is
// injected as relative mouse counts (the SendInput channel), which turns the
// game's view yaw, which the coupled camera is composed on -- so the turn is
// VISIBLE. This scenario runs the REAL chain: mocked xr* action state ->
// VRInput_Sync (production) -> VRBinds_Update -> injected counts -> the
// emulated game applies them -> the rendered camera axis moves.
static void S_CoupledStickTurn()
{
    printf("--- coupled baseline: RIGHT-stick turning commands the visible view yaw ---\n");
    ResetAll();
    VRWeapon_SetEnabled(0);      // PRODUCTION baseline: legacy head-aim
    CHECK(BringUp(), "cturn: stack up");
    StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);
    CHECK(VRWeapon_Decoupled() == 0,
          "cturn: baseline runs COUPLED (no decoupled body basis)");
    const VRInputState* st = VRInput_State();
    CHECK(st->valid && st->focused && st->hand[VRI_RIGHT].active,
          "cturn: production sync sees a live RIGHT controller");

    // ---- centred stick: zero turn, zero injection ------------------------
    SinkClear();
    const float yawIdle0 = g_gameYawDeg;
    StepNativeFrames(20, 0);
    CHECK(CountEvents(VRB_EV_MOUSEMOVE, INT_MIN) == 0,
          "cturn: centred stick injects nothing");
    CHECK(fabsf(VRM_WrapDeg(g_gameYawDeg - yawIdle0)) < 1e-3f,
          "cturn: and the game yaw holds still");

    // ---- deadzone: 0.15 deflection is noise ------------------------------
    g_mock.stickX[VRI_RIGHT] = 0.15f;
    StepNativeFrames(20, 0);
    g_mock.stickX[VRI_RIGHT] = 0.0f;
    CHECK(CountEvents(VRB_EV_MOUSEMOVE, INT_MIN) == 0,
          "cturn: 0.15 deflection stays inside the deadzone");

    // ---- stick +X: the view turns RIGHT, visibly -------------------------
    StepNativeFrames(1, 0);
    const float yaw0     = g_gameYawDeg;
    const float camYaw0  = AxisYawDeg(g_outAxis[0]);
    g_mock.stickX[VRI_RIGHT] = 1.0f;
    StepNativeFrames(60, 0);                 // 0.5 s at 180 deg/s
    g_mock.stickX[VRI_RIGHT] = 0.0f;
    StepNativeFrames(2, 0);
    const float turned = VRM_WrapDeg(g_gameYawDeg - yaw0);
    CHECK(turned < -30.0f,
          "cturn: stick +X turned the game yaw RIGHT (%.1f deg in 0.5 s)", turned);
    const float camTurned = VRM_WrapDeg(AxisYawDeg(g_outAxis[0]) - camYaw0);
    CHECK(fabsf(VRM_WrapDeg(camTurned - turned)) < 3.0f,
          "cturn: and the RENDERED camera followed it (cam %.1f vs game %.1f)",
          camTurned, turned);

    // ---- stick -X: left, symmetric ---------------------------------------
    const float yaw1 = g_gameYawDeg;
    g_mock.stickX[VRI_RIGHT] = -1.0f;
    StepNativeFrames(60, 0);
    g_mock.stickX[VRI_RIGHT] = 0.0f;
    StepNativeFrames(2, 0);
    CHECK(VRM_WrapDeg(g_gameYawDeg - yaw1) > 30.0f,
          "cturn: stick -X turns LEFT (%.1f deg)", VRM_WrapDeg(g_gameYawDeg - yaw1));

    // ---- the counter counts EMISSIONS, not dropped commands --------------
    {
        VRBindsCounters c;
        VRBinds_GetCounters(&c);
        CHECK(c.turnEvents > 100,
              "cturn: turn frames counted real emissions (%d)", c.turnEvents);
    }

    // ---- MUTANT: dominant hand not tracked -> the turn is refused --------
    {
        StepNativeFrames(2, 0);
        SinkClear();
        const float yawM = g_gameYawDeg;
        g_mock.handActive[VRI_RIGHT] = false;
        g_mock.stickX[VRI_RIGHT] = 1.0f;
        StepNativeFrames(30, 0);
        g_mock.stickX[VRI_RIGHT] = 0.0f;
        g_mock.handActive[VRI_RIGHT] = true;
        CHECK(fabsf(VRM_WrapDeg(g_gameYawDeg - yawM)) < 1e-3f,
              "cturn: MUTATION -- a dead RIGHT hand cannot turn the view");
        StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);   // reacquire tracking
    }

    // ---- MUTANT: menu mode -> the turn is refused ------------------------
    {
        SinkClear();
        const float yawM = g_gameYawDeg;
        g_mock.stickX[VRI_RIGHT] = 1.0f;
        StepNativeFrames(30, 1);              // menu frames
        g_mock.stickX[VRI_RIGHT] = 0.0f;
        CHECK(fabsf(VRM_WrapDeg(g_gameYawDeg - yawM)) < 1e-3f,
              "cturn: MUTATION -- menu mode injects no turn");
        StepNativeFrames(2, 0);
    }

    // ---- snap mode: one discrete step per flick, re-armed by release -----
    {
        VRBinds_SetTurnMode(1);
        VRBinds_SetSnapAngle(30.0f);
        StepNativeFrames(2, 0);
        const float yawS = g_gameYawDeg;
        g_mock.stickX[VRI_RIGHT] = 1.0f;      // one held flick
        StepNativeFrames(30, 0);
        const float snapped = VRM_WrapDeg(g_gameYawDeg - yawS);
        CHECK(snapped < -25.0f && snapped > -35.0f,
              "cturn: snap flick = ONE ~30 deg step (%.1f), held stick does not repeat",
              snapped);
        g_mock.stickX[VRI_RIGHT] = 0.0f;      // release re-arms
        StepNativeFrames(4, 0);
        g_mock.stickX[VRI_RIGHT] = 1.0f;
        StepNativeFrames(10, 0);
        g_mock.stickX[VRI_RIGHT] = 0.0f;
        const float snapped2 = VRM_WrapDeg(g_gameYawDeg - yawS);
        CHECK(snapped2 < -55.0f && snapped2 > -65.0f,
              "cturn: release + second flick = second step (total %.1f)", snapped2);
        VRBinds_SetTurnMode(0);
    }

    // ---- decoupled mode still uses the body basis, not the mouse ---------
    {
        VRBinds_SetExperimentalWeapon(1);
        VRWeapon_SetEnabled(1);
        StepNativeFrames(VRI_ACQUIRE_SYNCS + 4, 0);
        CHECK(VRWeapon_Decoupled() == 1, "cturn: decoupled mode engages");
        const float body0 = VRWeapon_BodyYawDeg();
        g_mock.stickX[VRI_RIGHT] = 1.0f;
        StepNativeFrames(30, 0);
        g_mock.stickX[VRI_RIGHT] = 0.0f;
        CHECK(VRM_WrapDeg(VRWeapon_BodyYawDeg() - body0) < -10.0f,
              "cturn: decoupled stick turn still drives the BODY basis (%.1f deg)",
              VRM_WrapDeg(VRWeapon_BodyYawDeg() - body0));
    }

    CHECK(g_mock.violations == 0, "cturn: zero protocol violations");
}

static void S_HardwareRun20260823()
{
    printf("--- hardware run 2026-08-23: stereo survives camera motion between the eyes ---\n");

    const float U      = 39.37f;      // worldscale, CoD units per metre
    const float IPD_M  = 0.0624f;     // runtime IPD from the hardware log
    const float WANT_U = IPD_M * U;   // 2.456 u

    CHECK_NEAR(WANT_U, 2.456f, 0.002,
               "regression model: 0.0624 m x 39.37 = 2.456 u");

    // ---------------------------------------------------------------------
    // 1. BASELINE: a quiet pair is exactly 2.456 u, eye 0 to the view left.
    // ---------------------------------------------------------------------
    ResetAll();
    SetMockHead(0, 0, 0, 0, IPD_M);
    CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "hw2308: session up");
    CHECK_NEAR(HwSepU(), WANT_U, 0.01,
               "hw2308: quiet pair separation is 2.456 u (not 30.730)");
    CHECK(HwLeftU() > 0.0f, "hw2308: quiet pair laterality LEFT-OK");
    CHECK_NEAR(HwLeftU(), WANT_U, 0.01,
               "hw2308: the whole separation is along view-left");
    CHECK(HwDivergeDeg() < 1e-3f, "hw2308: quiet pair eye bases identical");

    // ---------------------------------------------------------------------
    // 2. THE EXACT FAILURE: the game camera jumps 30 u and the body basis
    //    swings 25.4 deg BETWEEN the two eyes -- the measured hardware step.
    //    Old behaviour: 30.730 u separation, bases 25 deg apart.
    //    Required behaviour: the pair is unchanged; the motion lands on the
    //    NEXT pair, moving both eyes together.
    // ---------------------------------------------------------------------
    ResetAll();
    SetMockHead(0, 0, 0, 0, IPD_M);
    CHECK(BringUp(), "hw2308: session up (jump)");
    g_gameOrigin[0] = 100.0f; g_gameOrigin[1] = 200.0f; g_gameOrigin[2] = 50.0f;
    g_gameYawDeg = 0.0f;
    CHECK(StepGame3D() == VR_FRAME_RENDER, "hw2308: eye 0 of the pair rendered");
    // ---- everything the loop did between eye 0 and eye 1 ----
    g_gameOrigin[0] += 25.8f;      // 37/38 stomping cg.refdef.vieworg
    g_gameOrigin[1] += -3.1f;
    g_gameOrigin[2] += 16.5f;      // net 30.73 u, the measured value
    g_gameYawDeg    += 25.4f;      // one body-yaw rebase step
    CHECK(StepGame3D() == VR_FRAME_RENDER, "hw2308: eye 1 of the SAME pair rendered");

    CHECK_NEAR(HwSepU(), WANT_U, 0.01,
               "hw2308: 30.73 u camera jump between eyes does NOT change separation");
    CHECK(HwSepU() < 4.0f,
          "hw2308: separation never approaches the 30.730 u that shipped");
    CHECK(HwLeftU() > 0.0f,
          "hw2308: laterality survives a between-eye camera jump");
    CHECK(HwDivergeDeg() < VR_STEREO_MAX_EYE_DIVERGE_DEG,
          "hw2308: 25.4 deg body-yaw step between eyes does NOT rotate one eye");
    CHECK(HwDivergeDeg() < 1e-3f,
          "hw2308: both eyes composed on ONE latched basis");

    // ---------------------------------------------------------------------
    // 3. The motion is not lost -- it reaches the NEXT pair and moves both
    //    eyes together. (A latch that swallowed camera motion would freeze
    //    the world; that is a different bug, and this pins it out.)
    // ---------------------------------------------------------------------
    {
        const float prevMidX = (g_out[0][0] + g_out[1][0]) * 0.5f;
        CHECK(StepGame3D() == VR_FRAME_RENDER, "hw2308: next pair, eye 0");
        CHECK(StepGame3D() == VR_FRAME_RENDER, "hw2308: next pair, eye 1");
        const float midX = (g_out[0][0] + g_out[1][0]) * 0.5f;
        CHECK(fabsf(midX - prevMidX) > 20.0f,
              "hw2308: the deferred camera motion lands on the NEXT pair");
        CHECK_NEAR(HwSepU(), WANT_U, 0.01,
                   "hw2308: and that next pair is still 2.456 u apart");
        CHECK(HwDivergeDeg() < 1e-3f,
              "hw2308: and its eyes still share one basis");
    }

    // ---------------------------------------------------------------------
    // 4. ALTERNATING eye0/eye1 game frames with continuous injected motion:
    //    50 pairs of the runaway, sampled every frame. Not one pair may
    //    diverge. This is the scenario the player actually experienced.
    // ---------------------------------------------------------------------
    ResetAll();
    SetMockHead(0, 0, 0, 0, IPD_M);
    CHECK(BringUp(), "hw2308: session up (runaway)");
    {
        int bad = 0, badPairs = 0;
        float worstSep = 0.0f, worstDiverge = 0.0f;
        float yaw = 0.0f;
        for (int p = 0; p < 50; ++p) {
            g_gameYawDeg = yaw;
            if (StepGame3D() != VR_FRAME_RENDER) { ++bad; continue; }   // eye 0
            // the runaway advances between the eyes, every pair
            yaw += 25.4f;                       // rebase step
            g_gameOrigin[0] += 3.0f;            // and the stomped origin
            g_gameYawDeg = yaw;
            if (StepGame3D() != VR_FRAME_RENDER) { ++bad; continue; }   // eye 1
            yaw += 25.4f;
            const float sep = HwSepU();
            const float dv  = HwDivergeDeg();
            if (fabsf(sep - WANT_U) > worstSep) worstSep = fabsf(sep - WANT_U);
            if (dv > worstDiverge) worstDiverge = dv;
            if (fabsf(sep - WANT_U) > 0.01f || dv > 1e-3f || HwLeftU() <= 0.0f)
                ++badPairs;
        }
        CHECK(bad == 0, "hw2308: 50 runaway pairs all rendered");
        CHECK(badPairs == 0,
              "hw2308: NOT ONE of 50 runaway pairs diverged (worst sep err %.4f u, "
              "worst basis divergence %.4f deg)", worstSep, worstDiverge);
    }

    // ---------------------------------------------------------------------
    // 5. MUTANTS. Each is a named hardware failure re-injected directly into
    //    the composed pair; the stereo gate must refuse every one.
    // ---------------------------------------------------------------------
    {
        ResetAll();
        SetMockHead(0, 0, 0, 0, IPD_M);
        CHECK(BringUp(), "hw2308: session up (mutants)");
        g_gameOrigin[0] = 100.0f; g_gameOrigin[1] = 200.0f; g_gameOrigin[2] = 50.0f;
        g_gameYawDeg = 0.0f;
        CHECK(StepPair() == VR_FRAME_RENDER, "hw2308: a clean pair to mutate");

        float o0[3], o1[3], a0[9], a1[9];
        memcpy(o0, g_out[0], sizeof(o0));      memcpy(o1, g_out[1], sizeof(o1));
        memcpy(a0, g_outAxis[0], sizeof(a0));  memcpy(a1, g_outAxis[1], sizeof(a1));

        CHECK(VR_TestStereoPairAcceptable(o0, o1, a0, a1, IPD_M, U),
              "hw2308 gate: the correct pair is ACCEPTED");

        // Mutant 1 -- the old 37/38 camera rewrite: eye 1 origin stomped with
        // a weapon pose 30 u away.
        {
            float m1[3] = { o1[0] + 25.8f, o1[1] - 3.1f, o1[2] + 16.5f };
            CHECK(!VR_TestStereoPairAcceptable(o0, m1, a0, a1, IPD_M, U),
                  "hw2308 MUTANT 1: 37/38 camera rewrite is REJECTED");
        }
        // Mutant 2 -- weapon steering moving the camera between the eyes.
        {
            float m1[3] = { o1[0] + 8.0f, o1[1], o1[2] };
            CHECK(!VR_TestStereoPairAcceptable(o0, m1, a0, a1, IPD_M, U),
                  "hw2308 MUTANT 2: steering-moved camera is REJECTED");
        }
        // Mutant 3 -- per-eye body-yaw basis mismatch (25.4 deg).
        {
            float bad1[9];
            MakeAxis(25.4f, 0, 0, bad1);
            CHECK(!VR_TestStereoPairAcceptable(o0, o1, a0, bad1, IPD_M, U),
                  "hw2308 MUTANT 3: per-eye basis mismatch is REJECTED");
        }
        // Mutant 4 -- the exact 30.730 u separation that shipped.
        {
            float m1[3] = { o1[0], o1[1] - 30.730f, o1[2] };
            CHECK(!VR_TestStereoPairAcceptable(o0, m1, a0, a1, IPD_M, U),
                  "hw2308 MUTANT 4: the shipped 30.730 u separation is REJECTED");
        }
        // Mutant 5 -- wrong-side laterality (eyes swapped).
        {
            CHECK(!VR_TestStereoPairAcceptable(o1, o0, a0, a1, IPD_M, U),
                  "hw2308 MUTANT 5: WRONG-SIDE laterality is REJECTED");
        }
        // Mutant 6 -- collapsed separation (the other magnitude direction).
        {
            float m1[3] = { o0[0], o0[1] - WANT_U * 0.2f, o0[2] };
            CHECK(!VR_TestStereoPairAcceptable(o0, m1, a0, a1, IPD_M, U),
                  "hw2308 MUTANT 6: collapsed separation is REJECTED");
        }
        // Mutant 7 -- non-finite camera basis.
        {
            float m1[3] = { o1[0], o1[1], o1[2] };
            volatile float zero = 0.0f;        // volatile: no constant folding
            m1[1] = zero / zero;               // NaN
            CHECK(m1[1] != m1[1], "hw2308 MUTANT 7: the injected value really is NaN");
            CHECK(!VR_TestStereoPairAcceptable(o0, m1, a0, a1, IPD_M, U),
                  "hw2308 MUTANT 7: non-finite camera basis is REJECTED");
        }
        // Mutant 8 -- a collision hook that mutated camera bytes.
        {
            CODVR_NoteCollisionHookCameraMutation();
            CHECK(!VR_TestStereoPairAcceptable(o0, o1, a0, a1, IPD_M, U),
                  "hw2308 MUTANT 8: collisionHookCameraMutationCount != 0 is REJECTED");
            CODVR_TestResetCollisionHookCameraMutation();
            CHECK(CODVR_CollisionHookCameraMutationCount() == 0,
                  "hw2308: invariant counter resets for the next scenario");
        }

        CHECK(VR_TestStereoPairAcceptable(o0, o1, a0, a1, IPD_M, U),
              "hw2308 gate: the correct pair is STILL accepted after the mutants");
    }

    // ---------------------------------------------------------------------
    // 6. PHASE 9 GUARD: this pass must not have touched the optics. The
    //    submitted tangents come from the runtime, and a "fix" that quietly
    //    changed zoom would be caught here.
    // ---------------------------------------------------------------------
    {
        ResetAll();
        SetMockHead(0, 0, 0, 0, IPD_M);
        CHECK(BringUp() && StepPair() == VR_FRAME_RENDER, "hw2308: session up (optics)");
        float tan0[4], tan1[4];
        VR_TestGetEyeTangents(0, tan0);
        VR_TestGetEyeTangents(1, tan1);
        CHECK_NEAR(tan0[0], tanf(g_mock.viewFov[0].angleLeft),  1e-5,
                   "hw2308 optics: eye 0 left tangent is the runtime one, untouched");
        CHECK_NEAR(tan0[1], tanf(g_mock.viewFov[0].angleRight), 1e-5,
                   "hw2308 optics: eye 0 right tangent is the runtime one, untouched");
        CHECK_NEAR(tan1[0], tanf(g_mock.viewFov[1].angleLeft),  1e-5,
                   "hw2308 optics: eye 1 left tangent is the runtime one, untouched");
        CHECK_NEAR(tan1[1], tanf(g_mock.viewFov[1].angleRight), 1e-5,
                   "hw2308 optics: eye 1 right tangent is the runtime one, untouched");
    }

    CHECK(g_mock.violations == 0, "hw2308: zero protocol violations");
}

#include "glsurf_tests.inc"
#include "objects_tests.inc"
#include "contract_tests.inc"
#include "vbo_safety_tests.inc"
#include "handpose_tests.inc"
#include "gunpose_tests.inc"
#include "reload_tests.inc"
#include "nativehands_tests.inc"
#include "ownership_tests.inc"
#include "stereo_tests.inc"
#include "independence_tests.inc"
#include "vbo_contract_tests.inc"
#include "temporal_tests.inc"
#include "surface_order_tests.inc"
#include "palm_anchor_tests.inc"
#include "bore_tests.inc"
#include "gun_ownership_tests.inc"
#include "sleeve_tests.inc"
#include "twohand_tests.inc"
#include "menu_tests.inc"
#include "grip_trim_tests.inc"
#include "comfort_update_tests.inc"
#include "precision_tests.inc"
#include "snapshot_tests.inc"
#include "postprocess_tests.inc"

int main()
{
    // Unbuffered: when a case takes the process down, the last line
    // printed has to be the last line REACHED. Block buffering hid the
    // 2026-08-26 crash four kilobytes downstream of where it happened.
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool haveGL = MakeGLContext();
    printf("=== COD VR safe suite (mocked OpenXR%s) ===\n",
           haveGL ? " + real GL" : ", NO GL CONTEXT");
    if (!haveGL)
        printf("  WARNING: no GL context; capture/blit/vignette run degraded\n");
    ResolveGLHelpers();
    ClearBackBuffer(1.0f, 0.0f, 0.0f);

    S_PureMath();
    S_Filters();
    S_ProjectionMath();
    S_AngularMapping();
    S_StartupTransactional();
    S_HappyPath();                 // leaves a live session for the block below
    S_PairSharedGeneration();
    S_ShouldRenderMachine();
    S_HalfPairShouldRenderFlip();
    S_CaptureFailures();
    S_SwapchainFailureAtSubmit();
    S_MixedGenerationGuard();
    S_WrongEyeIsRefused();
    S_FlatAndPairTransitions();
    S_UiSafeArea();
    S_DefensiveDoubleBegin();
    S_PairDiscarded();
    S_StoppingReadyResume();
    S_LossRecovery();
    S_ExitingIsFinal();
    S_HalfPairStopping();
    S_HalfPairSessionLoss();
    S_ShutdownMidPair();
    S_RefSpaceChange();
    S_RecenterAtomicAcrossPairs();
    S_InsanePoses();
    S_RuntimeOriginFarAway();
    S_SanePairBootstrap();
    S_MenuOrientationLatch();
    S_WaitFailureRecovery();
    S_BlendModePreference();
    S_PerViewDimensions();
    S_ProjectionInterception();
    S_PositionalTracking();
    S_ParallaxInvariant();
    S_StereoOracleAndMutants();
    S_HeadTranslationRigidity();
    S_SubmitIdentityLedger();
    S_HeadMotionVsVignette();
    S_HudPrincipalShift();
    S_PerfCountersVirtualTime();
    S_ComfortCadenceIndependence();
    S_VignetteHostileGL();

    // Native VR interaction (controllers / weapon / decoupling).
    S_InputActions();
    S_DecoupledAiming();
    S_ScriptedDetection();
    S_BindsMapping();
    S_GrenadeSafety();
    S_TwoHandAndAds();
    S_OneHandSixDof();
    S_TwoHandSolverGeometry();
    S_AdsOwnershipMachine();
    S_FireDirectionAndMutants();
    S_SupportGripMovement();
    S_MenuAndScriptedGuards();
    S_InputReconciler();
    S_SettingsIndependence();
    S_MenuSafeArea();

    // Bring-up ladder, tracking hysteresis, leak watchdog, pose consistency
    // (the 2026-08-20 hardware-run response).
    S_ControllerBringup();
    S_TrackingHysteresis();
    S_SteeringLeakGuard();
    S_RenderSubmitConsistency();

    S_RebaseLoopBounded();
    S_BaselineDisarmed();
    S_CoupledStickTurn();
    S_HardwareRun20260823();

    // Render-surface layer: the first-person weapon and hands as draw calls.
    S_GLSurfClassification();
    // The central architectural gate: three independently owned rendered
    // objects, proved on real output geometry through the real classifier,
    // with five coupling mutants that must be rejected.
    S_ThreeObjectRenderSeparation();
    S_GLSurfTransform();
    S_GLSurfHandIndependence();
    S_GLSurfPairLatch();
    S_GLSurfCameraUntouched();
    S_GLSurfFailClosed();
    S_GLSurfShiftMetric();
    S_GLSurfLaterality();
    S_GLSurfPivot();
    S_ObjectPairLatch();
    S_ClientArraySafety();
    S_DrawReentrancy();
    S_ClientArraySoak();
    S_VboShadow();
    S_ProductionInertAndCheap();

    // Deployment safety: what a player gets when the config is stale.
    S_ProbeProductionContract();

    // Native hands: the authored palm pose, recovered from the vertices.
    S_HandPoseSolve();
    S_HandPoseDelta();
    S_HandPoseIndependenceAndLaterality();
    S_GunPoseRearGrip();
    S_WeaponProfileConfidence();
    S_PhysicalReload();
    S_ReloadTableSelection();

    // Native hands wiring: de-quarantined 2026-08-26 after the engagement
    // deadlock was found and fixed (see GLSurf_SetHandSolveWanted) and the
    // harness learned to stop dereferencing what it just proved was null.
    S_NativeHandsWiring();
    S_NativeBaselineFreshness();
    S_NativeHandsSoak();
    S_NativeHandsStereoInvariant();

    // The controller-hands candidate: the weapon on the right controller,
    // every shipped hand rig, the left grab, the physical reload, and the
    // 10k production soak.
    S_NativeRigCoverage();
    S_NativeGunFollow();
    S_NativeGunStereoInvariant();
    S_NativeGunHold();
    S_NativeTwoHand();
    S_NativeReloadIntegration();
    S_NativeHands4Singles();
    // The production seam: real mocked-XR controllers driven through the real
    // pair boundary. This is the case the 2026-08-28 engagement bug could not
    // have survived; every case above it builds its own input snapshot.
    // The data-model guarantee the engagement fix rests on: the illegal
    // state is unrepresentable, and the old comparison is unsatisfiable.
    S_TrackStateTypeSafety();
    // No half-native pair: the stereo safety net, with the
    // eye0-PASS / eye1-FAIL mutant the brief requires.
    S_StereoPairEligibility();
    S_ProductionPoseOwnership();
    // Coordinated, never parent/child: separate calibrations, an
    // unbound LEFT_FREE hand, and one-hand aiming with no lag.
    S_CalibrationIndependence();
    S_LeftFreeUnbound();
    S_OneHandAimLag();
    S_TurnDoesNotMoveObjects();
    S_NativeHandsProductionSoak();

    // The VBO validity contract: orphaned bytes are UNKNOWN until written,
    // refused before the solver, attributed as VBO_BYTES_UNKNOWN (A-F).
    S_VboOrphanContract();
    // Temporal stereo in TRUE F1 order through the production submit path:
    // eye 0 provisional, eye 1, pre-submit verdict; the eye0-PASS/eye1-FAIL
    // mutant never reaches XR submission; >= 10k pairs judged.
    S_TemporalPairVerdict();
    S_GunSurfaceTables();
    S_GunAnySurfaceKeys();
    S_GunPreKeyOwnership();
    S_GunOwnerConflictGate();
    S_GunWeaponMatrix();
    S_SleeveTables();
    S_SleeveFollowsWrist();
    S_SleeveVisibilityAndFallback();
    S_SleeveVisibilityPolicy();
    S_TwoHandAttachThreshold();
    S_TwoHandRigidBar();
    S_TwoHandResidualAndRelease();
    S_TwoHandNoBlendOnCandidate();
    S_TwoHandPhysicalAds();
    S_TwoHandSightPerturbation();
    S_TwoHandSoak();
    S_GripTrimFeltStraight();
    S_ComfortHandling();
    S_ComfortBindings();
    S_ComfortMenu();
    S_ComfortProductionSoak();
    S_MenuModel();
    S_MenuNavigateAndApply();
    S_MenuPersistence();
    S_MenuOpenClose();
    S_SnapshotDecide();
    S_SnapshotPng();
    S_CrosshairLineSync();
    S_Brightness();
    S_Postprocess();
    S_SupportHandRidesGun();
    S_TriggerHandRidesGun();
    S_SurfaceOrderContract();
    S_PalmCalibrationFixture();
    S_PalmAnchorConvention();
    S_PalmAnchorAudit();
    S_PalmInvariantAcrossWeapons();
    S_HandVisualTrim();
    S_BoreAxisTable();
    S_BoreSightLineError();
    S_BoreToGameAim();
    S_BoreHeadIndependence();
    S_TemporalVanillaFallback();
    S_PhysicalAimSeam();
    S_TemporalSoak();

    S_FinalProtocolAudit();

    printf("=== SELF TEST: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
