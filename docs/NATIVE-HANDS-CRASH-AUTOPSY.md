# 2026-08-26 crash-to-desktop and muddy audio — autopsy

Canonical tree: `H:\Projects\COD-VR-render-hands`

## Summary

Two independent problems. Neither was caused by the native-hands feature work.

| | |
|---|---|
| **Crash** | The GL surface layer dereferenced a **vertex buffer object byte offset** as if it were a CPU pointer. Latent since 2026-08-24; the 2026-08-26 run was the first time that code reached a viewmodel draw in VR. **Fixed and proven.** |
| **Audio** | The mod contains **no audio code and changes no audio setting**. The engine was running at **69 game frames/s against the 240/s** the paired scheduler needs. In a Quake3-lineage engine the sound mixer is serviced from the main loop, so main-loop starvation *is* muddy audio. **Explained; the remedy is a config change and needs a hardware run to confirm.** |

---

## 1. The crash

### Evidence

Windows Error Reporting, twice — 07:48:34 and 07:49:06, same fault bucket:

```
Faulting application   CoDSP.exe
Faulting module        opengl32.DLL          (our proxy)
Exception code         0xc0000005            (access violation)
Fault offset           0x00021c3a
```

`codvr_result.log` stops mid-frame on the **first stereo scene** — the first
frame on which viewmodel draws exist at all:

```
this frame renders eye 0 full-size: x=0 w=1440  fov 118.00 x 120.00
VR: eye 0 3D projection intercepted (engine near 4.00, far infinite 0.0)
<end of log>
```

### The faulting instruction

Disassembling the deployed binary at `0x21c3a` lands inside `SampledVertex()`:

```asm
10021C1B: mov   edx, ds:[g_st.vertex.stride]
10021C23: jne   10021C2C
10021C25: lea   edx, [esi*4]                 ; stride fallback = size * 4
10021C30: imul  edx, eax                     ; * index
10021C34: add   edx, ds:[g_st.vertex.p]      ; + the client array base
10021C3A: mov   eax, dword ptr [edx]         ; <-- FAULT
```

The index-type dispatch above it (`1403` = `GL_UNSIGNED_SHORT`, `1401` =
`GL_UNSIGNED_BYTE`) identifies the function unambiguously. The index read
succeeded; the **vertex** read faulted.

### Root cause

The engine does not hand us a CPU pointer. Every one of the 125 draws in the
archived flat capture (`runs/sweep-20260825`) records a vertex pointer under
64 KB:

```
cls=gun       vertPtr=0x00000014   vertStride=32
cls=gun       vertPtr=0x000001E0
cls=hand_L    vertPtr=0x000082C8   maxIdx=1673
cls=sleeve_R  vertPtr=0x000012C0
```

Those are **byte offsets into a bound vertex buffer object**. With a VBO
bound, `glVertexPointer`'s last argument stops being an address — and the
first 64 KB of a Win32 address space is never mapped. `mov eax,[0x14]` is a
textbook access violation.

We never saw the binds. The proxy intercepts `glVertexPointer` but not
`glBindBufferARB`, and the engine resolves buffer-object entry points through
`wglGetProcAddress`, so the layer had no way to know the value it was storing
had changed meaning.

### Why the first stereo frame

`GLSurf_OnDrawElements` calls `NoteClassPivot(cls, type, idx, count)`
**unconditionally for every non-merged draw**, before any class or transform
check. The gun batch is not merged. So the first viewmodel draw of the first
3D scene reached `SampledVertex` — whether or not anything was being
transformed, and whether or not any hand feature was enabled.

That is why the menu survived 13,700 draws (`dh 0` — no viewmodel draws) and
the first gameplay frame did not.

### Why the flat sweep survived

It did not exercise this path. The sweep ran with `vr_glsurf_route =
modelview delta`, which never touches vertex data, and the capture path
records the pointer *value* without dereferencing it. The pivot path was
reached, but the sweep's own screenshots and CSVs are the evidence that it
completed — so the honest statement is that the flat run did not hit the
faulting call with a buffer offset live, not that the code was ever safe.

### Why the test suite missed it

Every existing case calls `EnterViewmodelPass()`, which sets a **real, valid**
array. The harness modelled a client-array engine because that is what we
assumed the engine was. A suite that only ever supplies a good pointer cannot
discover that the code has no defence against a bad one.

### The fix

One choke point, `ClientRangeReadable()`, and every client-array reader now
goes through it — `SampledVertex`, `BuildHandDelta`, `TransformDrawScratch`,
and the normal array.

- A "pointer" below 64 KB is refused without asking the kernel: that is the
  VBO-offset case.
- Anything else is proved readable with `VirtualQuery` before it is touched,
  with the last validated region cached so the common case is two compares.
- An index beyond `1 << 20`, or address arithmetic that wraps, is refused.
- Every refusal increments a **reason-specific** counter, because "the layer
  declined" and "the engine is using buffer objects" are different facts.

A refusal costs one draw's pivot or hand transform. A wrong guess cost the
process.

### Proof

Reproduced flat, offline, no game and no headset. `tests/vbo_safety_tests.inc`
feeds the engine's own recorded offsets into the production path.

| | with the fix | with the probe neutered (mutant) |
|---|---|---|
| unmerged gun draw @ 0x14 | no fault | **0xC0000005** |
| merged hand batch @ 0x82C8 | no fault | **0xC0000005** |
| 9 offset × index-width combinations | 0 faults | **9 faults** |
| 10,000-frame soak | 0 faults | — |

Soak result: **10,000 frames, 0 access violations, 0 re-entries**, with 5,195
null-page / 3,221 unmapped / 237 wild-index refusals, and 26,583 probe cache
hits against 4,459 `VirtualQuery` calls.

---

## 2. Re-entrancy

`dllmain` fills `GLSurfDriver` with the **real** `p_gl*` entry points, not our
`i_gl*` wrappers, so a re-issued draw could never come back through the
interception. That was a property of one assignment in another file; it is now
a property of the layer as well — a depth counter declines and counts a
re-entry instead of recursing.

A mutant that wires the driver back to the interception bottoms out after 2
calls instead of 64, and the re-entry is counted. A split batch still produces
exactly two sub-draws.

---

## 3. The audio regression

### What did NOT change

The mod contains **no audio code**: no `snd_*`, no DirectSound, no OpenAL, no
mixer, no device selection. `codvr.cfg` carries **no audio settings**, and the
config deployed with the crashing candidate is byte-identical to the one
deployed before it (`F7AB7A2D…`). There is no audio configuration difference
to find.

### What did

```
VR: XR target cadence 120.0 Hz -- paired rendering wants ~240 game frames/s
perf: 105 game frames/s against a 120 Hz headset target
perf: WARNING -- the engine is not keeping the headset's pace
perf:  94 game frames/s
perf:   8 game frames/s
VR perf:
  game frames:  69.4 Hz        (needs ~240)
  XR submitted: 67.7 Hz
  fresh left:    0.0 Hz
  fresh right:   0.0 Hz
  XR skips: 10
```

The engine is delivering **29% of the required frame rate**, with excursions
to **8 fps** and ten dropped XR frames, and **never completed a fresh eye
pair**.

CoD1 is Quake3-lineage: the sound mixer is serviced from the main loop. A main
loop running at 69 fps with drops to 8 fps refills the audio buffer late and
irregularly — which is what "muddy" sounds like. Underrun and smear, not a
wrong sample rate or a downmix.

### It is not the interception

Measured, not argued (`S_ProductionInertAndCheap`):

| | per draw | per frame |
|---|---|---|
| classified viewmodel draw | **0.094 µs** | ~7 draws → 0.7 µs |
| non-scene draw (menu/world) | **0.008 µs** | ~14,000 draws → 112 µs |

Total interception cost is about **0.1 ms per frame** against a 4.17 ms budget
at 240 Hz. The shortfall being explained is roughly **10 ms per frame**. The
interception is two orders of magnitude too small to be the cause.

### What is the cause

`supersample 2.25` at a **120 Hz** headset target. The paired scheduler needs
two game frames per XR frame, so 120 Hz asks the engine for 240 game frames/s
while each one renders 3072×3264 per eye. The log's own advice is the right
advice.

**This predates the native-hands work.** The scheduler and the supersample
default are unchanged from Step-A, so if the headset is at 120 Hz the same
starvation would occur on Step-A too. What I cannot do from here is prove the
audio was *better* before, because there is no Step-A hardware run to compare
against — the candidate was deployed over it the same evening.

### Recommended remedy (needs one hardware run to confirm)

1. `supersample 2.25` → `1.5` in `codvr.cfg`. One line, reversible, and the
   single most expensive setting.
2. If the headset is at 120 Hz, drop it to 90 — that changes the paired demand
   from 240 to 180 game frames/s.
3. Longer term the paired scheduler is the structural problem: needing 2× the
   display rate in game frames is a hard ask of a 2003 engine at any
   supersample.

---

## 4. What else this turned up

### The Step-A checkpoint was mislabelled

`checkpoints/stepA-normal-vr/opengl32.dll` hashes to `A7032998…` — **the crash
candidate**, not Step-A. It was created after the candidate was deployed and
copied `dist/`, which by then held the candidate. The true Step-A binary
(`DB4C4FDD…`) exists nowhere and cannot be reproduced, because the sources
moved on.

Restoring "Step-A" as recorded would have redeployed the crash.

**Restored instead:** `checkpoints/deployed-as-found-2026-08-24` —
`B3705FE5…`, camera-stable, and containing **no glsurf layer at all**, so it
is structurally immune to this fault.

### Post-deploy wiring in the tree

`src/vr_native_hands.{h,cpp}` and `tests/nativehands_tests.inc` are timestamped
19:08–19:17 on 2026-08-25, **after** the 17:55 deploy. A string scan confirms
they are **not** in the crashing binary. They had never been compiled; on
first build the suite failed four cases and then crashed, because a case
dereferences `rMv->m` on the line after `CHECK(rMv != nullptr, …)` fails.

Quarantined to `quarantine/`, with an inert implementation
(`src/vr_native_hands_disabled.cpp`) keeping the call sites in `vr.cpp` and
`dllmain.cpp` compiling unchanged. See `quarantine/README.md`.

### The suite hid its own crash

Block-buffered stdout put the last printed line up to 4 KB behind the last
line *reached*, which pointed the first hour of this investigation at the
wrong test. `main()` now sets `setvbuf(stdout, nullptr, _IONBF, 0)`.
