# COD VR material and interaction pass

## Outcome requested

Reinterpret the complete texture library into believable WWII materials at
useful scale, deliver stereo post-processing, and improve physical VR weapon
interaction including manual reload. Existing optics, camera ownership,
controller calibration, hand geometry and game rules stay the baseline.

## Evidence from the initial audit

- 3,654 logical stock texture assets; 4,852 runtime filename aliases across
  27 mounted archives. The previous custom textures preserve aspect ratio.
- The previous pipeline constrained macro correlation to at least .985 and
  largely added high-frequency detail. This is not an artistic reinterpretation.
- Installed ReShade's last run skipped OpenXR. The native per-eye path can
  apply equivalent color effects reliably before the VR overlays and snapshots.
- Physical reload existed but trigger-hand profiles used a hand relative to a
  weapon owned by that same hand. Translation could not perform the gesture.

## Implementation

1. Build a fresh material library with built-in image generation. Start from
   stock masters and replace suitable world surfaces with complete materials.
   Match aspect ratio by isotropic sampling; preserve functional texture
   semantics, UV layouts and alpha silhouettes. Record every asset's route
   and distinguish reinterpretation, enhancement and faithful preservation.
2. Apply bounded color, clarity and highlight bloom independently per eye.
   Preserve OpenGL state and the captured original if shader initialization
   fails. Avoid depth-dependent or temporal effects without suitable evidence.
3. Use the support hand to reload at measured per-weapon loading zones, add
   feedback at gesture stages, release the support pose while reloading, and
   provide physical/assisted/button modes. The engine remains responsible for
   ammunition transfer, charging animation, damage and rate of fire.
4. Enable the already implemented physical crouch path. Keep all features
   individually configurable and preserve the working tracking/aim transforms.

## Verification and deployment

Inspect representative material/model/alpha outputs and repeat seams before
the bulk texture pass. Validate every filename, DDS format, mip chain,
dimensions and alpha coverage before packaging. Run the existing full VR/DLL
gate and added GPU pixel/state and reload tests. Preserve a checkpoint and
disable replaced texture archives recoverably. Hash-check installed artifacts.

Actual headset handling and in-level appearance need hardware evidence; mocked
controller tests and offscreen pixels are recorded separately.

## Remaining release gates

- Bulk Python/image-processing of AI-authored materials was authorized in the
  user's follow-up: keep the original look recognizable and protect alpha/maps.
- Build the representative pilot first, inspect repeat scale and model UVs,
  validate every exported alpha/mip chain, then process and review the full set.
  Preserve UI, functional masks and identities unless dedicated safe artwork
  is supplied; preservation is not reported as reinterpretation.
- Connect the Virtual Desktop application inside the Quest to this PC for
  genuine eye output and controller checks. PC-side Streamer alone is not a
  connected headset.
- Physical reload is a gesture request for the stock engine reload, not a new
  independent ammunition/chamber simulation. More invasive ammunition,
  detachable-magazine and bolt systems are not implemented or claimed.

Continue the pilot and headset validation in the current thread; no new
thread is required.

## Current verified state

The manual-reload and native stereo-effects candidate was installed with a
recoverable backup under `backups/realism-20261005-202753`. Its DLL hash is
`444DD785670164E56166049A6FFB62AEA0F660B37F902841E8763C3A83B5900E`.
After adding successful-reload trigger rearming coverage, 2,209 renderer/VR
checks and 138 DLL checks passed; the production DLL hash stayed identical.

A Steam-native launch initialized the installed DLL. The runtime log confirms
manual reload mode 2 and stereo processing enabled. The console reports model
LOD scale/bias 0, both picmips 0, and trilinear filtering. VirtualDesktopXR
reports `XR_ERROR_FORM_FACTOR_UNAVAILABLE`; no current headset eye captures or
controller feel validation have been obtained.

The engine's anisotropy-extension initialization forcibly sets its availability
cvar to 0 despite the driver extension being present. The minimal proxy filtering
fix passed GPU readback at the driver's 16x maximum, with all four requested mip
filters preserved, non-mip filtering unchanged and texture bytes unchanged.
The anisotropy checkpoint passed 2,209 VR checks and 159 DLL checks and was
installed with this hash:
`4F0F30CD5993180E81AB4D6D3177DB7F2C4BCAF923E6D8AF4538E6A3F3D4CA25`.
Setting the engine cvar to 1 alone does not establish real filtering.
The fresh Steam-native launch also logged a real game texture at 16.0x,
matching the driver's 16.0x maximum. Evidence is retained under
`evidence/runtime-20261005/anisotropy-startup-codvr_result.log`.
The stock blob-shadow cvar is protected and resets in normal missions; global
cheats and an unverified stencil-shadow mode are not enabled.

The stock in-game HUD is already baked into the captured eye texture and
receives color processing. Flat menus and the later VR Options overlay are
outside this pass. Default bloom is disabled until headset review so bright
HUD pixels cannot bloom; clarity and restrained color effects remain enabled.

The final combined release passed the coordinator's fresh full gate: 2,229
renderer/VR checks and 159 DLL checks, zero failures, with all 368 x86 exports.
Its SHA256 is
`AD11801FE39ECFD89DC0C0B5E28BAF91752D7EBFDF55ABDFC0552404D6BB37A4`.
Evidence: `evidence/final-combined-green-20261005.log`. The release includes
default/fallback bloom 0, actual fresh-pouch squeeze handling, tracked real
trigger release after reload, and ordinary fire during empty receiver-pouch
reach. Detachable-magazine handling still suspends fire. RED and corrected
GREEN regression evidence is retained separately; the initial empty-pouch
fixture was corrected before its RED evidence was accepted.
The installed hash and recoverable backup location are recorded in the latest
`backups/realism-*/deployment.json`. Deployment does not install a texture pack.

Final deployment: `backups/realism-20261005-213205/deployment.json`. Game DLL,
canonical release DLL and gated candidate all match the SHA256 above. Comparing
the actual `codvr.cfg` with the initial checkpoint after excluding the eight
feature keys found zero unrelated line differences. All 27 active PK3s remain.
The final fresh Steam launch used cvar **queries**, not quality assignments:
model LOD scale/bias 0, both picmips 0, trilinear filtering, detail textures 1,
dynamic lighting 1, dynamic-light quality 1; actual sampler filtering is 16x.
The loaded runtime reports manual reload mode 2, stereo processing ON and
`XR_ERROR_FORM_FACTOR_UNAVAILABLE`. Evidence is in
`evidence/runtime-20261005/final-startup-{console,codvr_result}.log`.
The game is running in its menu awaiting the headset; no new headset validation
or texture raster/deployment is implied by this startup check.

All 3,654 logical textures have been inventoried. Twelve new AI-authored material
swatches are available in `assets/materials`. The pipeline has 32 passing
metadata/format tests, but raster processing and texture deployment have not run.
It separates 1,976 planned material treatments from 1,678 functional/identity/
technical preservation routes rather than falsely reporting every image redrawn.
Preservation routes bypass raster export entirely: all 2,425 current winning
physical aliases retain their exact bytes, formats, dimensions and mipmaps.
This prevents 1,046 enlarged DDS assets being downgraded to stock resolution.
Two bespoke AI atlas drafts were rejected because small UV features moved.
Bulk image-processing authorization has now been obtained. No old texture pack
has been removed or disabled by this pass.

## Source-faithful texture pilot

The first material pilot was rejected: its complete world-material replacement
changed paving direction, masonry joints and period palettes. The corrected
source-backed treatment retains those features and adds bounded authored
surface grain; it is not described as a bespoke full repaint of every asset.
Atlas blend weights now feather only outside protected source features, so
seams/markings remain locked without hard-edged material patches.

The revised 12-asset pilot passed all static checks: eight material treatments,
four exact current-byte preservation routes. One rejected tree candidate had
an opaque 2x2 mip despite valid full-resolution alpha; its existing working
resource is retained, with rejection evidence recorded rather than a weakened
alpha test. The sole non-power-of-two source is retained because the actual
engine rejects non-power-of-two DDS sizes. Direct read-only engine lookup
establishes same-stem DDS-first loading for ordinary TGA/JPG references.
Evidence: `evidence/texture-engine-lookup-20261005.md` and
`evidence/texture-pilot-v4-feather-20261005.log`.

Thirty-seven metadata and real-raster tests passed after observed failing
regressions for source palette/joints, foliage trunk masks, non-power-of-two
formats, failed-candidate preservation and outward-only feathering. The next
step is full-library processing, static/visual review, reversible texture-only
deployment and in-game verification in this thread. Headset validation remains
unavailable until the Quest connects. No new thread is required.

## Full texture release staging

The full 3,654-asset build is processing in the distinct
`texture-production-release-20261005` staging folder. No live texture packs have
been replaced at this checkpoint. The coordinator is bounded to two workers,
checkpoints complete ordered receipts, and refuses packaging for a pilot or
any failed/missing asset. Functional and rejected-candidate preservation is
reported separately from material reinterpretation.

Before deployment, the sealed mount plan records fresh SHA256 hashes for all
11 optional texture packs and all 16 untouched stock packs. It preserves the
69 nonimage resources currently supplied by optional packs. Old optional
archives will be recoverably renamed, not deleted; otherwise their later
mount priority would obscure some new textures. No DLL, config, model, shader,
save or control changes belong to this texture deployment.

An engine-generated baseline at the initial Chateau mission view is retained
as `evidence/texture-baseline-chateau-20261005.jpg`, SHA256
`66D2EE192278CD7A90F401E8FAB4F09A7E850269238C41C1CCF0A401983E21FF`.
Existing saves were backed up before loading the mission. This is a flat-game
appearance baseline, not headset or stereoscopic validation. The next steps
are finished-library alpha/mip and art review, deployment admission, reversible
installation and a fresh game comparison in the current thread.

The incomplete v4 staging run was stopped and rejected after independent
atlas review found cloth marks on bare hands/forearms and coarse twill patches
on uniforms. Its diagnostic candidates remain in
`evidence/texture-v4-rejected-atlases-20261005`; nothing from it was installed.
The narrowly corrected v5 pipeline uses source-palette-matched fine grain for
character material treatments, honors declared metal donors for helmets,
retains hand identities and named leather boots/jerkins, and accepts reviewed
source-size locks for exposed skin, medical bands, badges and embedded leather.
World/weapon treatments and all runtime/VR code remain unchanged by this fix.

Fresh regression evidence reproduces the wrong hand/leather routing, uniform
palette drift and wrong helmet donor before correction. The current 60-test
suite passes, including explicit source-RGB/alpha locks and recoverable deploy
failure simulations. Evidence: `evidence/texture-v5-root-green-20261005.log`.
The corrected initial 22-asset pilot passed with 16 material treatments and six
current-byte preservation routes. This remains a sampled pilot, not full
coverage, installation, rendered-model or headset approval.

The game exited normally after baseline capture; all six existing save files
still match their pre-test backup byte-for-byte. No gameplay movement or firing
was performed. The next step remains a final masked pilot, complete release
build, static/art admission and a texture-only installation in this thread.

Final v5 pilot: 34 assets, 26 static-passed material treatments and eight exact
current-byte preservation routes, zero failures. Twenty-two reviewed binary
source masks cover skin, markings and embedded leather. The sampled decoded
DDS sheets preserve those regions; explicit PNG locks are also checked against
the source. The four world and two weapon pilot PNG/DDS pairs are byte-identical
to their earlier approved v4 samples. Cloth props use the same source-colored
fine-grain restriction rather than coarse donor patches.

The current frozen suite passes 61 tests. Final pilot evidence is
`evidence/texture-pilot-v5-final-20261005.log` and
`evidence/visual-review-v5-final-20261005/static-source-lock-review.json`.
Processor SHA256:
`669796A7F5E0D28B5DA5A66D8BEEAE89ACB113552236DD128CF84D1C3A505142`.
Protection-provenance SHA256:
`B928E020934F5A8DEFB5F056F195C7221C8E9062F1A2F2B3FECD6790A830CB6C`.
The full v5 build is now running with these frozen inputs, staging only.

## Additional maximum-detail and material admission evidence

The actual installed SP engine was read without writes during a fresh normal
menu launch. Its LOD metric is distance times the view-normalized scale plus
bias; zero/zero retains the first highest available model LOD. This agrees
with the active stock menu's Maximum preset, so no speculative larger scale
or developer override is applied. Exact bytes, registration, callers and
stock High/LOD asset records are in `evidence/lod-engine-readonly-20261005.md`.
The remaining active stock-menu quality controls also match the current
maximum-supported preset choices; mission fast flags are not graphics-only
controls and are not modified. See
`evidence/quality-menu-versus-autoexec-readonly.md`.

The test game exited normally without loading a mission or touching saves.
The fresh post-exit texture suite passed all 61 tests with no skipped recovery
scenarios: `evidence/texture-v5-postmenu-full-green-20261005.log`. An earlier
same-suite run while the game was open intentionally skipped four stopped-game
recovery simulations and is not cited as a full admission gate.

Representative full-build review found mixed cloth-tagged props acquiring
wool grain on leather/metal/wood. Those candidates cannot install unchanged.
Separate reviewed data-mask drafts retain the offending complete source
regions, while keeping genuine canvas/felt/shade areas eligible where safe.
Production inputs remain frozen until the active staging run ends; then the
affected records must be regenerated, visually reviewed and repackaged.

Isolated exporter diagnosis for three rejected alpha candidates found that
BC3 substitution and premultiplied-mip flag changes did not repair all failing
decoded coverage checks. Diagnostic files are not release inputs; alpha gates
remain unchanged, with exact current-byte preservation for failed candidates.
Evidence is `evidence/alpha-export-diagnostic-20261005/diagnosis.json`.

Next: complete staging, admit reviewed prop masks, rerun affected records and
full release gates, deploy textures reversibly, then compare actual game and
headset appearance. Continue in this thread; no new thread is required.

### Data-only material and alpha admission corrections

The reviewed manifest is a separate copy at
`assets/texture_manifest_material_review_20261005.csv`; the original 3,654-row
source manifest remains unchanged. A semantic comparison verifies unchanged
identity/order, source paths, dimensions, formats and alpha declarations.
Only reviewed material/routing fields differ. Full release reruns must pass
this file explicitly with `--manifest`; the tool default intentionally remains
the original source manifest.

Fresh prop review admitted four partial source locks (1157, 1176, 1202, 1224)
for actual cloth regions. Nine ambiguous/leather/mixed props instead take
exact current-winning-byte preservation (1170, 1171, 1175, 1180, 1182, 1187,
1191, 1214, 1223). Full-white draft masks are not admitted: comparing an enlarged
texture round-tripped to source size can falsely count resampling as new art.
The masked canteen pilot also added negligible actual donor detail, so it is
preserved rather than recompressed or reported as a substantial repaint.

BAR, sniper-rifle and searchlight atlases (1550, 1778, 1781) use reviewed metal
assignments instead of cloth. A separate Thompson world-model pilot (1818)
uses the existing mixed wood/metal weapon route. The mixed Thompson stock and
knife/sheath atlases (1821, 1851) are retained exactly rather than admitting
metal donor spill on wood/leather or forcing a named stock atlas entirely to
wood. These are data corrections, not new weapon/rendering code.

A read-only exact target-versus-resized-stock audit checks actual new RGB
artwork independently of the round-trip metric. Confirmed effectively
unchanged candidates must be routed to current-byte preservation before
release. Primary stock `.stype` shader templates also declare GE128 cutouts
for some cloth/metal props; foliage-only classification is not sufficient
evidence for those assets. Candidate-versus-current decoded mip review is
in progress, and unsafe alpha candidates cannot be admitted merely because
their source-sized master alpha is unchanged. Frozen processor/coordinator
code and the live game remain unchanged during this admission pass.

### Completed original-snapshot admission review

The first full v5 staging snapshot finished with 3,654 static-passed records,
but its technical PASS was not art admission. The untouched original snapshot
is preserved under `evidence/full-v5-original-static-snapshot-20261005`.
An independent exact target-size source comparison checked all 1,529 generated
candidates and identified 120 exact/near-zero-area treatments. Those take
current-winning-byte preservation instead of recompression being sold as art.
The frozen processor, coordinator and deployment helper are unchanged.

Original shader templates and explicit shader stages were checked separately.
Twenty-six additional masked candidates fail the existing decoded GE128 mip
coverage admission rule and are preserved. This is conservative admission;
the old HD winners can already fail similar checks, and some templates declare
`nomipmaps`, so this is not a claim that every failure is a new runtime defect.
Every complete cube-face group is preserved: 36 groups, 216 faces. The ocean
cloud set's incorrect masonry treatment and the glass environment reflection
map's masonry treatment were rejected before installation.

Actual stock/current/new-PNG/decoded-DDS pilot reviews admit corrected metal,
mixed wood/metal weapon treatments and six source-specific stock masks. Bad
Garand/Nagant world-model and K98 wood candidates remain current-byte routes;
technical locks did not override the visible patchy-grain rejection. The Mosin
wood/metal pilot is accepted offline. The final mask library has 32 records:
the original 22, four cloth-prop guards and six weapon-hardware guards. The
rejected K98 and full-white draft prop masks are not admitted.

Fresh tests after these DATA-only corrections pass all 61 scenarios with no
skips: `evidence/texture-final-data-masks-green-20261005.log`. Only reviewed
material/routing fields differ from the original source manifest. Source
identities, dimensions, paths, formats and alpha declarations remain exact.
Final corrected full build, accepted-output hash matching, package dry-run and
texture-only deployment remain required; offline atlases do not establish
physical UV scale, actual model appearance or headset/controller acceptance.

Next: finish the last shader-material pilot, rerun the frozen full build with
the reviewed manifest, verify archives, install reversibly and capture the
same Chateau view. Continue in this thread; no new thread is required.

### Corrected texture-only release installed, 2026-10-06

Final corrected staging completed and packaged with zero failures: 3,654
logical assets, 1,355 material treatments and 2,299 exact-current-byte
preservation routes. All 4,852 original winning aliases were reconciled;
the five image archives contain 4,680 output paths, including all 3,325
preserved physical aliases. Six separate archives retain 69 nonimage winners.
The original source CSV is unchanged; the reviewed copy differs in 196 rows,
only in its six allowed material/routing fields.

Final immutable manifest SHA256:
`DE3801C20BCC4D3F061EA5F6C99E64EF9CD73AFC4A18EAC21D559D0AC12C09FE`.
Package receipt SHA256:
`EDAA598084CFAECAB963B4A6E78326B74A7044F4A7466A3C2769F61C432C29A0`.
Mask provenance SHA256:
`08062ECF74539B1996A805ECBC63845D91E9D2D359A31DE222288805130E411B`.

The independent final byte/mask review verifies every image-package payload,
all 31 accepted pilot outputs, all 32 masks and 64 source anchors. Original22
mask record objects are unchanged; all generated white-region RGB is source
exact. All31 newly changed PNGs exceed the independently measured sparse-art
boundary. That threshold is not a universal perceptual-quality score.
Evidence: `evidence/FINAL-CORRECTED/REVIEW.md` and associated receipts.

The final shader-aware alpha audit scans 65 new alpha candidates. Remaining
exact GE128 candidates 1890, 2488 and 3430 pass the unchanged every-mip
cutout rule. All 26 rejected masked candidates, 216 cube faces and seven
known control-map/material-art exclusions retain exact current aliases.
Evidence: `evidence/FINAL-ALPHA-CORRECTED-20261005/review.md`. Existing imperfect
old alpha mips are preserved, not falsely reported as repaired.
Both painted ship-door metal pilots were rejected for repeated pale square
flecks; those doors and the two machined shafts remain byte-preserved.

All61 tests pass without skips after the final data freeze:
`evidence/texture-final-frozen-release-green-20261005.log`. The actual read-only
installer dry run passed all sealed archive hashes, complete output/alias
coverage, hypothetical mount priority and all69 nonimage winners.
Evidence: `evidence/texture-final-deployment-dry-run-20261005.log`.

Actual texture-only installation exited0, with 11 verified new copies and 11
recoverable old-pack renames, no deletions and zero rollback errors. Receipt:
`texture-production-release-20261005/deploy_receipts/texture-deploy-20261006-001358-9407882.json`.
This installation does not copy DLLs, configurations, controls, models,
shaders, scripts or saves. Fresh protected-file/stock/retained-entry checks and
actual game appearance validation remain the next gates. Physical headset
tracking, two-hand shot groups, stereo appearance and reload feel are not
established by static passing or a flat image.

Next: finish independent installed-byte verification, launch normally through
Steam, query quality again in the mission and capture the same Chateau view.
Then connect the Quest for actual VR acceptance. No new thread is required.

### Actual post-texture startup and mission acceptance, 2026-10-06

Independent installed-byte verification passed before launch. Normal Steam
startup then loaded the existing Chateau save, with no player movement or
firing. Actual engine captures retain the same scene composition, foliage
cutouts, weapon/HUD layout and familiar art direction. These are representative
flat-render smoke checks, not every-map, every-model UV or headset approval.
The settled after capture is `evidence/texture-after-chateau-settled-20261006.jpg`
(SHA256 `2A91A5E320A9212066F80ECEC4267145D0DB863E3F4883DE878FB56CBA98AED3`).

Fresh in-mission read-only inspection of the actual loaded engine confirms
`r_lodscale=0`, `r_lodbias=0`, `r_picmip=0`, `r_picmip2=0`, 32-bit textures,
trilinear filtering, detail textures, Everything dynamic lights and Nicest
dynamic-light quality. These are active values with no pending latched values.
The real stock corpse-count cvar is `ai_corpsecount=64`; `cg_corpses` is absent
and was not created. Stock protected shadows remain unchanged. Twenty-one
unrelated live render/control values exactly match the stored config baseline,
including resolution, FOV, gamma and frame-limit fields. Runtime driver sampler
evidence separately confirms supported maximum 16x anisotropy, despite the
engine's unused/reset anisotropy-availability cvar remaining zero.
Evidence: `evidence/cvar-live-mission-readonly-20261006.md` and its complete
52-name object receipt. Only query/read process access was used; no process
write, debugger or engine-function call occurred, and handles were closed.

The game was quit normally through its menu. A fresh full post-game check
exited0 with PASS: all10 protected runtime/config/preset files, all6 saves and
their original backups, all16 stock archives, all11 recoverable retired packs,
all11 installed packs and all69 retained nonimage winners remain exact. Active
archive names remain the declared27; the global precision/no-spread pack remains
inactive. Evidence: `evidence/texture-runtime-postgame-20261006.json`, SHA256
`B06D2CB02F10CE6B34CF142E1E1D74DF5BC580701608B502DED0AB54226FBFF7`.
All61 texture tests pass without skips after normal exit:
`evidence/texture-postgame-final-tests-20261006.log`. An earlier repeat while the
game was running correctly skipped four stopped-game recovery fixtures; that
repeat is not used as the complete-suite gate.

Canonical texture tooling/data promotion is bounded to42 declared files and
uses three verified C: backups before any replacement. It excludes all native
source, DLLs, runtime configurations, controls and live game assets. The final
promotion status is established only by its actual receipt under
`backups/canonical-texture-sync-*`, not by this pre-copy documentation. All46
canonical native source files must remain exact against their sealed baseline.

Fresh OpenXR still reports no presenting headset. Native per-eye effects,
two-handed ADS groupings, recoil/control feel and support-hand reload gestures
therefore remain physical acceptance items. The reload implementation requests
the original engine reload; it is not an independent magazine/bolt simulation.
The texture pass does not alter those installed runtime paths. No claim that
all possible realistic VR effects or all physical UV results are validated is
made. Next: connect the Quest through Virtual Desktop inside the headset and
perform stereo/tracking/shot-group/reload acceptance. Continue in this thread;
no new thread is required.
