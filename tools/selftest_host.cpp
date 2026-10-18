// Minimal 32-bit host that loads the proxy DLL with self-test enabled.
//
// It creates a REAL OpenGL context on a hidden window BEFORE loading the DLL,
// so the DLL's self test can exercise the GL paths -- texture creation,
// framebuffer completeness, the scissored blit -- against a live driver. Those
// paths are otherwise only reachable inside the game, where a mistake costs a
// whole play session to discover.
//
// The window is never shown and nothing is presented; we only need a context.

#include <windows.h>
#include <stdio.h>

// Resolve wgl entry points from the SYSTEM opengl32 explicitly. Linking
// normally would bind to whatever opengl32.dll sits next to this exe -- which
// is the proxy we are here to test, and loading it twice under two paths only
// creates confusion.
typedef HGLRC (WINAPI *PFN_wglCreateContext)(HDC);
typedef BOOL  (WINAPI *PFN_wglMakeCurrent)(HDC, HGLRC);
typedef BOOL  (WINAPI *PFN_wglDeleteContext)(HGLRC);

static HWND  g_wnd;
static HDC   g_dc;
static HGLRC g_rc;
static HMODULE g_sysGL;
static PFN_wglDeleteContext p_wglDeleteContext;

static bool MakeGLContext()
{
    char sys[MAX_PATH];
    if (!GetSystemDirectoryA(sys, MAX_PATH))
        return false;
    strcat_s(sys, "\\opengl32.dll");
    g_sysGL = LoadLibraryA(sys);
    if (!g_sysGL) {
        printf("  (system opengl32 not loadable; skipping GL context)\n");
        return false;
    }
    PFN_wglCreateContext  create  = (PFN_wglCreateContext)GetProcAddress(g_sysGL, "wglCreateContext");
    PFN_wglMakeCurrent    current = (PFN_wglMakeCurrent)GetProcAddress(g_sysGL, "wglMakeCurrent");
    p_wglDeleteContext = (PFN_wglDeleteContext)GetProcAddress(g_sysGL, "wglDeleteContext");
    if (!create || !current)
        return false;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "codvr_selftest";
    RegisterClassA(&wc);

    g_wnd = CreateWindowA("codvr_selftest", "codvr", WS_OVERLAPPEDWINDOW,
                          0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_wnd)
        return false;
    g_dc = GetDC(g_wnd);
    if (!g_dc)
        return false;

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(g_dc, &pfd);
    if (!pf || !SetPixelFormat(g_dc, pf, &pfd))
        return false;

    g_rc = create(g_dc);
    if (!g_rc)
        return false;
    if (!current(g_dc, g_rc))
        return false;

    printf("  GL context created on a hidden window (self test can exercise GL)\n");
    return true;
}

static void DestroyGLContext()
{
    if (g_sysGL) {
        PFN_wglMakeCurrent current = (PFN_wglMakeCurrent)GetProcAddress(g_sysGL, "wglMakeCurrent");
        if (current) current(nullptr, nullptr);
    }
    if (g_rc && p_wglDeleteContext) p_wglDeleteContext(g_rc);
    if (g_dc && g_wnd) ReleaseDC(g_wnd, g_dc);
    if (g_wnd) DestroyWindow(g_wnd);
}

// The DLL writes its verdict to codvr_result.log beside itself. The host is
// the only process that knows whether the suite ran at all, so IT owns the
// gate: a missing log, an empty log, or any FAIL line is a nonzero exit, and
// build.ps1 refuses to deploy on nonzero. Before this, a red suite still
// produced a green build -- the log was printed but never enforced.
static int CheckResultLog(const char* dllPath)
{
    char logPath[MAX_PATH];
    strcpy_s(logPath, dllPath);
    char* slash = strrchr(logPath, '\\');
    char* fwd   = strrchr(logPath, '/');
    if (fwd > slash) slash = fwd;
    if (slash) *(slash + 1) = '\0'; else logPath[0] = '\0';
    strcat_s(logPath, "codvr_result.log");

    FILE* f = nullptr;
    if (fopen_s(&f, logPath, "r") != 0 || !f) {
        printf("SELF TEST GATE: no result log at %s\n", logPath);
        return 3;
    }

    int passes = 0, fails = 0, summaryPassed = -1, summaryFailed = -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "  PASS  ")) ++passes;
        if (strstr(line, "  FAIL  ")) { ++fails; printf("  %s", line); }
        int p, q;
        if (sscanf_s(line, " === SELF TEST: %d passed, %d failed ===", &p, &q) == 2) {
            summaryPassed = p; summaryFailed = q;
        }
    }
    fclose(f);

    if (fails > 0 || summaryFailed > 0) {
        printf("SELF TEST GATE: %d FAIL line(s), summary says %d failed\n",
               fails, summaryFailed < 0 ? fails : summaryFailed);
        return 4;
    }
    if (passes == 0 || summaryPassed <= 0) {
        printf("SELF TEST GATE: no passing tests recorded (log incomplete?)\n");
        return 5;
    }
    printf("self test gate ok: %d passed, 0 failed\n", summaryPassed);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: selftest_host <path-to-opengl32.dll>\n");
        return 2;
    }
    SetEnvironmentVariableA("CODVR_SELFTEST", "1");

    // Context first: the DLL runs its tests during DLL_PROCESS_ATTACH, so the
    // context has to already be current by the time it loads.
    const bool haveGL = MakeGLContext();
    if (!haveGL)
        printf("  WARNING: no GL context; GL-dependent tests will be skipped\n");

    HMODULE h = LoadLibraryA(argv[1]);
    if (!h) {
        printf("LoadLibrary FAILED: %lu\n", GetLastError());
        DestroyGLContext();
        return 1;
    }
    printf("loaded %s at %p\n", argv[1], (void*)h);

    // Spot-check that forwarding is wired: these must resolve to our thunks.
    const char* probes[] = { "glBegin", "glReadPixels", "wglSwapBuffers",
                             "wglCreateContext", "glGetString" };
    int missing = 0;
    for (int i = 0; i < 5; ++i) {
        FARPROC p = GetProcAddress(h, probes[i]);
        printf("  %-18s %s\n", probes[i], p ? "exported" : "MISSING");
        if (!p) ++missing;
    }

    typedef const char* (WINAPI *GetStringFn)(unsigned);
    GetStringFn glGetString = (GetStringFn)GetProcAddress(h, "glGetString");
    if (glGetString) {
        const char* v = glGetString(0x1F02);   // GL_VERSION
        printf("  glGetString(GL_VERSION) -> %s\n", v ? v : "NULL (no context)");
    }

    FreeLibrary(h);
    DestroyGLContext();
    printf(missing ? "EXPORT CHECK FAILED\n" : "export check ok\n");
    if (missing)
        return 1;

    // The suite itself: enforce the log the DLL just wrote.
    return CheckResultLog(argv[1]);
}
