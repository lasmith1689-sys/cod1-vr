// ===========================================================================
// Pair-latched object poses (src/vr_objects.cpp).
//
// The camera learned this lesson in 2026-08-23: sampling a moving source
// between the two game frames of an XR pair gives the eyes two different
// worlds. These cases assert that a controller CANNOT do the same thing to
// the gun or the hands -- not by convention, but because the second sample
// inside a pair is refused.
// ===========================================================================

#include "../src/vr_objects.h"

static void ObjAxis(float a[9], float yawTurns)
{
    // Rows: forward / left / up, matching the camera-basis convention.
    const float c = cosf(yawTurns * 6.28318530718f);
    const float s = sinf(yawTurns * 6.28318530718f);
    a[0] =  c; a[1] = s; a[2] = 0;
    a[3] = -s; a[4] = c; a[5] = 0;
    a[6] =  0; a[7] = 0; a[8] = 1;
}

static void S_ObjectPairLatch()
{
    printf("--- object poses: one latch per XR pair ---\n");

    // ---- the second sample inside a pair is REFUSED ----------------------
    VRObj_Reset();
    VRObj_BeginPair(1);
    float a0[9]; ObjAxis(a0, 0.0f);
    const float p0[3] = { 1.0f, 2.0f, 3.0f };
    VRObj_LatchGrip(VROBJ_HAND_R, p0, a0, 1);

    // The controller keeps moving between the eyes. It must not get through.
    float a1[9]; ObjAxis(a1, 0.25f);
    const float p1[3] = { 99.0f, 99.0f, 99.0f };
    VRObj_LatchGrip(VROBJ_HAND_R, p1, a1, 1);

    const VRObjectSet* s = VRObj_Set();
    Check(fabsf(s->grip[VROBJ_HAND_R].pos[0] - 1.0f) < 1e-5f &&
          fabsf(s->grip[VROBJ_HAND_R].pos[1] - 2.0f) < 1e-5f &&
          fabsf(s->grip[VROBJ_HAND_R].pos[2] - 3.0f) < 1e-5f,
          "controller motion BETWEEN the eyes cannot reach the latched pose "
          "(%.1f, %.1f, %.1f)", s->grip[VROBJ_HAND_R].pos[0],
          s->grip[VROBJ_HAND_R].pos[1], s->grip[VROBJ_HAND_R].pos[2]);
    Check(memcmp(s->grip[VROBJ_HAND_R].axis, a0, sizeof(a0)) == 0,
          "...and neither can its rotation");
    unsigned late = 0;
    VRObj_GetCounters(NULL, NULL, &late, NULL);
    Check(late == 1, "the refused sample is COUNTED, not silently dropped (%u)", late);

    // ---- consuming an eye SEALS the pair ---------------------------------
    //
    // Refusing a repeat of an already-sampled slot is not enough by itself: a
    // slot nobody had filled yet -- a controller that only started tracking
    // after eye 0 -- would still be a way in. Once an eye has rendered from
    // the set, every slot must be shut.
    VRObjectSet eye0 = *VRObj_Set();
    VRObj_NoteEyeConsumed(0);
    VRObj_LatchGrip(VROBJ_HAND_L, p1, a1, 1);      // a hand that was NEVER
    VRObj_LatchHead(p1, a1, 1);                    // sampled in this pair
    VRObjectSet eye1 = *VRObj_Set();
    VRObj_NoteEyeConsumed(1);
    Check(eye1.grip[VROBJ_HAND_L].valid == 0,
          "a hand that starts tracking mid-pair cannot enter it either");
    Check(memcmp(&eye0, &eye1, sizeof(VRObjectSet)) == 0,
          "eye 0 and eye 1 read BYTE-IDENTICAL object sets");
    Check(VRObj_EyeConsumedSerial(0) == VRObj_EyeConsumedSerial(1),
          "both eyes consumed the same latch serial");
    unsigned splits = 0;
    VRObj_GetCounters(NULL, NULL, NULL, &splits);
    Check(splits == 0, "no eye-serial split on a clean pair");

    // ---- a new pair DOES take a fresh sample ------------------------------
    VRObj_BeginPair(2);
    VRObj_LatchGrip(VROBJ_HAND_R, p1, a1, 1);
    s = VRObj_Set();
    Check(fabsf(s->grip[VROBJ_HAND_R].pos[0] - 99.0f) < 1e-5f,
          "the NEXT pair samples afresh (%.1f)", s->grip[VROBJ_HAND_R].pos[0]);
    Check(s->pairSerial == 2, "the set carries the new serial");

    // ---- serials: a stale solved pose is caught --------------------------
    VRObj_Reset();
    VRObj_BeginPair(7);
    VRObj_SetGunPose(p0, a0);
    VRObj_SetHandPose(VROBJ_HAND_R, p0, a0);
    VRObj_SetHandPose(VROBJ_HAND_L, p0, a0);
    Check(VRObj_PairConsistent() == 1,
          "a pair whose poses were all solved this pair is consistent");
    Check(VRObj_Set()->gunSerial == 7 && VRObj_Set()->handSerial[0] == 7 &&
          VRObj_Set()->handSerial[1] == 7,
          "every object serial equals the pair serial");

    VRObj_BeginPair(8);
    VRObj_SetHandPose(VROBJ_HAND_R, p1, a1);       // only the right re-solved
    Check(VRObj_PairConsistent() == 0,
          "a pose left over from the PREVIOUS pair makes the set inconsistent");
    VRObj_NoteEyeConsumed(0);
    unsigned mism = 0;
    VRObj_GetCounters(NULL, &mism, NULL, NULL);
    Check(mism == 1, "the inconsistent set is counted when an eye consumes it");

    VRObj_SetGunPose(p1, a1);
    VRObj_SetHandPose(VROBJ_HAND_L, p1, a1);
    Check(VRObj_PairConsistent() == 1,
          "re-solving the stragglers makes the pair consistent again");

    // ---- solving outside a pair does not forge a serial -------------------
    VRObj_Reset();
    VRObj_BeginPair(3);
    VRObj_SetGunPose(p0, a0);
    VRObj_NoteEyeConsumed(0);
    VRObj_NoteEyeConsumed(1);
    // Now the pair is over in the caller's mind but no new one has begun.
    // A solve arriving here keeps serial 3, so the NEXT pair sees it as stale.
    VRObj_BeginPair(4);
    Check(VRObj_PairConsistent() == 0,
          "a pose carried over into a new pair is not silently accepted");

    // ---- left-hand state is latched too ----------------------------------
    VRObj_Reset();
    VRObj_BeginPair(1);
    VRObj_LatchLeftState(VROBJ_LEFT_SUPPORT, 1, 0.4f, 1.5f);
    VRObj_LatchLeftState(VROBJ_LEFT_FREE, 0, 0.0f, 0.0f);   // mid-pair change
    s = VRObj_Set();
    Check(s->leftState == VROBJ_LEFT_SUPPORT && s->twoHand == 1,
          "the left hand cannot change mode between the eyes of one pair");
    Check(fabsf(s->supportT - 0.4f) < 1e-5f,
          "...nor can its position along the support interval");

    // ---- tracking loss is carried, not hidden ----------------------------
    VRObj_Reset();
    VRObj_BeginPair(1);
    VRObj_LatchGrip(VROBJ_HAND_L, p0, a0, 0);      // not tracked
    VRObj_LatchGrip(VROBJ_HAND_R, p0, a0, 1);
    s = VRObj_Set();
    Check(s->handTracked[VROBJ_HAND_L] == 0 && s->handTracked[VROBJ_HAND_R] == 1,
          "per-hand tracking validity survives the latch");

    // ---- an out-of-range hand index is ignored, not written past ---------
    VRObj_Reset();
    VRObj_BeginPair(1);
    VRObj_LatchGrip(5, p1, a1, 1);
    VRObj_SetHandPose(-1, p1, a1);
    Check(VRObj_Set()->hand[0].valid == 0 && VRObj_Set()->hand[1].valid == 0,
          "an out-of-range hand index writes nothing");
}
