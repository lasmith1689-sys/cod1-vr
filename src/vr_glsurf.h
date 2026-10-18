// GL surface layer: see the first-person weapon and hands as DRAW CALLS.
//
// WHY THIS EXISTS
//
// Every earlier attempt to move the view weapon tried to change the pose the
// ENGINE authors -- rewriting a scene entity (syscalls 37/38, which turned out
// to be CM_PointContents on the camera), then writing DObj bone controllers
// (measured: the first-person DObj ignores all four enable paths, 0.00000
// units / 0.00000 degrees of movement). Both are dead.
//
// This layer does not ask the engine for anything. By the time geometry
// reaches OpenGL the engine has already chosen the weapon, animated it, skinned
// it, animated the fingers and produced final renderable vertices. We take the
// finished surface and move it. The camera is never touched: the weapon and the
// hands are objects in front of it, not part of it.
//
// The layer has three jobs, in the order they were built:
//
//   1. CAPTURE   -- record every draw call of a scene frame with the full GL
//                   state that reached it, so the viewmodel draws can be
//                   identified from evidence instead of a "near the camera"
//                   guess. Flat, automated, no headset.
//   2. CLASSIFY  -- turn that evidence into a stable per-draw identity
//                   (gun / left hand / right hand / left sleeve / right
//                   sleeve), which fails CLOSED: anything not positively
//                   identified is left completely alone.
//   3. TRANSFORM -- apply a rigid delta to exactly those draws.
//
// TESTABILITY. The layer never links GL. dllmain injects the driver's real
// entry points; the safe suite injects a recorder. That is what lets the
// transform, restore, normal and pair-latch invariants be tested with no game,
// no driver and no headset.

#pragma once

#include <windows.h>

// ---------------------------------------------------------------- GL enums
// Spelled out locally: this file must not depend on a GL header, because the
// safe suite compiles it without one.
enum {
    GLS_GL_TRIANGLES        = 0x0004,
    GLS_GL_TRIANGLE_STRIP   = 0x0005,
    GLS_GL_TRIANGLE_FAN     = 0x0006,
    GLS_GL_QUADS            = 0x0007,
    GLS_GL_UNSIGNED_BYTE    = 0x1401,
    GLS_GL_UNSIGNED_SHORT   = 0x1403,
    GLS_GL_UNSIGNED_INT     = 0x1405,
    GLS_GL_FLOAT            = 0x1406,
    GLS_GL_MODELVIEW        = 0x1700,
    GLS_GL_PROJECTION       = 0x1701,
    GLS_GL_MODELVIEW_MATRIX = 0x0BA6,
    GLS_GL_PROJECTION_MATRIX= 0x0BA7,
    GLS_GL_TEXTURE_2D       = 0x0DE1,
    GLS_GL_VERTEX_ARRAY     = 0x8074,
    GLS_GL_NORMAL_ARRAY     = 0x8075,
    GLS_GL_COLOR_ARRAY      = 0x8076,
    GLS_GL_TEXTURE_COORD_ARRAY = 0x8078,
    GLS_GL_DEPTH_TEST       = 0x0B71,
    GLS_GL_BLEND            = 0x0BE2,
    GLS_GL_ALPHA_TEST       = 0x0BC0,
    GLS_GL_CULL_FACE        = 0x0B44,
    GLS_GL_TEXTURE_WIDTH    = 0x1000,
    GLS_GL_TEXTURE_HEIGHT   = 0x1001,
};

// --------------------------------------------------------------- injection
//
// The real driver, handed in by whoever owns the GL boundary. Any member may
// be null; the layer degrades to "observe only" rather than crashing, which is
// what keeps a partially-resolved driver from taking the game down.
struct GLSurfDriver {
    void (WINAPI *drawElements)(unsigned mode, int count, unsigned type, const void* idx);
    void (WINAPI *drawArrays)(unsigned mode, int first, int count);
    void (WINAPI *loadMatrixf)(const float* m);
    void (WINAPI *matrixMode)(unsigned mode);
    void (WINAPI *getFloatv)(unsigned pname, float* out);
    void (WINAPI *getIntegerv)(unsigned pname, int* out);
    void (WINAPI *getDoublev)(unsigned pname, double* out);
    unsigned char (WINAPI *isEnabled)(unsigned cap);
    void (WINAPI *getTexLevelParameteriv)(unsigned target, int level, unsigned pname, int* out);
    void (WINAPI *vertexPointer)(int size, unsigned type, int stride, const void* p);
    void (WINAPI *normalPointer)(unsigned type, int stride, const void* p);
};

void GLSurf_SetDriver(const GLSurfDriver* drv);
void GLSurf_Reset();                 // full state reset; the suite calls it per case

// ------------------------------------------------------- state observation
//
// Called from the intercepted GL entry points. All are cheap: no driver query
// happens on the hot path, because classification must cost nothing on the
// ~10k draws a frame that are NOT the viewmodel.
void GLSurf_NoteVertexPointer(int size, unsigned type, int stride, const void* p);
void GLSurf_NoteNormalPointer(unsigned type, int stride, const void* p);
void GLSurf_NoteTexCoordPointer(int size, unsigned type, int stride, const void* p);
void GLSurf_NoteColorPointer(int size, unsigned type, int stride, const void* p);
void GLSurf_NoteClientState(unsigned cap, int on);
void GLSurf_NoteBindTexture(unsigned target, unsigned tex);
void GLSurf_NoteActiveTexture(unsigned unit);
void GLSurf_NoteDepthRange(double zn, double zf);
void GLSurf_NoteDepthMask(unsigned char flag);
void GLSurf_NoteDepthFunc(unsigned func);
void GLSurf_NoteCullFace(unsigned mode);
void GLSurf_NoteFrontFace(unsigned mode);
void GLSurf_NoteBlendFunc(unsigned s, unsigned d);
void GLSurf_NoteAlphaFunc(unsigned func, float ref);
void GLSurf_NoteEnable(unsigned cap, int on);
void GLSurf_NoteViewport(int x, int y, int w, int h);
void GLSurf_NoteScissor(int x, int y, int w, int h);
void GLSurf_NoteMatrixMode(unsigned mode);
void GLSurf_NoteLockArrays(int first, int count);
void GLSurf_NoteUnlockArrays();

// Frame / pass structure, driven by dllmain's existing classifier.
void GLSurf_BeginFrame();            // from the swap hook, for the NEXT frame
void GLSurf_SceneQueued(int eye);    // a 3D refdef was queued for this eye
void GLSurf_ScenePhase(int inScene); // scene <-> 2D overlay edge

// ------------------------------------------------------------ the hot path
//
// Return 1 when the layer has ISSUED THE DRAW ITSELF (transformed); the caller
// must then not call the driver again. Return 0 for "not mine, draw normally",
// which is the answer for every draw the classifier does not positively own.
int GLSurf_OnDrawElements(unsigned mode, int count, unsigned type, const void* idx);
int GLSurf_OnDrawArrays(unsigned mode, int first, int count);

// ----------------------------------------------------------- classification
enum GLSurfClass {
    GLSC_NONE = 0,        // not viewmodel, or not identified -- never touched
    GLSC_GUN,
    GLSC_HAND_L,
    GLSC_HAND_R,
    GLSC_SLEEVE_L,
    GLSC_SLEEVE_R,
    GLSC_VM_OTHER,        // depth-hacked but unidentified (muzzle flash, etc.)
    GLSC_COUNT
};

const char* GLSurf_ClassName(int cls);

// The asset-derived fingerprints the classifier matches against. Index counts
// come from the decoded xmodelsurfs of the installed game (see vr_hands.h);
// left and right are INDISTINGUISHABLE by count (3912/3912, 474/474), so the
// side is resolved by draw ordinal within the rig's run, which is the surface
// order the file declares. Nothing here reads a bone or surface NAME.
enum {
    GLS_HAND_INDICES   = 3912,   // 1304 tris: hand + fingers shell
    GLS_SLEEVE_INDICES = 474,    //  158 tris: sleeve / forearm
    GLS_HAND_VERTS     = 837,
    GLS_SLEEVE_VERTS   = 120,
    // viewmodel_hands4 (unused by SP but shipped) has its own counts.
    GLS_HAND4_INDICES   = 1149,  //  383 tris
    GLS_SLEEVE4_INDICES = 399,   //  133 tris
    GLS_HAND4_VERTS     = 302,   // per-surface single draws, not merged
    GLS_SLEEVE4_VERTS   = 93,
};

// -------------------------------------------------------------- capture API
//
// Arm a draw-stream capture. `sceneFramesToSkip` lets the map finish loading
// and settle before the first recorded frame; `frames` is how many consecutive
// scene frames to record. The dump is written beside the DLL.
void GLSurf_ArmCapture(int sceneFramesToSkip, int frames, const char* tag);
int  GLSurf_CaptureArmed();
int  GLSurf_CaptureDone();
int  GLSurf_CaptureRecordCount();
unsigned GLSurf_SceneFrameCount();   // 3D scenes queued since load
// Write the capture out. Returns the number of records written, or -1.
int  GLSurf_DumpCapture(const char* path);

// ---------------------------------------------------------------- transform
//
// The rigid delta applied to a class of surfaces, expressed as a 4x4 in the
// SAME space as the modelview the engine loaded (column-major, GL order).
// GLSurf_SetClassDelta installs it; identity means "leave alone".
//
// PAIR LATCHING. Deltas are pushed once per XR pair by the weapon layer and
// consumed unchanged by both eye renders. This module never samples a
// controller, never reads a clock and never interpolates -- so it is
// structurally incapable of giving eye 0 and eye 1 different object poses.
void GLSurf_SetClassDelta(int cls, const float m4x4[16]);
void GLSurf_GetClassDelta(int cls, float m4x4[16]);
void GLSurf_ClearAllDeltas();
void GLSurf_SetTransformEnabled(int on);
int  GLSurf_TransformEnabled();

// Which mechanism moves the geometry. Both are implemented so the choice can
// be MEASURED by the flat probe rather than assumed:
//   0 = modelview delta   -- left-multiply the engine's own matrix
//   1 = scratch arrays    -- copy verts+normals to CODVR memory, transform,
//                            draw, restore the engine's pointers
enum { GLS_ROUTE_MODELVIEW_ID = 0, GLS_ROUTE_SCRATCH_ID = 1 };
void GLSurf_SetRoute(int route);
int  GLSurf_Route();

// --------------------------------------------------- pose-driven hands
//
// A hand cannot be placed with a fixed delta, because where it currently IS
// changes every frame with the animation. So the weapon layer does not hand
// this module a matrix for the hands -- it hands over the ABSOLUTE desired
// palm pose, and the delta is built per draw as
//
//     delta_eye = Desired_eye * inverse(Authored_eye)
//
// against the pose recovered from THAT draw's own vertices (vr_handpose.h).
//
// THE STEREO RULE, restated (project-lead review 2026-08-29, section 5).
// What must be identical physically is the FINAL controller-owned object,
// not the raw authored animation. The engine legitimately advances the
// animation between the two game frames of an XR pair, so the two eyes'
// authored palms differ; each eye's delta absorbs its own authored pose and
// both land on ONE pair-latched desired pose. (The previous RELATIVE-offset
// form, final = authored + latchedOffset, passed that animation advance
// straight through into a false disparity.)
//
// EYE OFFSETS. "Eye space" is a different space for eye 0 and eye 1 -- they
// differ by the interpupillary offset -- so an absolute target expressed in
// the pair's head-midpoint frame must be shifted by each eye's own offset
// before the delta is built (docs/COORDINATE-SPACES.md, "an absolute target
// position is not eye-invariant"). The wiring installs the two offsets once
// per pair; this layer subtracts the current eye's. Zero offsets (a
// monoscopic fixture, or the flat probe) leave the target as given.
//
// `desiredPosMid` is the desired palm origin in the head-MIDPOINT eyeish
// frame (CoD units); `desiredAxis` rows-as-axes in the same frame.
void GLSurf_SetHandTarget(int cls, int enabled, const float desiredPosMid[3],
                          const float desiredAxis[9]);
// The flat diagnostic's form: run the solve on every hand draw and ask the
// hand to stay exactly where the engine drew it (identity delta). No target
// pose is involved, so the run LOOKS untouched while every verdict lands in
// the stats.
void GLSurf_SetHandTargetHold(int cls, int on);
void GLSurf_ClearHandTargets();

// The two eyes' own positions expressed in the head-midpoint eyeish frame,
// CoD units, for THIS pair. Subtracted from every absolute target (hands and
// gun) when the delta for that eye is built. Reset by GLSurf_Reset.
void GLSurf_SetEyeOffsets(const float offEyeish[2][3]);
void GLSurf_GetEyeOffsets(float out[2][3]);

// ----------------------------------------- per-eye apply records / verdict
//
// What each eye ACTUALLY did with each native object this pair, recorded at
// the draw. The pre-submit stereo verdict (vr_native_hands) reads both eyes'
// records for the pair serial about to be submitted and reconstructs the
// final objects into PairWorld; a pair in which one eye applied a native
// delta and the other did not, or in which the two finals disagree, is
// refused before xrEndFrame. This is the temporal safety net: eye 0 renders
// provisionally, eye 1 renders, and the PAIR is judged -- no eye ever needs
// to know the other's future.
struct GLSurfEyeApply {
    unsigned serial;        // latch serial the draw belonged to
    unsigned scene;         // GLSurf_SceneFrameCount() at the draw
    int      drew;          // this eye drew the class (solve attempted /
                            // keyed gun batch seen) under that serial
    int      applied;       // a native delta actually reached GL
    float    finalPos[3];   // the transformed anchor (palm origin / rear
                            // grip), THIS eye's eye space, CoD units
    float    finalAxis[9];  // its transformed basis, rows-as-axes
};
// cls: GLSC_HAND_R, GLSC_HAND_L or GLSC_GUN. Returns 0 for anything else.
int GLSurf_GetEyeApply(int cls, int eye, GLSurfEyeApply* out);

// ------------------------------------------------- palm attempt telemetry
//
// The first GLS_PALMTRY_MAX hand-solve attempts, one record each, so the next
// headset run's log names the sourcing of every early attempt (buffer,
// generations, offsets, whether the bytes were defined) alongside the verdict.
// Drained by vr.cpp at the pair boundary; nothing here logs.
enum { GLS_PALMTRY_MAX = 16 };
struct GLSurfPalmTry {
    unsigned serial;        // latch serial (pair)
    int      eye;
    int      cls;           // GLSC_HAND_R / GLSC_HAND_L
    int      rig;           // rig index on success (-1 default), -2 none
    unsigned vbo;           // array buffer id (0 = client array)
    unsigned gen, allocGen; // shadow generations at the attempt
    unsigned vertexOffset;  // glVertexPointer offset/pointer
    unsigned indexOffset;   // index pointer/offset
    int      span;          // vertex span (maxIndex + 1)
    int      validRange;    // 1 = every byte of the span was defined
    unsigned unknownBytes;  // undefined bytes in the span (0 when valid)
    int      ok;            // solve verdict
    int      site;          // refusal site: 1 idx, 2 base, 3 solve, 4 bytes
    int      reason;        // VRHP_REASON_* (OK on success)
    float    rmsM;          // best residual, metres (< 0 = none ran)
    float    shape;         // observed cloud gyration, vertex units
    int      swapped;       // surface order the solve read: 0 file, 1 swapped
};
int GLSurf_PopPalmTry(GLSurfPalmTry* out);   // 1 = one record popped

// ---------------------------------------------- ownership / identity stats
//
// ONE VISUAL OWNER (project-lead brief 2026-09-03, section 3). For a
// supported hand draw in an active native pair there is either a NATIVE
// draw (transformed to the controller-owned object) or a coherent vanilla
// fallback -- never both in one eye-scene, and never one per eye. These
// counters are the evidence: every hand draw is attributed to exactly one of
// the first two, and a would-be second owner is suppressed and counted.
struct GLSurfOwnerStats {
    unsigned primaryNativeDraw;          // hand draws issued under a native delta
    unsigned primaryVanillaFallbackDraw; // hand draws with a target enabled that fell through untransformed
    unsigned duplicateOwnerPrevented;    // a second draw of a class in one eye-scene that would have
                                         // shown the OTHER owner: kept coherent with the first
    // How hand identity was decided (docs/FLICKER-ROOT-CAUSE-2026-09-03.md):
    unsigned orderNormal, orderSwapped, orderUnknown;   // merged hand batches, by solved surface order
    unsigned singleByContent, singleByOrdinal;          // single-surface hand draws
    unsigned sleeveByProximity, sleeveByOrdinal;        // sleeve halves/singles, by cuff-to-palm distance
};
void GLSurf_GetOwnerStats(GLSurfOwnerStats* out);

// Keep the palm solve running on this class's draws even while no target is
// enabled. This is what breaks the engagement deadlock the quarantined wiring
// shipped with: a target could only engage once a baseline solve existed, but
// the solve only ran on draws that already had a target -- so nothing ever
// engaged. With solve-wanted set (the wiring asserts it every pair while
// native hands are enabled), the FIRST pair's draws seed the baseline and the
// second pair engages. Costs one small Procrustes per hand draw, ~2 draws a
// frame, only while native hands are on.
void GLSurf_SetHandSolveWanted(int cls, int on);
int  GLSurf_HandSolveWanted(int cls);

// What the last draw of this class recovered. Diagnostics only; a caller that
// gets 0 back must not substitute a pose of its own.
struct GLSurfHandSolve {
    int      valid;
    float    pos[3];
    float    axis[9];
    float    rmsM;
    unsigned solves;
    unsigned refusals;
    // Refusals split by SITE, because the 2026-08-27 run's single opaque
    // "1197 refusals" number could have been any of three different bugs.
    // refusals == refusedIdx + refusedBase + refusedSolve + refusedUnknownBytes.
    unsigned refusedIdx;    // the index range could not be read
    unsigned refusedBase;   // the vertex span would not resolve (unknown id,
                            // stale shadow, out of range, no modelview)
    unsigned refusedSolve;  // the solver itself said no (degenerate/rms)
    // VBO_BYTES_UNKNOWN: the store was allocated and in range but a
    // landmark's bytes were never written in the current allocation
    // generation. Refused BEFORE the solver ran -- distinct from every other
    // site so the next log can say whether the historical refusals were
    // sourcing or geometry.
    unsigned refusedUnknownBytes;
    int      lastRig;       // rig index of the last success (-1 = default set)
    // Deltas that actually REACHED GL for this class. Engagement and solve
    // counts do not imply this: a build can log eng=1 on every pair while
    // every draw still falls through to the engine untransformed (a rejected
    // midpoint split, a missing driver entry point). Without this counter a
    // hardware log cannot tell "the pose never engaged" from "the pose
    // engaged and never reached the renderer" -- two failures that look
    // identical in the headset.
    unsigned deltasApplied;
    // Freshness of the last SUCCESS, for the engagement baseline below.
    // pos/axis always hold the last successful recovery; `valid` only names
    // the most recent ATTEMPT's verdict.
    int      everSolved;
    unsigned lastGoodScene; // GLSurf_SceneFrameCount() at the last success
    // The shape of the last REFUSING draw, so a hardware log can say WHAT
    // refused instead of only how often. site: 1 idx, 2 base, 3 solve,
    // 4 bytes-unknown.
    int      lastRefSite;
    int      lastRefReason;         // VRHP_REASON_* of the last refusal
    unsigned lastRefUnknownBytes;   // undefined bytes in the refusing span
    unsigned lastRefGen, lastRefAllocGen;   // shadow generations then
    unsigned lastOkGen;                     // content generation at last OK
    // Surface order: solves that read the table in the SWAPPED half-order,
    // and the order of the last success (0 file order, 1 swapped).
    unsigned swappedSolves;
    int      lastSwapped;
    int      lastRefCount;      // index count of the refusing draw
    int      lastRefSpan;       // vertex span (hi+1), 0 when unreadable
    float    lastRefBestRmsM;   // best rms any landmark table reached, in
                                // METRES (< 0 = no table even ran); the gate
                                // is VRHP_MAX_RMS_M = 0.004
    // WHERE each verdict's vertices came from. The 2026-08-28 hardware run
    // refused ~50% of real hand draws at the solver with a CONSTANT 0.0455 m
    // residual while the other ~50% solved -- on a batch shape no other
    // shipped model has (1014-asset scan) and with all three rigs covered.
    // That makes vertex SOURCING the open question, so both verdicts record
    // the buffer and offset they read, and a log can say whether the two
    // differ.
    unsigned lastRefBuffer, lastRefOffset;
    unsigned lastOkBuffer,  lastOkOffset;
    // Rigid-invariant shape signature of each verdict's observed cloud (see
    // VRHandPoseSample::shape). Comparable across animation states, unlike the
    // raw positions, so one heartbeat line answers "same geometry, wrong
    // correspondence" vs "different geometry entirely".
    float    lastRefShape, lastOkShape;
    // The SAME recovery, before PoseToEyeSpace: the pose in the space the
    // vertices were SUBMITTED in. Both eyes of a latched pair are handed the
    // same authored viewmodel, so this is the ONE frame in which the two eyes'
    // recoveries are directly comparable with no inter-eye correction and no
    // assumption about whether the engine's viewmodel modelview carries the
    // eye offset. It is what the pair-eligibility check compares.
    float    submPos[3];
    float    submAxis[9];
};
int GLSurf_GetHandSolve(int cls, GLSurfHandSolve* out);

// The ENGAGEMENT baseline: the last successful recovery, provided it is
// recent. The 2026-08-28 hardware run proved the raw `valid` flag is not an
// engagement signal: roughly every other hand-solve ATTEMPT refuses in real
// gameplay, so "the very last attempt succeeded" reads false at almost every
// pair boundary and the hands never engage even though fresh good solves
// exist. A refusal must not un-know a solve that already happened; only AGE
// may. Returns 1 and fills `out` with the last-good data when that solve is
// at most GLS_BASELINE_FRESH_SCENES old; 0 otherwise (never engaged, or the
// hand really has stopped solving -- weapon lowered, cinematic, menu).
enum { GLS_BASELINE_FRESH_SCENES = 4 };   // 2 XR pairs of eye-scenes
int GLSurf_GetHandBaseline(int cls, GLSurfHandSolve* out);

// ------------------------------------------- pair-level stereo eligibility
//
// THE HALF-NATIVE PAIR, AND WHY IT IS NOT ALLOWED TO EXIST.
//
// The palm solve reads each eye's OWN draw, so the two eyes can disagree about
// whether a hand is recoverable -- and with any real refusal rate they will,
// roughly as often as a coin. A build that acts on each eye's answer as it
// arrives puts one eye's hand on the controller and the other eye's on the
// weapon. That is not a cosmetic glitch: it is two irreconcilable images of
// one hand, the same class of stereo fault as the 2026-08-23 moving-camera
// build that caused nausea.
//
// So native ownership is a property of the PAIR. The wiring gathers BOTH eyes'
// authored baselines at the pair boundary, checks they describe the same
// physical hand, and opens this gate once for the whole pair. Until it does, a
// hand delta is refused however well this eye's own solve went -- fail-closed,
// default 0, and reset by GLSurf_Reset.
void     GLSurf_SetHandPairEligible(int cls, int on);
int      GLSurf_HandPairEligible(int cls);
// Deltas the gate refused. A nonzero value with engagement on means the two
// eyes disagreed and the pair correctly fell back to vanilla on BOTH.
unsigned GLSurf_HandPairGateRefusals(int cls);

// The last authored recovery from ONE eye's own draws, subject to the same
// freshness rule as GLSurf_GetHandBaseline. This is the evidence the pair
// gate is computed from; a single shared slot cannot answer "did both eyes
// see the same hand".
int GLSurf_GetHandBaselineEye(int cls, int eye, GLSurfHandSolve* out);

// Has this eye actually RENDERED this class recently? An eye that never draws
// it poses no stereo hazard and is not required to agree about it -- a
// monoscopic context (the flat probe, or the game before VR engages) must not
// be refused for lacking a second eye. An eye that DOES draw it and produces
// no usable baseline is the half-native hazard, and fails the pair.
int GLSurf_HandEyeParticipating(int cls, int eye);

// Refusal-shape histogram per hand class: which (indexCount, vertexSpan)
// draw shapes are refusing, so one flat run names the refusing batch.
enum { GLS_REFUSE_BUCKETS = 4 };
struct GLSurfRefuseBucket { int count; int span; unsigned hits; };
// Returns the number of used buckets; `overflow` gets the refusals that
// matched no bucket once all buckets were taken.
int GLSurf_GetRefuseHisto(int cls, GLSurfRefuseBucket out[GLS_REFUSE_BUCKETS],
                          unsigned* overflow);

// ---- the gun on the right controller --------------------------------------
//
// The desired REAR-GRIP pose, installed once per pair by vr_native_hands in
// the same head-relative space the hand targets use. Unlike the hands (whose
// targets are RELATIVE offsets), the gun target is the ABSOLUTE desired
// rear-grip pose: the gun body's authored pose is re-solved fresh from each
// eye's own vertices, so the applied delta is per-eye while the physical
// object pose is pair-invariant -- the same invariant the hands carry.
//
// ONE rigid pose per scene: the delta is solved once, from the batch whose
// vertex count keys a shipped gun model, and every other gun-class batch of
// that scene reuses it, so all the weapon's surfaces move as one body.
// Unknown weapon / WEAK rear grip / failed solve: nothing is transformed --
// the vanilla weapon is always the fallback, never an invented pose.
void GLSurf_SetGunTarget(int enabled, const float desiredGripPosU[3],
                         const float desiredAxis[9]);

struct GLSurfGunSolve {
    unsigned solves;              // scene deltas built
    unsigned refusals;            // keyed batch seen but no usable delta
    unsigned refusedUnknownBytes; // ...of which: landmark bytes undefined
    int      lastReason;          // VRHP_REASON_* of the last keyed solve
    unsigned batchesTransformed;  // gun draws issued under a scene delta
    unsigned batchesVanilla;      // gun draws issued untouched with target on
    int      lastVertexCount;     // the keyed MODEL's body key (weapon identity)
    int      keySurface;          // asset surface index of the last batch that keyed
    int      keyBatchVerts;       // that batch's own vertex span
    // PRE-KEY accounting (brief 2026-09-04 P0): gun batches drawn before the
    // scene's body keyed. stale = placed by the carried previous-scene
    // baseline; suppressed = not drawn (no trustworthy baseline). Neither is
    // ever vanilla. staleErrMax* = worst measured deviation of a carried
    // baseline from the scene's fresh delta (the one-frame animation advance).
    unsigned batchesStale;
    unsigned batchesPartKeyed;    // pre-key parts placed from their own rest table
    unsigned batchesPartRefused;  // ...parts that were mid-animation, so hidden
    unsigned batchesSuppressed;
    int      pivotIsBodyOrigin;   // this weapon has no STRONG rear grip (grenades)
    unsigned staleScenes;
    unsigned ownerConflicts;      // eye-scenes with native AND vanilla weapon draws
    unsigned keyMisses;           // key-usable counts that did not reproduce (collisions, unreadable)
    float    staleErrMaxU;
    float    staleErrMaxDeg;
    float    lastRmsU;            // last body solve residual, engine units
    float    rearGripErrU;        // |delta(rearGrip) - target| after solve
    int      valid;               // the current scene has a usable delta
    // The AUTHORED gun as last recovered from real vertices -- the baseline
    // vr_native_hands composes the controller target against, exactly as
    // the hands' GLSurf_GetHandSolve baseline works. Submitted-space units.
    // EYE SPACE, like every other pose this layer publishes: the solve reads
    // submitted vertices and is mapped through the batch's own modelview at
    // the solve site (PoseToEyeSpace), because the delta built from it is
    // applied as delta*modelview and the controller target it must reach is
    // eye space too.
    int      haveBody;
    unsigned bodyScene;           // GLSurf_SceneFrameCount() at that solve;
                                  // consumers gate on freshness like the
                                  // hands' baseline (GLS_BASELINE_FRESH_SCENES)
    unsigned bodySerial;          // ++ on every successful body solve
    float    bodyPosU[3];
    float    bodyAxis[9];
    float    pivotU[3];           // the authored rear grip, EYE space
};
void GLSurf_GetGunSolve(GLSurfGunSolve* out);
int  GLSurf_GunVertexCount();     // last keyed gun batch span; 0 = none yet

// ---- per-eye-scene weapon draw accounting (brief 2026-09-04, sections 3-4)
//
// Every gun-class draw of an eye-scene is attributed to exactly one
// disposition. NATIVE: the scene's one fresh delta. STALE: a pre-key batch
// placed by the carried previous-scene baseline. SUPPRESSED: a pre-key batch
// with nothing trustworthy to place it by, not drawn. VANILLA: drawn as the
// engine placed it (only when the scene's weapon is refused as a whole, or
// under the test mutant). weaponOwnerConflict = native/stale AND vanilla in
// one eye-scene -- the two-weapon image; the pre-submit verdict refuses it.
// PARTKEY: a pre-key part that solved the body pose from its OWN rest table
// and was drawn on that (correct for this scene, self-validating -- see
// VRGunPose_SolveSurfaceEx). STALE: the carried previous-scene delta, which
// the 2026-09-04d hardware run measured drifting up to 4.5 u / 20.5 deg and
// which is no longer the default.
enum { GLS_GDISP_NATIVE = 1, GLS_GDISP_STALE = 2, GLS_GDISP_SUPPRESSED = 3,
       GLS_GDISP_VANILLA = 4, GLS_GDISP_PARTKEY = 5 };
enum { GLS_GUN_DISP_MAX = 12 };
struct GLSurfGunScene {
    unsigned scene;
    unsigned serial;              // latch serial (pair) the scene rendered under
    int      eye;
    int      modelKey;            // body key of the weapon (0 = none identified)
    unsigned deltaId;             // unique id of this scene's fresh delta (0 = none)
    int      weaponDrawSeen;
    int      weaponDrawNative;
    int      weaponDrawStale;
    int      weaponDrawPartKeyed;
    int      weaponDrawSuppressed;
    int      weaponDrawVanilla;
    int      weaponOwnerConflict;
    int      nDisp;
    int      dispVerts[GLS_GUN_DISP_MAX];   // per batch, in draw order
    signed char disp[GLS_GUN_DISP_MAX];     // GLS_GDISP_*
    float    staleErrU, staleErrDeg;        // -1 unless both stale and fresh happened
};
int  GLSurf_GetGunScene(int eye, GLSurfGunScene* out);   // latest record for that eye
// Cumulative per weapon (by body key), for the ownership table.
struct GLSurfWeaponStats {
    int      modelKey;
    unsigned scenes, draws, native, stale, partKeyed, suppressed, vanilla, conflicts;
};
int  GLSurf_WeaponStatsCount();
int  GLSurf_GetWeaponStats(int i, GLSurfWeaponStats* out);
// Pre-key FALLBACK policy, used only when the part cannot key itself:
// 0 = suppress (default since 2026-09-04d), 1 = carry the previous scene's
// delta. Never vanilla.
void GLSurf_SetGunPreKeyPolicy(int policy);
int  GLSurf_GunPreKeyPolicy();
// Test-only mutant: the N-th gun batch of every scene stays vanilla (-1 off).
void GLSurf_TestForceGunBatchVanilla(int ordinal);

// Keep the gun body solve running on every keyed gun batch even while no
// target is installed -- the same deadlock-breaker the hands needed: the
// target composition needs an authored baseline, and the baseline only
// exists if something solves before engagement.
void GLSurf_SetGunSolveWanted(int on);

// Serial of the latch the CURRENT deltas belong to. Both eyes of a pair must
// consume the same serial; a mismatch is what test 19-23 assert on.
void     GLSurf_BeginLatch(unsigned serial);
unsigned GLSurf_LatchSerial();
unsigned GLSurf_LatchCount();
// Per-eye record of which latch serial that eye's draws consumed.
unsigned GLSurf_EyeConsumedSerial(int eye);
unsigned GLSurf_PairMismatchCount();

// A merged hand/sleeve batch is only split when THIS frame's index buffer
// is proved to partition at the vertex midpoint. Rejected batches are drawn
// exactly as the engine issued them.
// Draws declined because the layer was already inside one. Must stay 0
// in production: a nonzero value means a re-issued draw came back
// through the interception, which is a wiring mistake, not a workload.
unsigned GLSurf_ReentryCount();

unsigned GLSurf_SplitVerifiedCount();
unsigned GLSurf_SplitRejectedCount();

// 1 -- the sleeve surfaces are ONE PER ARM. Re-derived 2026-08-29 from skin
// weights and bone ancestry (tools/derive_render_ownership.py): surface 2's
// 120 vertices are 100% weighted to bones under `bip01 r upperarm`, surface
// 3's 120 to `bip01 l upperarm`, with zero cross-arm vertices and zero seam
// triangles. The 2026-08-24 "71% pixel overlap -> one arm" reading is
// superseded: two forearms at the bottom of a viewmodel frame overlap on
// screen whichever arms they belong to, so that metric could not separate the
// hypotheses. A sleeve therefore takes its OWN arm's motion.
int GLSurf_SleeveSplitIsAnatomical();

// How each transformed sleeve draw got its motion: `palm` counts sleeves that
// took their arm's hand delta solved by THIS eye THIS scene (the forearm
// stays rigidly attached to the hand). `centroid` is kept for the log format
// and is always 0 now: a sleeve whose hand did not solve this scene stays
// vanilla rather than turning about its own centre while the hand it belongs
// to is vanilla -- the pair verdict then decides the pair.
void GLSurf_GetSleeveFollowCounts(unsigned* palm, unsigned* centroid);

// ---- the sleeves follow the wrist RIGIDLY (brief 2026-09-04, P1) ----------
//
// The sleeve is placed by ONE rigid motion about the anatomical wrist: the
// authored wrist point (the palm solve's origin, exact) goes to the desired
// palm origin, and the drawn forearm's orientation (solved from the
// forearm-dominated sleeve vertices, vr_sleeve_landmarks.inc) is turned to
// DesiredPalm * RestRelation -- the forearm continuing the knuckle
// direction (a straight wrist), roll from the idle animations. Never the
// sleeve's own centroid, never the camera, never the gun frame. When the
// forearm cannot be solved from a draw, the sleeve takes its hand's delta
// (the previous behaviour: connected, with the animation's wrist bend).
//
// VISIBILITY POLICY: a sleeve whose elbow would land at or behind the eye
// plane, or within GLS_SLEEVE_HEAD_CLEAR_U of the eye, is not drawn.
void GLSurf_SetSleeveAnatomical(int on);      // default 1
int  GLSurf_SleeveAnatomical();
// WHICH SLEEVES ARE DRAWN AT ALL (brief 2026-09-04d, P5). The 2026-09-04d
// hardware run still called the LEFT forearm disconnected, and a forearm
// crossing the view is worse than no forearm: the hand itself is useful and
// stays. 2 = both, 1 = the RIGHT sleeve only (default), 0 = neither. The
// HAND is never hidden by this.
void GLSurf_SetSleeveVisible(int mode);
int  GLSurf_SleeveVisible();
void GLSurf_SetSleeveHideNearHead(int on);    // default 1
int  GLSurf_SleeveHideNearHead();
enum { GLS_SLEEVE_HEAD_CLEAR_U = 6 };         // 15 cm
struct GLSurfSleeveStats {
    unsigned followedForearm;   // placed by the wrist-pivot / forearm solve
    unsigned followedPalm;      // fallback: the hand's delta
    unsigned hiddenNearHead;    // visibility policy applied
    unsigned hiddenByPolicy;    // hidden because vr_sleeve_visible excludes it
    unsigned solveRefusals;     // forearm solves that failed their gate
    float    lastRmsM;          // last forearm solve residual (metres)
    int      lastArm;           // GLSC_SLEEVE_R / _L of the last placement
    float    lastElbowEye[3];   // desired elbow, eye space, last placement
    float    lastForearmAxis[9];// desired forearm rows, last placement
};
void GLSurf_GetSleeveStats(GLSurfSleeveStats* out);

// Screen-pixel movement the transform produced, measured on REAL SAMPLED
// VERTICES of the batch: each is projected through the engine's modelview and
// through ours, and samples that are not in front of the near plane in both
// are rejected rather than divided by. This is the A/B evidence that does not
// depend on a screenshot, and therefore cannot be confused by one frame of
// idle animation.
//
// It replaces an origin-projection metric that reported 146004688 px for a
// valid 10 cm transform. Four of the seven viewmodel draws arrive already
// skinned into camera-relative coordinates, so their modelview translation is
// ~2e-5 rather than 0: the batch "origin" sits on the eye, clip w collapses,
// and a 1e-6 guard does not catch it. An origin is never a safe witness for
// where a surface went; the surface is.
struct GLSurfShift {
    float    medianPx;        // median displacement over this draw's samples
    float    p95Px;
    float    maxPx;           // worst single sample seen since reset
    float    lastX, lastY;    // signed mean displacement of the last draw
    unsigned validSamples;
    unsigned rejectedNearW;   // samples dropped for being at/behind the near plane
    unsigned draws;           // transformed draws that produced a sample
};
void GLSurf_GetShiftMetrics(GLSurfShift* out);
void GLSurf_GetShift(float* lastX, float* lastY, float* maxPx, unsigned* samples);

// The RETIRED origin-based diagnostic, exported ONLY so the suite can show it
// blowing up on the draws the real metric handles. Not called in shipping code.
float GLSurf_TestOriginShiftX(const float mv[16], const float pj[16], const int vp[4]);

// --------------------------------------------------------------- suppression
// Hide a class entirely (the sleeves, when native hands are on).
void GLSurf_SetClassHidden(int cls, int hidden);
int  GLSurf_ClassHidden(int cls);

// --------------------------------------------------------------- statistics
struct GLSurfStats {
    unsigned drawsSeen;
    unsigned drawsScene;
    unsigned drawsDepthHacked;
    unsigned classified[GLSC_COUNT];
    unsigned transformed;
    unsigned suppressed;
    unsigned matrixRestores;
    unsigned rejectedNoDriver;
    unsigned rejectedAmbiguous;
    // Why a client-array read was refused. Split by reason because "the layer
    // declined" and "the engine is using vertex buffer objects" are different
    // facts, and only the second one explains a crash.
    unsigned rejectedNullPage;    // "pointer" below 64 KB: a VBO byte offset
    unsigned rejectedUnmapped;    // address not committed and readable
    unsigned rejectedWildIndex;   // index outside anything a viewmodel has
    unsigned probeQueries;        // VirtualQuery calls actually made
    unsigned probeCacheHits;
    // Buffer-object sourcing (vr_vbo). Resolves served from the CODVR shadow,
    // and refusals when the shadow could not vouch for the range -- the
    // counters that say whether the viewmodel's geometry is actually reaching
    // the pivot/palm features on a VBO-drawing engine.
    unsigned vboVertResolves;
    unsigned vboVertMisses;
    unsigned vboIdxResolves;
    unsigned vboIdxMisses;
    unsigned rejectedVboScratch;  // scratch route refused: arrays live in a VBO
};
void GLSurf_GetStats(GLSurfStats* out);

// ------------------------------------------------------------- class pivots
//
// Where a surface IS, in eye space: the centroid of a sample of the vertices
// its draws reference, updated every frame the class is drawn.
//
// This is not readable from the modelview. Three of the five gun batches
// carry a real model->eye matrix, but the hands, the sleeves and the other two
// gun batches arrive already skinned into camera-relative coordinates -- pure
// axis-swap modelview, translation ~1e-5. Their authored position is in the
// vertices, which is also where the 146004688 px shift reading came from.
//
// Returns 0 until something of that class has been drawn.
int  GLSurf_ClassPivot(int cls, float outEye[3]);

// out = T(pivot) * Ryaw * T(-pivot) * T(d): spin the surface where it stands,
// then translate. A bare rotation about eye-space zero swings a viewmodel
// around the player's head instead of turning the weapon.
void GLSurf_BuildDeltaAboutPivot(const float pivotEye[3], float yawDeg,
                                 float dx, float dy, float dz, float out[16]);

// ------------------------------------------------------------------ probing
//
// The flat A/B probe (Phase 2): apply a known transform to ONE class and
// nothing else, so a screenshot pair can be scored automatically.
void GLSurf_SetProbe(int cls, float dxUnits, float dyUnits, float dzUnits, float yawDeg);
int  GLSurf_ProbeClass();
// Whether the probe found a pivot for its class, or had to fall back to
// rotating about the camera. The log says which, so a yaw stage is never
// silently the wrong experiment.
int  GLSurf_ProbePivotWasKnown();

// ---------------------------------------------------------- test-only hooks
// The last modelview the layer loaded for a transformed draw, and the matrix
// it restored afterwards -- the evidence for "engine state was put back".
int  GLSurf_TestLastTransformed(float loaded[16], float restored[16]);
int  GLSurf_TestClassOfLastDraw();
void GLSurf_TestForceScenePhase(int inScene);
int  GLSurf_TestSleeveNearest(unsigned type, const void* idx, int count, float* dR, float* dL);
void GLSurf_TestSetDepthRange(double zn, double zf);
