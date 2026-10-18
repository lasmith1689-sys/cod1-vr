// ===========================================================================
// OBJECT INDEPENDENCE -- calibration, LEFT_FREE, and one-hand aiming lag
// ===========================================================================
//
// The three-object separation gate proves the RENDER MATRICES are independent.
// These cases prove the DERIVATION is too, which is a different claim and the
// one the brief separates out: a build can hand three objects three distinct
// matrices and still compute two of them from a third, so that moving one
// silently drags another. The gun must not be the hand's child in the maths
// any more than in the renderer.

// The one-hand hold every case below starts from: hands + weapon authored at
// known poses, two pairs so the baselines seed and then engage.
struct IndepRig {
    const CodvrGunModel* gun;
    float rAxis[9], lAxis[9], gAxis[9];
    float rPos[3], lPos[3], gPos[3];
    float headPos[3], headAxis[9];
    VRInputState in;
    unsigned serial;
    double   now;
};

static void IndepGripQuatYaw(float degrees, float q[4])
{
    const float half = degrees * 0.5f * 0.01745329252f;
    q[0] = 0.0f; q[1] = sinf(half); q[2] = 0.0f; q[3] = cosf(half);
}

// One pair: both eye-scenes with their draws, then the next pair boundary.
static void IndepPair(IndepRig* R)
{
    for (int eye = 0; eye < 2; ++eye) {
        NHScene(eye);
        NHBuildHandVerts(R->rAxis, R->rPos, R->lAxis, R->lPos);
        NHDriveHandDraw();
        NHDriveGunDraw(R->gun, R->gAxis, R->gPos);
    }
    R->now += 1.0 / 120.0;
    VRNH_OnPairBegin(++R->serial, &R->in, R->headPos, R->headAxis, 1, R->now);
}

static int IndepSetup(IndepRig* R, int leftTracked)
{
    memset(R, 0, sizeof(*R));
    R->gun = VRGunPose_ByVertexCount(326);            // thompson body
    if (!R->gun) return 0;
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, R->rAxis);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, R->lAxis);
    HpAxisFromEuler(2.0f, 14.0f, 1.0f, R->gAxis);
    const float rp[3] = { 0.20f, -0.05f, 0.55f };
    const float lp[3] = { -0.20f, -0.05f, 0.55f };
    const float gp[3] = { 5.0f, -4.0f, -13.0f };
    memcpy(R->rPos, rp, sizeof(rp));
    memcpy(R->lPos, lp, sizeof(lp));
    memcpy(R->gPos, gp, sizeof(gp));
    NHIdentityAxis(R->headAxis);

    NHResetForCase();
    VRNH_SetEnabled(1);
    VRNH_SetTwoHandEnabled(0);          // LEFT_FREE: nothing may capture it
    VRNH_SetReloadEnabled(0);

    const float gripXr[3] = { 0.0f, 0.0f, -0.30f };
    const float qI[4] = { 0, 0, 0, 1 };
    R->in.hand[VRI_RIGHT] = NHHand(1, gripXr, qI);
    R->in.hand[VRI_LEFT]  = NHHand(leftTracked, gripXr, qI);
    R->serial = 20;
    R->now = 0.0;
    VRNH_OnPairBegin(R->serial, &R->in, R->headPos, R->headAxis, 1, R->now);
    IndepPair(R);        // seed
    IndepPair(R);        // engage
    return 1;
}

static int IndepPoseSame(const VRObjPose& a, const VRObjPose& b, float posTol,
                         float rotTol)
{
    if (a.valid != b.valid) return 0;
    for (int k = 0; k < 3; ++k) if (fabsf(a.pos[k] - b.pos[k]) > posTol) return 0;
    for (int k = 0; k < 9; ++k) if (fabsf(a.axis[k] - b.axis[k]) > rotTol) return 0;
    return 1;
}

static float IndepPosDelta(const VRObjPose& a, const VRObjPose& b)
{
    float d = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float r = a.pos[k] - b.pos[k];
        d += r * r;
    }
    return sqrtf(d);
}

// ===========================================================================
static void S_CalibrationIndependence()
{
    printf("--- calibration independence: hand and gun are coordinated, not "
           "parent/child ---\n");
    IndepRig R;
    if (!IndepSetup(&R, 0)) { CHECK(0, "thompson body table missing"); return; }

    VRNHDebug d; VRNH_GetDebug(&d);
    CHECK(d.hand[VRI_RIGHT].engaged == 1 && d.gun.targetInstalled == 1,
          "indep: one-hand hold established (hand %s, gun %s)",
          VRNH_HandReasonName(d.hand[VRI_RIGHT].reason),
          VRNH_GunReasonName(d.gun.reason));

    const VRObjPose hand0 = VRObj_Set()->hand[VROBJ_HAND_R];
    const VRObjPose gun0  = VRObj_Set()->gun;
    CHECK(hand0.valid && gun0.valid, "indep: both objects published a pose");

    // ---- A: change the RIGHT PALM calibration ONLY ----------------------
    //
    // The controller has not moved. Only the operator's idea of where the palm
    // sits inside their grip has. The HAND must move -- and (brief 2026-09-03,
    // sections 8-9) the WEAPON must move WITH it as one held object: the
    // authored palm->rear-grip relation is preserved exactly. The gun is
    // still not the hand's child in the render matrices (three-object gate)
    // or in its own parameter (case B); it is coordinated through the
    // explicit rear-grip relationship.
    {
        const float palmOff[3] = { 0.03f, -0.02f, 0.01f };   // 3 cm, 2 cm, 1 cm
        const float noRot[3] = { 0, 0, 0 };
        float rel0[3];
        {
            const float d[3] = { gun0.pos[0] - hand0.pos[0], gun0.pos[1] - hand0.pos[1], gun0.pos[2] - hand0.pos[2] };
            VRM_WorldToLocalVec(hand0.axis, d, rel0);
        }
        VRNH_SetCalibration(VRI_RIGHT, palmOff, noRot);
        IndepPair(&R);
        const VRObjPose hand1 = VRObj_Set()->hand[VROBJ_HAND_R];
        const VRObjPose gun1  = VRObj_Set()->gun;

        CHECK(IndepPosDelta(hand1, hand0) > 0.5f,
              "indep: palm calibration alone MOVES the right hand (%.4f u)",
              IndepPosDelta(hand1, hand0));
        CHECK(IndepPosDelta(gun1, gun0) > 0.5f,
              "indep: ...and the WEAPON moves with the calibrated palm (%.4f u) -- "
              "coordinated through the rear-grip relationship", IndepPosDelta(gun1, gun0));
        {
            const float d[3] = { gun1.pos[0] - hand1.pos[0], gun1.pos[1] - hand1.pos[1], gun1.pos[2] - hand1.pos[2] };
            float rel1[3];
            VRM_WorldToLocalVec(hand1.axis, d, rel1);
            const float e = sqrtf((rel1[0]-rel0[0])*(rel1[0]-rel0[0]) + (rel1[1]-rel0[1])*(rel1[1]-rel0[1]) + (rel1[2]-rel0[2])*(rel1[2]-rel0[2]));
            CHECK(e < 1e-3f,
                  "indep: the palm->rear-grip relation is PRESERVED to %.5f u -- the "
                  "gun stays held exactly as authored", e);
        }
        // The LEFT calibration reaches neither the right hand nor the gun.
        {
            const float leftOff[3] = { 0.05f, 0.04f, -0.03f };
            VRNH_SetCalibration(VRI_LEFT, leftOff, noRot);
            IndepPair(&R);
            CHECK(IndepPoseSame(VRObj_Set()->hand[VROBJ_HAND_R], hand1, 1e-3f, 1e-4f)
                  && IndepPoseSame(VRObj_Set()->gun, gun1, 1e-3f, 1e-4f),
                  "indep: LEFT calibration moves neither the RIGHT hand nor the gun");
            VRNH_SetCalibration(VRI_LEFT, noRot, noRot);
        }

        // restore
        VRNH_SetCalibration(VRI_RIGHT, noRot, noRot);
        IndepPair(&R);
        CHECK(IndepPoseSame(VRObj_Set()->hand[VROBJ_HAND_R], hand0, 1e-3f, 1e-4f),
              "indep: restoring the palm calibration restores the hand exactly");
    }

    // ---- B: change the GUN REAR-GRIP calibration ONLY -------------------
    {
        const float gunOff[3] = { -0.04f, 0.015f, 0.02f };
        const float noRot[3] = { 0, 0, 0 };
        // The physical chain: with the trigger-hand lock OFF the drawn hand
        // is the controller's palm and the gun's own trim never touches it.
        VRNH_SetTriggerHandLock(0);
        IndepPair(&R);
        const VRObjPose hand0u = VRObj_Set()->hand[VROBJ_HAND_R];
        VRNH_SetGunCalibration(gunOff, noRot);
        IndepPair(&R);
        const VRObjPose hand2 = VRObj_Set()->hand[VROBJ_HAND_R];
        const VRObjPose gun2  = VRObj_Set()->gun;

        CHECK(IndepPosDelta(gun2, gun0) > 0.5f,
              "indep: gun calibration alone MOVES the weapon (%.4f u)",
              IndepPosDelta(gun2, gun0));
        CHECK(IndepPoseSame(hand2, hand0u, 1e-3f, 1e-4f),
              "indep (lock off): ...and the RIGHT HAND is byte-identical -- the physical palm is not "
              "the weapon's child (moved %.6f u)", IndepPosDelta(hand2, hand0u));
        // The DRAWN hand with the lock ON (production, 2026-09-06): it rides
        // the weapon, so the same trim carries it by exactly the weapon's
        // displacement -- the grip never opens.
        VRNH_SetTriggerHandLock(1);
        IndepPair(&R);
        {
            const VRObjPose hand3 = VRObj_Set()->hand[VROBJ_HAND_R];
            const VRObjPose gun3  = VRObj_Set()->gun;
            float dh[3], dg[3];
            for (int k = 0; k < 3; ++k) { dh[k] = hand3.pos[k] - hand0u.pos[k]; dg[k] = gun3.pos[k] - gun0.pos[k]; }
            const float diff = sqrtf((dh[0]-dg[0])*(dh[0]-dg[0]) + (dh[1]-dg[1])*(dh[1]-dg[1]) + (dh[2]-dg[2])*(dh[2]-dg[2]));
            CHECK(IndepPosDelta(hand3, hand0u) > 0.5f && diff < 2e-3f,
                  "indep (lock on): the DRAWN hand rides the trimmed weapon by the same displacement (%.4f u, mismatch %.1e)",
                  IndepPosDelta(hand3, hand0u), diff);
        }

        VRNH_SetGunCalibration(noRot, noRot);
        IndepPair(&R);
        CHECK(IndepPoseSame(VRObj_Set()->gun, gun0, 1e-3f, 1e-4f),
              "indep: restoring the gun calibration restores the weapon exactly");
        CHECK(IndepPoseSame(VRObj_Set()->hand[VROBJ_HAND_R], hand0, 1e-3f, 1e-4f),
              "indep: ...and the drawn hand is back where it started (%.6f u)",
              IndepPosDelta(VRObj_Set()->hand[VROBJ_HAND_R], hand0));
    }

    // ---- C: with BOTH default, the two coincide -------------------------
    //
    // Independence is only useful if the default still lines up: the hand and
    // the weapon agree on screen because both calibrations are zero against
    // one grip, not because one is derived from the other.
    CHECK(IndepPoseSame(VRObj_Set()->hand[VROBJ_HAND_R], hand0, 1e-3f, 1e-4f)
          && IndepPoseSame(VRObj_Set()->gun, gun0, 1e-3f, 1e-4f),
          "indep: with both calibrations default, hand and weapon return to "
          "their coinciding poses");
}

// ===========================================================================
static void S_LeftFreeUnbound()
{
    printf("--- LEFT_FREE: the left hand is physically unbound from the "
           "weapon ---\n");
    IndepRig R;
    if (!IndepSetup(&R, 1)) { CHECK(0, "thompson body table missing"); return; }

    VRNHDebug d; VRNH_GetDebug(&d);
    CHECK(d.hand[VRI_LEFT].engaged == 1,
          "left: the left hand engages (reason %s)",
          VRNH_HandReasonName(d.hand[VRI_LEFT].reason));
    CHECK(d.gun.twoHandState == VRNH_2H_LEFT_FREE,
          "left: and starts LEFT_FREE (state %d)", d.gun.twoHandState);

    const VRObjPose left0  = VRObj_Set()->hand[VROBJ_HAND_L];
    const VRObjPose right0 = VRObj_Set()->hand[VROBJ_HAND_R];
    const VRObjPose gun0   = VRObj_Set()->gun;

    // ---- 1.0 METRE AWAY --------------------------------------------------
    //
    // The automated form of "pull my left hand several feet away from the
    // gun". No proximity constraint, no support-point snap, no weapon-local
    // parenting may pull it back or drag anything with it.
    {
        // XR -> Quake room is (-v2, -v0, v1), so an XR +x of 1.0 m is 1.0 m
        // of room -y: a clean lateral metre.
        const float gripFar[3] = { 1.0f, 0.0f, -0.30f };
        const float qI[4] = { 0, 0, 0, 1 };
        R.in.hand[VRI_LEFT] = NHHand(1, gripFar, qI);
        IndepPair(&R);

        const VRObjPose left1  = VRObj_Set()->hand[VROBJ_HAND_L];
        const VRObjPose right1 = VRObj_Set()->hand[VROBJ_HAND_R];
        const VRObjPose gun1   = VRObj_Set()->gun;

        const float moved = IndepPosDelta(left1, left0);
        const float metreU = 1.0f * 39.37f;
        CHECK(left1.valid && fabsf(moved - metreU) < 0.5f,
              "left: LEFT_FREE + 1.0 m of controller travel moves the LEFT "
              "hand 1.0 m (%.3f u, want %.3f)", moved, metreU);
        CHECK(IndepPoseSame(right1, right0, 1e-3f, 1e-4f),
              "left: ...the RIGHT hand does not move (%.6f u)",
              IndepPosDelta(right1, right0));
        CHECK(IndepPoseSame(gun1, gun0, 1e-3f, 1e-4f),
              "left: ...and the GUN does not move (%.6f u)",
              IndepPosDelta(gun1, gun0));
        VRNH_GetDebug(&d);
        CHECK(d.gun.twoHandState == VRNH_2H_LEFT_FREE,
              "left: no support-point snap captured it at range (state %d)",
              d.gun.twoHandState);
    }

    // ---- arbitrary rotation, a metre out --------------------------------
    {
        float q[4]; IndepGripQuatYaw(70.0f, q);
        const float gripFar[3] = { 1.0f, 0.0f, -0.30f };
        R.in.hand[VRI_LEFT] = NHHand(1, gripFar, q);
        const VRObjPose leftBefore  = VRObj_Set()->hand[VROBJ_HAND_L];
        const VRObjPose rightBefore = VRObj_Set()->hand[VROBJ_HAND_R];
        const VRObjPose gunBefore   = VRObj_Set()->gun;
        IndepPair(&R);
        const VRObjPose left2 = VRObj_Set()->hand[VROBJ_HAND_L];

        int turned = 0;
        for (int k = 0; k < 9; ++k)
            if (fabsf(left2.axis[k] - leftBefore.axis[k]) > 1e-3f) turned = 1;
        CHECK(turned, "left: rotating the LEFT controller turns the LEFT hand");
        CHECK(IndepPoseSame(VRObj_Set()->hand[VROBJ_HAND_R], rightBefore, 1e-3f, 1e-4f),
              "left: ...the RIGHT hand is untouched by it");
        CHECK(IndepPoseSame(VRObj_Set()->gun, gunBefore, 1e-3f, 1e-4f),
              "left: ...and so is the weapon");
    }
}

// ===========================================================================
static void S_OneHandAimLag()
{
    printf("--- one-hand aiming: the gun takes the controller's orientation "
           "with no lag ---\n");
    IndepRig R;
    if (!IndepSetup(&R, 0)) { CHECK(0, "thompson body table missing"); return; }

    // A step input on the RIGHT controller must be fully expressed in the gun's
    // orientation after the SAME pair. Continuous orientation smoothing --
    // which this build removed -- shows up here as a fraction of the step
    // surviving into the next frame, i.e. exactly the aiming lag a player
    // feels. Smoothing may run only during an explicit two-hand transition,
    // and TwoHand is off for this whole case.
    const float steps[4] = { 1.0f, 5.0f, 25.0f, 90.0f };
    float worst = 0.0f;
    for (int i = 0; i < 4; ++i) {
        // Return to zero, settle, then apply the step.
        float q0[4]; IndepGripQuatYaw(0.0f, q0);
        const float grip[3] = { 0.0f, 0.0f, -0.30f };
        R.in.hand[VRI_RIGHT] = NHHand(1, grip, q0);
        IndepPair(&R);
        IndepPair(&R);
        const VRObjPose gunAt0 = VRObj_Set()->gun;

        float q1[4]; IndepGripQuatYaw(steps[i], q1);
        R.in.hand[VRI_RIGHT] = NHHand(1, grip, q1);
        IndepPair(&R);                       // ONE pair -- no settling allowed
        const VRObjPose gunStep = VRObj_Set()->gun;

        // How far the gun actually turned, against how far the controller did.
        const float turned = NHBasisAngleDeg(gunAt0.axis, gunStep.axis);
        const float err = fabsf(turned - steps[i]);
        if (err > worst) worst = err;
        CHECK(err < 0.05f,
              "lag: a %.0f deg step reaches the gun in ONE pair (turned "
              "%.4f deg, error %.4f deg)", steps[i], turned, err);
    }
    CHECK(worst < 0.05f,
          "lag: worst one-pair angular error across 1/5/25/90 deg is %.4f deg "
          "-- numerical noise, not smoothing", worst);
}

// ===========================================================================
// TURNING MUST NOT MOVE THE OBJECTS -- and the REAL modelview both eyes use
// ===========================================================================
//
// Two claims in one case, because they need the same rig:
//
//  1. The hardware-proven path (RIGHT stick X -> visible game yaw) is frozen.
//     Turning changes the CAMERA basis; it must not disturb the physical
//     relationship between the controller-owned objects and their controllers.
//     With the head and both controllers held exactly still, an object that
//     jumps while the world turns is an object whose pose has leaked a
//     dependency on the game basis.
//
//  2. The submitted -> eye space correction is exercised against a REAL
//     CAPTURED non-identity modelview (MockRealViewmodelModelview: the
//     Quake-view -> GL-eye permutation with the live translation from
//     runs/vbogame-20260827-155348), on BOTH eyes. An identity-modelview
//     fixture is exactly what hid the ~12-unit space error the last cycle
//     found: with identity, submitted space and eye space coincide and the
//     bug is invisible.
static void S_TurnDoesNotMoveObjects()
{
    printf("--- right-stick turning: the game basis turns, the objects do "
           "not jump (REAL modelview, both eyes) ---\n");

    const CodvrGunModel* gun = VRGunPose_ByVertexCount(516);   // the BAR
    if (!gun) { CHECK(0, "turn: BAR body table missing"); return; }
    float gunAxis[9];
    HpAxisFromEuler(4.0f, -12.0f, 2.0f, gunAxis);
    const float gunPosU[3] = { 6.0f, -4.0f, -14.0f };

    ResetAll();
    NHResetForCase();
    MockRealViewmodelModelview();          // the REAL captured matrix, not identity
    VRNH_SetEnabled(1);
    VRNH_SetTwoHandEnabled(0);
    VRNH_SetReloadEnabled(0);
    float rAxis0[9], lAxis0[9];
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis0);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis0);
    const float rPos0[3] = { 0.20f, -0.05f, 0.55f }, lPos0[3] = { -0.20f, -0.05f, 0.55f };
    NHBuildHandVerts(rAxis0, rPos0, lAxis0, lPos0);

    SetMockHead(0, 0, 0, 0);               // FIXED head for the whole case
    CHECK(BringUp(), "turn: real mocked-XR session up");
    SetHandGripPos(VRI_RIGHT, 0.20f, -0.10f, -0.35f);
    SetHandGripPos(VRI_LEFT, -0.20f, -0.10f, -0.35f);
    SetHandAim(VRI_RIGHT, 0, 0);
    SetHandAim(VRI_LEFT, 0, 0);
    NHProdPairs(VRI_ACQUIRE_SYNCS + 6, gun, gunAxis, gunPosU);

    VRNHDebug d0; VRNH_GetDebug(&d0);
    CHECK(d0.hand[VRI_RIGHT].engaged && d0.hand[VRI_LEFT].engaged,
          "turn: both hands engaged under the REAL modelview (R %s, L %s)",
          VRNH_HandReasonName(d0.hand[VRI_RIGHT].reason),
          VRNH_HandReasonName(d0.hand[VRI_LEFT].reason));
    CHECK(GLSurf_HandPairEligible(GLSC_HAND_R) == 1
          && GLSurf_HandPairEligible(GLSC_HAND_L) == 1,
          "turn: BOTH eyes agreed about BOTH hands through a non-identity "
          "modelview -- the submitted->eye correction survives real data");
    CHECK(d0.hand[VRI_RIGHT].pairPosErrU <= VRNH_PAIR_POS_TOL_U
          && d0.hand[VRI_RIGHT].pairRotErrDeg <= VRNH_PAIR_ROT_TOL_DEG
          && d0.hand[VRI_LEFT].pairPosErrU <= VRNH_PAIR_POS_TOL_U
          && d0.hand[VRI_LEFT].pairRotErrDeg <= VRNH_PAIR_ROT_TOL_DEG,
          "turn: pair reconstruction within 0.5 mm / 0.05 deg (R %.6f u/%.6f "
          "deg, L %.6f u/%.6f deg)",
          d0.hand[VRI_RIGHT].pairPosErrU, d0.hand[VRI_RIGHT].pairRotErrDeg,
          d0.hand[VRI_LEFT].pairPosErrU, d0.hand[VRI_LEFT].pairRotErrDeg);

    const VRObjPose right0 = VRObj_Set()->hand[VROBJ_HAND_R];
    const VRObjPose left0  = VRObj_Set()->hand[VROBJ_HAND_L];
    const VRObjPose gun0   = VRObj_Set()->gun;
    const float yaw0 = g_gameYawDeg;
    VRBindsCounters tc0; VRBinds_GetCounters(&tc0);
    float bodyBefore[9];
    const int decoupled = VRWeapon_GetViewBasisRows(bodyBefore);

    // ---- now TURN, with everything else nailed down ---------------------
    g_mock.stickX[VRI_RIGHT] = 1.0f;
    int jumps = 0;
    float maxDelta = 0.0f;
    for (int i = 0; i < 60; ++i) {
        NHProdPairs(1, gun, gunAxis, gunPosU);
        const VRObjPose r = VRObj_Set()->hand[VROBJ_HAND_R];
        const VRObjPose l = VRObj_Set()->hand[VROBJ_HAND_L];
        const VRObjPose g = VRObj_Set()->gun;
        const float dr = IndepPosDelta(r, right0);
        const float dl = IndepPosDelta(l, left0);
        const float dg = IndepPosDelta(g, gun0);
        if (dr > maxDelta) maxDelta = dr;
        if (dl > maxDelta) maxDelta = dl;
        if (dg > maxDelta) maxDelta = dg;
        if (!IndepPoseSame(r, right0, 2e-3f, 2e-4f)
            || !IndepPoseSame(l, left0, 2e-3f, 2e-4f)
            || !IndepPoseSame(g, gun0, 2e-3f, 2e-4f))
            ++jumps;
    }
    g_mock.stickX[VRI_RIGHT] = 0.0f;

    // WHICH signal moved depends on the mode, and both are the turn working:
    // with the weapon DECOUPLED the stick drives the body basis directly
    // (VRWeapon_AddBodyYawDeg); coupled, it injects mouse counts the game
    // consumes as yaw. Asserting only the latter is what made an earlier
    // version of this guard read 0 while the turn was working perfectly --
    // the same coupled/decoupled split that produced the hardware turn no-op.
    VRBindsCounters tc1; VRBinds_GetCounters(&tc1);
    float bodyAfter[9];
    VRWeapon_GetViewBasisRows(bodyAfter);
    const float gameTurned = fabsf(VRM_WrapDeg(g_gameYawDeg - yaw0));
    const float bodyTurned = decoupled ? NHBasisAngleDeg(bodyBefore, bodyAfter)
                                       : 0.0f;
    CHECK(tc1.turnEvents > tc0.turnEvents && (gameTurned > 5.0f || bodyTurned > 5.0f),
          "turn: the RIGHT stick actually drove the turn (%d events; game yaw "
          "%.1f deg, body basis %.1f deg, mode %s) -- an inert stick would make "
          "the rest of this case vacuous", tc1.turnEvents - tc0.turnEvents,
          gameTurned, bodyTurned, decoupled ? "decoupled" : "coupled");
    CHECK(jumps == 0,
          "turn: across 60 turning pairs with a FIXED head and FIXED "
          "controllers, no controller-owned object moved (%d jumps, worst "
          "%.6f u)", jumps, maxDelta);
    VRNHDebug d1; VRNH_GetDebug(&d1);
    CHECK(d1.hand[VRI_RIGHT].engaged && d1.hand[VRI_LEFT].engaged,
          "turn: ...and both hands are still engaged afterwards");
    CHECK(VRObj_PairConsistent() && GLSurf_PairMismatchCount() == 0,
          "turn: zero pair-consistency violations throughout");
}
