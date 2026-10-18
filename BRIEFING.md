# Call of Duty 1 (2003) → Quest 3 VR — Full Project Briefing

**Audience:** a coordinating agent picking this up cold, with no prior conversation context.
**Date of this snapshot:** 2026-08-17.
**Repo root:** `H:\Projects\COD VR` (not a git repo — no version control, edits are live).

---

## 1. What this project is

Turn the original **Call of Duty (2003)**, a closed-source Windows game, into a *seamless,
native-feeling* PCVR experience on a **Quest 3**, streamed over **Virtual Desktop**.
Single-player only. The user owns the game on Steam. Nothing is redistributed.

There is no engine source and no open reimplementation. The entire mod is therefore an
**injection mod**: a proxy `opengl32.dll` that the game loads instead of the system one, which
then drives OpenXR and rewrites the game's camera per eye.

### Scope decision that must not be reversed

An earlier task — *dump the Steam-DRM-packed `CoDSP.exe` from memory to recover the full syscall
table by disassembling it* — was **deliberately dropped on 2026-08-17 and its code deleted.**

Two reasons, both still valid:
1. **It was never a blocker.** A captured runtime frame trace plus static analysis of the
   *unpacked* `cgamex86.dll` already yield everything the dump would have.
2. **It is the one part of this project that pattern-matches to DRM circumvention / malware
   unpacking**, regardless of the benign intent. Working on it repeatedly tripped model safety
   guardrails and stalled sessions.

**Coordinator instruction:** do not re-introduce memory-image dumping, DRM defeat, or anything
framed as unpacking a protected binary. Keep all work in the ordinary graphics/VR-engineering
lane. Everything remaining genuinely is ordinary graphics work.

---

## 2. Current state, in one paragraph

The mod loads, hooks the game's camera, brings up an OpenXR session on VirtualDesktopXR, and
puts a stereo image on the headset with correct head tracking. The headline remaining defect is
that **entities (soldiers, the player's weapon) appear in only one eye**, because the engine
consumes its entity list on the first render. A **scene-replay system that fixes this was
implemented, unit-tested, and deployed today, but has never been run on hardware.** The immediate
next action is a single validation run by the user, which will self-report its result into a log.

---

## 3. Environment

| Thing | Value |
|---|---|
| Game | `H:\SteamLibrary\steamapps\common\Call of Duty` (Steam app 2620, build 1.3, win-x86) |
| Headset | Meta Quest 3 via Virtual Desktop (VDXR runtime) |
| Host OS | Windows 11 Pro 26200 |
| Toolchain | VS Build Tools 2026 x86 (`cl` 19.50), bundled CMake, Python 3.14 |
| OpenXR | Loader 1.0.10 (nuget), vendored at `third_party/openxr/` |
| Everything is **32-bit** | the game is a 32-bit process, so the loader and DLL must be too |

---

## 4. How the mod works (architecture)

```
CoDSP.exe (32-bit, Steam-DRM packed)
   │
   ├── loads opengl32.dll  ← OUR PROXY (not a KnownDLL, so this works)
   │      ├── forwards all 368 exports to the real C:\WINDOWS\system32\opengl32.dll
   │      ├── intercepts wglSwapBuffers  → per-frame VR orchestration
   │      └── drives OpenXR (session, swapchains, per-eye submit)
   │
   └── loads cgamex86.dll (NOT packed, keeps Quake 3 module ABI)
          └── its engine-syscall function pointer is redirected through our hook,
              which is how we see and rewrite the camera
```

**Per-frame flow:**
1. `wglSwapBuffers` (our hook) — submit last frame to the compositor, then `xrWaitFrame` /
   `xrBeginFrame` / `xrLocateViews` to get fresh head poses for the frame about to be built.
2. The game builds its scene, issuing many syscalls through our hook. We snapshot entity
   additions as they go by.
3. The game calls the render syscall (**71**) with a `refdef` (camera) struct. Our hook:
   - saves the original struct,
   - for eye 0: rewrites viewport to the left half + eye-0 camera, calls the real renderer,
   - **replays the captured entity additions** (the engine emptied its entity list),
   - for eye 1: rewrites viewport to the right half + eye-1 camera, calls the real renderer,
   - restores the struct exactly.
4. Back in `wglSwapBuffers`, each half of the back buffer is blitted into its eye's OpenXR
   swapchain image and submitted as a projection layer.

**Coordinate systems** (the easiest thing to get wrong; already verified correct by hand):
- OpenXR: right-handed, +X right, +Y up, **−Z forward**, metres.
- Quake/idTech3: **+X forward, +Y left, +Z up**, ~39.37 units per metre.
- Conversion: `XrToQuake(v) = (-v.z, -v.x, v.y)`. Lives in one place, `vr.cpp`.

---

## 5. What works / what is broken

**Working:**
- Proxy DLL loads inside the DRM-packed process; all 368 exports forward correctly.
- cgame syscall pointer hooked; address re-derived at runtime (module relocates between runs).
- OpenXR session comes up on VirtualDesktopXR; compositor takes the headset; audio fine.
- Stereo image reaches the headset; menus and loading screens display as a centred panel.
- Head *look* direction is correct (yaw-left→left, pitch-up→up), verified by hand computation
  and pinned by unit tests.

**Broken / not native yet:**

| Symptom | Cause | Status |
|---|---|---|
| Soldiers + weapon in ONE eye | engine consumes entity list on first render | **fix deployed, needs validation run** |
| Image reads dark | swapchain was sRGB, back buffer is linear | **fixed today, needs validation run** |
| HUD split across both eyes | HUD drawn once across the full framebuffer | open — task 3 |
| Soft / blurry | each eye renders 1280×1440, upscaled to 3072×3264 | open — task 4 |
| Mouse pitch does nothing to view | only the game's yaw is kept; head owns pitch/roll | open — task 5 |

---

## 6. The scene-replay system (just built — the thing to validate)

**The problem.** idTech3's `RE_RenderScene` ends with `r_firstSceneEntity = r_numentities`,
i.e. it *consumes* the entity list. Calling it a second time for the right eye therefore renders
world geometry with **zero entities** — no soldiers, no weapon. Exactly the observed symptom.

**Why the obvious references don't apply.** RTCWQuest solves this from *inside* the engine
(`vr_stereoReplay` captures the command buffer and patches `drawSurfsCommand_t` per eye). We
cannot touch the command buffer — closed engine. ioq3quest's multiview approach needs GLSL and
`GL_OVR_multiview2`; CoD1 is fixed-function OpenGL 1.x. So we replay at the **syscall layer**.

**How ours works** (`src/dllmain.cpp`):
- As each entity-add candidate syscall passes through the hook, we **snapshot the struct it
  points at** (256 bytes). The snapshot is mandatory — the caller's pointer is transient and that
  memory is reused by the time we replay.
- Between the two eye renders, `ReplayCapturedEntities()` re-issues the captures. Because the
  engine already set `firstSceneEntity == numentities`, re-adding appends a fresh batch that the
  second render consumes. **No ClearScene needed.**
- **Entities are identified by argument *shape*, not by a hardcoded syscall number:** a candidate
  is replayed only if its snapshot contains an orthonormal 3×3 axis *and* an in-range `reType`
  int at offset 0. This self-discovers which syscall is the real `AddRefEntityToScene`, and is
  **crash-safe**: the engine calls `ri.Error(ERR_DROP)` on a bad `reType`, so anything that
  doesn't clearly look like an entity is skipped rather than replayed. This is also why being
  generous with the candidate set is safe.
- Captures are scoped to **one scene, not one frame** (RAII guard clears on every exit path from
  the render hook). The engine can render more than one scene per frame — portal/skybox or a
  separate viewmodel pass — and without per-scene scoping the second scene would replay the
  first scene's entities on top of its own.
- Only **entities** are replayed. World/BSP geometry is re-walked from the refdef on every render
  and needs no replay. **Polys and dynamic lights are a known follow-up** — they carry
  variable-size pointer arguments and would need their secondary buffers snapshotted too
  (affects decals/marks and muzzle-flash lighting in the second eye).

**Default capture set:** `{7, 34, 37, 38, 41, 42, 43, 86, 88}`, taken from the pre-render bulk in
the captured frame trace.

---

## 7. THE CRITICAL WORKFLOW CONSTRAINT

**The game cannot be launched programmatically.** Direct exec, `SteamAppId` env var,
`steam_appid.txt`, `steam://rungameid`, and `steam.exe -applaunch` all fail with a modal
"Failed to find Steam". **The user must start it by hand from the Steam UI**, with Virtual
Desktop already connected from inside the headset.

Consequences that shape every decision:
- **The user's play sessions are the scarce resource.** Do not treat them as a manual test loop.
- **All instrumentation must be self-scoring.** One run must yield a conclusive answer, written
  to a log, not a "try this and tell me what you see" ping-pong.
- Prefer config keys over rebuilds where a next-run adjustment is plausible.
- Keep responses to the user short.

---

## 8. What the next run must produce

User launches from Steam with Virtual Desktop connected in-headset, plays ~30s of a mission,
quits. Log lands at `<game folder>\codvr_result.log`.

Expect a one-shot block on the first VR frame:

```
VR replay: N captured, M re-issued to the second eye
  syscall  34: 120 captured, 118 entity-shaped
  syscall  42:  60 captured, 0 entity-shaped (skipped)
  ...
```

**Reading the result:**

| Log shows | Means | Next action |
|---|---|---|
| `M` > 0, entities in both eyes | replay works | proceed to task 3 (per-eye HUD) |
| `M` = 0, every id `(skipped)` | `reType` is not at offset 0 in CoD's refEntity | find real offset, adjust `ReplayIsEntity` |
| `M` > 0 but weapon still one-eyed | weapon uses an add we don't capture | add that id via `replayAdd` in `codvr.cfg` |
| no `VR replay:` line at all | no scene rendered, or VR never activated | read the `VR:` lines above it |

Also confirm: `VR: per-eye ... swapchain format 0x8058` (linear RGBA8). If it still says `0x8C43`
the sRGB fix did not take and the image will look dark.

---

## 9. Next tasks, in order

1. **Validate the replay** (above). Blocking everything else.
2. **Extend replay to polys + dynamic lights** — needs snapshotting of the secondary
   (vertex/point) buffers those calls point at. Restores decals and muzzle-flash lighting in the
   second eye.
3. **Per-eye HUD.** The 2D/HUD block (syscall 220 group, ~10,700 calls, all *after* the render
   call) is drawn once across the whole framebuffer, so each eye currently gets half of it.
   Either replay that block per eye, or blit the HUD region into both eyes as an overlay.
4. **Offscreen FBO per eye at native resolution.** Today each eye renders into half of a
   2560-wide back buffer (1280×1440) and is upscaled to 3072×3264 — hence the softness. Render
   each eye into its own full-res target instead; bind before the render call.
5. **Aim model.** Currently the game's yaw is kept and the headset supplies pitch/roll, so mouse
   pitch never reaches the view. Target: view follows mouse yaw+pitch with head adding a free-look
   delta on top. True 6DOF motion-controller aiming is the endgame and is a separate large effort
   (needs controller input plumbing, which does not exist yet).

---

## 10. Verified technical facts — do NOT re-derive

- `CoDSP.exe` is **Steam-DRM packed** (2 sections, `.text` entropy 8.00, 600 KB on disk expanding
  to ~21 MB). Static analysis of it is worthless. *(Do not attempt to unpack — see §1.)*
- **`cgamex86.dll` is NOT packed** and keeps Quake 3's module ABI (`vmMain`, `dllEntry`). This is
  why the project is feasible: the camera lives in a statically analysable DLL.
- `dllEntry` is `mov eax,[esp+4] / mov [global],eax / ret`. The syscall-pointer global sits at
  **RVA 0x657E8**, but **always re-derive it from those bytes at runtime** — the module has loaded
  at three different bases across runs (0x30000000, 0x09480000, 0x41AE0000).
- The **refdef is byte-identical to Quake 3's `refdef_t`**: ints x,y,w,h at +0; floats `fov_x` +16,
  `fov_y` +20; `vieworg` +24; `viewaxis` (orthonormal 3×3) +36; minimum 72 bytes.
- **Syscall 71 is `R_RenderScene`** — confirmed by x86 call-site arity analysis (it is the only
  candidate whose sites consistently clean up 8 bytes, i.e. exactly one argument) plus its
  position in the frame trace. Syscall 243 is a different once-per-frame refdef consumer.
- **Frame order** (from a real captured trace, 11,395 calls): bulk entity/poly adds
  (7, 86, 34, 38, 42, 43, 41, 37…) → 87, 88 → **71 (RenderScene)** → ~10,750 calls of syscall 220
  plus 62/162 (the 2D HUD/world draw) → 5 → 185, 28, 29, 31, 206, 207.
- Single-player levels load with **`spmap <name>`**, not `map` (which is the multiplayer path and
  leaves you at the menu).
- 32-bit OpenXR works: VDXR ships `virtualdesktop-openxr-32.dll` registered under WOW6432Node,
  and `XR_KHR_opengl_enable` is supported, so GL textures go straight to the compositor with no
  D3D bridge.
- The headset only appears once Virtual Desktop is launched **inside the Quest** and connected;
  the PC-side Streamer alone gives `XR_ERROR_FORM_FACTOR_UNAVAILABLE`.
- Link the OpenXR loader with **`/DELAYLOAD`**. As a hard import, a missing `openxr_loader.dll`
  would stop `opengl32.dll` loading entirely and the game would refuse to start rather than
  running flat.

---

## 11. Traps — things that look like obvious fixes but are NOT

These were each identified in audit and deliberately left alone. Re-doing them naively will
cause regressions.

1. **Do not raise `REPLAY_SNAP`** (the 256-byte entity snapshot) to avoid field truncation.
   Capture is gated on `Readable(p, REPLAY_SNAP)`, so a *larger* snapshot silently **rejects**
   legitimate entities that sit near the end of a committed memory region. Missing entities is a
   worse failure than truncated ones. (Truncation risk: wrong skin/colour on replayed entities,
   still visible. Q3's `refEntity_t` is ~148 bytes, so 256 is very likely sufficient.)
2. **Do not call `VR_Shutdown()` from `DLL_PROCESS_DETACH` unguarded.** At real game exit that is
   a process-termination unwind (`lpReserved != NULL`), where calling back into the delay-loaded
   OpenXR loader can fault or hang *worse* than the passive `xrEndFrame` wait it is meant to fix.
   Needs an `lpReserved` check or a hook on the engine's own shutdown.
3. **Do not edit live C++ with PowerShell `-replace` regex.** This was done in an earlier session
   and caused real regressions — dead code, duplicated blocks, and literal `` `n `` escape
   artifacts left inside source comments (one was found and cleaned today). Use real
   read-modify-write edits.
4. **Prefer linear `GL_RGBA8` for the swapchain, never `GL_SRGB8_ALPHA8`.** The engine's back
   buffer is linear and we raw-blit those bytes across; an sRGB swapchain makes the compositor
   re-interpret them and the whole image reads dark.

---

## 12. Known code-health issues (not yet addressed)

- **`src/dllmain.cpp` is ~1,050 lines doing four jobs**: proxy plumbing, probe/diagnostics,
  syscall hooking, and VR orchestration. It should be split — at minimum lift the
  probe/self-test/phase machinery into its own file — before much more is built on top of it.
- **Config/assembly decoupling:** the naked-assembly interposer diverts two hardcoded candidate
  syscall numbers (71, 243) while the VR path keys on the configurable `renderSyscall`. Setting
  `renderSyscall` to anything else in the config silently disables VR. These should be tied
  together. Low value until someone actually needs to change it.

---

## 13. Build / deploy / run

```powershell
cd "H:\Projects\COD VR"
.\build.ps1 -SelfTest -Deploy
```

- Regenerates export thunks, compiles x86, runs **29 headless unit tests**, deploys.
- `-SelfTest` runs `build\selftest_host.exe`, which loads the DLL with `CODVR_SELFTEST=1`. **No
  game and no headset needed** — this is the fast feedback loop, use it constantly.
- Test coverage: the naked-asm interposer, refdef detection, struct save/restore, coordinate
  transform, and scene-replay capture/filter/scope.
- Deploy copies `opengl32.dll` + `openxr_loader.dll` (+ `codvr.cfg`) into the game folder.
- **Run:** the *user* launches from Steam, headset connected via Virtual Desktop. See §7.
- Log: `<game folder>\codvr_result.log`.
- Uninstall: delete `opengl32.dll`, `openxr_loader.dll`, `codvr.cfg` from the game folder.

**`codvr.cfg` keys:**

| Key | Default | Meaning |
|---|---|---|
| `renderSyscall` | 71 | which cgame syscall is `R_RenderScene` |
| `worldscale` | 39.37 | engine units per metre (raise → world feels bigger / you shrink) |
| `probe` | 0 | 1 runs a self-scoring diagnostic instead of VR |
| `replayAdd <n>` | — | repeatable; adds a syscall id to the entity-capture set |

---

## 14. File map

| Path | Role |
|---|---|
| `src/dllmain.cpp` | proxy entry, syscall hook, scene replay, VR orchestration, probe (**needs split**) |
| `src/vr.cpp` / `src/vr.h` | all OpenXR: session, swapchains, per-eye camera, submit/blit |
| `src/gl_thunks.inc`, `src/opengl32.def` | **GENERATED** by `tools/gen_gl_proxy.py` — never hand-edit |
| `build.ps1` | build / selftest / deploy |
| `tools/gen_gl_proxy.py` | proxy export generator (368 exports) |
| `tools/find_render_syscall.py` | x86 arity analysis (how syscall 71 was confirmed; uses capstone) |
| `tools/selftest_host.cpp`, `tools/xr_probe.cpp` | headless probes |
| `dist/` | built artifacts + `codvr.cfg` |
| `HANDOFF.md` | working session handoff (more tactical than this document) |

---

## 15. Reference material

- **RTCWQuest** — https://github.com/Team-Beef-Studios/RTCWQuest — *the* applicable reference.
  idTech3 → Quest, closest engine. See its `vr_stereoReplay` path in
  `Projects/Android/jni/rtcw/src/renderer/tr_cmds.c`.
- **ioq3quest** — https://github.com/Team-Beef-Studios/ioq3quest — multiview approach; **not
  usable here** (needs shaders; CoD1 is fixed-function).
- **CoDExtended** — https://github.com/xtnded/codextended-client — only a partial syscall enum
  (`CG_R_ADDREFENTITYTOSCREEN=61`, `CG_GETDOBJ=162`). Official 1.1x source is private.
  *Note: 61 appears in our trace before the render call and is a plausible entity-add id worth
  trying via `replayAdd` if validation shows a gap.*
- Reddit: r/OculusQuest, "original call of duty vr mod vibe code testing" — same effort.

---

## 16. Persistent project memory

Notes that survive across sessions live at
`%USERPROFILE%\.claude\projects\H--Projects-COD-VR\memory\`, indexed by `MEMORY.md`:
- `cod1-vr-hook-findings.md` — the reverse-engineering facts that cost real investigation
- `dont-use-user-as-tester.md` — instrumentation must self-score; play sessions are scarce
