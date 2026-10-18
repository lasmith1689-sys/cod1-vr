// Pair-latched object poses: the gun and each hand, sampled ONCE per XR pair.
//
// WHY THIS EXISTS
//
// The camera already works this way, and it had to be learned the hard way:
// the 2026-08-23 build sampled a MOVING camera between the two game frames of
// an XR pair, so the eyes saw two different worlds and the result was nausea.
// vr.cpp fixed that by latching one base camera at eye 0 and forbidding eye 1
// from seeing the live game state at all.
//
// Viewmodel objects need exactly the same discipline for exactly the same
// reason. A controller moves continuously; the two eyes of a pair are rendered
// on two separate game frames. If eye 0 draws the gun where the controller was
// at t0 and eye 1 draws it where it was at t1, the gun has stereo disparity
// that does not correspond to any depth, and the viewer's eyes are asked to
// converge on an object that is not anywhere.
//
// So: one sample, at the start of the pair, consumed unchanged by both eyes.
// Every pose carries the serial of the latch it belongs to, and a mismatch is
// counted rather than silently rendered.
//
// ---------------------------------------------------------------------------
// THE STEREO RULE, which is not obvious and is easy to get backwards
//
// The GL modelview the engine loads for a viewmodel draw already contains that
// EYE's view transform, so "eye space" is a different space for eye 0 and eye
// 1 -- the two differ by the interpupillary offset. The consequence splits in
// two, and the halves behave differently:
//
//   TRANSLATION is eye-invariant. The two eye bases are PARALLEL (same
//   orientation, offset laterally), so the same numeric translation in each
//   eye's coordinates is the same displacement in the world. A pair-latched
//   translation may therefore be handed to both eyes unchanged.
//
//   A PIVOT is NOT eye-invariant. A point's coordinates differ between the
//   eyes by that same offset, so reusing one numeric pivot for both eyes would
//   rotate the object about two different world points -- disparity again, of
//   the kind that has no depth. The pivot must be re-derived per eye, from
//   that eye's own draw. GLSurf_ClassPivot() does exactly that: it is the
//   centroid of the vertices under the engine's own per-eye modelview, so it
//   is automatically correct for whichever eye is being drawn.
//
// Hence the split enforced here: this module latches ROTATION and OFFSET, both
// eye-invariant, and never latches a pivot.
//
// TESTABILITY. Nothing here calls OpenXR, GL or a clock. The caller hands in
// controller poses; the module only decides what a pair is allowed to see.

#pragma once

enum { VROBJ_HAND_R = 0, VROBJ_HAND_L = 1, VROBJ_HANDS = 2 };

// What the left hand is doing. Ordered so that > VROBJ_LEFT_SUPPORT means
// "busy with a reload gesture" without listing every state.
enum {
    VROBJ_LEFT_FREE = 0,      // 6DoF, follows its controller, touches nothing
    VROBJ_LEFT_SUPPORT,       // attached to the weapon's foregrip
    VROBJ_LEFT_RELOAD_REACH,
    VROBJ_LEFT_RELOAD_GRABBED,
    VROBJ_LEFT_AMMO_ACQUIRE,
    VROBJ_LEFT_INSERT,
    VROBJ_LEFT_COMMIT,
    VROBJ_LEFT_RECOVER,
    VROBJ_LEFT_STATES
};

// A rigid pose. `axis` is 3x3 row-major in the same convention vr_math uses
// for camera bases: rows are forward / left / up.
struct VRObjPose {
    float pos[3];
    float axis[9];
    int   valid;
};

// Everything a frame is allowed to know about where the objects are. Both
// eyes of a pair read this struct and get identical bytes.
struct VRObjectSet {
    unsigned pairSerial;          // the latch this set belongs to
    int      valid;               // a latch has happened at all

    VRObjPose head;               // HMD, at latch time
    VRObjPose grip[VROBJ_HANDS];  // raw controller grip poses, at latch time
    VRObjPose hand[VROBJ_HANDS];  // where each hand SHELL should render
    VRObjPose gun;                // solved weapon pose

    int      handTracked[VROBJ_HANDS];
    int      leftState;           // VROBJ_LEFT_*
    int      twoHand;             // left is attached to the foregrip
    float    supportT;            // 0..1 along the legal foregrip interval
    float    supportResidualCm;

    // Per-object serials. All must equal pairSerial or the set is inconsistent
    // and must not be rendered.
    unsigned gunSerial;
    unsigned handSerial[VROBJ_HANDS];
};

// ------------------------------------------------------------- lifecycle

void VRObj_Reset();

// Open a new pair. Call ONCE, at eye 0, with the pair's serial. Everything
// sampled after this and before the pair closes belongs to this serial.
void VRObj_BeginPair(unsigned pairSerial);

// Feed the latch. These are no-ops unless a pair is open AND this is the
// first sample of that pair -- which is what makes "controller motion between
// the eyes cannot reach the objects" a property of the code rather than a
// convention the caller has to remember.
void VRObj_LatchHead(const float pos[3], const float axis[9], int valid);
void VRObj_LatchGrip(int hand, const float pos[3], const float axis[9],
                     int tracked);
void VRObj_LatchLeftState(int state, int twoHand, float supportT,
                          float residualCm);

// Publish the solved poses for this pair. Stamps the serials.
void VRObj_SetHandPose(int hand, const float pos[3], const float axis[9]);
void VRObj_SetGunPose(const float pos[3], const float axis[9]);
// A disengaged pair must publish the ABSENCE of a pose (valid=0, serial
// refreshed); leaving last pair's pose marked valid is the stale-pose
// inheritance PairConsistent exists to catch.
void VRObj_ClearHandPose(int hand);
void VRObj_ClearGunPose();

// ------------------------------------------------------------- consumption

// The latched set. Never changes between the eyes of a pair.
const VRObjectSet* VRObj_Set();

// Record that `eye` consumed the set, and check it against the open pair.
void     VRObj_NoteEyeConsumed(int eye);
unsigned VRObj_EyeConsumedSerial(int eye);

// Every serial in the set equals pairSerial.
int VRObj_PairConsistent();

// Diagnostics. mismatches counts eyes that consumed a set whose serials did
// not agree; lateSamples counts latch calls that arrived after the pair was
// already sampled (which are refused, not applied).
void VRObj_GetCounters(unsigned* pairs, unsigned* mismatches,
                       unsigned* lateSamples, unsigned* eyeSerialSplits);
