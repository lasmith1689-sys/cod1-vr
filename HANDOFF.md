# Call of Duty 1 (2003) → Quest 3 VR — Handoff

**Goal:** a seamless, native-feeling PCVR experience for the original *Call of
Duty* on a Quest 3 via Virtual Desktop. Single-player only, user's own Steam copy,
nothing redistributed.

User-facing documentation is in [README.md](README.md). This file is the
engineering handoff.

---

## Status (2026-08-17, end of session)

**Confirmed working on hardware:**
- Proxy `opengl32.dll` loads inside the DRM-packed process; all 368 exports forward.
- OpenXR session on VirtualDesktopXR; stereo image reaches the headset.
- **Soldiers and weapon appear in BOTH eyes** — the headline bug, fixed by the
  alternating-eye architecture.
- Camera control is proven: the probe's authority test measured a **483×**
  response to a camera displacement, so refdef writes genuinely drive the engine.

**Built and unit-tested, NOT yet seen on hardware:**
- Zoom fix (the last run was hugely zoomed; cause found and fixed — see below)
- Supersampled offscreen rendering, HUD aspect correction, HUD depth
- Aim model (mouse pitch now reaches the view)
- Comfort vignette (aperture-based), recenter, snap turn, frame-rate watch
- Square 1440×1440 render buffer

**92 unit tests pass**, roughly a third exercising real OpenGL through a harness
that creates a live GL context. The one thing that cannot be verified here is the
actual headset: the game cannot be launched by script (Steam DRM) and needs
Virtual Desktop connected from inside the Quest.

**Issues found and fixed by self-review after the last hardware run** (an
adversarial audit workflow was attempted three times and never produced usable
output — the agents stalled or read files mid-edit; the review below was done by
hand):
1. `BlitToEye` read the game's rectangle out of a headset-sized texture — the
   zoom bug. Parameters removed entirely so it cannot recur.
2. `g_hudPhase` defaulted false, so menus and loading screens — which never
   issue a render call — got no aspect correction on the square buffer.
3. The vignette faded a fixed mask instead of shrinking the aperture. Research
   on field-of-view restrictors is clear that closing the opening is what works;
   rebuilt as procedural ring geometry at the published 0.25/0.65 radii.
4. The vignette restored whether blending was enabled but not the blend
   *function*, nor the shade model — either would have corrupted the engine's
   own drawing.
5. Flat screens were letterboxed twice (once into the HUD band, once into the
   eye), making menus needlessly small. Capture now reads just the band.

---

## The bug fixed at the end of the session

The last hardware run was "way too zoomed in", the menu was a fragment in a
corner, and head motion felt wrong. One cause for all three:

`BlitToEye` copied from the eye texture using **the game's back-buffer
rectangle** as the source — `src 0,0..1440,1431` out of a `3072×3264` texture —
and stretched that corner over the whole eye. The parameters had been marked
unused when the function was simplified, but the blit call still used them.

Fixed, and the parameters were removed entirely so there is nothing left to pass
wrongly. The log line `first blit - src 0,0..W,H -> ...` now reports the eye
texture's own size; if those ever disagree again, that line says so.

---

## Architecture

```
CoDSP.exe (32-bit, Steam-DRM packed)
   ├── opengl32.dll  ← OUR PROXY (not a KnownDLL, so this works)
   │      ├── forwards 368 exports to the real driver
   │      ├── intercepts glViewport/glScissor  → scaled into the offscreen target
   │      ├── intercepts glDrawBuffer/glReadBuffer → GL_BACK is illegal on an FBO
   │      ├── intercepts wglGetProcAddress → forces vsync off
   │      └── intercepts wglSwapBuffers → the whole VR frame
   └── cgamex86.dll (NOT packed, keeps Quake 3's module ABI)
          └── its syscall pointer is redirected through our hook (the camera)
```

**Per frame:**
1. `wglSwapBuffers`: unbind the offscreen target, mirror to the window, capture
   the finished frame into **this frame's eye texture**, draw the comfort
   vignette on it, blit both eye textures into their swapchains, submit.
2. Poll comfort keys, advance the vignette, `xrWaitFrame`/`xrBeginFrame`/
   `xrLocateViews`, then bind the offscreen target for the next frame.
3. The engine builds its scene; entity adds pass through our hook.
4. Render syscall (**71**): we write the eye's camera and FOV into the refdef and
   let the engine draw one complete scene. Everything after that call is the 2D
   pass, flagged so the HUD gets its aspect correction and per-eye depth nudge.

**Files**

| Path | Role |
|---|---|
| `src/dllmain.cpp` | proxy entry, syscall hook, GL interception, frame orchestration, self test |
| `src/vr.cpp` / `vr.h` | all OpenXR + the GL present path (capture, blit, submit, camera) |
| `src/comfort.cpp` | vignette, recenter, snap turn, key parsing |
| `src/probe.cpp` | the headset-free diagnostic |
| `src/codvr.h` | shared internals between the above |
| `src/gl_thunks.inc`, `src/opengl32.def` | **GENERATED** by `tools/gen_gl_proxy.py` — never hand-edit |
| `tools/selftest_host.cpp` | test host; creates a real GL context before loading the DLL |
| `tools/find_render_syscall.py` | x86 arity analysis (how syscall 71 and the entity arities were found) |

---

## Design decisions worth not re-litigating

**Alternating eyes, not two renders per frame.** `RE_RenderScene` consumes the
entity list, so a second render in the same frame draws no soldiers and no
weapon. Replaying the scene-building syscalls to refill it was tried twice,
never worked, and once crashed the game to desktop. Alternating sidesteps the
problem entirely: every frame is an ordinary complete scene. It also doubles the
width each eye gets and gives each eye a whole HUD pass. The cost is that each
eye updates at half the game's frame rate, which is irrelevant because
`com_maxfps` is 0 and the game runs into the hundreds.

**Square render buffer.** Each eye is ~3072×3264. A 16:9 buffer wastes its
pixels horizontally and starves the vertical. 1440×1440 at 2.25× supersample is
3240×3240 — almost exactly one eye, at *fewer* total pixels than the old 16:9
setup. The HUD is remapped into a 16:9 band so it keeps its intended proportions.

**Symmetric superset + sub-rectangle for FOV.** The runtime frustum is asymmetric
(measured L−40 R+54 U+44 D−55). The refdef can only express a symmetric FOV, so
we render the smallest symmetric frustum containing the real one and submit the
sub-rectangle that matches it, tagged with the true angles.

---

## Traps — verified the hard way

1. **`glBlitFramebuffer` and `glClear` are both subject to the scissor test.**
   The engine leaves its own scissor box set. This silently clipped every blit
   and cost a whole session ("top half of the screen missing"). Proven against a
   real driver by `VR_TestBlitScissorBehaviour`. Always disable and restore it.
2. **`glDrawBuffer(GL_BACK)` is illegal while an FBO is bound** and silently
   drops all rendering. The engine calls it every frame; it must be translated.
3. **Check syscall arity before re-issuing anything.** Replaying a 4-argument
   syscall with zeros crashed the game on mission load.
   `tools/find_render_syscall.py` answers this offline, for free.
4. **CoD's entity struct is NOT Quake 3's.** It is `float origin[3]` at +0 and an
   orthonormal `axis[9]` at +12, carried by syscalls 37 and 38. A classifier
   written for Q3's `reType`-at-0 matched zero of 181 captures.
5. **Do not raise `REPLAY_SNAP`.** Capture is gated on the whole snapshot being
   readable, so a larger one silently *rejects* entities near a region boundary.
6. **`VR_Shutdown()` only when `DllMain`'s `lpReserved == NULL`.** On process
   termination, calling into the delay-loaded OpenXR loader can fault or hang.
7. **The runtime can return a garbage head pose** — one run reported y = −1592 m.
   Sanity-gate it and clamp the delta.
8. **Do not run a code-audit workflow while editing the files.** The agents read
   mid-edit and produce nonsense.
9. **When cutting a block out of `dllmain.cpp`, check what is between the section
   markers.** The render hook sits between the probe's globals and its scoring
   code; a naive line-range delete takes it with them.

---

## What is left

- **Hardware validation of everything built after the last run.** Nothing below
  matters until the current build has been seen in the headset.
- **6DOF / motion controllers** — explicitly out of scope for now. This is the
  large remaining feature: controller input plumbing, per-hand poses, weapon
  attachment to the dominant hand, and a real aiming model to replace the
  mouse-plus-free-look one.
- **Head-aimed weapon** as an option: drive the game's aim toward the head with
  injected mouse movement, so the weapon follows where you look. The plumbing for
  it exists (`VR_GetHeadAimOffset`, and snap turn already injects mouse), but it
  needs a damping model and hardware testing to avoid a feedback loop.
- **Roomscale positional movement** beyond leaning — currently clamped to 2 m.
- **`renderSyscall` config is still decoupled** from the hardcoded 71/243 in the
  assembly interposer; changing it in the config silently disables VR.
- **The self test lives in `dllmain.cpp`** on purpose: it tests that file's
  internals, and moving it would mean exporting them purely for testing.

---

## Build

```powershell
cd "H:\Projects\COD VR"
.\build.ps1 -SelfTest -Deploy
```

Regenerates the export thunks, compiles x86, rebuilds the test host, runs 87
tests, and deploys. `-SelfTest` needs no game and no headset — it is the fast
loop, and roughly a third of the tests drive a real OpenGL context.

Logs from every hardware run so far are in `logs/`.
