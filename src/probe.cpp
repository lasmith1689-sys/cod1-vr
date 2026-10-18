// The self-scoring diagnostic, split out of dllmain.cpp.
//
// This runs INSTEAD of VR (codvr.cfg "probe 1") and needs no headset: the game
// runs flat on the monitor and writes a plain-language verdict to the log. That
// makes it the cheap way to settle questions which would otherwise cost a play
// session, and the game cannot be launched by script -- so every question that
// can be answered here is one the user does not have to test by hand.

#include "codvr.h"
#include "vr.h"

#include <math.h>
#include <float.h>

// The two syscalls that each receive the camera struct once per frame. Owned by
// dllmain because the naked assembly interposer branches on them directly.
extern int g_candA, g_candB;

static bool g_enabled;

bool Probe_Enabled()          { return g_enabled; }
void Probe_SetEnabled(bool on) { g_enabled = on; }

// ------------------------------------------------------------- experiment

enum { PH_BASE0 = 0, PH_HALF_A, PH_HALF_B, PH_DBL_A, PH_DBL_B, PH_DONE, PH_COUNT };
static const char* kPhaseName[PH_COUNT] = {
    "baseline (untouched)",
    "halve viewport via syscall 71",
    "halve viewport via syscall 243",
    "double render via syscall 71",
    "double render via syscall 243",
    "restored (untouched)"
};

enum { PHASE_MS = 5000, PHASE_MIN_FRAMES = 120 };

static volatile LONG g_phase;
static DWORD g_phaseStartTick;
static LONG  g_framesInPhase;
static bool  g_started;

static double g_ratio[PH_COUNT];
static bool   g_ratioValid[PH_COUNT];

// ------------------------------------------------ camera authority test
//
// THE decisive question: do our writes to the refdef actually drive the
// renderer? The old left/right viewport test cannot answer it -- it assumes the
// engine clears the half we stop drawing into, and if the engine never clears,
// the stale half looks unchanged whether our write worked or not. That false
// negative sent a whole session chasing the wrong layer.
//
// This test cannot be fooled that way. It toggles a drastic camera displacement
// on alternate frames and measures how much consecutive frames differ, then
// compares that against how much they differ with no toggling at all (ordinary
// scene motion). If our writes reach the renderer, toggling teleports the camera
// every other frame and the difference is enormous. If they do not, the two are
// the same. No assumption about clearing, viewports, or symmetry.
enum { CAM_SAMPLE_W = 512, CAM_SAMPLE_H = 8,
       CAM_PIX_BYTES = CAM_SAMPLE_W * CAM_SAMPLE_H * 4,
       CAM_SETTLE_FRAMES = 60, CAM_MEASURE_FRAMES = 120 };
enum { CAM_SETTLE = 0, CAM_MOTION, CAM_TOGGLE, CAM_DONE };

static int    g_camStage;
static int    g_camFrames;
static double g_camMotionSum;  static int g_camMotionN;
static double g_camToggleSum;  static int g_camToggleN;
static bool   g_camDisplaceNow;
static BYTE   g_camCur[CAM_PIX_BYTES];
static BYTE   g_camPrev[CAM_PIX_BYTES];
static bool   g_camPrevValid;

enum { GL_RGBA_ = 0x1908, GL_UNSIGNED_BYTE_ = 0x1401 };

typedef void (WINAPI *ReadPixelsFn)(int,int,int,int,unsigned,unsigned,void*);

static ReadPixelsFn ReadPixels()
{
    static ReadPixelsFn fn = nullptr;
    static bool looked = false;
    if (!looked) { looked = true; fn = (ReadPixelsFn)CODVR_GetGLProc("glReadPixels"); }
    return fn;
}

// Samples a fixed band of the frame and accumulates how much it changes between
// consecutive frames.
static void StepCameraAuthorityTest()
{
    if (g_camStage == CAM_DONE)
        return;

    ReadPixelsFn readPixels = ReadPixels();
    int w = (int)g_viewW, h = (int)g_viewH;
    if (!readPixels || w < CAM_SAMPLE_W || h < 64)
        return;                       // no scene yet; do not advance the test

    const int x0 = w/2 - CAM_SAMPLE_W/2;
    const int y0 = h/2 - CAM_SAMPLE_H/2;
    memset(g_camCur, 0, sizeof(g_camCur));
    readPixels(x0, y0, CAM_SAMPLE_W, CAM_SAMPLE_H, GL_RGBA_, GL_UNSIGNED_BYTE_, g_camCur);

    if (g_camPrevValid) {
        double sum = 0;
        for (int i = 0; i < CAM_PIX_BYTES; ++i) {
            int d = (int)g_camCur[i] - (int)g_camPrev[i];
            sum += d < 0 ? -d : d;
        }
        const double mad = sum / CAM_PIX_BYTES;
        if (g_camStage == CAM_MOTION)      { g_camMotionSum += mad; ++g_camMotionN; }
        else if (g_camStage == CAM_TOGGLE) { g_camToggleSum += mad; ++g_camToggleN; }
    }
    memcpy(g_camPrev, g_camCur, CAM_PIX_BYTES);
    g_camPrevValid = true;

    ++g_camFrames;
    const int limit = (g_camStage == CAM_SETTLE) ? CAM_SETTLE_FRAMES : CAM_MEASURE_FRAMES;
    if (g_camFrames < limit) {
        // Set here, at the end of a frame, so it applies to the frame the
        // engine builds next.
        g_camDisplaceNow = (g_camStage == CAM_TOGGLE) && ((g_camFrames & 1) != 0);
        return;
    }

    g_camFrames = 0;
    g_camPrevValid = false;
    ++g_camStage;

    if (g_camStage == CAM_DONE) {
        g_camDisplaceNow = false;
        const double motion = g_camMotionN ? g_camMotionSum / g_camMotionN : 0.0;
        const double toggle = g_camToggleN ? g_camToggleSum / g_camToggleN : 0.0;
        Log("");
        Log("========= CAMERA AUTHORITY TEST =========");
        Log("  ordinary frame-to-frame change : %.2f  (%d samples)", motion, g_camMotionN);
        Log("  with camera toggled every other: %.2f  (%d samples)", toggle, g_camToggleN);
        if (g_camMotionN < 10 || g_camToggleN < 10) {
            Log("  INCONCLUSIVE - too few frames with a scene on screen.");
            Log("  Load a mission and play continuously for ~20 seconds.");
        } else if (motion < 0.5 && toggle < 0.5) {
            Log("  INCONCLUSIVE - the sampled band never changed at all.");
            Log("  The back buffer may not be where the scene is being drawn.");
        } else if (toggle > motion * 3.0) {
            Log("  RESULT: our refdef writes DO drive the renderer.");
            Log("  Teleporting the camera changed the image %.1fx more than",
                motion > 0.01 ? toggle/motion : 0.0);
            Log("  ordinary motion. The camera hook is sound; the remaining");
            Log("  faults are in presentation (blit/FOV/pose), not authority.");
        } else {
            Log("  RESULT: our refdef writes DO **NOT** drive the renderer.");
            Log("  Teleporting the camera every other frame changed the image");
            Log("  no more than ordinary motion did. The engine is reading its");
            Log("  camera from somewhere other than the struct we are editing.");
            Log("  The whole per-eye camera approach needs to change: find the");
            Log("  real camera source before doing any more VR work.");
        }
        Log("=========================================");
    }
}

// ---------------------------------------------------- framebuffer scoring

enum { PIX_BYTES = 4096 * 16 * 4 };     // wide enough for a 4K-wide viewport
static BYTE g_pixels[PIX_BYTES];

// Detail level, not a hash. A hash only says two regions differ, which is
// uninformative for an asymmetric scene. What separates "something rendered
// here" from "nothing did" is texture: a drawn scene has high luminance
// variance; a region nothing wrote to collapses to the clear colour.
struct HalfStats { double mean, stddev; long nonBlack, total; };

static HalfStats MeasureHalf(const BYTE* px, int w, int rows, int x0, int x1)
{
    HalfStats s = {0, 0, 0, 0};
    double sum = 0, sumSq = 0;
    for (int y = 0; y < rows; ++y) {
        const BYTE* row = px + (size_t)y * w * 4;
        for (int x = x0; x < x1; ++x) {
            const BYTE* q = row + (size_t)x * 4;
            double luma = 0.299*q[0] + 0.587*q[1] + 0.114*q[2];
            sum += luma; sumSq += luma * luma;
            if (q[0] | q[1] | q[2]) ++s.nonBlack;
            ++s.total;
        }
    }
    if (s.total) {
        s.mean = sum / s.total;
        double var = sumSq / s.total - s.mean * s.mean;
        s.stddev = var > 0 ? sqrt(var) : 0.0;
    }
    return s;
}

static void MeasurePhase(int phase)
{
    ReadPixelsFn readPixels = ReadPixels();
    if (!readPixels) { Log("  phase %d: glReadPixels unavailable", phase); return; }

    int w = (int)g_viewW, h = (int)g_viewH;
    if (w < 64 || h < 64) { Log("  phase %d: no viewport yet", phase); return; }

    // Adapt the row count to the width so any resolution is measurable rather
    // than skipped -- a skipped phase wastes the whole session.
    int rows = PIX_BYTES / (w * 4);
    if (rows > 16) rows = 16;
    if (rows < 1)  { Log("  phase %d: viewport %d too wide to sample", phase, w); return; }

    memset(g_pixels, 0, (size_t)w * rows * 4);
    readPixels(0, h/2 - rows/2, w, rows, GL_RGBA_, GL_UNSIGNED_BYTE_, g_pixels);

    const int half = w / 2;
    HalfStats L = MeasureHalf(g_pixels, w, rows, 0, half);
    HalfStats R = MeasureHalf(g_pixels, w, rows, half, w);
    double ratio = (L.stddev > 1.0) ? (R.stddev / L.stddev) : -1.0;

    Log("  phase %d [%s]  (%d rows of %d px)", phase, kPhaseName[phase], rows, w);
    Log("     left  mean %6.1f stddev %6.2f nonblack %ld/%ld",
        L.mean, L.stddev, L.nonBlack, L.total);
    Log("     right mean %6.1f stddev %6.2f nonblack %ld/%ld",
        R.mean, R.stddev, R.nonBlack, R.total);
    Log("     right/left detail ratio = %.3f", ratio);

    if (ratio >= 0 && phase >= 0 && phase < PH_COUNT) {
        g_ratio[phase] = ratio;
        g_ratioValid[phase] = true;
    } else {
        Log("     (inconclusive: left half had no detail either)");
    }
}

static void Conclude()
{
    Log("");
    Log("=================== CONCLUSIONS ===================");

    if (!g_ratioValid[PH_BASE0]) {
        Log("  Baseline never scored -- not enough gameplay was rendered.");
        Log("  Load a mission and play ~30 seconds without pausing.");
        Log("===================================================");
        return;
    }
    const double base = g_ratio[PH_BASE0];
    Log("  baseline right/left detail ratio = %.3f", base);

    struct { int half, dbl, id; } cand[2] = {
        { PH_HALF_A, PH_DBL_A, g_candA },
        { PH_HALF_B, PH_DBL_B, g_candB }
    };

    int renderCall = -1;
    bool doubleWorks = false;

    for (int i = 0; i < 2; ++i) {
        Log("");
        Log("  --- syscall %d ---", cand[i].id);
        if (!g_ratioValid[cand[i].half]) {
            Log("    viewport phase not scored");
            continue;
        }
        double rh = g_ratio[cand[i].half];
        bool consumed = rh < 0.25 * base;
        Log("    halve viewport : ratio %.3f -> %s", rh,
            consumed ? "RIGHT HALF WENT FLAT: this refdef IS consumed by the renderer"
                     : "no change: this refdef is NOT what the renderer reads");
        if (!consumed)
            continue;
        renderCall = cand[i].id;

        if (!g_ratioValid[cand[i].dbl]) {
            Log("    double render  : not scored");
            continue;
        }
        double rd = g_ratio[cand[i].dbl];
        bool refilled = rd > 0.5 * base;
        Log("    double render  : ratio %.3f -> %s", rd,
            refilled ? "RIGHT HALF REFILLED: two scene renders per frame ACCEPTED"
                     : "stayed flat: the second render drew nothing");
        if (refilled)
            doubleWorks = true;
    }

    Log("");
    if (renderCall < 0) {
        Log("  RESULT: neither candidate's refdef drives the renderer.");
        Log("  The camera is read from somewhere else; the cgame syscall hook");
        Log("  is not sufficient and the architecture needs revisiting.");
    } else if (!doubleWorks) {
        Log("  RESULT: syscall %d IS the render call and our writes reach it,", renderCall);
        Log("  but the engine renders only the first scene per frame. Stereo");
        Log("  cannot be done by calling it twice; it will need either a");
        Log("  per-eye alternating frame or a lower-level hook.");
    } else {
        Log("  RESULT: syscall %d is R_RenderScene, our writes drive the camera,", renderCall);
        Log("  and the engine accepts two scene renders per frame with");
        Log("  independent viewports and cameras.");
        Log("  This is precisely the mechanism stereo VR requires. CONFIRMED.");
    }
    Log("===================================================");
}

void Probe_TestSetPhase(int phase)
{
    InterlockedExchange(&g_phase, phase);
    // The camera authority test runs ahead of the phases in a real session;
    // the self test targets the phase machine directly, so mark it finished.
    g_camStage = CAM_DONE;
    g_camDisplaceNow = false;
    g_started = (phase != PH_BASE0);
}

void Probe_ConcludeIfRunning()
{
    if (g_phase > PH_BASE0 && g_phase < PH_DONE) {
        Log("");
        Log("(process exited mid-experiment at phase %ld -- partial results)", g_phase);
        Conclude();
    }
}

// ------------------------------------------------------------- hook points

bool Probe_RenderHook(int num, void* p,
                      int a2, int a3, int a4, int a5, int a6, int a7, int* ret)
{
    if (!g_enabled)
        return false;

    int*   iv = (int*)p;
    float* fv = (float*)p;

    if (!g_started) {
        g_started = true;
        g_phaseStartTick = GetTickCount();
    }

    // Camera authority test: teleport the camera far above the map on the
    // frames the test asks for. If our writes drive the renderer this is
    // impossible to miss; if they do not, nothing changes.
    if (g_camDisplaceNow && num == g_renderSyscall) {
        const float oz = fv[RD_ORG+2];
        fv[RD_ORG+2] = oz + 4096.0f;
        *ret = g_realSyscall(num, p, a2, a3, a4, a5, a6, a7);
        fv[RD_ORG+2] = oz;
        return true;
    }

    const LONG phase = g_phase;
    const bool isA = (num == g_candA), isB = (num == g_candB);
    const bool halve   = (phase == PH_HALF_A && isA) || (phase == PH_HALF_B && isB);
    const bool doubled = (phase == PH_DBL_A  && isA) || (phase == PH_DBL_B  && isB);
    if (!halve && !doubled)
        return false;                     // let the caller forward it normally

    const int   sx = iv[RD_X], sy = iv[RD_Y], sw = iv[RD_W], sh = iv[RD_H];
    const float ox = fv[RD_ORG], oy = fv[RD_ORG+1], oz = fv[RD_ORG+2];
    const int   half = sw / 2;

    // Left half only. If this write reaches the renderer, nothing draws into
    // the right half and it collapses to the clear colour.
    iv[RD_X] = sx; iv[RD_W] = half;
    int r = g_realSyscall(num, p, a2, a3, a4, a5, a6, a7);

    if (doubled) {
        // Second scene render, same frame, own viewport and own camera.
        iv[RD_X] = sx + half; iv[RD_W] = half;
        fv[RD_ORG+2] = oz + 500.0f;
        g_realSyscall(num, p, a2, a3, a4, a5, a6, a7);
    }

    iv[RD_X] = sx; iv[RD_Y] = sy; iv[RD_W] = sw; iv[RD_H] = sh;
    fv[RD_ORG] = ox; fv[RD_ORG+1] = oy; fv[RD_ORG+2] = oz;
    *ret = r;
    return true;
}

bool Probe_FrameHook()
{
    if (!g_enabled)
        return false;

    // The camera authority test runs first and gates everything else: if our
    // writes do not reach the renderer, the later phases are noise.
    if (g_camStage != CAM_DONE) {
        StepCameraAuthorityTest();
        return true;                      // caller presents immediately
    }

    if (!g_started)
        return false;

    ++g_framesInPhase;

    // Advance one phase at a time, and only once the phase has both run long
    // enough AND actually rendered enough frames. Gating on frames as well as
    // time keeps a pause or alt-tab from silently skipping phases while nothing
    // is being drawn.
    LONG phase = g_phase;
    if (phase < PH_DONE
        && (GetTickCount() - g_phaseStartTick) >= PHASE_MS
        && g_framesInPhase >= PHASE_MIN_FRAMES)
    {
        MeasurePhase((int)phase);         // scores the frame just drawn
        InterlockedExchange(&g_phase, phase + 1);
        g_phaseStartTick = GetTickCount();
        g_framesInPhase = 0;
        if (phase + 1 == PH_DONE)
            Conclude();
    }
    return false;
}
