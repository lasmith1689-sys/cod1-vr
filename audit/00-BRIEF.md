# Audit request: Call of Duty (2003) → Quest 3 VR mod

You are reviewing a VR mod for the original *Call of Duty*. I want an
adversarial technical review: architecture critique, bug-hunting in the code,
and a recommended plan for what remains. Be blunt. If a core decision is wrong,
say so and say what to do instead.

**The single most valuable thing you can give me: bugs I cannot find myself,
because I cannot run the thing.** See "The hard constraint" below.

---

## 1. What it is

A mod that turns *Call of Duty* (2003, closed-source, id Tech 3–derived engine)
into a PCVR experience on a Quest 3 over Virtual Desktop. Single-player only,
the user's own Steam copy, nothing redistributed or patched on disk.

It works as a **proxy `opengl32.dll`**. The game loads it instead of the system
one; it forwards all 368 exports to the real driver while intercepting a handful
to take over the frame, and separately hooks the game's own script-VM syscall
pointer to take over the camera.

Target: something that feels native, not a tech demo. Comfort options included.
Motion controllers / 6DOF hand tracking are explicitly out of scope for now.

---

## 2. The hard constraint (this shapes everything)

**I cannot launch the game.** Steam's DRM refuses every scripted launch path
(direct exec, `-applaunch`, `steam://`, `steam_appid.txt`), and the mod needs
Virtual Desktop connected *from inside the headset*. Only the human user can
start it, wearing the headset.

Consequences:
- Every hardware run is expensive and scarce. The user has done ~6 total.
- All instrumentation must be **self-scoring**: one run must produce a
  conclusive written verdict in a log, not "try it and tell me what you saw."
- I built a **headless test harness** that creates a real OpenGL context on a
  hidden window before loading the DLL, so the GL paths can be exercised at
  build time. 92 unit tests pass, roughly a third driving real GL.
- **Everything in section 6 is unvalidated on hardware.** Treat claims about it
  as "believed correct", not "known correct".

**So: prioritise findings that a unit test on a hidden GL window would NOT
catch.** Frame-order bugs, coordinate-space mismatches, VR comfort/perception
problems, OpenXR protocol misuse, and engine-behaviour assumptions.

---

## 3. Established facts (measured, not assumed — please don't re-litigate)

| Fact | How it was established |
|---|---|
| `CoDSP.exe` is Steam-DRM packed; useless statically | entropy analysis |
| `cgamex86.dll` is NOT packed, keeps Quake 3's module ABI (`vmMain`/`dllEntry`) | static analysis |
| The syscall pointer global is at RVA `0x657E8`, re-derived at runtime from `dllEntry`'s bytes | module has loaded at 3+ different bases |
| **Syscall 71 = `R_RenderScene`** | x86 call-site arity (only candidate with consistent `add esp,8`) + frame trace |
| The refdef struct is **byte-identical to Quake 3's `refdef_t`** | ints x,y,w,h @0; fov_x/y @16/20; vieworg @24; orthonormal viewaxis @36 |
| **Writing that refdef genuinely drives the renderer** | a self-scoring probe teleported the camera on alternate frames and measured a **483×** change vs ordinary motion |
| CoD's entity struct is **NOT** Quake 3's: `float origin[3]` @0, orthonormal `axis[9]` @12 | raw byte dump; origin matched the player's eye position exactly |
| Entity adds are carried by syscalls **37 (2 args)** and **38 (4 args)** | arity analysis + runtime capture |
| The runtime frustum is **strongly asymmetric**: L−40° R+54° U+44° D−55° | logged from `xrLocateViews` |
| Per-eye swapchain is **3072×3264** (nearly square) | logged from the runtime |
| `com_maxfps 0` and `r_swapInterval 0` were already set | read from the game's config |

**Two OpenGL facts that each cost a wasted hardware session:**
1. `glBlitFramebuffer` **and** `glClear` are subject to the scissor test. The
   engine leaves its own scissor box set, which silently clipped every blit to
   the game's rectangle. Symptom: "top half of the screen is missing". Proven
   against a real driver by a dedicated test.
2. `glDrawBuffer(GL_BACK)` is **illegal while an FBO is bound** and silently
   drops all rendering. The engine issues it every frame.

---

## 4. Architecture

### 4.1 Frame flow

```
wglSwapBuffers hook (our code):
  1. unbind the offscreen render target
  2. mirror it to the game window (monitor preview)
  3. capture the finished frame into THIS FRAME'S EYE texture
  4. draw the comfort vignette onto that eye texture
  5. blit both eye textures into their OpenXR swapchain images, submit layer
  6. flip which eye the next frame belongs to
  7. poll comfort keys, advance vignette, note frame rate
  8. xrWaitFrame / xrBeginFrame / xrLocateViews  (poses for the NEXT frame)
  9. bind the offscreen target for the next frame
  10. call the real wglSwapBuffers

engine renders the next frame into the offscreen target:
  - scene-building syscalls pass through our hook
  - syscall 71 (R_RenderScene): we overwrite the refdef's camera + FOV with
    this frame's eye, let the engine draw ONE ordinary scene
  - everything after that call is the 2D/HUD pass (flagged)
```

### 4.2 Key decision: ALTERNATING EYES

`RE_RenderScene` **consumes the entity list** (`r_firstSceneEntity =
r_numentities` at the end), so rendering a second eye in the same frame draws
world geometry with **zero entities** — no soldiers, no weapon.

I tried the obvious fix twice: capture the scene-building syscalls and replay
them for the second eye. It never worked (first attempt matched zero entities
due to the Q3-vs-CoD struct layout; second crashed the game to desktop by
replaying a 4-argument syscall with zeros in the extra slots). 

**Current approach: each frame renders ONE eye across the whole buffer, frames
alternate, and both eyes are submitted every frame** — one fresh, one from the
previous frame, each tagged with the pose it was actually rendered at.

Benefits: no entity problem at all (every frame is an ordinary complete scene
the engine builds itself), double the horizontal resolution per eye, and each
eye gets a complete HUD pass instead of half of one.

Cost: each eye refreshes at half the game's frame rate. Research says this
technique (used by the GTA V "R.E.A.L." mod and vorpX) "falls apart completely
with any framerate below the headset refresh rate". `com_maxfps` is 0 and this
is a 2003 game, so the headroom should be large — but supersampling eats into
it. The mod logs frame rate and warns below 150 fps.

**→ Question for you: is alternating-eye the right call here, or is there a
better way to defeat the entity-list consumption that I have missed?**

### 4.3 Key decision: SQUARE RENDER BUFFER + SUPERSAMPLING

Each eye is 3072×3264 — nearly square. A 16:9 buffer spends its pixels on
horizontal detail the eye cannot use and starves the vertical, which is what
made the image look soft.

The game is therefore configured to **1440×1440 windowed** (`r_mode -1`,
`r_customwidth/height 1440`, `r_fullscreen 0`), and rendering is redirected into
an **offscreen FBO at 2.25× = 3240×3240**, with the engine's `glViewport` and
`glScissor` calls intercepted and scaled by the same factor. The engine believes
it is drawing at 1440×1440; its pixels are simply larger. The 2D layer scales
with it, so the HUD sharpens too.

Windowed because 1440×1440 is not a real display mode.

**→ Question: is scaling the engine's viewport/scissor a sound way to
supersample a fixed-function GL 1.x engine, or will it break things I have not
thought of (readbacks, sub-viewport effects, anything that queries the viewport)?**

### 4.4 Key decision: SYMMETRIC SUPERSET + SUB-RECTANGLE for FOV

The refdef can only express a **symmetric** FOV, but the headset frustum is
asymmetric (L−40 R+54 U+44 D−55). So:

- render the smallest symmetric frustum that **contains** the real one
  (±54 × ±55, written into the refdef as `fov 108 × 110`)
- at submit time, hand the compositor the **sub-rectangle** of the eye image
  corresponding to the true asymmetric frustum, tagged with the true angles

Measured result from the log: `submitting rect 600,0 2472x2736 of 3072x3264`.
Verified by hand: `u0 = (tan(−40°)+tan(54°)) / (2·tan(54°)) = 0.195`, and
`600/3072 = 0.195`. ✓

**→ Question: is this correct OpenXR usage? Anything about `subImage.imageRect`
+ per-view `fov` that I am getting wrong? Roughly 33% of rendered pixels are
discarded as a result — is there a cheaper formulation?**

### 4.5 Key decision: AIM MODEL (no controllers)

Originally only the game's **yaw** was kept and the headset owned pitch/roll
entirely — so mouse pitch moved the weapon but never the view.

Now the head's rotation is **composed onto the game's full view basis**:
`out = ComposeAxis(headLocal, gameAxis)`, where `headLocal` is the head
orientation with its startup yaw cancelled. With the head centred you look
exactly down the gun; turning your head is free-look on top of the mouse aim.

Head *position* is measured relative to a neutral point latched at startup
(re-latched by recenter), sanity-gated (one real run returned a head position of
**−1592 metres**), and clamped to 2 m.

**→ Question: is this the right model for a no-controller VR shooter? The
alternative is head-aiming — inject relative mouse movement to steer the game's
aim toward the head direction, so the weapon follows your gaze. The plumbing
exists (snap turn already injects mouse via `SendInput`). Which gives a better
experience, and what are the failure modes of the injection approach?**

### 4.6 Comfort

- **Tunnelling vignette** keyed to the player's *virtual* motion (translation
  speed and turn rate measured from the game's own camera between frames, not
  from the head — physically turning your head does not cause sickness). Drawn
  as procedural ring geometry (no texture, no shaders — fixed function only),
  shrinking the **aperture radius** from off-screen down to 0.25 inner / 0.65
  outer, which is what the field-of-view-restrictor literature describes.
  Opens more slowly than it closes.
- **Recenter** on F9.
- **Snap turn**, optional, via injected mouse movement.
- **Frame-rate watch** that warns in the log when alternating-eye rendering
  would start to show.

**→ Question: are the defaults sensible? Anything important missing from a
comfort standpoint?**

---

## 5. What is confirmed working on hardware

From the user's actual runs:
- The proxy loads inside the DRM-packed process; all 368 exports forward.
- OpenXR session comes up on VirtualDesktopXR; stereo reaches the headset.
- **Soldiers and the weapon appear in BOTH eyes** — the headline bug, fixed by
  alternating eyes.
- Camera control is proven (the 483× probe result).
- The game is playable; the user completed a mission.

---

## 6. What is NOT validated on hardware (built after the last run)

Everything here passes unit tests and my own review, and nothing more:

1. The **zoom fix** (see section 7).
2. Supersampled offscreen rendering (the whole of 4.3).
3. HUD aspect correction and HUD depth separation.
4. The aim model (4.5) — mouse pitch reaching the view.
5. All comfort features (4.6).
6. The square 1440×1440 buffer.
7. Guarded `VR_Shutdown` (only when `DllMain`'s `lpReserved == NULL`, because on
   process termination calling into the delay-loaded OpenXR loader can fault).

---

## 7. The last hardware run and the bug it exposed

User's report: *"wayyyy toooo zoomed in. head seems to move weird as well. menu
is just a quarter of the screen. in game video seems to go fullscreen though. I
can see the guns and guys in both eyes."*

The log line that explained all of it:

```
VR: first blit - dst FBO status 0x8CD5 (COMPLETE), src 0,0..1440,1431 -> 3072x3264
```

`BlitToEye` was using the **game's back-buffer rectangle** as the blit source
while reading from an eye texture sized to the **headset** — taking a 1440×1431
corner out of a 3072×3264 texture and stretching it over the whole eye. I had
simplified the function and marked its parameters unused with `(void)`, but the
blit call still used them.

Fixed by removing the parameters entirely.

**→ Question: what else of this class is still in there?** Full source is in the
bundle.

---

## 8. Self-review findings after that run

An adversarial multi-agent audit was attempted three times and never produced
usable output (agents stalled or read files mid-edit), so I reviewed by hand and
found five more real defects:

1. `g_hudPhase` defaulted false, so menus and loading screens — which never
   issue a render call — got no aspect correction on the square buffer.
2. The vignette faded a fixed mask rather than shrinking the aperture (weaker,
   and not what the research supports). Rebuilt.
3. The vignette restored *whether* blending was enabled but not the blend
   **function**, nor the **shade model** — either would corrupt the engine's
   own subsequent drawing.
4. Flat screens were letterboxed twice (into the HUD band, then into the eye),
   making menus needlessly small.
5. The monitor mirror used the engine's render size rather than the window's
   real client area.

---

## 9. What I want from you

In rough priority order:

1. **Bugs.** Especially in the frame ordering, the coordinate-space chain
   (game pixels → supersampled → eye texture → swapchain sub-rect → compositor
   FOV), OpenXR usage, and GL state handling. Anything a headless test would
   miss. Cite file and function.
2. **Architecture challenges.** Sections 4.2–4.5 each end in a question. If you
   think a decision is wrong, say what to do instead and why.
3. **VR feel/perception.** Things that will be subtly wrong in a headset that I
   cannot reason my way to: the alternating-eye pose handling, the HUD depth
   approach, world scale, the comfort defaults.
4. **The path to 6DOF / motion controllers.** This is the big remaining feature.
   Given the constraints (closed engine, syscall-level hook, no source), what is
   the sanest route to controller-driven weapon aiming? What would you hook?
5. **Anything I have convinced myself of that is actually wrong.** I have been
   wrong at least four times in this project — I blamed FOV for a scissor bug
   for three sessions, and I nearly concluded the whole camera architecture was
   broken when it was fine. Assume similar blind spots remain.

---

## 10. Contents of this package

| File | What it is |
|---|---|
| `00-BRIEF.md` | this document |
| `01-source-bundle.md` | all mod source, concatenated with file headers |
| `02-log-last-hardware-run.txt` | the most recent real run (the zoomed one) |
| `03-log-camera-authority-probe.txt` | the flat probe that proved camera control (483×) |
| `04-log-entity-struct-dump.txt` | the run that recovered CoD's entity layout |
| `05-config-and-build.md` | user config, game video config, build script |

Source layout, for orientation:

| File | Lines | Role |
|---|---|---|
| `src/dllmain.cpp` | ~1900 | proxy entry, syscall hook, GL interception, frame orchestration, self-test |
| `src/vr.cpp` | ~1490 | all OpenXR + the GL present path (capture, blit, submit, camera) |
| `src/comfort.cpp` | ~530 | vignette, recenter, snap turn, frame-rate watch |
| `src/probe.cpp` | ~410 | the headset-free self-scoring diagnostic |
| `src/codvr.h`, `src/vr.h` | ~240 | interfaces |
| `src/gl_thunks.inc`, `src/opengl32.def` | generated | 368 export thunks — not included, machine-generated |
