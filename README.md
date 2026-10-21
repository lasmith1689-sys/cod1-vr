# Call of Duty (2003) in VR

A VR mod for the original *Call of Duty*, single-player, on a Quest 3 over
Virtual Desktop. Stereo 3D, head tracking, supersampled rendering, and the
comfort options a modern VR title is expected to have.

It works by replacing the game's `opengl32.dll` with a proxy that forwards every
call to the real driver while quietly taking over the camera and the frame. The
game itself is untouched — nothing is patched on disk, and uninstalling is
deleting three files.

---

## Requirements

- *Call of Duty* (2003) on Steam — single-player
- A Quest 3 (or similar) with **Virtual Desktop**
- Windows, 64-bit, with a GPU that can drive ~3240×3240 per eye

---

## Install

1. Copy `opengl32.dll`, `openxr_loader.dll`, and `codvr.cfg` from `dist/` in
   this repository into the game folder, beside `CoDSP.exe`.
2. Launch **Virtual Desktop inside the headset** and connect to the PC. This
   matters: the PC-side streamer alone is not enough, and the game will report
   that no headset is present.
3. Start the game from Steam as normal.

The game window on your monitor becomes a 1440×1440 square showing a mirror of
what the headset sees. That is expected — see *Why the window is square* below.

**Uninstall:** delete those three files. To also restore the original video
settings, copy `Main/autoexec.cfg.pre-vr-backup` over `Main/autoexec.cfg`.

---

## Controls

Everything is the usual mouse and keyboard, plus:

| Key | Action |
|---|---|
| **HOME** | Recenter — makes where you are sitting and facing the new "straight ahead". Also the left controller's stick click. Change the key with `recenterKey` |
| **Q / E** | Snap turn, if you enable it (off by default) |

**Aiming.** The mouse aims, exactly as in the flat game — both left/right and
up/down. Your head then looks around freely on top of that. With your head
centred, you are looking straight down the weapon; turn your head and you can
look around without changing where you are pointing.

---

## Settings

All in `codvr.cfg`, next to the game executable. Every setting is read at
startup, so changes take effect on the next launch — nothing needs rebuilding.

`codvr.cfg` documents every key in its own comments; this page covers only the common ones. Many settings can also be changed in the headset: hold the left stick click for VR Options, which saves back to `codvr.cfg`.

### If it runs badly

Lower **`supersample`** first. It is by far the most expensive setting.

```
supersample 2.25    # default: renders 3240x3240 per eye
supersample 1.75    # noticeably cheaper, still sharp
supersample 1.0     # off; draws at the game's own resolution
```

**Frame rate matters more here than in a normal game.** Frames alternate between
your eyes, so each eye is refreshed at half the game's rate — at 300 fps that is
150 per eye, comfortably above the headset, and you will never notice. If the
game drops toward the headset's own refresh rate, the two eyes start visibly
disagreeing and it becomes uncomfortable.

The mod watches for this. It logs the frame rate to `codvr_result.log` and prints
a plain warning there if it falls low enough to matter, telling you to lower
`supersample`. Call of Duty is a 2003 game and modern hardware has an enormous
amount of headroom, so this should not come up — but if the image ever feels
subtly "off" in a way you cannot place, check the log first.

### If you feel unwell

The **vignette** is the important one. It narrows your view while you move or
turn, which removes the peripheral motion that causes VR sickness, and opens
back up when you stop.

```
vr_comfort_vignette 1   # off in the shipped codvr.cfg; 1 turns it on
vignetteStrength 1.0    # how far it closes; try 0.6 for subtler
vignetteSpeed 40        # LOWER = closes sooner when walking
vignetteTurn 45         # LOWER = closes sooner when turning
```

`vignette` is an older name for the same switch. `vr_comfort_vignette` comes later in `codvr.cfg`, so it is the one that counts; it is also what the in-headset VR Options panel saves.

If turning specifically bothers you, switch to snap turning:

```
snapTurn 30             # degrees per step; 0 is smooth mouse turning
snapTurnKeys Q,E
```

Snap turn works by feeding the game mouse movement, so its size depends on your
in-game sensitivity. If a step turns too far, lower `snapMouseScale`.

### If the scale feels wrong

```
worldscale 39.37    # engine units per metre
```

Raise it to make the world feel bigger and yourself smaller; lower it to feel
like a giant. This is sensitive — try 36 or 42 before anything more extreme.

### If the HUD bothers you

```
hudshift 12         # how far in front of you the HUD sits; 0 puts it at infinity
hudAspect 1.777     # 16:9 proportions; 1.333 for classic 4:3; 0 to stretch to fill
```

If the HUD ever looks doubled or hard to fuse, set `hudshift 0`.

---

## Why the window is square

Each headset eye is about 3072×3264 — nearly square. A normal 16:9 buffer spends
most of its pixels on horizontal detail the eye cannot use, while starving the
vertical, which is what makes a VR image look soft. A square buffer matches the
eye, so the same pixel budget buys a noticeably sharper picture.

The game therefore runs windowed at 1440×1440, set in `Main/autoexec.cfg`.
Rendering actually happens offscreen at 3240×3240; the window is only a preview.

Windowed rather than fullscreen because 1440×1440 is not a display mode any
monitor has, so fullscreen could not set it.

---

## How it works

The mod sits between the game and OpenGL.

**Stereo.** The engine hands its entity list to the first render of a frame and
empties it, so rendering both eyes in one frame leaves the second without
soldiers or a weapon. Instead, **frames alternate eyes**: each frame draws one
complete, ordinary scene that the engine builds for itself, and both eyes are
submitted every frame — one fresh, one from the previous frame, each tagged with
the pose it was actually rendered at. Every eye therefore gets a full scene, a
full HUD, and the whole width of the buffer.

**Sharpness.** Rendering is redirected into an offscreen target at a multiple of
the game's resolution, with the engine's own viewport and scissor rectangles
scaled to match. The engine never notices; its pixels are simply larger. The 2D
layer sharpens along with the world.

**Field of view.** A headset's frustum is asymmetric, but the engine's camera can
only express a symmetric one. The mod renders the smallest symmetric frustum that
contains the real one, then hands the compositor the sub-rectangle matching the
true asymmetric frustum, tagged with the true angles — so nothing is cropped or
skewed.

**Camera.** The head's rotation is composed onto the game's own view, so mouse
aim and head free-look coexist instead of fighting. Head position is measured
relative to a neutral point latched at startup (and re-latched by recenter), and
clamped, so a tracking glitch cannot throw the camera through the level.

---

## Troubleshooting

**The game crashes on mission load.** Set `replay 0` in `codvr.cfg`. This should
not happen — the feature disables itself if it faults — but the switch is there.

**Everything is black in the headset, or badly wrong.** Set `supersample 1.0`,
which bypasses the offscreen render target entirely. If that fixes it, the
offscreen path is at fault; send the log.

**The game will not start, or the resolution looks broken.** Restore
`Main/autoexec.cfg.pre-vr-backup` over `Main/autoexec.cfg` to go back to 2560×1440
fullscreen.

**No headset detected.** Virtual Desktop must be running *inside the Quest* and
connected. The PC-side streamer alone gives `XR_ERROR_FORM_FACTOR_UNAVAILABLE`.

**Something else.** The mod writes `codvr_result.log` into the game folder every
run, including the resolutions it chose, the runtime's field of view, the head
pose, and any errors. That log is the fastest way to diagnose anything.

There is also a headset-free diagnostic: set `probe 1` and play flat on the
monitor for about 30 seconds. It writes a plain-language verdict to the log about
whether the mod's camera control is reaching the engine. The picture will flicker
and split while it runs — that is the test working.

---

## Building from source

Needs the MSVC **x86** toolchain ("Desktop development with C++", from Visual
Studio or the standalone Build Tools) and Python 3. Everything is 32-bit
because CoD1 is a 32-bit process, including the OpenXR loader -- which is
vendored in `third_party/openxr`, so there is nothing else to fetch.

```powershell
.\build.ps1 -SelfTest
```

`-SelfTest` is the gate. It builds the DLL, then runs the DLL's own suite
against a real hidden GL context and the renderer suite against a mocked
OpenXR runtime. Any failing check, a missing result log, a wrong export count
or a non-x86 binary fails the build. `-Deploy` copies the result into the game
folder and always runs the gate first, so a red build cannot reach the game.

The link step passes `/Brepro`, so the build is reproducible: building a clean
clone of this repository reproduces the `dist/opengl32.dll` committed here byte
for byte. That is how you can check the shipped binary really is what the
shipped source builds, rather than taking it on trust.

The toolchain, Python and the game folder are discovered automatically.
Override any of them when discovery picks wrong:

```powershell
.\build.ps1 -SelfTest -VcVars "C:\...\vcvarsall.bat" -Python "C:\...\python.exe"
.\build.ps1 -Deploy  -GameDir "D:\SteamLibrary\steamapps\common\Call of Duty"
```

`CODVR_VCVARS`, `CODVR_PYTHON` and `CODVR_GAME_DIR` do the same thing from the
environment.

If you are picking this project up rather than just building it, start with
[CONTRIBUTING.md](CONTRIBUTING.md): it says what the gate means, which design
notes are current, and which are history.

---

## License

The mod is MIT licensed -- see [LICENSE](LICENSE).

`third_party/openxr` is the Khronos OpenXR Loader, redistributed under
Apache-2.0. *Call of Duty* itself is neither included nor modified: no game
file is patched, and uninstalling is deleting three files.
