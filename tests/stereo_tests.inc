// ===========================================================================
// PAIR-LEVEL STEREO ELIGIBILITY -- no half-native pair, ever
// ===========================================================================
//
// eye 0 and eye 1 recover the palm from their OWN draws, so with any real
// refusal rate they will disagree about whether a hand is recoverable roughly
// as often as a coin. A build that acts on each answer as it arrives renders
// one eye's hand on the controller and the other eye's on the weapon: two
// irreconcilable images of one hand, the same class of stereo fault as the
// 2026-08-23 moving-camera build that caused nausea.
//
// The required mutant is stated in the brief -- eye 0 solve PASS, eye 1 solve
// FAIL, expected NO half-native pair. It is case B below.
static void S_StereoPairEligibility()
{
    printf("--- pair-level stereo eligibility: no half-native pair ---\n");

    float rAxis[9], lAxis[9];
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis);
    const float rPos[3] = { 6.0f, -2.0f, -12.0f };
    const float lPos[3] = { -6.0f, -2.0f, -12.0f };
    const float headPos0[3] = { 0, 0, 0 };
    float headAxisI[9]; NHIdentityAxis(headAxisI);
    const float qI[4] = { 0, 0, 0, 1 };
    const float gripXr[3] = { 0.0f, 0.0f, -0.30f };

    // ---- A: both eyes recover the same hand -> eligible, both transform --
    {
        NHResetForCase();
        VRNH_SetEnabled(1);
        NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
        VRInputState in; memset(&in, 0, sizeof(in));
        in.hand[VRI_RIGHT] = NHHand(1, gripXr, qI);
        in.hand[VRI_LEFT]  = NHHand(1, gripXr, qI);

        VRNH_OnPairBegin(2, &in, headPos0, headAxisI, 1, 0.0);
        for (int eye = 0; eye < 2; ++eye) { NHScene(eye); NHDriveHandDraw(); }
        VRNH_OnPairBegin(3, &in, headPos0, headAxisI, 1, 0.010);

        VRNHDebug d; VRNH_GetDebug(&d);
        CHECK(GLSurf_HandEyeParticipating(GLSC_HAND_R, 0) == 1
              && GLSurf_HandEyeParticipating(GLSC_HAND_R, 1) == 1,
              "stereo: BOTH eyes are recorded as participating in the hand");
        CHECK(d.hand[VRI_RIGHT].pairEligible == 1
              && GLSurf_HandPairEligible(GLSC_HAND_R) == 1,
              "stereo: agreeing eyes -> the pair gate OPENS (pos %.6f u, rot "
              "%.6f deg)", d.hand[VRI_RIGHT].pairPosErrU,
              d.hand[VRI_RIGHT].pairRotErrDeg);
        CHECK(d.hand[VRI_RIGHT].engaged == 1,
              "stereo: and the hand engages (reason %s)",
              VRNH_HandReasonName(d.hand[VRI_RIGHT].reason));

        // Both eyes must actually MOVE the geometry -- an open gate that
        // reaches only one eye is the same fault by another route.
        GLSurfHandSolve before, after;
        GLSurf_GetHandSolve(GLSC_HAND_R, &before);
        for (int eye = 0; eye < 2; ++eye) { NHScene(eye); NHDriveHandDraw(); }
        GLSurf_GetHandSolve(GLSC_HAND_R, &after);
        CHECK(after.deltasApplied - before.deltasApplied == 2,
              "stereo: the delta reached BOTH eyes (%u applied)",
              after.deltasApplied - before.deltasApplied);
    }

    // ---- B: THE MUTANT -- eye 0 PASS, eye 1 FAIL ------------------------
    //
    // eye 1's draw is fed geometry the palm solve cannot recover, which is
    // exactly what a real refusal looks like from the layer's point of view.
    // Expected: NO half-native pair -- BOTH eyes fall back to vanilla.
    {
        NHResetForCase();
        VRNH_SetEnabled(1);
        NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
        VRInputState in; memset(&in, 0, sizeof(in));
        in.hand[VRI_RIGHT] = NHHand(1, gripXr, qI);
        in.hand[VRI_LEFT]  = NHHand(1, gripXr, qI);
        VRNH_OnPairBegin(2, &in, headPos0, headAxisI, 1, 0.0);

        NHScene(0); NHDriveHandDraw();                 // eye 0: solves
        for (int i = 0; i < 4096 * 3; ++i) g_verts[i] = 12345.0f;   // eye 1: refuses
        NHScene(1); NHDriveHandDraw();
        NHBuildHandVerts(rAxis, rPos, lAxis, lPos);    // restore for later draws

        VRNH_OnPairBegin(3, &in, headPos0, headAxisI, 1, 0.010);
        VRNHDebug d; VRNH_GetDebug(&d);

        CHECK(GLSurf_GetHandBaselineEye(GLSC_HAND_R, 0, nullptr) == 1,
              "stereo/MUTANT: eye 0 DID recover the hand");
        CHECK(GLSurf_GetHandBaselineEye(GLSC_HAND_R, 1, nullptr) == 0
              && GLSurf_HandEyeParticipating(GLSC_HAND_R, 1) == 1,
              "stereo/MUTANT: eye 1 participated and did NOT -- the "
              "half-native setup");
        CHECK(d.hand[VRI_RIGHT].pairEligible == 0
              && GLSurf_HandPairEligible(GLSC_HAND_R) == 0,
              "stereo/MUTANT: the pair gate is CLOSED");
        CHECK(d.hand[VRI_RIGHT].engaged == 0
              && d.hand[VRI_RIGHT].reason == VRNH_HAND_STEREO_DISAGREE,
              "stereo/MUTANT: the hand disengages for the PAIR (reason %s)",
              VRNH_HandReasonName(d.hand[VRI_RIGHT].reason));

        // The decisive assertion: NEITHER eye moved the hand.
        GLSurfHandSolve before, after;
        GLSurf_GetHandSolve(GLSC_HAND_R, &before);
        for (int eye = 0; eye < 2; ++eye) { NHScene(eye); NHDriveHandDraw(); }
        GLSurf_GetHandSolve(GLSC_HAND_R, &after);
        CHECK(after.deltasApplied == before.deltasApplied,
              "stereo/MUTANT: NO HALF-NATIVE PAIR -- zero deltas reached "
              "either eye (%u)", after.deltasApplied - before.deltasApplied);
        CHECK(VRObj_Set()->hand[VROBJ_HAND_R].valid == 0,
              "stereo/MUTANT: ...and the published pose says ABSENT, not stale");
    }

    // ---- C: both eyes solve, but their AUTHORED poses differ --------------
    //
    // The two eyes render on two consecutive GAME frames and the engine
    // advances the animation between them, so different authored recoveries
    // are the NORMAL case, not a fault (review 2026-08-29, section 5). The
    // predictive gate must stay OPEN, each eye's delta absorbs its own
    // authored pose, and the pre-submit verdict reconstructs both FINAL
    // objects to one PairWorld pose. (Until 2026-09-01 this case asserted
    // the opposite -- that an authored difference closed the gate -- which
    // would have refused every pair in which the hands were animating.)
    {
        NHResetForCase();
        VRNH_SetEnabled(1);
        VRInputState in; memset(&in, 0, sizeof(in));
        in.hand[VRI_RIGHT] = NHHand(1, gripXr, qI);
        in.hand[VRI_LEFT]  = NHHand(1, gripXr, qI);
        VRNH_OnPairBegin(2, &in, headPos0, headAxisI, 1, 0.0);

        const float rPosMoved[3] = { rPos[0] + 3.0f, rPos[1], rPos[2] };
        float rAxisMoved[9]; HpAxisFromEuler(5.0f + 4.0f, 20.0f - 3.0f, -3.0f, rAxisMoved);
        NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
        NHScene(0); NHDriveHandDraw();
        NHBuildHandVerts(rAxisMoved, rPosMoved, lAxis, lPos);   // 3 inches on
        NHScene(1); NHDriveHandDraw();

        VRNH_OnPairBegin(3, &in, headPos0, headAxisI, 1, 0.010);
        VRNHDebug d; VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].pairEligible == 1 && d.hand[VRI_RIGHT].engaged == 1,
              "stereo: two eyes recovering DIFFERENT authored poses keep the gate "
              "OPEN -- animation between the game frames is not a fault");

        // The pair renders in true order; the verdict compares the FINALS.
        NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
        NHScene(0); NHDriveHandDraw();
        NHBuildHandVerts(rAxisMoved, rPosMoved, lAxis, lPos);
        NHScene(1); NHDriveHandDraw();
        VRNHPairVerdict pv;
        const int ok = VRNH_PairPreSubmit(3, &pv);
        CHECK(ok && pv.obj[VRNH_OBJ_R].applied[0] && pv.obj[VRNH_OBJ_R].applied[1],
              "stereo: the pre-submit verdict is OK with RIGHT applied in both eyes");
        CHECK(pv.obj[VRNH_OBJ_R].posErrMm >= 0.0f
              && pv.obj[VRNH_OBJ_R].posErrMm <= VRNH_PAIRWORLD_POS_TOL_MM
              && pv.obj[VRNH_OBJ_R].rotErrDeg <= VRNH_PAIRWORLD_ROT_TOL_DEG,
              "stereo: both eyes' FINAL right palms reconstruct to one PairWorld "
              "object (%.4f mm, %.4f deg) although their authored poses were "
              "3 in and 5 deg apart",
              pv.obj[VRNH_OBJ_R].posErrMm, pv.obj[VRNH_OBJ_R].rotErrDeg);

        // The layer-level half-native mutant, judged at pre-submit: eye 1
        // refuses after eye 0 applied. The verdict, not the next boundary,
        // is what refuses it.
        VRNH_OnPairBegin(4, &in, headPos0, headAxisI, 1, 0.020);
        NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
        NHScene(0); NHDriveHandDraw();
        for (int i = 0; i < 4096 * 3; ++i) g_verts[i] = 12345.0f;
        NHScene(1); NHDriveHandDraw();
        NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
        const int ok2 = VRNH_PairPreSubmit(4, &pv);
        CHECK(!ok2 && pv.reason == VRNH_VERDICT_HALF_NATIVE
              && pv.obj[VRNH_OBJ_R].applied[0] && !pv.obj[VRNH_OBJ_R].applied[1]
              && pv.obj[VRNH_OBJ_R].drew[1],
              "stereo/MUTANT: eye 0 applied, eye 1 drew and refused -> the PAIR "
              "is refused as %s at pre-submit", VRNH_VerdictName(pv.reason));
    }
}
