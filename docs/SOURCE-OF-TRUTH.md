# Source of truth

> **Reading this from a clone?** This document is historical. It records how,
> on 2026-08-24, the canonical source tree was identified among four candidates
> on the original author's machine, by hashing each tree's build output against
> the deployed DLL. **This repository is that tree.** The `H:\Projects\...`
> paths below are that machine's local layout; they will not exist on yours,
> and there are no other trees here to choose between.

**Canonical tree: `H:\Projects\COD-VR-render-hands`**

Established 2026-08-24. Build from here. Do not build from any other tree.

---

## Why this one

The deployed `opengl32.dll` was hashed against every candidate tree's build
output:

| tree | `dist/opengl32.dll` sha256 | size | matches deployed |
|---|---|---|---|
| **COD-VR-render-hands** | `a0dc7b8542966724d48f9adc5173661b7b0232edbc30773c07c7180bcd6550a3` | 408064 | **yes** |
| COD-VR-native-hands | `b3705fe51711c0dca623159c6317a79cd4640bb7b4583e3b6345165182249cc6` | 386048 | no |
| COD VR | `2409de02d3cbe241a6f99ae6668c35d6d3ee49cd7d4d9539be5b514f75684af0` | 382464 | no |
| COD-VR-offline-hardening | `68d63072725bc9fb24d88e8695f9f6e5ebd932aae796d675ce8457f4a8d08fe1` | 423424 | no |

Deployed file, `H:\SteamLibrary\steamapps\common\Call of Duty\opengl32.dll`:
`a0dc7b8542966724d48f9adc5173661b7b0232edbc30773c07c7180bcd6550a3`.

`COD-VR-native-hands` was the canonical tree as of 2026-08-23 18:01 and is
recorded as such in older notes. It stopped being canonical when the GL
surface sweep was deployed at 2026-08-24 14:24. Notes predating that deploy
are not wrong about their own moment; they are simply stale now.

### render-hands is a strict superset of native-hands

Every shared source file is byte-identical:

| file | differing lines |
|---|---|
| codvr.h, comfort.cpp, vr.cpp, vr.h, vr_binds.{cpp,h}, vr_input.{cpp,h}, vr_math.h, vr_weapon.{cpp,h}, probe.cpp | **0** |
| dllmain.cpp | 777 (glsurf wiring, +33 KB) |
| vr_glsurf.cpp, vr_glsurf.h | render-hands only |

So render-hands carries the camera stability, grenade fixes, gamma/visual
changes and runtime-eye-aspect work verbatim, and adds the GL surface layer
on top. Nothing is lost by making it canonical.

---

## Source -> build -> deploy is proven

Rebuilding the canonical tree reproduced the deployed DLL **byte for byte
except four bytes**, both of them PE build-time stamps:

| offset | field | deployed | rebuild |
|---|---|---|---|
| `0x00000108` | COFF `TimeDateStamp` | `0x6A8C8C40` (14:24:00) | `0x6A8CB687` (17:24:23) |
| `0x00059074` | debug directory `TimeDateStamp` | `0x6A8C8C40` | `0x6A8CB687` |

Every code and data byte matched. The deployed binary demonstrably came from
this tree.

`build.ps1` now links with `/Brepro`, which replaces both stamps with a hash
of the content. Two consecutive builds from an unchanged tree now produce an
identical file (`5C1F62BE5906722BE20E1B4DE2063D52B238D394D60AC65CB56AD359F5468668`),
so from here on the provenance question is answerable by comparing one hash
instead of by diffing around timestamps.

Note the consequence: `/Brepro` changes the stamps, so a rebuild of the
sweep sources no longer hashes to `a0dc7b85...`. That deployed artifact is
preserved verbatim at
`checkpoints/2026-08-24-glsurf-sweep-deployed/opengl32.dll`.

---

## Gate

Canonical tree, 2026-08-24: **1107 checks, 0 failures**
(978 safe suite with mocked OpenXR + real GL, 129 DLL suite).

```powershell
.\build.ps1 -SelfTest
```

---

## Retired trees

These are kept for history and for the code noted below. **Do not build or
deploy from them.** Each carries a `RETIRED.md` saying so.

| tree | status |
|---|---|
| `COD VR` | superseded 2026-08-23. Original trunk. |
| `COD-VR-native-hands` | superseded 2026-08-24. Canonical until the sweep deploy; render-hands is this tree plus the GL surface layer. |
| `COD-VR-offline-hardening` | **divergent, not superseded.** See below. |

### offline-hardening holds code that render-hands does not

It is *not* an ancestor of render-hands — its `vr.cpp` is 18 KB smaller and
differs by 389 lines, so it predates the camera-stability work and must not
be merged wholesale. But it is the only tree containing:

| file | bytes | why it matters |
|---|---|---|
| `vr_weapon_profile_table.inc` | 45067 | 48 weapons, 29 ADS sight offsets |
| `vr_weapon_profile.{cpp,h}` | 43598 | profile lookup |
| `vr_hands.h` | 31257 | hand/skeleton geometry |
| `vr_dobj.h`, `vr_dobj_probe.{cpp,h}` | 74127 | DObj bone probe (bone *writes* refuted; reads work) |

The weapon profile table is a prerequisite for any two-hand solver that
needs a rear grip point, a foregrip segment and a barrel axis. Port those
files forward into the canonical tree deliberately, file by file, rather
than merging the branch.
