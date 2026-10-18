// ---------------------------------------------------------------------------
// AUTHORED HAND POSE, RECOVERED FROM THE SUBMITTED VERTICES
// ---------------------------------------------------------------------------
//
// THE BLOCKER THIS ANSWERS
//
// The camera-relative hand draws do not expose an authored orientation through
// their GL modelview. That is not a gap in the interception -- it is what a
// Quake3-era engine does. The hands are skinned on the CPU: the engine walks
// the bone hierarchy, blends every vertex by its weights, and hands the driver
// a flat array of already-posed positions. The matrix that goes with them is
// just the view transform. There is no per-bone orientation to read, because
// the pose is in the VERTICES.
//
// THE WAY THROUGH
//
// Skinning is not lossy for a vertex bound to ONE bone at full weight. Such a
// vertex satisfies
//
//     submittedPos = palmTransform * bindLocalPos
//
// exactly, on every frame of every animation. So given a set of such vertices
// -- their indices in the submitted array and their bind-space positions --
// the palm's full rigid transform is RECOVERED, in closed form, by an
// orthogonal Procrustes solve. Not fitted to a shape. Solved for a transform
// that provably exists.
//
// The shipped `viewmodel_hands_new` rig has 65 vertices bound rigidly to one
// palm bone and 66 to the other, spread over about 3 x 3 x 1.4 cm. No finger
// bone influences any of them, so finger animation cannot rotate or flip the
// recovered frame -- which is the failure mode that rules out fitting a basis
// to the hand's shape.
//
// PROVEN OFFLINE, ON THE SHIPPED ASSETS
//
// tools/verify_palm_solve.py poses the merged rig through every animation each
// weapon uses and compares the recovered transform against the one the
// engine's own skeleton evaluation produces. Over all 28 carried weapons:
//
//     worst rotation error                 0.0001 deg
//     worst origin error                   0.0002 mm
//     worst solve-vs-authored step         0.000002 deg
//
// The last number is the flip guard, and it is deliberately relative: the
// panzerfaust's fire animation really does rotate the authored palm 97 degrees
// in one frame as the tube is discarded, so an absolute threshold reports a
// flip where there is none. What matters is that the recovered basis turns by
// the same amount the authored one does.
//
// LATERALITY -- RE-CORRECTED 2026-08-28: THE ASSET SIDE IS THE RENDERED SIDE
//
// Surface 0 (vertices [0,837), bones `bip01 l *`) is the player's LEFT /
// support hand; surface 1 (vertices [837,1674), bones `bip01 r *`) is the
// RIGHT / trigger hand. The earlier "inversion" finding read submitted-space
// coordinates as eye coordinates; the live hand modelview is the Quake-view
// -> GL-eye permutation (det +1, submitted +x is eye-FORWARD), and in that
// space the solved surface-1 palm is the near/right/low trigger hand while
// surface 0 is the fore hand 15 inches ahead of it -- matching the authored
// inter-hand geometry and the muzzle-distance physics across six weapons
// (tools/check_grip_hand.py). See MergedHalves() in vr_glsurf.cpp for the
// full evidence chain.

#pragma once

// One landmark: where to read it in the submitted array, and where it sits in
// the palm bone's own frame.
struct CodvrPalmLandmark {
    int   index;        // vertex index within the merged hand batch
    float bindM[3];     // bind-space position, metres, palm-bone frame
};

// Which rendered hand. These are the RENDERED sides, not the asset labels.
enum { VRHP_TRIGGER = 0, VRHP_SUPPORT = 1, VRHP_HANDS = 2 };

// A rigid pose. `axis` is 3x3 row-major; row k is the palm frame's k-th axis
// expressed in the space the vertices were submitted in.
struct VRHandPose {
    float pos[3];
    float axis[9];
    int   valid;
    float rmsM;         // residual of the solve; a sanity witness, not a fit
};

// How far off the landmarks may sit before the solve is refused. The shipped
// rig reproduces to under a micrometre, so anything near this means the batch
// is not the hand batch -- a different model, a LOD, a mod -- and the honest
// answer is "no pose" rather than a plausible wrong one. The gate is in the
// LANDMARK tables' units (metres) and is scaled by the solve's unitsPerMetre,
// so it means the same physical distance in any vertex space.
#define VRHP_MAX_RMS_M 0.004f

// The landmark tables store bind positions in METRES (the extractor divides
// by this), but the ENGINE submits vertices in game units -- 1 unit = 1 inch,
// proven live. A rotation-only Procrustes has no scale term, so comparing a
// metre-sized bind cloud against an inch-unit observed cloud can only refuse:
// that is exactly what the 2026-08-27 attended run measured (0 solves, 1197
// refusals per hand, everything upstream healthy). Every solve therefore
// takes an explicit unitsPerMetre for the space `verts` live in: pass THIS
// for engine-submitted arrays, 1.0f for metre-space arrays.
#define CODVR_UNITS_PER_M 39.3701f

// The batch geometry this module expects, from the shipped rig.
int VRHandPose_BatchVerts();     // 1674
int VRHandPose_SplitVert();      // 837
int VRHandPose_LandmarkCount(int hand);

// Solve one hand's palm pose from a submitted vertex array.
//
// `verts` points at the first vertex; `strideBytes` is the byte stride between
// consecutive vertices; `count` is how many vertices the draw covers, used to
// refuse a batch that cannot contain the landmarks. `unitsPerMetre` names the
// units `verts` are in (CODVR_UNITS_PER_M for engine arrays, 1.0f for metre
// arrays); the recovered pose comes back in those same units. Positions are
// read as three floats. Returns 1 and fills `out` on success, 0 otherwise --
// and a 0 leaves the hand exactly where the engine put it, which is the only
// safe failure.
int VRHandPose_Solve(int hand, const float* verts, int strideBytes, int count,
                     float unitsPerMetre, VRHandPose* out);

// One shipped hand rig's landmark tables. Three rigs ship (hands_new,
// hands_sten, hands); hands_new and hands_sten have IDENTICAL batch shapes
// and can only be told apart by which bind cloud reproduces to a micrometre,
// which is why the solver tries rigs in order instead of keying on a count.
// The generated kHandRigDefs (vr_palm_landmarks.inc) is the try-order.
struct CodvrHandRigDef {
    const char*              name;        // parts name, e.g. viewmodel_hands_new4
    int                      batchVerts;  // vertex span of the draw the tables index
    int                      splitVert;   // merged half boundary; 0 = single draws
    const CodvrPalmLandmark* trig;        // rendered RIGHT / trigger hand
    int                      nTrig;
    const CodvrPalmLandmark* supp;        // rendered LEFT / support hand
    int                      nSupp;
};

int  VRHandPose_RigCount();
const CodvrHandRigDef* VRHandPose_Rig(int i);

// Solve one hand across EVERY known rig: first the currently installed set
// (the proven default, or a test-injected table), then each shipped rig's
// table for that hand. The first solve that passes the residual gate wins;
// `outRig` reports which rig it was (-1 = the installed default set). A
// per-hand last-success cache keeps the common case at one solve.
// `outBestRmsM` (optional) reports the smallest residual any attempted table
// reached, in METRES: on a refusal it says how near the nearest miss was --
// the difference between "alien geometry" (metres off) and "gate too tight"
// (millimetres off). Left negative when no table could even run (landmark
// indices outside the batch).
int VRHandPose_SolveAny(int hand, const float* verts, int strideBytes,
                        int count, float unitsPerMetre, VRHandPose* out,
                        int* outRig, float* outBestRmsM = 0);

// THE READ GATE (project-lead review 2026-08-29, section 2).
//
// A landmark reader over a buffer-object shadow may only dereference bytes
// the engine actually WROTE in the store's current allocation generation.
// The solver does not know where its `verts` came from, so the caller hands
// it a predicate: canRead(ctx, vertexIndex) returns 1 when that vertex's 12
// position bytes are defined. Every landmark of a table is checked BEFORE a
// single byte of it is read; a table with an unreadable landmark is refused
// with VRHP_REASON_BYTES_UNKNOWN and Horn's method never runs on it. A null
// predicate means "every vertex is readable" (client arrays already proved
// by the OS, or a fixture).
typedef int (*VRHandPoseCanRead)(void* ctx, int vertexIndex);

// Why a solve refused, in precedence order when several tables were tried:
// unknown bytes outrank a residual verdict (a residual computed next to
// undefined bytes is not evidence), which outranks "no table fit the batch".
enum {
    VRHP_REASON_OK = 0,
    VRHP_REASON_INDEX_RANGE,     // every table's landmarks fall outside count
    VRHP_REASON_BYTES_UNKNOWN,   // a table's landmark bytes were never written
    VRHP_REASON_DEGENERATE,      // Horn's eigenvector had no direction
    VRHP_REASON_RMS,             // every runnable table missed the residual gate
};
const char* VRHandPose_ReasonName(int reason);

// SURFACE ORDER (root cause of the 2026-09-03 flicker, see
// docs/FLICKER-ROOT-CAUSE-2026-09-03.md). The engine packs the two hand
// surfaces of a merged rig in EITHER order from frame to frame: surface 0
// then 1 (the file order every table was extracted in), or 1 then 0. A table
// read against the swapped order lands on the OTHER hand's shell and refuses
// with a constant residual (0.0455 m for the RIGHT table on the BAR). So a
// merged-rig table is tried in BOTH orders: as extracted, and with its
// indices shifted by -splitVert (RIGHT table, now in the first half) or
// +splitVert (LEFT table, now in the second half). `outSwapped` reports which
// order solved (0 file order, 1 swapped); `preferSwapped` (-1 = the last
// order that solved for this hand, 0/1 = try that order first) keeps the
// common case at one solve. `firstIndex`..`count-1` is the index range the
// DRAW actually references: a table whose shifted landmarks fall outside it
// is not tried, so a second-half draw can never be "identified" by reading
// the first half's bytes.
int VRHandPose_SolveAnyEx(int hand, const float* verts, int strideBytes,
                          int count, float unitsPerMetre, VRHandPose* out,
                          int* outRig, float* outBestRmsM,
                          VRHandPoseCanRead canRead, void* readCtx,
                          int* outReason,
                          int* outSwapped = 0, int preferSwapped = -1,
                          int firstIndex = 0);

// WHICH HAND is in this draw, from the vertices alone. Tries every hand
// table in both orders, restricted to tables whose landmarks lie inside
// [firstIndex, count). Quiet: touches no statistics and no sample record --
// it is a classifier's question, asked before any attempt is attributed to
// a hand. Returns 1 and fills outHand (VRHP_TRIGGER/SUPPORT), outSwapped,
// outRig (-1 default set) and outPose on the first table that solves; 0
// when none does (the draw is not a recoverable hand: leave it alone).
int VRHandPose_Identify(const float* verts, int strideBytes, int firstIndex,
                        int count, float unitsPerMetre,
                        VRHandPoseCanRead canRead, void* readCtx,
                        int* outHand, int* outSwapped, int* outRig,
                        VRHandPose* outPose);

// Test seam: forbid the swapped order (models the pre-2026-09-03 solver so
// the swapped-batch mutant can be shown to refuse exactly as hardware did).
void VRHandPose_TestSetSwapAllowed(int on);

// Diagnostic snapshot of the LAST solve attempt: the observed landmark
// positions exactly as read from the submitted array, plus the verdict.
// Exists so an attended run leaves the RAW inputs of the palm solve in the
// log -- the 2026-08-27 13:42 run's "0 solves / 1197 refusals" could not be
// re-solved offline because nothing had kept the sampled vertices.
enum { VRHP_MAX_LM = 80 };
struct VRHandPoseSample {
    unsigned attempts;       // total solve calls for this hand so far
    int      n;              // landmarks captured below
    int      ok;             // last attempt's verdict
    float    rms;            // last attempt's rms, in vertex units
    float    unitsPerMetre;  // what the last attempt was told
    // A RIGID-INVARIANT signature of the observed cloud: the rms distance of
    // the sampled landmarks from their own centroid (radius of gyration), in
    // vertex units. Independent of where the hand is and how it is turned, so
    // a REFUSED cloud and a SOLVED cloud captured at different animation
    // states are still directly comparable -- which raw positions are not.
    // Equal signatures mean the same point set (a correspondence/indexing
    // fault); different signatures mean genuinely different geometry.
    float    shape;
    int      shift;          // index shift the attempt read with (0 or +-split)
    int      idx[VRHP_MAX_LM];
    float    obs[VRHP_MAX_LM * 3];
};
void VRHandPose_GetLastSample(int hand, VRHandPoseSample* out);

// The GL delta that carries `authored` onto `desired`, as a 4x4 in the same
// space the vertices were submitted in (column-major, GL order).
//
// This rotates the hand about ITS OWN PALM, not about the camera: the pivot is
// the authored palm origin, which came out of the solve above rather than
// being assumed. A delta built without that pivot swings the hand around the
// viewer's head, which is what the first gun_yaw probe did.
void VRHandPose_Delta(const VRHandPose* authored, const float desiredPos[3],
                      const float desiredAxis[9], float outM4[16]);

// Compose a desired pose from a controller grip pose and the calibrated
// controller->palm offset. Kept here so the one place that decides what
// "the hand should be here" means is next to the one that decides where it is.
void VRHandPose_DesiredFromGrip(const float gripPos[3], const float gripAxis[9],
                               const float palmOffsetM[3], const float palmRotDeg[3],
                               float outPos[3], float outAxis[9]);

// The calibrated form (project-lead brief 2026-09-03, section 7):
//     DesiredPalm = ControllerGrip * ControllerGripToPalm
// `calRows` is the palm BONE frame expressed in the grip's Quake rows
// (fwd, left, up) -- from src/vr_palm_calibration.inc, derived from the
// shipped skeleton against the OpenXR grip convention -- applied first;
// `palmOffsetM` (grip frame, metres) and `palmRotDeg` (an operator trim,
// applied about the calibrated palm's own axes) on top. Null calRows means
// identity, i.e. the uncalibrated form above.
void VRHandPose_DesiredFromGripCal(const float gripPos[3], const float gripAxis[9],
                                   const float calRows[9],
                                   const float palmOffsetM[3], const float palmRotDeg[3],
                                   float outPos[3], float outAxis[9]);

// ---------------------------------------------------------------------------
// THE GUN, BY THE SAME MECHANISM
//
// The gun's vertices arrive already posed too, so its pivot has the same
// problem and the same answer: landmarks rigidly bound to the model's body
// bone give the body's transform in view space, and the rear grip -- expressed
// in that bone's frame -- is then a real point ON the weapon.
//
// WHICH MODEL IS BEING DRAWN is answered by counting the batch's vertices.
// Every one of the 24 gun models the game ships has a distinct total, checked
// in tools/extract_gun_landmarks.py with zero collisions. That is an exact
// key, not a signature match -- and it replaces the baked-offset
// identification the profile layer used, which was explicitly a hypothesis.

struct CodvrGunModel {
    int                      vertexCount;    // the BODY SURFACE's vertex count
    const CodvrPalmLandmark* lm;             // LOCAL indices within that surface
    int                      n;
    float                    rearGripM[3];   // body-bone frame, metres
    int                      rearGripUsable; // 0 unless STRONG or better
    int                      rearGripConf;   // VRWP_CONF_*
    // Two-hand support geometry, body-bone frame. The legal support segment
    // is supportM +/- a bounded slide along barrelFwd; supportRadiusM is the
    // measured grab radius. supportUsable is 1 only when point, direction
    // AND radius are all STRONG+ -- a weaker chain must not invent a grab.
    float                    supportM[3];
    float                    barrelFwd[3];   // unit, body-bone frame
    float                    supportRadiusM;
    int                      supportUsable;
    // THE BORE (brief 2026-09-04d, P1): the direction a shot actually
    // follows and where it leaves the weapon, body-bone frame. Derived from
    // the weapon's SIGHT LINE cross-checked against the muzzle tag's own
    // rotation -- NOT from the chord between the weapon frame's origin and
    // the muzzle tag, which is what barrelFwd carries and which tilts 6.64
    // deg above the Thompson's sights (0.91 m high at 7.9 m: the
    // 2026-09-04d hardware report). barrelFwd stays as the weapon's body
    // long axis, which is what the two-hand support slide runs along.
    float                    boreFwd[3];
    float                    muzzleM[3];
    int                      boreUsable;    // 0 unless bore AND muzzle are STRONG+
    int                      boreConf;      // VRWP_CONF_*
};

// Null when no shipped model has this vertex count -- which is the honest
// answer for a modded or unrecognised weapon, and must leave it untouched.
const CodvrGunModel* VRGunPose_ByVertexCount(int vertexCount);
int VRGunPose_ModelCount();

// Solve the gun body's rigid transform from the submitted array.
// `unitsPerMetre` as for VRHandPose_Solve.
int VRGunPose_Solve(int vertexCount, const float* verts, int strideBytes,
                    int count, float unitsPerMetre, VRHandPose* out);

// Try EVERY model whose key matches this batch's vertex count -- surface
// keys may legally collide (kar98k and kar98kscoped share one body mesh) --
// and return the model whose bind cloud actually reproduces from the
// vertices. Null when none does, which is the honest answer for an unknown
// or modded weapon and must leave it exactly as the engine drew it.
const CodvrGunModel* VRGunPose_SolveByCount(int vertexCount, const float* verts,
                                            int strideBytes, int count,
                                            float unitsPerMetre, VRHandPose* out);
// Same, through the read gate (see VRHandPoseCanRead). `outReason` gets a
// VRHP_REASON_* code on refusal.
const CodvrGunModel* VRGunPose_SolveByCountEx(int vertexCount, const float* verts,
                                              int strideBytes, int count,
                                              float unitsPerMetre, VRHandPose* out,
                                              VRHandPoseCanRead canRead,
                                              void* readCtx, int* outReason);

// The rear-grip pivot in the space the vertices were submitted in.
//
// Returns 0 when the model is unknown OR the rear grip is below STRONG. A 0
// means "no physical anchor is known for this weapon" -- the caller may fall
// back to the geometric centroid as a DIAGNOSTIC, but must not present that
// as the weapon's grip. A WEAK number that looks like a pivot is worse than
// no pivot, because it will be believed.
int VRGunPose_RearGripPivot(int vertexCount, const VRHandPose* body,
                            float unitsPerMetre, float outPivot[3]);

// The delta that carries the gun to `desired`, rotating about `pivot` --
// a point in the submitted vertex space, from VRGunPose_RearGripPivot.
//
// The pivot is a separate argument rather than being taken from the body
// origin on purpose: the body bone's origin is not the grip, and using it
// would reintroduce a wrong-pivot rotation of exactly the kind the first
// gun_yaw probe produced.
void VRGunPose_Delta(const VRHandPose* body, const float pivot[3],
                     const float desiredAxis[9], const float desiredPivotPos[3],
                     float outM4[16]);

// ---- per-SURFACE tables (vr_gun_surfaces.inc; brief 2026-09-04 P0) -------
//
// The engine draws a weapon's surfaces in SHADER-SORTED order, so the first
// gun batch of a scene is often not the body surface kGunModels keys on.
// Every shipped gun surface is bound 100 % rigidly to ONE bone, so a table
// per surface with positions in the BODY-bone frame lets ANY body-bound
// surface key the body pose exactly, whichever the engine draws first.
// Surfaces bound to a moving part are listed for identification only: a
// part that has moved with its animation still fits its own table and
// would report its own pose as the body's, so bodyRigid=0 never keys.
struct CodvrGunSurface {
    int                      modelKey;     // the model's body key (kGunModels vertexCount)
    int                      surface;      // asset surface index
    int                      vertexCount;  // this surface's vertex span
    const CodvrPalmLandmark* lm;           // LOCAL indices, body-bone frame, metres
    int                      n;
    int                      bodyRigid;    // bound to the body bone
    int                      keyUsable;    // bodyRigid with enough points / spread to key
    int                      restStable;   // still relative to the body over the idle anim
    float                    minSpreadM;
    const char*              model;
    const char*              bone;
};
// Solve the BODY pose from ONE named surface, key-usable or not. A moving
// part's table is expressed in the body frame AT REST, so this succeeds only
// while the part actually IS at rest -- when it has moved with its own
// animation the landmarks do not reproduce and the residual gate refuses.
// That is what makes it safe to place a pre-key part with it (brief
// 2026-09-04d, P2): a solve means "this part is where the body says", and a
// refusal means "it is mid-animation", which is the one case to hide it.
const CodvrGunModel* VRGunPose_SolveSurfaceEx(const CodvrGunSurface* s, const float* verts,
                                              int strideBytes, int count, float unitsPerMetre,
                                              VRHandPose* out, VRHandPoseCanRead canRead,
                                              void* readCtx, int* outReason);
// A part may key its own placement only with enough points and spread for a
// trustworthy orientation.
#define VRGP_PARTKEY_MIN_POINTS 8
#define VRGP_PARTKEY_MIN_SPREAD_M 0.014f

int VRGunPose_SurfaceCount();
const CodvrGunSurface* VRGunPose_Surface(int i);
// Which surface of model `m` (by body key) has this vertex span; null = none.
const CodvrGunSurface* VRGunPose_SurfaceOfModel(const CodvrGunModel* m, int vertexCount);
// The same, but null when the span is AMBIGUOUS within the model -- two
// surfaces of one weapon can share a vertex count (the Enfield ships two
// 46-vertex clips) and, being translations of each other in the body frame,
// both reproduce a rigid solve. The residual cannot choose between them, so
// neither may key anything: measured, a wrong choice put the body pose 6 u
// (15 cm) out while passing every residual gate.
const CodvrGunSurface* VRGunPose_UniqueSurfaceOfModel(const CodvrGunModel* m, int vertexCount);
// 1 when some key-usable surface of some model has this vertex span.
int VRGunPose_AnyKeySurface(int vertexCount);
// Solve the BODY pose from any KEY-USABLE surface with this vertex span.
// Surfaces of `prefer` (the model the baseline belongs to; may be null) are
// tried first, then every model's. Returns the model (grip / support
// geometry) and, via outSurf, the surface that solved. Null when no
// key-usable surface reproduces from the vertices.
const CodvrGunModel* VRGunPose_SolveAnySurfaceEx(int vertexCount, const float* verts,
                                                 int strideBytes, int count,
                                                 float unitsPerMetre, VRHandPose* out,
                                                 const CodvrGunModel* prefer,
                                                 const CodvrGunSurface** outSurf,
                                                 VRHandPoseCanRead canRead,
                                                 void* readCtx, int* outReason);

// ---- the SLEEVES (vr_sleeve_landmarks.inc; brief 2026-09-04 P1) ----------
//
// The sleeve surface is skinned across forearm, upper arm, cuff, wrist and
// hand bones -- it is not a rigid body. What can be recovered is the
// FOREARM's orientation, from the vertices the forearm dominates, behind a
// wider residual gate (rmsGateM). restRows is the constructed anatomical
// palm->forearm relation: the forearm's axes (X elbow->wrist, Y, Z) in the
// palm-bone frame with the wrist straight; forearmLenM the bone length.
struct CodvrSleeveDef {
    const char*              rig;          // parts name, matches kHandRigDefs
    int                      hand;         // VRHP_TRIGGER / VRHP_SUPPORT (rendered side)
    int                      surfaceVerts; // the sleeve surface's vertex span
    const CodvrPalmLandmark* lm;           // LOCAL indices, forearm-bone frame, metres
    int                      n;
    float                    restRows[9];  // forearm axes in the palm-bone frame
    float                    forearmLenM;
    float                    rmsGateM;
};
int VRSleevePose_DefCount();
const CodvrSleeveDef* VRSleevePose_Def(int i);
// The def for a rig (by kHandRigDefs index, -1 = any) and rendered side.
const CodvrSleeveDef* VRSleevePose_DefFor(int rig, int hand);
// Solve the FOREARM pose from one sleeve surface / half. `firstIndex` is the
// half's first vertex index (0 or the split) and doubles as the landmark
// shift, exactly as the hands' split solve. Tries the preferred rig's def
// first, then every def for that side. Null when none passes its gate.
const CodvrSleeveDef* VRSleevePose_SolveAny(int hand, const float* verts, int strideBytes,
                                            int count, float unitsPerMetre, VRHandPose* out,
                                            int preferRig, VRHandPoseCanRead canRead,
                                            void* readCtx, int* outReason, int firstIndex,
                                            float* outRmsM);

// Testing seam: swap in a synthetic landmark set so the solver can be
// exercised without the shipped assets.
void VRHandPose_TestSetLandmarks(int hand, const CodvrPalmLandmark* lm, int n);
void VRHandPose_TestRestore();

// Diagnostics.
struct VRHandPoseStats {
    unsigned solves[VRHP_HANDS];
    unsigned refusals[VRHP_HANDS];
    float    lastRmsM[VRHP_HANDS];
    // Times Horn's method actually RAN (covariance + eigen-solve). A refusal
    // for unknown bytes must not move this: that is the witness that a mixed
    // valid/unknown point cloud never reached the solver.
    unsigned solverRuns;
    // Tables refused because a landmark's bytes were undefined, per hand.
    unsigned refusedUnknownBytes[VRHP_HANDS];
};
void VRHandPose_GetStats(VRHandPoseStats* out);
void VRHandPose_ResetStats();
