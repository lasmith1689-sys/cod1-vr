"""Wire src/vr_glsurf.cpp into the proxy.

Kept as a script rather than as a hand-edited diff so the whole render-surface
integration can be re-applied to a fresh copy of the deployed source and the
result compared -- which is exactly what had to be done once already, when the
tree the milestone named turned out not to be the one actually deployed.

Idempotent: running twice is a no-op, and a missing anchor is a hard error
rather than a silent partial patch.
"""

import io
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"


def read(p):
    return io.open(p, encoding="utf-8", errors="surrogateescape").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", errors="surrogateescape").write(s)


class Patcher:
    def __init__(self, path):
        self.path = path
        self.text = read(path)
        self.applied = 0
        self.skipped = 0

    def sub(self, old, new, marker):
        if marker in self.text:
            self.skipped += 1
            return
        n = self.text.count(old)
        if n != 1:
            sys.exit(f"{self.path.name}: anchor found {n} times (want 1): {old[:70]!r}")
        self.text = self.text.replace(old, new, 1)
        self.applied += 1

    def save(self):
        write(self.path, self.text)
        print(f"  {self.path.name}: {self.applied} applied, {self.skipped} already present")


# --------------------------------------------------------------- proxy gen
GEN_ADD = '''    "glPopMatrix",
    # ---- the render-surface layer (src/vr_glsurf.cpp) ----------------------
    # The first-person weapon and hands are taken at the draw call, AFTER the
    # engine has animated and skinned them. Two kinds of entry point are
    # needed: the draws themselves, and the state that identifies which draw
    # is which. None of the state wrappers query the driver -- they only
    # remember what the engine set, so an ordinary frame pays a switch.
    "glDrawElements",
    "glDrawArrays",
    "glVertexPointer",
    "glNormalPointer",
    "glTexCoordPointer",
    "glColorPointer",
    "glEnableClientState",
    "glDisableClientState",
    "glBindTexture",
    # glDepthRange is the discriminator that matters: id Tech 3 narrows it
    # around RF_DEPTHHACK geometry, which is the engine itself saying "this is
    # the first-person overlay". That is an engine semantic, not a "near the
    # camera" guess.
    "glDepthRange",
    "glDepthMask",
    "glDepthFunc",
    "glCullFace",
    "glFrontFace",
    "glBlendFunc",
    "glAlphaFunc",
    "glEnable",
    "glDisable",
'''

# ------------------------------------------------------------- dllmain block
SURFACE_BLOCK = r'''// ------------------------------------------ render-surface interception
//
// Everything below hands the draw stream to src/vr_glsurf.cpp. Two rules
// govern this whole block:
//
//   * NOTHING here queries the driver. A glGet on the ~10k draws of an
//     ordinary frame would stall the pipeline for information we can simply
//     remember, so every wrapper does a store (or a switch) and forwards.
//   * The surface layer decides whether it OWNS a draw. When it returns 1 it
//     has already issued the draw itself; the wrapper must not issue it
//     again. Returning 0 -- the answer for every draw that is not positively
//     identified as the first-person weapon or hands -- means the engine's
//     own call goes through byte-for-byte, which is what "fails closed" means
//     at this layer.

static bool g_glSurfDriverReady;

static void InstallGLSurfDriver()
{
    if (g_glSurfDriverReady)
        return;
    // These are the REAL driver entry points resolved at load: calling back
    // through our own i_* wrappers would recurse forever the moment the
    // surface layer re-issued a draw.
    GLSurfDriver d;
    memset(&d, 0, sizeof(d));
    d.drawElements  = (void (WINAPI*)(unsigned,int,unsigned,const void*))p_glDrawElements;
    d.drawArrays    = (void (WINAPI*)(unsigned,int,int))p_glDrawArrays;
    d.loadMatrixf   = (void (WINAPI*)(const float*))p_glLoadMatrixf;
    d.matrixMode    = (void (WINAPI*)(unsigned))p_glMatrixMode;
    d.getFloatv     = (void (WINAPI*)(unsigned,float*))p_glGetFloatv;
    d.getIntegerv   = (void (WINAPI*)(unsigned,int*))p_glGetIntegerv;
    d.getDoublev    = (void (WINAPI*)(unsigned,double*))p_glGetDoublev;
    d.isEnabled     = (unsigned char (WINAPI*)(unsigned))p_glIsEnabled;
    d.getTexLevelParameteriv =
        (void (WINAPI*)(unsigned,int,unsigned,int*))p_glGetTexLevelParameteriv;
    d.vertexPointer = (void (WINAPI*)(int,unsigned,int,const void*))p_glVertexPointer;
    d.normalPointer = (void (WINAPI*)(unsigned,int,const void*))p_glNormalPointer;
    if (!d.drawElements || !d.loadMatrixf || !d.getFloatv)
        return;                            // try again on the next make-current
    GLSurf_SetDriver(&d);
    g_glSurfDriverReady = true;
    Log("glsurf: driver installed (draw/matrix/query entry points resolved)");
}

extern "C" void WINAPI i_glDrawElements(unsigned mode, int count, unsigned type,
                                        const void* idx)
{
    typedef void (WINAPI *Fn)(unsigned,int,unsigned,const void*);
    if (GLSurf_OnDrawElements(mode, count, type, idx))
        return;                            // the surface layer issued it
    ((Fn)p_glDrawElements)(mode, count, type, idx);
}

extern "C" void WINAPI i_glDrawArrays(unsigned mode, int first, int count)
{
    typedef void (WINAPI *Fn)(unsigned,int,int);
    if (GLSurf_OnDrawArrays(mode, first, count))
        return;
    ((Fn)p_glDrawArrays)(mode, first, count);
}

extern "C" void WINAPI i_glVertexPointer(int size, unsigned type, int stride, const void* p)
{
    typedef void (WINAPI *Fn)(int,unsigned,int,const void*);
    GLSurf_NoteVertexPointer(size, type, stride, p);
    ((Fn)p_glVertexPointer)(size, type, stride, p);
}

extern "C" void WINAPI i_glNormalPointer(unsigned type, int stride, const void* p)
{
    typedef void (WINAPI *Fn)(unsigned,int,const void*);
    GLSurf_NoteNormalPointer(type, stride, p);
    ((Fn)p_glNormalPointer)(type, stride, p);
}

extern "C" void WINAPI i_glTexCoordPointer(int size, unsigned type, int stride, const void* p)
{
    typedef void (WINAPI *Fn)(int,unsigned,int,const void*);
    GLSurf_NoteTexCoordPointer(size, type, stride, p);
    ((Fn)p_glTexCoordPointer)(size, type, stride, p);
}

extern "C" void WINAPI i_glColorPointer(int size, unsigned type, int stride, const void* p)
{
    typedef void (WINAPI *Fn)(int,unsigned,int,const void*);
    GLSurf_NoteColorPointer(size, type, stride, p);
    ((Fn)p_glColorPointer)(size, type, stride, p);
}

extern "C" void WINAPI i_glEnableClientState(unsigned cap)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteClientState(cap, 1);
    ((Fn)p_glEnableClientState)(cap);
}

extern "C" void WINAPI i_glDisableClientState(unsigned cap)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteClientState(cap, 0);
    ((Fn)p_glDisableClientState)(cap);
}

extern "C" void WINAPI i_glBindTexture(unsigned target, unsigned tex)
{
    typedef void (WINAPI *Fn)(unsigned,unsigned);
    GLSurf_NoteBindTexture(target, tex);
    ((Fn)p_glBindTexture)(target, tex);
}

extern "C" void WINAPI i_glDepthRange(double zn, double zf)
{
    typedef void (WINAPI *Fn)(double,double);
    GLSurf_NoteDepthRange(zn, zf);
    ((Fn)p_glDepthRange)(zn, zf);
}

extern "C" void WINAPI i_glDepthMask(unsigned char flag)
{
    typedef void (WINAPI *Fn)(unsigned char);
    GLSurf_NoteDepthMask(flag);
    ((Fn)p_glDepthMask)(flag);
}

extern "C" void WINAPI i_glDepthFunc(unsigned func)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteDepthFunc(func);
    ((Fn)p_glDepthFunc)(func);
}

extern "C" void WINAPI i_glCullFace(unsigned mode)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteCullFace(mode);
    ((Fn)p_glCullFace)(mode);
}

extern "C" void WINAPI i_glFrontFace(unsigned mode)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteFrontFace(mode);
    ((Fn)p_glFrontFace)(mode);
}

extern "C" void WINAPI i_glBlendFunc(unsigned s, unsigned d)
{
    typedef void (WINAPI *Fn)(unsigned,unsigned);
    GLSurf_NoteBlendFunc(s, d);
    ((Fn)p_glBlendFunc)(s, d);
}

extern "C" void WINAPI i_glAlphaFunc(unsigned func, float ref)
{
    typedef void (WINAPI *Fn)(unsigned,float);
    GLSurf_NoteAlphaFunc(func, ref);
    ((Fn)p_glAlphaFunc)(func, ref);
}

extern "C" void WINAPI i_glEnable(unsigned cap)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteEnable(cap, 1);
    ((Fn)p_glEnable)(cap);
}

extern "C" void WINAPI i_glDisable(unsigned cap)
{
    typedef void (WINAPI *Fn)(unsigned);
    GLSurf_NoteEnable(cap, 0);
    ((Fn)p_glDisable)(cap);
}

// Extension entry points reach the engine through wglGetProcAddress, so their
// wrappers are installed there rather than in the export table. Only the ones
// that change surface identity are wrapped: the multitexture unit selector
// (which texture binding belongs to which unit) and the compiled-vertex-array
// lock, which tells us the engine has frozen a vertex range.
static void (WINAPI *p_glActiveTextureARB)(unsigned);
static void (WINAPI *p_glClientActiveTextureARB)(unsigned);
static void (WINAPI *p_glLockArraysEXT)(int, int);
static void (WINAPI *p_glUnlockArraysEXT)(void);

extern "C" void WINAPI i_glActiveTextureARB(unsigned unit)
{
    GLSurf_NoteActiveTexture(unit);
    if (p_glActiveTextureARB) p_glActiveTextureARB(unit);
}

extern "C" void WINAPI i_glClientActiveTextureARB(unsigned unit)
{
    if (p_glClientActiveTextureARB) p_glClientActiveTextureARB(unit);
}

extern "C" void WINAPI i_glLockArraysEXT(int first, int count)
{
    GLSurf_NoteLockArrays(first, count);
    if (p_glLockArraysEXT) p_glLockArraysEXT(first, count);
}

extern "C" void WINAPI i_glUnlockArraysEXT(void)
{
    GLSurf_NoteUnlockArrays();
    if (p_glUnlockArraysEXT) p_glUnlockArraysEXT();
}

// Called from i_wglGetProcAddress. Returns our wrapper for the handful of
// extension entry points that carry surface identity, and null (meaning "hand
// back the driver's own function") for everything else -- the engine's shader
// compiler, VBO paths and anything else stay completely untouched.
static PROC GLSurfHookExtension(LPCSTR name, PROC real)
{
    struct Entry { const char* name; void** slot; void* wrapper; };
    static const Entry kHooks[] = {
        { "glActiveTextureARB",       (void**)&p_glActiveTextureARB,       (void*)&i_glActiveTextureARB },
        { "glClientActiveTextureARB", (void**)&p_glClientActiveTextureARB, (void*)&i_glClientActiveTextureARB },
        { "glLockArraysEXT",          (void**)&p_glLockArraysEXT,          (void*)&i_glLockArraysEXT },
        { "glUnlockArraysEXT",        (void**)&p_glUnlockArraysEXT,        (void*)&i_glUnlockArraysEXT },
    };
    for (size_t i = 0; i < sizeof(kHooks)/sizeof(kHooks[0]); ++i) {
        if (strcmp(name, kHooks[i].name) == 0) {
            *kHooks[i].slot = (void*)real;
            return (PROC)kHooks[i].wrapper;
        }
    }
    return nullptr;
}

// ---- flat draw-stream capture ---------------------------------------------
//
// The whole Phase 1 census runs on a monitor. codvr.cfg arms it, the game is
// launched with `+spmap`, the capture writes itself out and the process is
// asked to close normally. No headset, no human watching a screen, and no
// judgement call about what "looks like" the weapon.

// Flat-automation kill switch (codvr.cfg "vr_disable 1"). Without it a flat
// run is only flat because the headset happens to be off -- which is a
// property of the room, not of the build, and makes a capture unreproducible.
// With it, no XR session can be created at all.
static int  g_vrDisabled    = 0;
static int  g_glcapFrames   = 0;      // 0 disables the capture entirely
static int  g_glcapSkip     = 240;    // scene frames to let the map settle
static int  g_glcapAutoQuit = 0;
static int  g_glcapDumped   = 0;
static int  g_glcapQuitAfter = 0;     // scene frames before a no-capture quit
static char g_glcapTag[48]  = "flat";

// The flat A/B probe (Phase 2). One class, one known rigid delta, everything
// else untouched -- so a screenshot pair can be scored by a script instead of
// by someone looking at a monitor and deciding it "moved a bit".
static int   g_glprobeClass    = GLSC_NONE;
static float g_glprobeDelta[4] = { 0.0f, 0.0f, 0.0f, 0.0f };  // dx, dy, dz, yaw

static void ApplyGLProbe()
{
    GLSurf_SetProbe(g_glprobeClass, g_glprobeDelta[0], g_glprobeDelta[1],
                    g_glprobeDelta[2], g_glprobeDelta[3]);
    Log("config: glsurf probe class=%s d=(%.3f %.3f %.3f) yaw=%.2f",
        GLSurf_ClassName(g_glprobeClass), g_glprobeDelta[0], g_glprobeDelta[1],
        g_glprobeDelta[2], g_glprobeDelta[3]);
}

// Build a path beside this DLL. Used for the capture dump, which must land in
// the game folder next to codvr_result.log rather than in a temp directory.
static bool PathBesideModule(const char* leaf, char* out, size_t outSize)
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&PathBesideModule, &self);
    char path[MAX_PATH];
    if (!GetModuleFileNameA(self, path, MAX_PATH))
        return false;
    char* slash = strrchr(path, '\\');
    if (!slash)
        return false;
    slash[1] = 0;
    if (strlen(path) + strlen(leaf) + 1 > outSize)
        return false;
    strcpy_s(out, outSize, path);
    strcat_s(out, outSize, leaf);
    return true;
}

static void LogGLSurfStats(const char* when)
{
    GLSurfStats s;
    GLSurf_GetStats(&s);
    Log("glsurf [%s]: draws %u (scene %u, depth-hacked %u)", when,
        s.drawsSeen, s.drawsScene, s.drawsDepthHacked);
    Log("glsurf [%s]: gun %u  handL %u  handR %u  sleeveL %u  sleeveR %u  vmOther %u",
        when, s.classified[GLSC_GUN], s.classified[GLSC_HAND_L],
        s.classified[GLSC_HAND_R], s.classified[GLSC_SLEEVE_L],
        s.classified[GLSC_SLEEVE_R], s.classified[GLSC_VM_OTHER]);
    Log("glsurf [%s]: transformed %u  suppressed %u  restores %u  "
        "rejected(noDriver %u, ambiguous %u)  pairMismatch %u",
        when, s.transformed, s.suppressed, s.matrixRestores,
        s.rejectedNoDriver, s.rejectedAmbiguous, GLSurf_PairMismatchCount());
}

static void GLSurfQuitCleanly(HDC hdc, const char* why)
{
    // Close the way the user would: the engine runs its own shutdown, so the
    // next launch does not meet the "Run In Safe Mode?" prompt that a forced
    // termination leaves behind.
    HWND hwnd = WindowFromDC(hdc);
    if (!hwnd) hwnd = GetActiveWindow();
    if (hwnd) {
        Log("glsurf: %s -- posting WM_CLOSE for a clean exit", why);
        PostMessageA(hwnd, WM_CLOSE, 0, 0);
    } else {
        Log("glsurf: %s but no window found to close", why);
    }
}

static void GLSurfCaptureTick(HDC hdc)
{
    static unsigned sceneFrames = 0;
    if (g_glScenePending || g_glScene2DDone)
        ++sceneFrames;

    if (g_glcapFrames <= 0) {
        // No capture, but a bounded flat run was still asked for (used by the
        // A/B probe, which needs screenshots rather than a draw census).
        if (g_glcapQuitAfter > 0 && (int)sceneFrames >= g_glcapQuitAfter && !g_glcapDumped) {
            g_glcapDumped = 1;
            LogGLSurfStats("run");
            if (g_glcapAutoQuit) GLSurfQuitCleanly(hdc, "flat run complete");
        }
        return;
    }
    if (!GLSurf_CaptureArmed()) {
        GLSurf_ArmCapture(g_glcapSkip, g_glcapFrames, g_glcapTag);
        Log("glsurf: capture armed -- skip %d scene frames, record %d, tag '%s'",
            g_glcapSkip, g_glcapFrames, g_glcapTag);
        return;
    }
    if (!GLSurf_CaptureDone() || g_glcapDumped)
        return;
    g_glcapDumped = 1;

    char leaf[96], path[MAX_PATH];
    sprintf_s(leaf, "codvr_draws_%s.csv", g_glcapTag);
    if (PathBesideModule(leaf, path, sizeof(path))) {
        const int n = GLSurf_DumpCapture(path);
        Log("glsurf: wrote %d draw records to %s", n, path);
    } else {
        Log("glsurf: could not build a dump path");
    }
    LogGLSurfStats("capture");
    if (g_glcapAutoQuit)
        GLSurfQuitCleanly(hdc, "capture complete");
}

'''

CONFIG_KEYS = r'''        // ---------------------------------- render-surface (glsurf) ------
        } else if (!_stricmp(key, "vr_disable")) {
            g_vrDisabled = (val != 0);
            Log("config: VR %s", g_vrDisabled ? "DISABLED (flat run)" : "enabled");
        } else if (!_stricmp(key, "vr_glcap_frames")) {
            g_glcapFrames = (int)val;
            Log("config: glsurf capture frames = %d", g_glcapFrames);
        } else if (!_stricmp(key, "vr_glcap_skip")) {
            g_glcapSkip = (int)val;
            Log("config: glsurf capture skip = %d scene frames", g_glcapSkip);
        } else if (!_stricmp(key, "vr_glcap_autoquit")) {
            g_glcapAutoQuit = (val != 0);
            Log("config: glsurf capture auto-quit = %d", g_glcapAutoQuit);
        } else if (!_stricmp(key, "vr_glcap_quit_after")) {
            g_glcapQuitAfter = (int)val;
            Log("config: glsurf quit after %d scene frames", g_glcapQuitAfter);
        } else if (!_stricmp(key, "vr_glcap_tag")) {
            strncpy_s(g_glcapTag, text, sizeof(g_glcapTag) - 1);
            Log("config: glsurf capture tag = %s", g_glcapTag);
        } else if (!_stricmp(key, "vr_glsurf_route")) {
            GLSurf_SetRoute((int)val);
            Log("config: glsurf transform route = %s",
                GLSurf_Route() == GLS_ROUTE_SCRATCH_ID ? "scratch arrays" : "modelview delta");
        } else if (!_stricmp(key, "vr_glprobe_class")) {
            g_glprobeClass = (int)val;
            ApplyGLProbe();
        } else if (!_stricmp(key, "vr_glprobe_dx") || !_stricmp(key, "vr_glprobe_dy")
                || !_stricmp(key, "vr_glprobe_dz") || !_stricmp(key, "vr_glprobe_yaw")) {
            if (!_stricmp(key, "vr_glprobe_dx"))  g_glprobeDelta[0] = val;
            if (!_stricmp(key, "vr_glprobe_dy"))  g_glprobeDelta[1] = val;
            if (!_stricmp(key, "vr_glprobe_dz"))  g_glprobeDelta[2] = val;
            if (!_stricmp(key, "vr_glprobe_yaw")) g_glprobeDelta[3] = val;
            ApplyGLProbe();
        } else if (!_stricmp(key, "vr_glsurf_hide_sleeves")) {
            GLSurf_SetClassHidden(GLSC_SLEEVE_L, val != 0);
            GLSurf_SetClassHidden(GLSC_SLEEVE_R, val != 0);
            Log("config: glsurf sleeves %s", val != 0 ? "HIDDEN" : "drawn");
'''


def patch_gen():
    p = ROOT / "tools" / "gen_gl_proxy.py"
    q = Patcher(p)
    q.sub('    "glPopMatrix",\n', GEN_ADD, '"glDrawElements",')
    q.save()


def patch_dllmain():
    p = SRC / "dllmain.cpp"
    q = Patcher(p)

    q.sub('#include "vr_binds.h"\n',
          '#include "vr_binds.h"\n#include "vr_glsurf.h"\n',
          '#include "vr_glsurf.h"')

    q.sub('''extern "C" PROC WINAPI i_wglGetProcAddress(LPCSTR name)
{
    typedef PROC (WINAPI *Fn)(LPCSTR);
    if (name && !strcmp(name, "wglSwapIntervalEXT"))
        return (PROC)&i_wglSwapIntervalEXT;
    return ((Fn)p_wglGetProcAddress)(name);
}''',
          '''// Defined with the render-surface block far below; declared here because
// wglGetProcAddress is the ONLY place an extension entry point can be caught,
// and it sits at the top of the file.
static PROC GLSurfHookExtension(LPCSTR name, PROC real);

extern "C" PROC WINAPI i_wglGetProcAddress(LPCSTR name)
{
    typedef PROC (WINAPI *Fn)(LPCSTR);
    if (name && !strcmp(name, "wglSwapIntervalEXT"))
        return (PROC)&i_wglSwapIntervalEXT;
    PROC real = ((Fn)p_wglGetProcAddress)(name);
    if (name && real) {
        PROC hooked = GLSurfHookExtension(name, real);
        if (hooked) return hooked;
    }
    return real;
}''',
          'GLSurfHookExtension')

    q.sub('// ----------------------------------------- projection-stack interception\n',
          SURFACE_BLOCK + '// ----------------------------------------- projection-stack interception\n',
          'render-surface interception')

    q.sub('''        g_curDC = hdc;
        g_curRC = rc;
    }''',
          '''        g_curDC = hdc;
        g_curRC = rc;
        // A live context is the first moment the surface layer can be given
        // the real driver; before that every entry point is still unresolved.
        InstallGLSurfDriver();
    }''',
          'InstallGLSurfDriver();\n    }')

    q.sub('''static void UpdateScenePhase()
{
    const bool now = GLScenePhase();
    if (now != g_glScenePhaseApplied) {''',
          '''static void UpdateScenePhase()
{
    const bool now = GLScenePhase();
    // The surface layer must never claim a HUD or menu draw, so it is told
    // the phase from the SAME classifier the viewport mapping uses -- there is
    // only one answer to "are we in the 3D pass" in this build.
    GLSurf_ScenePhase(now ? 1 : 0);
    if (now != g_glScenePhaseApplied) {''',
          'GLSurf_ScenePhase(now ? 1 : 0);')

    q.sub('    g_lastRawViewportValid = true;\n',
          '    g_lastRawViewportValid = true;\n    GLSurf_NoteViewport(x, y, w, h);\n',
          'GLSurf_NoteViewport(x, y, w, h);')

    q.sub('    g_lastRawScissorValid = true;\n',
          '    g_lastRawScissorValid = true;\n    GLSurf_NoteScissor(x, y, w, h);\n',
          'GLSurf_NoteScissor(x, y, w, h);')

    q.sub('    InterlockedExchange(&g_glMatrixMode, (LONG)mode);\n',
          '    InterlockedExchange(&g_glMatrixMode, (LONG)mode);\n    GLSurf_NoteMatrixMode(mode);\n',
          'GLSurf_NoteMatrixMode(mode);')

    q.sub('''    if (num == g_renderSyscall) {
        static bool flatGeomLogged = false;''',
          '''    if (num == g_renderSyscall) {
        // Tell the surface layer a 3D scene is starting, in EVERY mode. The
        // whole point of the flat runs is that draw classification is proved
        // on a monitor, so this must not be inside the VR-only branch.
        GLSurf_SceneQueued(VR_Active() ? g_vrEye : 0);
        static bool flatGeomLogged = false;''',
          'GLSurf_SceneQueued(VR_Active() ? g_vrEye : 0);')

    q.sub('''        VR_Startup(g_curDC ? g_curDC : hdc, g_curRC);''',
          '''        if (!g_vrDisabled)
            VR_Startup(g_curDC ? g_curDC : hdc, g_curRC);''',
          'if (!g_vrDisabled)')

    q.sub('''        CODVR_GLFrameReset();       // a frame is 2D until a scene render says otherwise''',
          '''        CODVR_GLFrameReset();       // a frame is 2D until a scene render says otherwise
        GLSurf_BeginFrame();
        GLSurfCaptureTick(hdc);''',
          'GLSurfCaptureTick(hdc);')

    q.sub('''        } else if (!_stricmp(key, "supersample")) {''',
          CONFIG_KEYS + '''        } else if (!_stricmp(key, "supersample")) {''',
          'vr_glcap_frames')

    q.save()


def patch_build():
    p = ROOT / "build.ps1"
    q = Patcher(p)
    q.sub(r'''"`"$src\vr_input.cpp`" `"$src\vr_weapon.cpp`" `"$src\vr_binds.cpp`"")''',
          '"`"$src\\vr_input.cpp`" `"$src\\vr_weapon.cpp`" `"$src\\vr_binds.cpp`" " +\n'
          '     "`"$src\\vr_glsurf.cpp`"")',
          r'`"$src\vr_glsurf.cpp`"")')
    q.sub(r'''"`"$out\vr_input.obj`" `"$out\vr_weapon.obj`" `"$out\vr_binds.obj`" `"$xrLib`" " +''',
          '"`"$out\\vr_input.obj`" `"$out\\vr_weapon.obj`" `"$out\\vr_binds.obj`" " +\n'
          '     "`"$out\\vr_glsurf.obj`" `"$xrLib`" " +',
          r'vr_glsurf.obj')
    q.sub(r'''"`"$src\vr_weapon.cpp`" `"$src\vr_binds.cpp`" " +''',
          '"`"$src\\vr_weapon.cpp`" `"$src\\vr_binds.cpp`" `"$src\\vr_glsurf.cpp`" " +',
          r'`"$src\vr_binds.cpp`" `"$src\vr_glsurf.cpp`"')
    q.save()


if __name__ == "__main__":
    print("wiring vr_glsurf into the proxy:")
    patch_gen()
    patch_dllmain()
    patch_build()
    print("done")
