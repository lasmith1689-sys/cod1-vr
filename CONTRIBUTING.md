# Contributing

This is a working reverse-engineering project, not a finished product. If you
are picking it up, read this first: it says where things are, what the build
gate means, and which documents are current.

## Orientation: read in this order

1. **[README.md](README.md)** — what the mod does, how to install it, every
   setting, and a *How it works* section covering the rendering architecture.
2. **The dated notes in [`docs/`](docs), newest first.** Each records a
   specific investigation and its outcome. The most recent is
   `docs/REALISM-PASS-2026-10-05.md`; before it,
   `docs/MANUAL-RELOAD.md`, `docs/NATIVE-HANDS-PHASE-STATUS.md` and
   `docs/COMFORT-UPDATE-2026-09-06.md`.
3. **`HANDOFF.md` and `BRIEFING.md`** — the original engineering handoff and
   project briefing. Both are dated **2026-08-17** and the project moved a long
   way afterwards. Read them for background and for the reverse-engineering
   findings, not as a description of current state.

## Layout

| Path | What it is |
|---|---|
| `src/` | The mod itself: 17 `.cpp` modules plus headers, built into `opengl32.dll` |
| `tests/` | The suites the build gate runs |
| `tools/` | Python and PowerShell research tooling — asset extraction, geometry solvers, diagnostic probes |
| `docs/` | Dated design notes, and the evidence behind them |
| `dist/` | The shipping files: built DLL, OpenXR loader, configs |
| `third_party/openxr/` | Vendored Khronos OpenXR loader (Apache-2.0) |

Within `src/`: `dllmain.cpp` is the proxy entry point and GL interception;
`vr.cpp` owns the camera, projection and frame submission; the `vr_*` modules
split out input, weapons, hands, reload, menu and snapshots.

## Building, and the gate

Prerequisites and path overrides are in *Building from source* in the README.

```powershell
.\build.ps1 -SelfTest
```

**The gate is this project's contract.** `-SelfTest` builds the DLL, then runs
the renderer suite against a mocked OpenXR runtime and the DLL's own suite
against a real hidden GL context — 2229 and 159 checks respectively at the last
run, zero failed. One failing check, a missing result log, a wrong export count
or a non-x86 binary fails the build. `-Deploy` always runs the gate first, so a
red build cannot reach the game.

`.\build.ps1 -GateSelfCheck` proves the gate itself still works, by feeding its
parser a deliberately failing log and requiring it to throw.

**Do not weaken or skip the gate to land a change.** Much of this codebase was
derived by measurement against a packed binary, and the suites encode findings
that were expensive to establish. When a check fails it is more often telling
you something true than being wrong.

## Submitting changes

1. Fork, then branch.
2. Keep the surrounding style — the comments explain *why*, not *what*.
3. `.\build.ps1 -SelfTest` must be green, and new behaviour should arrive with
   checks in `tests/`.
4. Open a pull request describing what you **measured**, not only what you
   changed.

## Things worth knowing before you start

- **Everything is 32-bit.** CoD1 is a 32-bit process, so the whole chain —
  including the OpenXR loader — is x86.
- **The game is never patched.** This is an `opengl32.dll` proxy: installing is
  copying three files, uninstalling is deleting them. Keep it that way.
- **Builds are reproducible.** The link passes `/Brepro`, so building a clean
  clone reproduces the committed `dist/opengl32.dll` byte for byte — which is
  how you can check the shipped binary against the shipped source.
- **Headset time is the scarce resource.** A great deal can be settled offline
  through the suites and the probes in `tools/`; prefer that over asking
  someone to put a headset on to find out.
- **The reinterpreted material textures are not in this repository.** They
  derive from stock game assets and are not redistributed here.
