# Configuration and build — CoD1 VR mod

Reference material for the audit: the user-facing settings, the game's own video
configuration, and how the thing is built and tested.

---

## `codvr.cfg` — the mod's settings

Lives beside `opengl32.dll` in the game folder, read once at startup. Every
setting is runtime-configurable; nothing here needs a rebuild.

```ini
# =========================================================================
#  Call of Duty (2003) -- VR configuration
#
#  Lives beside opengl32.dll in the game folder. Read once at startup, so
#  changes take effect on the next launch.
#
#  Format: one "name value" pair per line. Lines starting with # are ignored.
#  Nothing here needs a rebuild -- every setting is read at runtime.
# =========================================================================


# ------------------------------------------------------------ IMAGE QUALITY

# SUPERSAMPLING. Each headset eye is about 3072x3264, far more than the game
# draws on its own, so without this the image is upscaled and soft. Rendering is
# redirected into an offscreen target this many times the game's resolution and
# sampled down once for the headset. The engine's own coordinates are scaled to
# match, so the HUD sharpens along with the world.
#
# Paired with the square 1440x1440 buffer set in Main/autoexec.cfg, 2.25 renders
# 3240x3240 -- almost exactly one eye.
#
# Lower this first if the frame rate suffers: 1.75 and 1.5 still look good.
# 1.0 disables it entirely and draws straight into the game's own buffer.
supersample 2.25


# ------------------------------------------------------------------ COMFORT

# TUNNELLING VIGNETTE. Motion sickness in VR comes from seeing movement your
# body cannot feel. Narrowing the field of view while you move or turn removes
# the peripheral motion that causes it, and it opens back up when you stop.
# This is the single most effective comfort setting in VR.
#
# Turn it off if you are not prone to sickness -- a wider view is more immersive.
vignette 1

# How far it closes at full speed. 1.0 is a strong tunnel, 0.5 is subtle.
vignetteStrength 1.0

# How much motion it takes to start closing. RAISE these to make the vignette
# rarer and weaker; LOWER them if you still feel uneasy.
#   speed: engine units per second of travel (walking is roughly 100)
#   turn : degrees per second of turning
vignetteSpeed 40
vignetteTurn 45

# SNAP TURN. Rotating in fixed steps instead of sweeping past the whole world is
# much gentler if you are sickness-prone. Set the step in degrees; 0 disables it
# and you simply turn with the mouse as normal. 30 or 45 are typical.
snapTurn 0

# Keys for snap turn, as "left,right". Only used when snapTurn is non-zero.
snapTurnKeys Q,E

# Snap turn works by feeding the game mouse movement, so its size depends on
# your in-game sensitivity. If a snap turns too far, lower this; too little,
# raise it.
snapMouseScale 4.0

# RECENTER. Makes wherever you are sitting and whichever way you are facing the
# new "straight ahead". Seated tracking drifts, so this gets used often.
# Accepts F1-F12, HOME, END, INSERT, DELETE, PAGEUP, PAGEDOWN, arrow keys,
# TAB, or a single letter/digit. Use "none" to unbind.
recenterKey F9


# ---------------------------------------------------------------------- VIEW

# WORLD SCALE: engine units per real-world metre. Quake-family engines are built
# in inches, so 39.37 is the true value. RAISE it to make the world feel larger
# and yourself smaller; LOWER it to feel like a giant. Small changes matter --
# try 36 or 42 before anything more extreme.
worldscale 39.37

# HUD DEPTH. The 2D overlay is drawn the same for both eyes, which puts it at
# infinite distance -- tiring to focus on with the world only a few metres away.
# This nudges it per eye so it sits at roughly arm's length.
# Raise to bring the HUD closer, lower to push it away, 0 to leave it at
# infinity. If the HUD ever looks doubled, set this to 0.
hudshift 12

# HUD SHAPE. The render buffer is square, because that matches the headset's
# eye and wastes no pixels -- but the game lays its HUD out for a wide screen,
# so drawn directly it would come out stretched tall. This draws the 2D layer
# into a correctly-proportioned band instead.
#
# 1.777 is 16:9, matching how the HUD looked before VR. Use 1.333 for classic
# 4:3 proportions (a slightly larger, squarer HUD), or 0 to disable the
# correction and let the HUD stretch to fill the square.
hudAspect 1.777


# ------------------------------------------------------------- ENGINE / DEBUG

# Which cgame syscall is R_RenderScene. Confirmed as 71 by call-site argument
# analysis plus a captured frame trace. There should be no reason to change it.
renderSyscall 71

# Diagnostic mode. 1 disables VR and runs the game FLAT on the monitor, writing
# a self-scoring verdict to codvr_result.log -- no headset needed. Only useful
# when something is badly wrong and we need to know which layer is at fault.
probe 0

# Scene replay: no longer used for stereo (frames alternate eyes instead, which
# removes the problem it existed to solve) but kept behind this switch. It
# disables itself automatically if it ever faults.
replay 1
```

---

## `Main/autoexec.cfg` — the game's own video settings

Set by the mod author. The square resolution is deliberate (see BRIEF 4.3);
`com_maxfps 0` and `r_swapInterval 0` were already present before this project.
A backup of the original is kept at `Main/autoexec.cfg.pre-vr-backup`.

```
// SQUARE render target for VR (was 2560x1440 fullscreen).
//
// Each headset eye is 3072x3264 -- very nearly square -- while a 16:9 buffer
// spends most of its pixels on horizontal detail the eye cannot use and starves
// the vertical, which is what made the view look soft. A square buffer matches
// the eye, so the same pixel budget buys sharper vertical detail.
//
// Windowed on purpose: 1440x1440 is not a mode any monitor can display, so
// fullscreen would fail to set it. The mod renders offscreen and mirrors to the
// window anyway, so the window is only a preview.
//
// To go back to flat play, restore Main/autoexec.cfg.pre-vr-backup.
seta r_mode "-1"
seta r_customwidth "1440"
seta r_customheight "1440"
seta r_customaspect "1"
seta r_fullscreen "0"
seta cg_fov "96.418"

// Skip legacy intro playback, which can fail at custom resolutions.
seta com_introplayed "1"

// Disable hardware overbrightening so captures and shadows are not abnormally dark.
seta r_overBrightBits "0"
seta r_ignorehwgamma "0"
seta r_gamma "1.0"

// Remove both the engine FPS cap and vertical-sync refresh cap.
seta com_maxfps "0"
seta r_swapInterval "0"
```

---

## `build.ps1` — build, test, deploy

`.uild.ps1 -SelfTest -Deploy` regenerates the 368 export thunks, compiles x86,
rebuilds the test host, runs the test suite, and deploys.

Note the self-test step: the host creates a **real OpenGL context on a hidden
window BEFORE loading the DLL**, so the DLL's `DLL_PROCESS_ATTACH` self-test can
exercise the actual capture/blit/vignette code against a live driver. That is the
only way to de-risk GL work without spending a hardware session.

```powershell
<#
  Builds the 32-bit opengl32.dll proxy that carries the VR mod.

  Everything is x86 because CoD1 is a 32-bit process -- including the OpenXR
  loader, which is why the Win32 (not x64) loader from the SDK is linked.
#>
param([switch]$Deploy, [switch]$SelfTest)

$ErrorActionPreference = 'Stop'
$root    = $PSScriptRoot
$src     = Join-Path $root 'src'
$out     = Join-Path $root 'build'
$dist    = Join-Path $root 'dist'
$xrInc   = Join-Path $root 'third_party\openxr\include'
$xrLib   = Join-Path $root 'third_party\openxr\native\Win32\release\lib\openxr_loader.lib'
$xrDll   = Join-Path $root 'third_party\openxr\native\Win32\release\bin\openxr_loader.dll'
$vcvars  = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
$gameDir = 'H:\SteamLibrary\steamapps\common\Call of Duty'

foreach ($p in @($vcvars, $xrLib, $xrDll)) {
    if (-not (Test-Path $p)) { throw "missing prerequisite: $p" }
}
New-Item -ItemType Directory -Force -Path $out, $dist | Out-Null

Write-Host '[1/4] regenerating export thunks' -ForegroundColor Cyan
& C:\Python314\python.exe (Join-Path $root 'tools\gen_gl_proxy.py')
if ($LASTEXITCODE -ne 0) { throw 'export generation failed' }

Write-Host '[2/4] compiling (x86)' -ForegroundColor Cyan
$bat = Join-Path $out '_build.bat'
$lines = @(
    '@echo off'
    "call `"$vcvars`" x86 >nul 2>&1"
    ('cl /nologo /c /O2 /MT /W3 /EHsc /GS- /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WINDOWS ' +
     "/I`"$src`" /I`"$xrInc`" /Fo`"$out\\`" " +
     "`"$src\dllmain.cpp`" `"$src\vr.cpp`" `"$src\probe.cpp`" `"$src\comfort.cpp`"")
    'if errorlevel 1 exit /b 1'
    # Delay-load the OpenXR loader. Linked normally it becomes a hard import,
    # and a missing openxr_loader.dll would stop opengl32.dll loading at all --
    # which means the game would refuse to start rather than just run flat.
    ("link /nologo /DLL /MACHINE:X86 /DEF:`"$src\opengl32.def`" " +
     "/OUT:`"$out\opengl32.dll`" /IMPLIB:`"$out\opengl32.lib`" " +
     '/DELAYLOAD:openxr_loader.dll ' +
     "`"$out\dllmain.obj`" `"$out\vr.obj`" `"$out\probe.obj`" `"$out\comfort.obj`" `"$xrLib`" " +
     'delayimp.lib kernel32.lib user32.lib gdi32.lib advapi32.lib')
    'if errorlevel 1 exit /b 1'
    'exit /b 0'
)
Set-Content -Path $bat -Value $lines -Encoding ascii

$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& cmd /c "`"$bat`"" 2>&1 | ForEach-Object { Write-Host $_ }
$code = $LASTEXITCODE
$ErrorActionPreference = $prev
if ($code -ne 0) { throw "build failed (exit $code)" }

Copy-Item "$out\opengl32.dll" $dist -Force
Copy-Item $xrDll $dist -Force
Write-Host "[3/4] built $dist\opengl32.dll" -ForegroundColor Green

# Verify we produced a 32-bit DLL whose export table still matches the real one.
$verify = @"
import struct
d=open(r'$out\opengl32.dll','rb').read()
pe=struct.unpack_from('<I',d,0x3C)[0]
mach=struct.unpack_from('<H',d,pe+4)[0]
print('  machine :', {0x14c:'x86 (32-bit) OK',0x8664:'x64 WRONG'}.get(mach,hex(mach)))
opt=pe+24; n=struct.unpack_from('<H',d,pe+6)[0]; osz=struct.unpack_from('<H',d,pe+20)[0]
erva,_=struct.unpack_from('<II',d,opt+96)
secs=[]
for i in range(n):
    s=opt+osz+i*40
    vs,va,rs,ro=struct.unpack_from('<IIII',d,s+8); secs.append((va,vs,ro,rs))
def o(r):
    for va,vs,ro,rs in secs:
        if va<=r<va+max(vs,rs): return ro+(r-va)
    raise SystemExit('bad rva')
print('  exports :', struct.unpack_from('<I',d,o(erva)+24)[0], '(must be 368)')
"@
$verify | & C:\Python314\python.exe -

if ($SelfTest) {
    Write-Host '[4/4] self test' -ForegroundColor Cyan
    # Rebuild the host too: it brings up a real GL context so the DLL's tests
    # can exercise the capture/blit paths against a live driver.
    $hostBat = Join-Path $out '_buildhost.bat'
    $hostLines = @(
        '@echo off'
        "call `"$vcvars`" x86 >nul 2>&1"
        ("cl /nologo /O2 /MT /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS " +
         "/Fo`"$out\\`" /Fe`"$out\selftest_host.exe`" " +
         "`"$root\tools\selftest_host.cpp`" user32.lib gdi32.lib")
        'if errorlevel 1 exit /b 1'
        'exit /b 0'
    )
    Set-Content -Path $hostBat -Value $hostLines -Encoding ascii
    $prevH = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & cmd /c "`"$hostBat`"" 2>&1 | ForEach-Object { Write-Host "  $_" }
    $hostCode = $LASTEXITCODE
    $ErrorActionPreference = $prevH
    if ($hostCode -ne 0) { Write-Host '  (host rebuild failed; using existing exe)' -ForegroundColor DarkYellow }

    Remove-Item "$dist\codvr_result.log" -ErrorAction SilentlyContinue
    & "$out\selftest_host.exe" "$dist\opengl32.dll"
    if (Test-Path "$dist\codvr_result.log") {
        Get-Content "$dist\codvr_result.log" | Where-Object { $_ -match 'PASS|FAIL|SELF TEST|VR:' }
    }
} else {
    Write-Host '[4/4] self test skipped (pass -SelfTest)' -ForegroundColor DarkGray
}

if ($Deploy) {
    if (-not (Test-Path $gameDir)) { throw "game dir not found: $gameDir" }
    Copy-Item "$dist\opengl32.dll" (Join-Path $gameDir 'opengl32.dll') -Force
    # The OpenXR loader must sit beside the game exe so the DLL can find it.
    Copy-Item $xrDll (Join-Path $gameDir 'openxr_loader.dll') -Force
    Write-Host "deployed opengl32.dll + openxr_loader.dll -> $gameDir" -ForegroundColor Yellow
}
```

---

## `tools/selftest_host.cpp` — the GL test host

```cpp
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
    return missing ? 1 : 0;
}
```

---

## `tools/find_render_syscall.py` — static arity analysis

This is how syscall 71 was identified as `R_RenderScene`, and how the argument
counts of the entity-add syscalls (37 takes 2, 38 takes 4) were recovered. It
disassembles the UNPACKED `cgamex86.dll` and reads the stack cleanup after each
indirect call through the syscall pointer. Costs no hardware session.

```python
"""Decide which cgame syscall is R_RenderScene by disassembling cgamex86.dll.

Both syscall 71 and 243 appeared to receive the refdef pointer at runtime, but
that reading is unreliable: for a zero-argument syscall the "first argument" we
sampled is just whatever was left on the stack, which can easily be a refdef
pointer pushed moments earlier.

Arity settles it. Quake 3's trap wrappers compile to:

    push <arg>            ; only when the call takes one
    push <syscall number>
    call [g_syscall]
    add  esp, N           ; N = 4 * (1 + argument count)

so N == 8 means one argument (a refdef -> R_RenderScene) and N == 4 means none
(-> R_ClearScene). The module is not packed, so this is readable statically.

Anchors on the indirect call through the syscall pointer rather than on a
`push imm`, because the compiler is free to materialise the number in other
ways. Disassembly restarts after invalid bytes so the whole section is covered
-- a single linear pass stops at the first bad opcode and silently misses most
of the code.
"""

import struct
from collections import defaultdict
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

GAME = Path(r"H:\SteamLibrary\steamapps\common\Call of Duty")
MODULE = GAME / "cgamex86.dll"
SYSCALL_RVA = 0x657E8
CANDIDATES = (71, 243)


def load_text(path):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from("<I", data, opt + 28)[0]
    for i in range(nsec):
        s = opt + optsz + i * 40
        name = data[s:s + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, rawsz, rawoff = struct.unpack_from("<IIII", data, s + 8)
        if name == ".text":
            return data[rawoff:rawoff + rawsz], base + va, base
    raise SystemExit("no .text")


def disasm_all(code, addr):
    """Linear sweep that resumes one byte past each invalid instruction."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    out = []
    off = 0
    while off < len(code):
        produced = 0
        for ins in md.disasm(code[off:], addr + off):
            out.append(ins)
            produced += ins.size
        off += produced + 1 if produced == 0 else produced
    return out


def main():
    code, text_va, base = load_text(MODULE)
    target = base + SYSCALL_RVA
    insns = disasm_all(code, text_va)
    print(f"{MODULE.name}: {len(insns)} instructions recovered from .text "
          f"({len(code)} bytes)")
    print(f"syscall pointer global: 0x{target:08X}\n")

    needle = f"0x{target:x}"
    results = defaultdict(list)
    call_sites = 0

    for n, ins in enumerate(insns):
        if ins.mnemonic != "call" or needle not in ins.op_str.lower():
            continue
        call_sites += 1

        # Walk back for the pushed syscall number.
        number = None
        for k in range(n - 1, max(-1, n - 8), -1):
            p = insns[k]
            if p.mnemonic == "push":
                try:
                    number = int(p.op_str.strip(), 0)
                except ValueError:
                    number = None
                break
            if p.mnemonic == "call":
                break

        # Walk forward for the stack cleanup.
        cleanup = None
        for k in range(n + 1, min(len(insns), n + 4)):
            a = insns[k]
            if a.mnemonic == "add" and a.op_str.replace(" ", "").startswith("esp,"):
                try:
                    cleanup = int(a.op_str.split(",")[1].strip(), 0)
                except ValueError:
                    pass
                break
            if a.mnemonic in ("ret", "call", "jmp"):
                break

        if number is not None:
            results[number].append(cleanup)

    print(f"indirect calls through the syscall pointer: {call_sites}")
    print(f"distinct syscall numbers resolved: {len(results)}\n")

    for cand in CANDIDATES:
        print(f"=== syscall {cand} ===")
        hits = results.get(cand)
        if not hits:
            print("  not found at any resolvable call site\n")
            continue
        for c in hits:
            args = (c // 4 - 1) if c else None
            print(f"  cleanup add esp,{c}"
                  f"{f'  -> {args} argument(s)' if args is not None else '  -> unknown'}")
        print()

    print("=== VERDICT ===")
    verdict = None
    for cand in CANDIDATES:
        vals = {c for c in results.get(cand, []) if c}
        if vals == {8}:
            print(f"  syscall {cand}: ONE argument -> receives the refdef -> R_RenderScene")
            verdict = cand
        elif vals == {4}:
            print(f"  syscall {cand}: NO arguments -> not the render call")
            print(f"                  (the refdef we sampled at runtime was stale stack)")
        elif vals:
            print(f"  syscall {cand}: inconsistent cleanups {sorted(vals)}")
        else:
            print(f"  syscall {cand}: undetermined")

    print()
    if verdict:
        print(f"  => renderSyscall {verdict}")
    else:
        print("  => inconclusive; runtime auto-detection decides")

    # Context: what else is called once per frame, for sanity.
    common = sorted(results.items(), key=lambda kv: -len(kv[1]))[:8]
    print("\nmost-referenced syscalls in cgame (number: call sites):")
    for num, lst in common:
        print(f"  {num:4d}: {len(lst)}")


if __name__ == "__main__":
    main()
```
