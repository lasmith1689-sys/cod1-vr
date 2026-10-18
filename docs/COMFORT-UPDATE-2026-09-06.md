# CODVR comfort update — 6 September 2026

This update keeps the existing native controller hands and physical aim architecture, and adjusts their calibration and handling. The working pre-update source, configuration, and mod DLL are backed up before installation. The game executable is not modified.

## Changes and defaults

- **Level hand/gun hold:** apply a measured rotation to the shared palm calibration, retaining the palm position and authored hand-to-gun relation. The recorded Thompson pose from snapshot 35 changes from 23.2° down to approximately 0.1° down in replay. Right palm trim is `-22.66, -3.18, -3.30` degrees; the mirrored left trim is `22.71, -2.91, 3.21`. These are rotations in the calibrated palm's axes, not simple world pitch offsets. Both hand and weapon follow the correction.
- **Floating hands:** both complete sleeve/arm surface classes are hidden, including while native hand tracking reacquires its baseline. Hand surfaces remain visible. Arm visibility can be restored in VR Options.
- **Steadier supported weapons:** left grip near the fore-end activates support. Stabilization defaults to 75%, with an adaptive orientation filter that damps tremor and responds faster to larger deliberate turns. Support release, tracking loss, weapon changes, and discontinuities reset filter history. The filter operates in room space once per stereo pair and feeds both the rendered barrel and physical aim target.
- **Reduced supported kick:** 80% reduction of the authored firing animation's displacement relative to the stable hold, while support is engaged. This reduces visible/aim kick; it does not edit the game's weapon definitions, spread, damage, or reload timing.
- **Menu fit:** menu/loading scale reduced from 0.80 to 0.65, preserving the existing UI safe-area setting. Size and distance are adjustable in the headset.
- **Controls:** X tap reloads and X hold switches weapons. Tapping the left stick click recenters on release; holding it opens options without also recentering. Previous weapon-switch layouts remain selectable. Grenade hold threshold is now 0.45 seconds.
- **VR options:** 48 entries including save/help actions cover snap/smooth/off turning, turn speed/angle, head/left-hand movement direction, seated height offset, physical crouching and threshold, comfort vignette, haptics and strength, control layout, grenade hold, arm visibility, support stability/kick reduction, physical reload gesture, hand/gun calibration, physical aim, HUD/UI sizing, menu size/distance, and supersampling. Scrolling accounts for section headers so the selected item stays visible. Saving retains calibration to hundredths of a degree.
- Automatic in-game snapshot readback is disabled for ordinary play; it can still be enabled for diagnostics.

## Default Touch controller layout

| Control | Action |
|---|---|
| Left stick | Move |
| Left stick click, tap | Recenter |
| Left stick click, hold | VR Options |
| Right stick | Turn |
| Right stick click | Melee |
| Right trigger | Fire |
| Right grip | Aim |
| Left grip near weapon fore-end | Attach steady two-hand support; release to detach |
| A | Jump |
| B tap / hold | Crouch / prone |
| X tap / hold | Reload / next weapon |
| Y | Use/interact |
| Left trigger hold | Select grenade slot |
| Left menu | Pause/back |
| Right stick + right trigger in game menus | Cursor + click |

The physical reload gesture remains available. The in-headset controls reference describes the default layout; selecting a legacy weapon-switch layout changes X's behavior.

## Validation

The staged x86 build passed **2,092 safe-suite checks and 135 DLL checks**, with all 368 proxy exports verified. These tests use a mocked OpenXR runtime and real hidden OpenGL context; they do not launch Call of Duty or exercise a headset.

- Recorded Thompson hold replay: approximately -23.2° to -0.1°, unchanged palm position.
- Production support fixture: accumulated jitter travel 1.5103 to 0.1081; supported firing animation kick 9.90° to 2.00°.
- 2,000 stereo pairs with production comfort settings, head movement, alternating surface order, Thompson/BAR switches, and support attach/release: 1,508 supported pairs and 598 supported firing pairs. Zero stereo rejections, XR protocol violations, camera mutations, or published barrel/aim-target mismatches. Both arm classes remained hidden throughout. Barrel comparisons use the model identified by each published pose, including weapon-transition frames.
- Existing hostile 10,000-pair renderer soak remains green.
- New input tests cover X tap/hold, hold cancellation, turning off, and options/recenter separation. Persistence checks retain calibration precision.
- Menu fixture leaves 13.4° horizontal and 23.8° vertical edge margins. Actual GL renders of the options and controls pages were inspected for readability and clipping.

In-headset feel is still to be confirmed. The level correction uses one recorded neutral hold and is applied across weapons; other weapons and hand poses may need a small trim after play. The support filter is an assist, not a simulated physical stock. The engine's existing bullet spread/recoil rules remain in place. Teleportation, a new locomotion system, and fully mirrored left-handed weapon handling are not added by this update.

## Controller and comfort references

The review used Valve's [Half-Life: Alyx VR guidance](https://www.half-life.com/en/alyx/vr), which describes seated/standing use, controller support, and locomotion choices, and the official [Ghosts of Tabor patch notes](https://ghostsoftabor.com/patch-0-4-2651-11973-2/) adding an optional virtual stock. The COD-specific layout above is an adaptation for this mod's existing actions, not a claim to reproduce Alyx's bindings. The practical choices here are exposed comfort settings, conventional stick movement/turning, trigger firing, grip-based support, and accessible reload/weapon selection.

## Recovery and provenance

The installation script records the exact backup directory and deployed hashes in `install-result.json` beside the staged project. The backup contains original source/tests, changed-file originals, the original deployed mod DLL/config/loader, and original deployment provenance. With the game closed, restoring the three files from that backup's `game` directory restores the previous runtime. Do not restore unrelated game files.

Original deployed DLL SHA-256: `566A046540B64A3D4884AAC6380127B75F735217A655F3E670B97D780A265A79`.

Original deployed config SHA-256: `56BCAF26F823623C2A8135547067624901DB7A956A2BDEDA40C9A15B16E6FB78`.

Final deployed hashes and the canonical source revision are also recorded by the existing gated build in `dist/deploy-provenance.json`.
