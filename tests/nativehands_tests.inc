// ===========================================================================
// Native hands wiring (src/vr_native_hands.cpp) -- included by vr_safetest.cpp
// AFTER glsurf_tests.inc, objects_tests.inc and handpose_tests.inc: it reuses
// their mock GL driver/recorder and synthetic-landmark helpers rather than
// duplicating them, so these cases exercise the REAL production path --
// VRNH_OnPairBegin -> GLSurf_SetHandTarget -> BuildHandDelta -> an actual
// transformed draw -- not a reimplementation of VRNH's arithmetic.
//
// What's under test: the glue between "here is a controller pose" and "the
// hand is rendered there", specifically that it (1) places the RIGHT and LEFT
// rendered hands at the controller-derived target, independently, (2) fails
// closed with no tracking / no head / disabled / no prior authored recovery,
// and (3) the pair-latched offset is applied identically regardless of which
// authored baseline a given draw recovers -- the actual mathematical content
// of "translation is eye-invariant" -- so a mutant that reused one draw's
// raw GL delta on another's vertices is caught.
// ===========================================================================

#include "../src/vr_native_hands.h"

// The same bind-position spread as handpose_tests.inc's kTestLandmarks
// (non-symmetric, well-posed), placed at the REAL production index ranges
// (re-corrected 2026-08-28, see MergedHalves in vr_glsurf.cpp):
// [837,1674) for the rendered RIGHT/trigger half, [0,837) for LEFT/support.
// Like the shipped rig, the two synthetic hands are MIRROR IMAGES (the
// LEFT bind cloud is the RIGHT's with y negated: no proper rotation maps one
// onto the other) at DISJOINT local indices (RIGHT at local 100..170, LEFT
// at 0..70). Both properties are what make identity-from-content possible:
// a table read in the wrong half must refuse, and a table shifted into the
// other half must land on that half's own bytes, not on the other table's.
// (Until 2026-09-03 the two tables were identical at identical local
// indices, so a swapped read solved perfectly -- a fixture that could not
// have modelled the real defect.)
static const CodvrPalmLandmark kNHRight[] = {
    { 837 + 100, { 0.000f,  0.000f,  0.000f } },
    { 837 + 110, { 0.040f,  0.000f,  0.000f } },
    { 837 + 120, { 0.000f,  0.030f,  0.000f } },
    { 837 + 130, { 0.000f,  0.000f,  0.014f } },
    { 837 + 140, { 0.035f,  0.025f,  0.004f } },
    { 837 + 150, { 0.012f, -0.018f,  0.011f } },
    { 837 + 160, { 0.028f,  0.009f, -0.013f } },
    { 837 + 170, { 0.005f,  0.022f,  0.008f } },
};
static const CodvrPalmLandmark kNHLeft[] = {
    {   0, { 0.000f,  0.000f,  0.000f } },
    {  10, { 0.040f,  0.000f,  0.000f } },
    {  20, { 0.000f, -0.030f,  0.000f } },
    {  30, { 0.000f,  0.000f,  0.014f } },
    {  40, { 0.035f, -0.025f,  0.004f } },
    {  50, { 0.012f,  0.018f,  0.011f } },
    {  60, { 0.028f, -0.009f, -0.013f } },
    {  70, { 0.005f, -0.022f,  0.008f } },
};
enum { NH_N = (int)(sizeof(kNHRight) / sizeof(kNHRight[0])) };

// Write the merged 1674-vertex hand batch with the RIGHT half's landmarks at
// rightAxis/rightPos and the LEFT half's at leftAxis/leftPos, plus filler
// elsewhere -- reuses glsurf_tests.inc's g_verts array.
//
// The batch is built in ENGINE UNITS (bind cloud scaled by CODVR_UNITS_PER_M
// before posing), because that is what the production solve inside
// SolveHandFromDraw now expects. Until 2026-08-27 this fixture built
// metre-scale batches and the solver assumed metres, so the whole wiring
// was green against a vertex space the real engine never submits.
static void NHBuildHandVerts(const float rightAxis[9], const float rightPos[3],
                             const float leftAxis[9], const float leftPos[3])
{
    for (int i = 0; i < 4096; ++i) {
        g_verts[i*3+0] = 9.0f + i;
        g_verts[i*3+1] = -4.0f - i;
        g_verts[i*3+2] = 2.5f * i;
    }
    for (int i = 0; i < NH_N; ++i) {
        const float bindU[3] = { kNHRight[i].bindM[0] * CODVR_UNITS_PER_M,
                                 kNHRight[i].bindM[1] * CODVR_UNITS_PER_M,
                                 kNHRight[i].bindM[2] * CODVR_UNITS_PER_M };
        float r[3];
        HpApply(rightAxis, bindU, r);
        float* v = &g_verts[kNHRight[i].index * 3];
        v[0] = rightPos[0] + r[0]; v[1] = rightPos[1] + r[1]; v[2] = rightPos[2] + r[2];
    }
    for (int i = 0; i < NH_N; ++i) {
        const float bindU[3] = { kNHLeft[i].bindM[0] * CODVR_UNITS_PER_M,
                                 kNHLeft[i].bindM[1] * CODVR_UNITS_PER_M,
                                 kNHLeft[i].bindM[2] * CODVR_UNITS_PER_M };
        float r[3];
        HpApply(leftAxis, bindU, r);
        float* v = &g_verts[kNHLeft[i].index * 3];
        v[0] = leftPos[0] + r[0]; v[1] = leftPos[1] + r[1]; v[2] = leftPos[2] + r[2];
    }
}

static void NHDriveHandDraw()
{
    GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                          GLS_GL_UNSIGNED_INT, g_handIdx);
}

static void NHIdentityAxis(float a[9])
{
    memset(a, 0, 9 * sizeof(float));
    a[0] = a[4] = a[8] = 1.0f;
}

static void NHResetForCase()
{
    ResetLayerForCase();               // glsurf_tests.inc: driver + identity MV + scene
    EnterViewmodelPass();
    BuildSyntheticGeometry();          // lays out g_handIdx/g_sleeveIdx
    VRHandPose_TestSetLandmarks(VRHP_TRIGGER, kNHRight, NH_N);
    VRHandPose_TestSetLandmarks(VRHP_SUPPORT, kNHLeft,  NH_N);
    VRObj_Reset();
    VRNH_TestReset();
    GLSurf_ClearHandTargets();
}

// A hand state shaped EXACTLY as the real producer (vr_input.cpp) writes it.
//
// This function used to set `hs.stable = VRI_TRACK_ACTIVE` (2) on a struct
// that carried BOTH a boolean `stable` and an int `trackState`. vr_input can
// only ever write 0 or 1 to a boolean, so every case below was validating
// against a snapshot the production layer cannot produce. The shipped
// engagement gate asked `stable == VRI_TRACK_ACTIVE`, the fixture answered
// "2 == 2, sure", and the hands were unreachable on hardware for weeks behind
// 1419 green tests.
//
// The field is now GONE and trackState is a VRITrack enum class, so that
// fixture line no longer compiles -- the illegal state is unrepresentable
// rather than merely untested. S_ProductionPoseOwnership pins this shape
// against the live producer; S_TrackStateTypeSafety is the regression mutant.
static VRHandState NHHand(int trackedActive, const float gripPosXr[3], const float gripQuat[4])
{
    VRHandState hs; memset(&hs, 0, sizeof(hs));
    if (trackedActive) {
        hs.active     = 1;
        hs.aimValid   = 1;
        hs.gripValid  = 1;
        hs.trackState = VRI_TRACK_ACTIVE;   // the ONE tracking field
        memcpy(hs.gripPos, gripPosXr, sizeof(hs.gripPos));
        memcpy(hs.gripQuat, gripQuat, sizeof(hs.gripQuat));
    } else {
        hs.trackState = VRI_TRACK_UNAVAILABLE;
    }
    return hs;
}

// A hand that is LOST (inside the hysteresis grace): still USABLE, but no
// longer ACTIVE. Only the real encoding distinguishes these two.
static VRHandState NHHandLost(const float gripPosXr[3], const float gripQuat[4])
{
    VRHandState hs = NHHand(1, gripPosXr, gripQuat);
    hs.trackState = VRI_TRACK_LOST;
    return hs;
}

// ===========================================================================
static void S_NativeHandsWiring()
{
    printf("--- native hands: controller -> rendered hand wiring ---\n");

    const float headPos0[3] = { 0, 0, 0 };
    float headAxisI[9]; NHIdentityAxis(headAxisI);
    const float qIdentity[4] = { 0, 0, 0, 1 };   // x,y,z,w

    // ---- 1: disabled -- nothing moves --------------------------------
    //
    // "Untouched" at this layer means the batch is NOT CLAIMED: the call
    // returns 0 and the engine issues the draw itself, exactly as it arrived.
    // (The first version of this case demanded two sub-draws -- a split --
    // which is a contract the layer never offered for an untransformed batch;
    // splitting it would be work with no purpose. That wrong expectation was
    // one of the four quarantine FAILs.)
    {
        NHResetForCase();
        float rAxis[9], lAxis[9];
        NHIdentityAxis(rAxis); NHIdentityAxis(lAxis);
        const float rPos[3] = { 0.20f, -0.05f, 0.55f }, lPos[3] = { -0.20f, -0.05f, 0.55f };
        NHBuildHandVerts(rAxis, rPos, lAxis, lPos);

        VRNH_SetEnabled(0);
        const float gripXr[3] = { 0, 0, -0.10f };
        VRHandState right = NHHand(1, gripXr, qIdentity);
        VRInputState in; memset(&in, 0, sizeof(in));
        in.hand[VRI_RIGHT] = right;

        VRNH_OnPairBegin(1, &in, headPos0, headAxisI, 1, 0.0);

        RecReset();
        const int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES,
                                                GLS_HAND_INDICES * 2,
                                                GLS_GL_UNSIGNED_INT, g_handIdx);
        CHECK(owned == 0,
              "disabled: the layer does not claim the batch -- the engine "
              "draws it as issued");
        CHECK(CountKind(REC_LOADMV) == 0,
              "disabled: the hand draw is not transformed at all (%d loadMatrixf calls)",
              CountKind(REC_LOADMV));
        CHECK(CountKind(REC_DRAW) == 0,
              "disabled: the layer issued no draws of its own (%d)",
              CountKind(REC_DRAW));
        GLSurfHandSolve hsR;
        CHECK(GLSurf_GetHandSolve(GLSC_HAND_R, &hsR) == 0 && hsR.solves == 0,
              "disabled: no palm solve ran either (solve-wanted follows enable)");
    }

    // ---- 2/3: enabled, tracked, one prior baseline -- RIGHT lands on the
    //           controller target; LEFT (untracked) stays disengaged --------
    float rightTargetPos[3], rightTargetAxis[9];
    {
        NHResetForCase();
        VRNH_SetEnabled(1);

        float rAxis0[9], lAxis0[9];
        HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis0);
        HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis0);
        const float rPos0[3] = { 0.20f, -0.05f, 0.55f }, lPos0[3] = { -0.20f, -0.05f, 0.55f };
        NHBuildHandVerts(rAxis0, rPos0, lAxis0, lPos0);
        NHDriveHandDraw();                          // seeds BOTH baselines

        // RIGHT controller: some room-space translation + a known rotation.
        // XR->Quake is out=(-v2,-v0,v1); pick v so the room result is tidy.
        const float gripXr[3] = { 0.0f, 0.0f, -0.30f };  // room (quake) = (0.30, 0, 0)
        float gripQuatXr[4];
        {
            // A small, known rotation about the XR Y axis so orientation is
            // exercised too, not just translation.
            const float half = 12.0f * 0.5f * 0.01745329252f;
            gripQuatXr[0] = 0.0f; gripQuatXr[1] = sinf(half);
            gripQuatXr[2] = 0.0f; gripQuatXr[3] = cosf(half);
        }
        VRHandState right = NHHand(1, gripXr, gripQuatXr);
        VRInputState in; memset(&in, 0, sizeof(in));
        in.hand[VRI_RIGHT] = right;
        in.hand[VRI_LEFT]  = NHHand(0, gripXr, qIdentity);     // untracked

        // TWO pairs, by design. Engagement needs a baseline solve, and the
        // solve runs on draws made while solve-wanted is set -- which the
        // wiring asserts from the first ENABLED pair on. So pair 2 comes up
        // disengaged (no baseline yet), its draws seed the baseline, and
        // pair 3 engages. The quarantined version demanded engagement on the
        // first pair, which no execution order could deliver: the solve only
        // ran for already-engaged targets. That deadlock was the real defect
        // behind three of the four quarantine FAILs.
        VRNH_OnPairBegin(2, &in, headPos0, headAxisI, 1, 0.0);
        {
            VRNHDebug d0; VRNH_GetDebug(&d0);
            CHECK(d0.hand[VRI_RIGHT].engaged == 0 && d0.hand[VRI_RIGHT].haveBaseline == 0,
                  "RIGHT: FIRST enabled pair is honestly disengaged -- no "
                  "baseline exists yet");
        }
        NHDriveHandDraw();                          // solve-wanted seeds the baseline
        {
            GLSurfHandSolve hs;
            CHECK(GLSurf_GetHandSolve(GLSC_HAND_R, &hs) == 1 && hs.solves >= 1,
                  "...and that pair's draw ran the palm solve with no target "
                  "engaged (%u solves)", hs.solves);
        }
        VRNH_OnPairBegin(3, &in, headPos0, headAxisI, 1, 0.0);

        VRNHDebug dbg; VRNH_GetDebug(&dbg);
        CHECK(dbg.hand[VRI_RIGHT].engaged == 1, "RIGHT: tracked + baseline -> engaged");
        CHECK(dbg.hand[VRI_LEFT].engaged == 0, "LEFT: untracked -> stays disengaged");
        memcpy(rightTargetPos, dbg.hand[VRI_RIGHT].targetEyeish, sizeof(rightTargetPos));

        // Independently reproduce the expected target with the SAME shared
        // primitives production uses (vr_math.h), so this checks the WIRING
        // (right mapping, right calibration defaults, right composition
        // order) rather than re-deriving the coordinate math a second way.
        float roomM[3]; VRM_XrToQuakeVec(gripXr, roomM);
        float gripAxisQuake[9]; VRM_QuatToQuakeAxis(gripQuatXr, gripAxisQuake);
        float localM[3]; VRM_WorldToLocalVec(headAxisI, roomM, localM);
        const float localU[3] = { localM[0]*39.37f, localM[1]*39.37f, localM[2]*39.37f };
        float wantPos[3]; VRM_QuakeToGLEyeVec(localU, wantPos);
        float localAxis[9]; VRM_WorldToLocalAxis(headAxisI, gripAxisQuake, localAxis);
        float wantAxis[9]; VRM_QuakeToGLEyeAxis(localAxis, wantAxis);
        // zero calibration (default): DesiredFromGrip(offset=0,rot=0) == grip pose,
        // then the visual limb correction (2026-09-04): 180 deg about grip X,
        // which on the eyeish grip rows (grip -Z, -X, +Y) negates rows 0 and 2.
        memcpy(rightTargetAxis, wantAxis, sizeof(rightTargetAxis));

        CHECK_NEAR(HpDist(rightTargetPos, wantPos), 0.0, 1e-4,
                   "RIGHT target (%.4f %.4f %.4f) matches the independently "
                   "derived controller-eyeish position (%.4f %.4f %.4f)",
                   rightTargetPos[0], rightTargetPos[1], rightTargetPos[2],
                   wantPos[0], wantPos[1], wantPos[2]);

        // ---- drive the SECOND draw (same animation state) and check where
        //      the RIGHT half actually lands, end to end ---------------
        RecReset();
        NHDriveHandDraw();
        CHECK(CountKind(REC_LOADMV) >= 2, "RIGHT engaged, LEFT not: at least one transformed half");
        const GLRec* rMv = NthKind(REC_LOADMV, 0);   // RIGHT drawn first (MergedHalves)
        CHECK(rMv != nullptr, "RIGHT half's applied matrix was recorded");
        // GUARDED: a harness that dereferences what its own assertion just
        // proved was null cannot report -- it can only take the process down.
        // That crash cost the first hour of the 2026-08-26 investigation.
        if (rMv) {
            float landed[3];
            HpXform4(rMv->m, rPos0, landed);
            CHECK_NEAR(HpDist(landed, rightTargetPos), 0.0, 1e-4,
                       "end-to-end: the RIGHT palm origin actually lands on the "
                       "controller target (%.4f %.4f %.4f) vs (%.4f %.4f %.4f)",
                       landed[0], landed[1], landed[2],
                       rightTargetPos[0], rightTargetPos[1], rightTargetPos[2]);

            // Rotation landed too: a point 5cm out along the AUTHORED forward
            // axis must land 5cm out along the TARGET forward axis, not the
            // authored one -- the palm actually turned.
            float authOffsetPt[3] = { rPos0[0] + rAxis0[0]*0.05f, rPos0[1] + rAxis0[1]*0.05f,
                                      rPos0[2] + rAxis0[2]*0.05f };
            float landedOffset[3]; HpXform4(rMv->m, authOffsetPt, landedOffset);
            float wantOffset[3] = { rightTargetPos[0] + wantAxis[0]*0.05f,
                                    rightTargetPos[1] + wantAxis[1]*0.05f,
                                    rightTargetPos[2] + wantAxis[2]*0.05f };
            CHECK_NEAR(HpDist(landedOffset, wantOffset), 0.0, 1e-4,
                       "end-to-end: orientation landed on the controller's rotation too");
        }

        // LEFT was never engaged: its half draws WITHOUT a transform.
        int leftTransformed = 0;
        for (int i = 0; i < g_recN; ++i)
            if (g_rec[i].kind == REC_LOADMV) ++leftTransformed;
        // (RIGHT contributes exactly 2 LOADMV entries: wanted + restore.)
        CHECK(leftTransformed == 2,
              "LEFT (disengaged) contributes no LOADMV of its own (saw %d total, "
              "want exactly RIGHT's 2)", leftTransformed);
    }

    // ---- 4: no prior baseline -- disengage for THIS pair, not a guess -----
    {
        GLSurf_Reset(); InstallMockDriver(); MockIdentity(g_mockModelview);
        MockIdentity(g_mockProjection);
        g_mockProjection[0] = 2.0f; g_mockProjection[5] = 2.0f;
        g_mockProjection[11] = -1.0f; g_mockProjection[15] = 0.0f;
        RecReset();
        GLSurf_BeginFrame();
        GLSurf_SceneQueued(0);
        EnterViewmodelPass();
        BuildSyntheticGeometry();
        VRHandPose_TestSetLandmarks(VRHP_TRIGGER, kNHRight, NH_N);
        VRHandPose_TestSetLandmarks(VRHP_SUPPORT, kNHLeft,  NH_N);
        VRObj_Reset(); VRNH_TestReset(); GLSurf_ClearHandTargets();
        VRNH_SetEnabled(1);
        // NOTE: no seeding draw this time -- GLSurf_GetHandSolve has nothing.

        const float gripXr[3] = { 0.0f, 0.0f, -0.10f };
        VRInputState in; memset(&in, 0, sizeof(in));
        in.hand[VRI_RIGHT] = NHHand(1, gripXr, qIdentity);

        VRNH_OnPairBegin(1, &in, headPos0, headAxisI, 1, 0.0);
        VRNHDebug dbg; VRNH_GetDebug(&dbg);
        CHECK(dbg.hand[VRI_RIGHT].engaged == 0,
              "no prior authored recovery -> disengaged this pair, not guessed");
        CHECK(dbg.hand[VRI_RIGHT].haveBaseline == 0, "...and it says why");
    }

    // ---- 5: head invalid -- both hands disengaged (fail closed) -----------
    {
        NHResetForCase();
        VRNH_SetEnabled(1);
        float rAxis[9], lAxis[9]; NHIdentityAxis(rAxis); NHIdentityAxis(lAxis);
        const float rPos[3] = { 0.2f, 0, 0.5f }, lPos[3] = { -0.2f, 0, 0.5f };
        NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
        NHDriveHandDraw();

        const float gripXr[3] = { 0, 0, -0.1f };
        VRInputState in; memset(&in, 0, sizeof(in));
        in.hand[VRI_RIGHT] = NHHand(1, gripXr, qIdentity);
        in.hand[VRI_LEFT]  = NHHand(1, gripXr, qIdentity);

        VRNH_OnPairBegin(3, &in, headPos0, headAxisI, /*headValid=*/0, 0.0);
        VRNHDebug dbg; VRNH_GetDebug(&dbg);
        CHECK(dbg.hand[VRI_RIGHT].engaged == 0 && dbg.hand[VRI_LEFT].engaged == 0,
              "no located head this pair -> both hands disengaged");
        CHECK(VRObj_Set()->head.valid == 0, "...and the latched head is marked invalid too");
    }

    // ---- 6: controller sampled once per pair, THROUGH this wiring ---------
    {
        NHResetForCase();
        VRNH_SetEnabled(1);
        float rAxis[9], lAxis[9]; NHIdentityAxis(rAxis); NHIdentityAxis(lAxis);
        const float rPos[3] = { 0.2f, 0, 0.5f }, lPos[3] = { -0.2f, 0, 0.5f };
        NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
        NHDriveHandDraw();

        const float gripXr0[3] = { 0, 0, -0.10f };
        VRInputState in0; memset(&in0, 0, sizeof(in0));
        in0.hand[VRI_RIGHT] = NHHand(1, gripXr0, qIdentity);
        VRNH_OnPairBegin(9, &in0, headPos0, headAxisI, 1, 0.0);

        // A second grip sample for the SAME still-open pair (no new
        // VRObj_BeginPair -- that is exactly what "resampled between the
        // eyes" looks like if something called into this layer twice inside
        // one pair), with the controller having moved a long way.
        const float gripXr1[3] = { 0, 0, -9.0f };
        VRObj_LatchGrip(VROBJ_HAND_R, gripXr1, headAxisI, 1);

        unsigned lateSamples = 0;
        VRObj_GetCounters(nullptr, nullptr, &lateSamples, nullptr);
        CHECK(lateSamples >= 1,
              "a grip sample arriving after an eye consumed the pair is refused, "
              "not applied (late=%u)", lateSamples);
    }

    VRHandPose_TestRestore();
    VRNH_TestReset();
}

// ===========================================================================
// BASELINE FRESHNESS -- the 2026-08-28 hardware-run response.
//
// The hardware run measured ~5500 solver refusals interleaved with ~6100
// solves, and the refusing draw was reliably the last one before each pair
// boundary -- so the raw "last attempt succeeded" flag read false at nearly
// every engagement decision and the hands never engaged in a 4-minute run.
// The contract now under test: a refusal must not un-know a solve that
// already happened; only AGE (GLS_BASELINE_FRESH_SCENES of scenes) may.
// ===========================================================================
static void S_NativeBaselineFreshness()
{
    printf("--- native hands: refusals cannot starve engagement; age can ---\n");

    const float headPos0[3] = { 0, 0, 0 };
    float headAxisI[9]; NHIdentityAxis(headAxisI);
    const float qIdentity[4] = { 0, 0, 0, 1 };

    NHResetForCase();
    VRNH_SetEnabled(1);
    float rAxis[9], lAxis[9]; NHIdentityAxis(rAxis); NHIdentityAxis(lAxis);
    const float rPos[3] = { 0.20f, -0.05f, 0.55f }, lPos[3] = { -0.20f, -0.05f, 0.55f };
    NHBuildHandVerts(rAxis, rPos, lAxis, lPos);

    const float gripXr[3] = { 0, 0, -0.30f };
    VRInputState in; memset(&in, 0, sizeof(in));
    in.hand[VRI_RIGHT] = NHHand(1, gripXr, qIdentity);

    // Two-pair convergence, as in production: pair 1 arms solve-wanted, its
    // draw seeds the baseline, pair 2 engages.
    VRNH_OnPairBegin(1, &in, headPos0, headAxisI, 1, 0.0);
    NHDriveHandDraw();
    VRNH_OnPairBegin(2, &in, headPos0, headAxisI, 1, 0.0);
    {
        VRNHDebug d; VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].engaged == 1, "freshness: seeded pair engages");
        CHECK(d.hand[VRI_RIGHT].reason == VRNH_HAND_ENGAGED,
              "freshness: ...and the reason says ENGAGED");
    }

    // Record the good baseline, then drive a REFUSING draw: same indices,
    // but the vertex array is all zeros -- a degenerate cloud no landmark
    // table reproduces. This is the production shape: some draw of the hand
    // class refuses AFTER the good solve.
    GLSurfHandSolve before;
    CHECK(GLSurf_GetHandBaseline(GLSC_HAND_R, &before) == 1,
          "freshness: baseline readable before the refusal");
    memset(g_verts, 0, sizeof(g_verts));
    NHDriveHandDraw();
    {
        GLSurfHandSolve hs;
        const int valid = GLSurf_GetHandSolve(GLSC_HAND_R, &hs);
        CHECK(valid == 0 && hs.refusedSolve > 0,
              "freshness: the degenerate draw REFUSED at the solver (%u)",
              hs.refusedSolve);
        GLSurfHandSolve base;
        CHECK(GLSurf_GetHandBaseline(GLSC_HAND_R, &base) == 1,
              "freshness: the refusal did NOT un-know the baseline");
        CHECK_NEAR(HpDist(base.pos, before.pos), 0.0, 1e-6,
                   "freshness: ...and the last-good pose is byte-identical");
        // Engaged targets split the merged batch, so the refusing draw the
        // telemetry names is ONE half: 3912 indices.
        CHECK(hs.lastRefSite == 3 && hs.lastRefCount == GLS_HAND_INDICES,
              "freshness: refusal telemetry names the draw shape (site %d idx %d)",
              hs.lastRefSite, hs.lastRefCount);
        CHECK(hs.lastRefBestRmsM > VRHP_MAX_RMS_M,
              "freshness: ...and how far the nearest table was (%.4f m)",
              hs.lastRefBestRmsM);
    }

    // The pair AFTER the refusal: engagement HOLDS. This is the exact pair
    // the 2026-08-28 hardware run lost, ~7000 times in a row.
    VRNH_OnPairBegin(3, &in, headPos0, headAxisI, 1, 0.0);
    {
        VRNHDebug d; VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].engaged == 1,
              "freshness: a refusing draw between pairs does NOT disengage");
    }

    // AGE: advance the scene counter past the freshness window with no
    // successful solve. Engagement must now refuse, honestly.
    for (int i = 0; i < GLS_BASELINE_FRESH_SCENES + 1; ++i)
        GLSurf_SceneQueued(i & 1);
    CHECK(GLSurf_GetHandBaseline(GLSC_HAND_R, nullptr) == 0,
          "freshness: %d scenes of silence age the baseline out",
          GLS_BASELINE_FRESH_SCENES + 1);
    VRNH_OnPairBegin(4, &in, headPos0, headAxisI, 1, 0.0);
    {
        VRNHDebug d; VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].engaged == 0,
              "freshness: a stale baseline disengages (fail closed)");
        CHECK(d.hand[VRI_RIGHT].reason == VRNH_HAND_NO_BASELINE,
              "freshness: ...and the reason enum says NO_BASELINE");
    }

    // Recovery: one good draw re-seeds; the next pair re-engages on its own.
    NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
    NHDriveHandDraw();
    VRNH_OnPairBegin(5, &in, headPos0, headAxisI, 1, 0.0);
    {
        VRNHDebug d; VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].engaged == 1,
              "freshness: one good draw is enough to re-engage");
    }

    // ---- THE TRACKING GATE, ISOLATED ------------------------------------
    //
    // Every disengage case in this file also clears gripValid in the same
    // breath, and gripValid == 0 short-circuits to NO_GRIP before the tracking
    // comparison is ever evaluated -- so `bool engage = true;` would pass all
    // of them. These cases hold gripValid = 1 and vary ONLY the tracking
    // fields, which is the one shape that can tell the two gates apart.
    {
        const float gxr[3] = { 0, 0, -0.30f };
        const float qi[4]  = { 0, 0, 0, 1 };

        // ACQUIRING: stable is 0 and trackState is not ACTIVE. This is the
        // ONLY state where the boolean and the enum disagree in a way that
        // must NOT engage -- the case that catches the encoding swap in
        // either direction.
        VRInputState acq; memset(&acq, 0, sizeof(acq));
        acq.hand[VRI_RIGHT] = NHHand(1, gxr, qi);
        acq.hand[VRI_RIGHT].trackState = VRI_TRACK_ACQUIRING;
        VRNH_OnPairBegin(20, &acq, headPos0, headAxisI, 1, 100.0);
        VRNHDebug d; VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].engaged == 0
              && d.hand[VRI_RIGHT].reason == VRNH_HAND_TRACKING,
              "gate: ACQUIRING with a VALID grip refuses at the TRACKING gate, "
              "not the grip gate (reason %s)",
              VRNH_HandReasonName(d.hand[VRI_RIGHT].reason));

        // Re-engage cleanly, then exercise the bounded grace with a hand that
        // is LOST but still stable -- the ride-through production performs on
        // every micro-dropout, which had zero coverage.
        VRInputState act; memset(&act, 0, sizeof(act));
        act.hand[VRI_RIGHT] = NHHand(1, gxr, qi);
        VRNH_OnPairBegin(21, &act, headPos0, headAxisI, 1, 100.0);
        NHDriveHandDraw();
        VRNH_OnPairBegin(22, &act, headPos0, headAxisI, 1, 100.0);
        VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].engaged == 1, "gate: ACTIVE re-engages");

        VRInputState lost; memset(&lost, 0, sizeof(lost));
        lost.hand[VRI_RIGHT] = NHHandLost(gxr, qi);   // gripValid stays 1
        VRNH_OnPairBegin(23, &lost, headPos0, headAxisI, 1, 100.2);
        VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].engaged == 1,
              "gate: LOST inside the 0.5 s grace KEEPS the hand engaged "
              "(reason %s)", VRNH_HandReasonName(d.hand[VRI_RIGHT].reason));

        VRNH_OnPairBegin(24, &lost, headPos0, headAxisI, 1, 101.5);  // past it
        VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].engaged == 0
              && d.hand[VRI_RIGHT].reason == VRNH_HAND_TRACKING,
              "gate: ...and past the grace it disengages at the TRACKING gate "
              "(reason %s)", VRNH_HandReasonName(d.hand[VRI_RIGHT].reason));
    }

    // Reason enums for the other refusal paths, so the telemetry the next
    // hardware log prints is pinned to the states it claims to name.
    {
        VRNH_SetEnabled(0);
        VRNH_OnPairBegin(6, &in, headPos0, headAxisI, 1, 0.0);
        VRNHDebug d; VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].reason == VRNH_HAND_DISABLED,
              "freshness: disabled pairs say DISABLED");
        VRNH_SetEnabled(1);

        VRInputState dead; memset(&dead, 0, sizeof(dead));
        dead.hand[VRI_RIGHT] = NHHand(0, gripXr, qIdentity);
        VRNH_OnPairBegin(7, &dead, headPos0, headAxisI, 1, 0.0);
        VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].reason == VRNH_HAND_NO_GRIP
              || d.hand[VRI_RIGHT].reason == VRNH_HAND_TRACKING,
              "freshness: an untracked hand says NO_GRIP/TRACKING (got %s)",
              VRNH_HandReasonName(d.hand[VRI_RIGHT].reason));

        VRNH_OnPairBegin(8, &in, headPos0, headAxisI, /*headValid=*/0, 0.0);
        VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].reason == VRNH_HAND_NO_HEAD,
              "freshness: a headless pair says NO_HEAD");
    }

    VRHandPose_TestRestore();
    VRNH_TestReset();
    ResetLayerForCase();
}

// ===========================================================================
// Flat soak: the full wiring against the hostile-array world.
//
// The quarantine's release condition: the controller wiring must survive the
// same client-array probe the 2026-08-26 crash fix added -- VBO offsets,
// wild pointers, nulls, buffer churn -- while pairs begin, tracking drops in
// and out, and enablement toggles. Nothing may fault; refusals must be
// counted; the wiring must actually ENGAGE on the frames where everything is
// genuinely present (or the soak proves nothing about the live path).
// ===========================================================================
static void S_NativeHandsSoak()
{
    printf("--- native hands: 10k-frame hostile soak through the wiring ---\n");
    NHResetForCase();

    const float headPos0[3] = { 0, 0, 0 };
    float headAxisI[9]; NHIdentityAxis(headAxisI);
    const float qIdentity[4] = { 0, 0, 0, 1 };
    float rAxis[9], lAxis[9]; NHIdentityAxis(rAxis); NHIdentityAxis(lAxis);
    const float rPos[3] = { 0.20f, -0.05f, 0.55f }, lPos[3] = { -0.20f, -0.05f, 0.55f };
    NHBuildHandVerts(rAxis, rPos, lAxis, lPos);   // g_verts carries real landmarks

    const void* hostiles[] = {
        g_verts,                                   // the genuine article
        VBO_HAND_OFFSET, VBO_GUN_OFFSET,
        (const void*)0,
        (const void*)(uintptr_t)0xDEADBEEFu,
    };

    int faults = 0;
    unsigned engagedPairs = 0;
    const int kFrames = 10000;
    for (int f = 0; f < kFrames; ++f) {
        if ((f % 5) == 0) { GLSurf_BeginFrame(); GLSurf_SceneQueued(f & 1); }
        GLSurf_ScenePhase(1);
        GLSurf_NoteDepthRange(0.0, 0.2);
        GLSurf_NoteViewport(0, 0, 1440, 1440);

        // Buffer churn under the wiring, same shapes as the vbo soak.
        switch (f % 13) {
            case 0:  UploadHandVbo(); break;
            case 4:  VBO_NoteBindBuffer(VBO_GL_ARRAY_BUFFER, 999u); break;
            case 8:  { const unsigned dead = VBOT_ARRAY_ID;
                       VBO_NoteDeleteBuffers(1, &dead); } break;
            case 11: VBO_NoteBindBuffer(VBO_GL_ARRAY_BUFFER, 0); break;
            default: break;
        }
        GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, hostiles[f % 5]);
        GLSurf_NoteClientState(GLS_GL_VERTEX_ARRAY, 1);

        // The wiring: enablement toggles, tracking flickers, pairs begin.
        VRNH_SetEnabled((f % 97) < 90);
        VRInputState in; memset(&in, 0, sizeof(in));
        const float gripXr[3] = { 0.01f * (f % 7), 0, -0.10f - 0.001f * (f % 11) };
        in.hand[VRI_RIGHT] = NHHand((f % 3) != 2, gripXr, qIdentity);
        in.hand[VRI_LEFT]  = NHHand((f % 4) != 3, gripXr, qIdentity);
        __try {
            VRNH_OnPairBegin((unsigned)f + 1, &in, headPos0, headAxisI,
                             (f % 23) != 22, (double)f * 0.008);
        } __except (EXCEPTION_EXECUTE_HANDLER) { ++faults; }

        VRNHDebug dbg; VRNH_GetDebug(&dbg);
        if (dbg.hand[VRI_RIGHT].engaged) ++engagedPairs;

        if (DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                        GLS_GL_UNSIGNED_INT, g_handIdx))
            ++faults;
        if (faults > 3) break;
    }

    GLSurfStats st; GLSurf_GetStats(&st);
    CHECK(faults == 0, "%d frames of wiring + hostile arrays, %d faults",
          kFrames, faults);
    CHECK(GLSurf_ReentryCount() == 0, "0 re-entries (%u)", GLSurf_ReentryCount());
    // Good frames are deliberately rare (valid landmarks AND no buffer bound
    // AND tracked AND enabled AND head located all have to line up), so the
    // bar is "engaged repeatedly", not a fraction of the run.
    CHECK(engagedPairs > 100,
          "the wiring genuinely ENGAGED on the good frames (%u pairs) -- a "
          "soak that never engages proves nothing about the live path",
          engagedPairs);
    GLSurfHandSolve hs; GLSurf_GetHandSolve(GLSC_HAND_R, &hs);
    CHECK(hs.solves > 0 && hs.refusals > 0,
          "solves ran (%u) AND hostile frames were refused (%u), by count",
          hs.solves, hs.refusals);
    VRHandPose_TestRestore();
    VRNH_TestReset();
    ResetLayerForCase();
}

// ===========================================================================
// Stereo invariant (Phase 13): the pair-latched offset/rotation is applied
// IDENTICALLY to whatever a given draw's own vertices recover -- not to a
// cached absolute matrix from a different draw. That is the actual content
// of "translation is eye-invariant", and it is what makes eye 0 and eye 1
// (two draws with genuinely different authored recoveries, standing in for
// the two eyes of a pair) reconstruct the SAME physical target.
// ===========================================================================
static void S_NativeHandsStereoInvariant()
{
    printf("--- native hands: pair-latched offset is eye-invariant ---\n");

    NHResetForCase();
    VRNH_SetEnabled(1);
    const float headPos0[3] = { 0, 0, 0 };
    float headAxisI[9]; NHIdentityAxis(headAxisI);
    const float qIdentity[4] = { 0, 0, 0, 1 };

    // "Eye 0": the baseline authored recovery this pair's offset gets built
    // against.
    float axis0[9]; HpAxisFromEuler(4.0f, -6.0f, 2.0f, axis0);
    const float pos0[3] = { 0.18f, -0.02f, 0.50f };
    float lAxis[9]; NHIdentityAxis(lAxis);
    const float lPos[3] = { -0.2f, 0, 0.5f };
    NHBuildHandVerts(axis0, pos0, lAxis, lPos);
    NHDriveHandDraw();                              // seeds the baseline (= "eye 0")

    const float gripXr[3] = { 0, 0, -0.30f };
    VRInputState in; memset(&in, 0, sizeof(in));
    in.hand[VRI_RIGHT] = NHHand(1, gripXr, qIdentity);
    // Pair 4 arms solve-wanted (disengaged, no baseline yet); its draw seeds
    // the baseline; pair 5 engages against it. Same two-pair convergence the
    // wiring performs in production.
    VRNH_OnPairBegin(4, &in, headPos0, headAxisI, 1, 0.0);
    NHDriveHandDraw();
    VRNH_OnPairBegin(5, &in, headPos0, headAxisI, 1, 0.0);

    // "eye 0" draw: same vertices as the baseline (a real eye 0 would use its
    // OWN authored recovery, which for a frozen pair is this one).
    RecReset();
    NHDriveHandDraw();
    const GLRec* mv0 = NthKind(REC_LOADMV, 0);
    CHECK(mv0 != nullptr, "eye 0's applied matrix was recorded");
    if (!mv0) { VRHandPose_TestRestore(); VRNH_TestReset(); return; }   // cannot continue without it
    // COPY the matrix out: mv0 points into the recorder ring, and the
    // RecReset before eye 1's draw reuses that very slot. Comparing through
    // the stale pointer compares eye 1 with itself and the mutant check
    // reads 0.00000 m -- which is how this line earned its existence.
    float m0[16]; memcpy(m0, mv0->m, sizeof(m0));
    float target0[3]; HpXform4(m0, pos0, target0);

    // "eye 1": a SECOND draw whose OWN authored recovery genuinely differs --
    // this is the case the brief calls out explicitly ("engine authored
    // viewmodel geometry differs between game frames/eyes"). The SAME
    // pair-latched offset/rotation (never resampled, never recomputed) must
    // still be what BuildHandDelta applies.
    float axis1[9]; HpAxisFromEuler(4.0f + 3.0f, -6.0f - 2.0f, 2.0f + 1.0f, axis1);
    const float pos1[3] = { pos0[0] + 0.01f, pos0[1] - 0.006f, pos0[2] + 0.004f };
    NHBuildHandVerts(axis1, pos1, lAxis, lPos);
    RecReset();
    NHDriveHandDraw();
    const GLRec* mv1 = NthKind(REC_LOADMV, 0);
    CHECK(mv1 != nullptr, "eye 1's applied matrix was recorded");
    if (!mv1) { VRHandPose_TestRestore(); VRNH_TestReset(); return; }
    float target1[3]; HpXform4(mv1->m, pos1, target1);

    // The correct invariant (review 2026-08-29, section 5): the two draws'
    // authored poses DIFFER, and both FINAL palms land on the SAME
    // pair-latched desired pose -- each draw's delta absorbed its own
    // authored recovery. (The earlier relative-offset form let the authored
    // difference through into the finals, i.e. rendered the animation advance
    // between the game frames as disparity; this case used to assert that.)
    CHECK_NEAR(HpDist(target1, target0), 0.0, 1e-4,
               "both draws' finals land on ONE desired pose (%.5f u apart) "
               "although their authored poses differ by %.4f u",
               HpDist(target1, target0), HpDist(pos1, pos0));
    {
        VRNHDebug dd; VRNH_GetDebug(&dd);
        CHECK_NEAR(HpDist(target0, dd.hand[VRI_RIGHT].targetEyeish), 0.0, 1e-4,
                   "...and that pose is the controller target");
    }

    // The mutant Phase 13 requires this suite to catch: reusing eye 0's whole
    // applied matrix on eye 1's vertices instead of recomputing from eye 1's
    // own recovery. With a nonzero rotation between the two authored poses,
    // that must land somewhere measurably different from the correct target.
    float mutantTarget1[3]; HpXform4(m0, pos1, mutantTarget1);
    const float mutantErr = HpDist(mutantTarget1, target1);
    CHECK(mutantErr > 1e-4f,
          "MUTANT CHECK: reusing eye 0's raw delta on eye 1's vertices is "
          "measurably wrong (%.5f m) -- proves this suite would catch it", mutantErr);

    VRHandPose_TestRestore();
    VRNH_TestReset();
}

// ===========================================================================
// THE GUN ON THE RIGHT CONTROLLER (glsurf layer)
// ===========================================================================

static float    g_gunVertsBuf[2048 * 3];
static unsigned g_gunIdxBuf[4096];

// Engine-scale gun batch from a REAL shipped model's own landmark table.
// Returns the index count; indices cover [0, vertexCount) with minIdx 0.
static int NHBuildGunBatch(const CodvrGunModel* m, const float axis[9],
                           const float posU[3])
{
    const int nv = m->vertexCount;
    for (int i = 0; i < nv * 3 && i < 2048 * 3; ++i)
        g_gunVertsBuf[i] = 500.0f + (float)(i % 13);
    for (int i = 0; i < m->n; ++i) {
        const float bindU[3] = { m->lm[i].bindM[0] * 39.3701f,
                                 m->lm[i].bindM[1] * 39.3701f,
                                 m->lm[i].bindM[2] * 39.3701f };
        float r[3];
        HpApply(axis, bindU, r);
        float* v = &g_gunVertsBuf[m->lm[i].index * 3];
        v[0] = posU[0] + r[0];
        v[1] = posU[1] + r[1];
        v[2] = posU[2] + r[2];
    }
    int ic = 0;
    for (int i = 0; i < nv && ic < 4096; ++i)
        g_gunIdxBuf[ic++] = (unsigned)i;
    while ((ic % 3) != 0 && ic < 4096)
        g_gunIdxBuf[ic++] = (unsigned)(nv - 1);
    return ic;
}

static int NHDriveGunDraw(const CodvrGunModel* m, const float axis[9],
                          const float posU[3])
{
    const int ic = NHBuildGunBatch(m, axis, posU);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_gunVertsBuf);
    const int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, ic,
                                            GLS_GL_UNSIGNED_INT, g_gunIdxBuf);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_verts);
    return owned;
}

// A second, NON-keyed gun batch (another surface of the same weapon): any
// depth-hacked unblended triangle batch whose count matches no fingerprint.
static int NHDriveOtherGunBatch()
{
    for (int i = 0; i < 600; ++i)
        g_gunIdxBuf[i] = (unsigned)(i % 200);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_gunVertsBuf);
    const int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 600,
                                            GLS_GL_UNSIGNED_INT, g_gunIdxBuf);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_verts);
    return owned;
}

static void NHScene(int eye)
{
    GLSurf_BeginFrame();
    GLSurf_SceneQueued(eye);
    EnterViewmodelPass();
}

// BOTH eye-scenes of one XR pair, with the same viewmodel draws in each.
//
// FIXTURE FIDELITY. A case that renders only eye 0 is modelling half a pair,
// which the production renderer cannot produce -- and once pair-level stereo
// eligibility exists, half a pair is correctly refused. Every case that wants
// a hand to ENGAGE therefore has to show the layer what a real pair looks
// like: two eye-scenes, each with its own draws, before the next pair boundary.
static void NHDriveHandDraw();
static int  NHDriveGunDraw(const CodvrGunModel* m, const float axis[9],
                           const float posU[3]);

static void NHPairScenes(const CodvrGunModel* gun, const float gAxis[9],
                         const float gPos[3])
{
    for (int eye = 0; eye < 2; ++eye) {
        NHScene(eye);
        NHDriveHandDraw();
        if (gun) NHDriveGunDraw(gun, gAxis, gPos);
    }
}

static void S_NativeGunFollow()
{
    printf("--- the gun follows the right controller (glsurf layer) ---\n");
    const CodvrGunModel* m = VRGunPose_ByVertexCount(326);   // thompson body
    CHECK(m && m->rearGripUsable, "the thompson body (key 326) has a usable grip");
    if (!m) return;

    float axis0[9];
    HpAxisFromEuler(4.0f, -12.0f, 2.0f, axis0);
    const float pos0[3] = { 6.0f, -4.0f, -14.0f };
    float pivot0[3];
    {
        const float gU[3] = { m->rearGripM[0]*39.3701f, m->rearGripM[1]*39.3701f,
                              m->rearGripM[2]*39.3701f };
        float r[3]; HpApply(axis0, gU, r);
        for (int k = 0; k < 3; ++k) pivot0[k] = pos0[k] + r[k];
    }

    // ---- baseline: solve-wanted fills the authored body with NO target ----
    NHResetForCase();
    GLSurf_SetGunSolveWanted(1);
    int owned = NHDriveGunDraw(m, axis0, pos0);
    CHECK(owned == 0, "no target: the gun batch is left to the engine");
    GLSurfGunSolve gs; GLSurf_GetGunSolve(&gs);
    CHECK(gs.haveBody == 1 && gs.lastVertexCount == 326,
          "baseline: the keyed batch solved (verts %d)", gs.lastVertexCount);
    CHECK_NEAR(HpDist(gs.pivotU, pivot0), 0.0, 1e-3,
               "baseline: the authored rear grip was recovered (%.4f u off)",
               HpDist(gs.pivotU, pivot0));

    // ---- the delta pins the rear grip to the target -----------------------
    float wantAxis[9];
    HpAxisFromEuler(4.0f + 20.0f, -12.0f + 15.0f, 2.0f, wantAxis);
    const float wantGrip[3] = { pivot0[0] + 2.0f, pivot0[1] - 1.0f, pivot0[2] + 3.0f };
    GLSurf_SetGunTarget(1, wantGrip, wantAxis);
    NHScene(0);
    RecReset();
    owned = NHDriveGunDraw(m, axis0, pos0);
    CHECK(owned == 1, "with a target, the keyed gun batch is transformed");
    const GLRec* d0 = NthKind(REC_LOADMV, 0);
    CHECK(d0 != nullptr, "the applied gun matrix was recorded");
    if (d0) {
        float landed[3];
        HpXform4(d0->m, pivot0, landed);
        CHECK_NEAR(HpDist(landed, wantGrip), 0.0, 1e-3,
                   "the REAR GRIP lands on the controller target (%.4f u off)",
                   HpDist(landed, wantGrip));
    }
    GLSurf_GetGunSolve(&gs);
    CHECK(gs.valid == 1 && gs.rearGripErrU < 1e-3f,
          "rearGripErr %.5f u -- the pin is exact", gs.rearGripErrU);

    // ---- ONE rigid pose per scene: every other gun batch reuses it --------
    RecReset();
    owned = NHDriveOtherGunBatch();
    CHECK(owned == 1, "a second gun batch in the same scene is transformed too");
    const GLRec* d1 = NthKind(REC_LOADMV, 0);
    CHECK(d0 && d1 && memcmp(d0->m, d1->m, sizeof(d0->m)) == 0,
          "...with the SAME cached scene delta: one rigid weapon");
    GLSurf_GetGunSolve(&gs);
    CHECK(gs.batchesTransformed >= 2, "both batches counted (%u)", gs.batchesTransformed);

    // ---- a batch no shipped weapon has, before any key this scene -------
    // (2026-09-04 P0: never vanilla beside a native weapon. With no key yet
    // and no surface of the current model to carry the baseline for, the
    // batch is SUPPRESSED -- owned, drawn nowhere -- and counted.)
    NHScene(0);
    RecReset();
    {
        GLSurfGunSolve g0; GLSurf_GetGunSolve(&g0);
        owned = NHDriveOtherGunBatch();     // keyed batch never appears this scene
        GLSurfGunSolve g1; GLSurf_GetGunSolve(&g1);
        CHECK(owned == 1 && CountKind(REC_LOADMV) == 0
              && g1.batchesSuppressed == g0.batchesSuppressed + 1 && g1.batchesVanilla == g0.batchesVanilla,
              "no key this scene, unknown surface: SUPPRESSED (no matrix, not vanilla)");
    }

    // ---- WEAK rear grip: placed by the AUTHORED relation about the body
    //      origin (2026-09-04d). Refusing it used to leave a head-coupled
    //      object beside a controller-owned hand, which on hardware was a
    //      grenade floating two feet above the hand. Nothing is invented:
    //      the palm->body relation is measured live, exactly as the rifle's
    //      palm->rear-grip relation is; only the point it is expressed
    //      about changes, and a rigid placement does not depend on that.
    const CodvrGunModel* weak = VRGunPose_ByVertexCount(542);   // m1carbine
    CHECK(weak && !weak->rearGripUsable, "the m1carbine's grip is below STRONG");
    if (weak) {
        NHScene(0);
        RecReset();
        owned = NHDriveGunDraw(weak, axis0, pos0);
        GLSurf_GetGunSolve(&gs);
        CHECK(owned == 1 && CountKind(REC_LOADMV) > 0 && gs.pivotIsBodyOrigin == 1,
              "a weapon with no STRONG rear grip is still controller-owned, pivoting on "
              "its body origin (never head-coupled beside a native hand)");
        CHECK(gs.batchesVanilla == 0, "...and nothing of it is drawn vanilla");
    }

    GLSurf_SetGunTarget(0, nullptr, nullptr);
    GLSurf_SetGunSolveWanted(0);
}

// One physical weapon per pair: per-eye deltas differ, but both eyes'
// reconstructed PairWorld rear grip is the SAME point.
static void S_NativeGunStereoInvariant()
{
    printf("--- the gun: per-eye deltas, one PairWorld pose ---\n");
    const CodvrGunModel* m = VRGunPose_ByVertexCount(326);
    if (!m) { CHECK(0, "thompson body table missing"); return; }

    NHResetForCase();
    GLSurf_SetGunSolveWanted(1);

    // The pair's ONE latched target.
    float wantAxis[9];
    HpAxisFromEuler(10.0f, 30.0f, -5.0f, wantAxis);
    const float wantGrip[3] = { 3.0f, -5.0f, -12.0f };
    GLSurf_SetGunTarget(1, wantGrip, wantAxis);

    // Eye 0 and eye 1 recover DIFFERENT authored poses (the IPD shift plus a
    // touch of rotation), exactly as live per-eye solves do.
    float axisE[2][9];
    float posE[2][3] = { { 6.0f, -4.0f, -14.0f }, { 8.5f, -4.1f, -13.8f } };
    HpAxisFromEuler(4.0f, -12.0f, 2.0f, axisE[0]);
    HpAxisFromEuler(4.5f, -11.0f, 2.2f, axisE[1]);

    float recon[2][3];
    float deltas[2][16];
    for (int eye = 0; eye < 2; ++eye) {
        NHScene(eye);
        RecReset();
        const int owned = NHDriveGunDraw(m, axisE[eye], posE[eye]);
        CHECK(owned == 1, "eye %d: keyed batch transformed", eye);
        const GLRec* d = NthKind(REC_LOADMV, 0);
        CHECK(d != nullptr, "eye %d: matrix recorded", eye);
        if (!d) return;
        memcpy(deltas[eye], d->m, sizeof(deltas[eye]));
        float pivot[3];
        {
            const float gU[3] = { m->rearGripM[0]*39.3701f,
                                  m->rearGripM[1]*39.3701f,
                                  m->rearGripM[2]*39.3701f };
            float r[3]; HpApply(axisE[eye], gU, r);
            for (int k = 0; k < 3; ++k) pivot[k] = posE[eye][k] + r[k];
        }
        HpXform4(deltas[eye], pivot, recon[eye]);
    }
    CHECK(memcmp(deltas[0], deltas[1], sizeof(deltas[0])) != 0,
          "the raw per-eye deltas DIFFER (each eye's own authored pose)");
    // The PairWorld gate: 0.5 mm = 0.0197 units.
    CHECK_NEAR(HpDist(recon[0], recon[1]), 0.0, 0.0197,
               "both eyes reconstruct the SAME physical rear grip "
               "(%.5f u apart)", HpDist(recon[0], recon[1]));
    CHECK_NEAR(HpDist(recon[0], wantGrip), 0.0, 1e-3,
               "...and it is the latched target");

    // MUTANT: reusing eye 0's delta on eye 1's authored pose misses by the
    // authored difference -- the stereo gate would catch it loudly.
    float pivot1[3];
    {
        const float gU[3] = { m->rearGripM[0]*39.3701f, m->rearGripM[1]*39.3701f,
                              m->rearGripM[2]*39.3701f };
        float r[3]; HpApply(axisE[1], gU, r);
        for (int k = 0; k < 3; ++k) pivot1[k] = posE[1][k] + r[k];
    }
    float wrong[3];
    HpXform4(deltas[0], pivot1, wrong);
    CHECK(HpDist(wrong, wantGrip) > 0.5f,
          "MUTANT: eye 0's delta on eye 1's weapon misses by %.3f u -- caught",
          HpDist(wrong, wantGrip));

    GLSurf_SetGunTarget(0, nullptr, nullptr);
    GLSurf_SetGunSolveWanted(0);
}

// ===========================================================================
// EVERY SHIPPED HAND RIG SOLVES; A MIRRORED HAND NEVER DOES
// ===========================================================================
static void S_NativeRigCoverage()
{
    printf("--- every shipped hand rig solves (real tables, engine scale) ---\n");
    VRHandPose_TestRestore();                    // the REAL tables
    CHECK(VRHandPose_RigCount() == 3, "three shipped rigs are tabled (%d)",
          VRHandPose_RigCount());

    static float verts[2048 * 3];
    for (int ri = 0; ri < VRHandPose_RigCount(); ++ri) {
        const CodvrHandRigDef* rd = VRHandPose_Rig(ri);
        for (int hand = 0; hand < VRHP_HANDS; ++hand) {
            const CodvrPalmLandmark* lm = (hand == VRHP_TRIGGER) ? rd->trig : rd->supp;
            const int n = (hand == VRHP_TRIGGER) ? rd->nTrig : rd->nSupp;
            float axis[9];
            HpAxisFromEuler(7.0f + 5.0f * ri, -20.0f + 9.0f * hand, 3.0f, axis);
            const float posU[3] = { 4.0f + ri, -3.0f - hand, -11.0f };
            for (int i = 0; i < rd->batchVerts * 3 && i < 2048 * 3; ++i)
                verts[i] = 900.0f + (float)(i % 11);
            for (int i = 0; i < n; ++i) {
                const float bindU[3] = { lm[i].bindM[0] * 39.3701f,
                                         lm[i].bindM[1] * 39.3701f,
                                         lm[i].bindM[2] * 39.3701f };
                float r[3];
                HpApply(axis, bindU, r);
                float* v = &verts[lm[i].index * 3];
                v[0] = posU[0] + r[0]; v[1] = posU[1] + r[1]; v[2] = posU[2] + r[2];
            }
            VRHandPose hp;
            int gotRig = -3;
            const int ok = VRHandPose_SolveAny(hand, verts, 12, rd->batchVerts,
                                               39.3701f, &hp, &gotRig);
            CHECK(ok == 1, "rig %d (%s) %s hand solves at engine scale",
                  ri, rd->name, hand == VRHP_TRIGGER ? "TRIGGER" : "SUPPORT");
            CHECK_NEAR(HpDist(hp.pos, posU), 0.0, 0.08,
                       "rig %d %s: position recovered (%.4f u off)", ri,
                       hand == VRHP_TRIGGER ? "R" : "L", HpDist(hp.pos, posU));
            // hands_new IS the installed default set, so it may resolve as
            // -1; hands_sten's palm cloud is identical to hands_new within a
            // micrometre, so either identity is a correct answer for it.
            if (ri == 2)
                CHECK(gotRig == 2, "the hands4 rig identifies as itself (got %d)",
                      gotRig);

            // The MIRROR mutant with REAL data: the other hand's table must
            // refuse this batch -- a proper rotation cannot mirror a palm.
            // (Not for the merged rigs' opposite table, whose indices simply
            // lie outside this half's content -- that is the index gate, also
            // fine -- but for hands4 both tables share [0,302): geometry alone
            // must decide.)
            if (rd->splitVert == 0) {
                VRHandPose hp2;
                int rig2 = -3;
                const int wrongHand = 1 - hand;
                const int ok2 = VRHandPose_SolveAny(wrongHand, verts, 12,
                                                    rd->batchVerts, 39.3701f,
                                                    &hp2, &rig2);
                CHECK(ok2 == 0,
                      "MUTANT: the mirrored (other) palm table REFUSES this "
                      "hands4 batch -- laterality cannot silently swap");
            }
        }
    }
}

// ===========================================================================
// THE FULL HOLD: controller -> right hand -> weapon, one coherent chain
// ===========================================================================
static void S_NativeGunHold()
{
    printf("--- native hold: gun target composed from the authored relation ---\n");
    const float headPos0[3] = { 0, 0, 0 };
    float headAxisI[9]; NHIdentityAxis(headAxisI);
    const float qIdentity[4] = { 0, 0, 0, 1 };
    const CodvrGunModel* gun = VRGunPose_ByVertexCount(326);
    if (!gun) { CHECK(0, "thompson body table missing"); return; }

    NHResetForCase();
    VRNH_SetEnabled(1);
    VRNH_SetTwoHandEnabled(0);
    VRNH_SetReloadEnabled(0);

    // Authored scene content: hands + the weapon, at known poses.
    float rAxis0[9], lAxis0[9], gAxis0[9];
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis0);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis0);
    HpAxisFromEuler(2.0f, 14.0f, 1.0f, gAxis0);
    const float rPos0[3] = { 0.20f, -0.05f, 0.55f };
    const float lPos0[3] = { -0.20f, -0.05f, 0.55f };
    const float gPos0[3] = { 5.0f, -4.0f, -13.0f };

    const float gripXr[3] = { 0.0f, 0.0f, -0.30f };
    VRInputState in; memset(&in, 0, sizeof(in));
    in.hand[VRI_RIGHT] = NHHand(1, gripXr, qIdentity);

    // Pair 1: seeds both baselines (hand + gun) under solve-wanted.
    VRNH_OnPairBegin(21, &in, headPos0, headAxisI, 1, 0.0);
    NHBuildHandVerts(rAxis0, rPos0, lAxis0, lPos0);
    NHDriveHandDraw();
    NHDriveGunDraw(gun, gAxis0, gPos0);
    // Pair 2: everything engages, the gun target composes.
    NHScene(0);
    VRNH_OnPairBegin(22, &in, headPos0, headAxisI, 1, 0.011);

    VRNHDebug d; VRNH_GetDebug(&d);
    CHECK(d.hand[VRI_RIGHT].engaged == 1, "pair 2: the right hand engages");
    CHECK(d.gun.targetInstalled == 1, "pair 2: the gun target installs with it");
    CHECK(d.gun.vertexCount == 326, "the weapon identity rode along (%d)",
          d.gun.vertexCount);

    // THE CALIBRATION CHAIN, CHECKED AS AN EQUATION. The desired rear grip
    // is the controller-driven palm target carrying the AUTHORED palm->grip
    // relation; residual is zero by construction, and this asserts it.
    float authPivot[3];
    {
        const float gU[3] = { gun->rearGripM[0]*39.3701f, gun->rearGripM[1]*39.3701f,
                              gun->rearGripM[2]*39.3701f };
        float r[3]; HpApply(gAxis0, gU, r);
        for (int k = 0; k < 3; ++k) authPivot[k] = gPos0[k] + r[k];
    }
    // target palm == grip pose (identity calibration), derived exactly as
    // case 2/3 does.
    float wantPalm[3], wantPalmAxis[9];
    {
        float roomM[3]; VRM_XrToQuakeVec(gripXr, roomM);
        float lm[3]; VRM_WorldToLocalVec(headAxisI, roomM, lm);
        const float lu[3] = { lm[0]*39.37f, lm[1]*39.37f, lm[2]*39.37f };
        VRM_QuakeToGLEyeVec(lu, wantPalm);
        NHIdentityAxis(wantPalmAxis);
        float la[9]; VRM_WorldToLocalAxis(headAxisI, wantPalmAxis, la);
        VRM_QuakeToGLEyeAxis(la, wantPalmAxis);
    }
    float rel[3], relLocal[3];
    for (int k = 0; k < 3; ++k) rel[k] = authPivot[k] - rPos0[k];
    VRM_WorldToLocalVec(rAxis0, rel, relLocal);
    float carried[3], wantGrip[3];
    VRM_LocalToWorldVec(wantPalmAxis, relLocal, carried);
    for (int k = 0; k < 3; ++k) wantGrip[k] = wantPalm[k] + carried[k];
    CHECK_NEAR(HpDist(d.gun.desiredGripEyeish, wantGrip), 0.0, 2e-3,
               "palm/rear-grip calibration residual %.5f u (~%.2f mm) -- the "
               "authored relation is carried exactly",
               HpDist(d.gun.desiredGripEyeish, wantGrip),
               HpDist(d.gun.desiredGripEyeish, wantGrip) * 25.4f);

    // The weapon actually renders there: this pair's gun draw lands the
    // rear grip on the composed target.
    RecReset();
    NHDriveGunDraw(gun, gAxis0, gPos0);
    const GLRec* dm = NthKind(REC_LOADMV, 0);
    CHECK(dm != nullptr, "the gun draw was transformed");
    if (dm) {
        float landed[3];
        HpXform4(dm->m, authPivot, landed);
        CHECK_NEAR(HpDist(landed, d.gun.desiredGripEyeish), 0.0, 1e-3,
                   "end to end: the rendered rear grip lands on the target");
    }

    // ---- ADS/accuracy state CANNOT move the physical pose ----------------
    // The posing pipeline has no ADS input; prove it stays byte-stable when
    // the ADS control fires between two otherwise identical pairs.
    float before[3];
    memcpy(before, d.gun.desiredGripEyeish, sizeof(before));
    VRBinds_TapControl(VRB_ADS);
    NHScene(0);
    NHDriveHandDraw();
    NHDriveGunDraw(gun, gAxis0, gPos0);
    VRNH_OnPairBegin(23, &in, headPos0, headAxisI, 1, 0.022);
    VRNH_GetDebug(&d);
    CHECK_NEAR(HpDist(d.gun.desiredGripEyeish, before), 0.0, 1e-5,
               "an ADS tap between pairs changes the physical gun pose by "
               "%.6f u -- it must not move at all",
               HpDist(d.gun.desiredGripEyeish, before));

    // ---- tracking loss: bounded grace, then the weapon disengages --------
    in.hand[VRI_RIGHT].trackState = VRI_TRACK_UNAVAILABLE;
    in.hand[VRI_RIGHT].active = 0;
    in.hand[VRI_RIGHT].gripValid = 0;
    NHScene(0);
    VRNH_OnPairBegin(24, &in, headPos0, headAxisI, 1, 5.0);
    VRNH_GetDebug(&d);
    CHECK(d.gun.targetInstalled == 0,
          "right tracking gone -> the gun disengages to vanilla");

    VRNH_TestReset();
    VRHandPose_TestSetLandmarks(VRHP_TRIGGER, kNHRight, NH_N);
    VRHandPose_TestSetLandmarks(VRHP_SUPPORT, kNHLeft, NH_N);
}

// ===========================================================================
// TWO-HAND SUPPORT: grab, slide, release, detach
// ===========================================================================
static void S_NativeTwoHand()
{
    printf("--- two-hand support: deliberate grab, sliding contact ---\n");
    const float headPos0[3] = { 0, 0, 0 };
    float headAxisI[9]; NHIdentityAxis(headAxisI);
    const float qIdentity[4] = { 0, 0, 0, 1 };
    const CodvrGunModel* gun = VRGunPose_ByVertexCount(326);
    if (!gun || !gun->supportUsable) {
        CHECK(gun && gun->supportUsable, "thompson support chain is STRONG+");
        return;
    }

    NHResetForCase();
    VRNH_SetEnabled(1);
    VRNH_SetTwoHandEnabled(1);
    VRNH_SetReloadEnabled(0);

    float rAxis0[9], lAxis0[9], gAxis0[9];
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis0);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis0);
    HpAxisFromEuler(0.0f, 0.0f, 0.0f, gAxis0);
    const float rPos0[3] = { 0.20f, -0.05f, 0.55f };
    const float lPos0[3] = { -0.20f, -0.05f, 0.55f };
    const float gPos0[3] = { 5.0f, -4.0f, -13.0f };

    const float gripXr[3] = { 0.0f, 0.0f, -0.30f };
    VRInputState in; memset(&in, 0, sizeof(in));
    in.hand[VRI_RIGHT] = NHHand(1, gripXr, qIdentity);
    in.hand[VRI_LEFT]  = NHHand(1, gripXr, qIdentity);   // will be re-aimed

    // Seed pair, then engage pair.
    VRNH_OnPairBegin(31, &in, headPos0, headAxisI, 1, 0.0);
    NHBuildHandVerts(rAxis0, rPos0, lAxis0, lPos0);
    NHDriveHandDraw();
    NHDriveGunDraw(gun, gAxis0, gPos0);
    NHScene(0);
    NHDriveHandDraw();
    NHDriveGunDraw(gun, gAxis0, gPos0);
    VRNH_OnPairBegin(32, &in, headPos0, headAxisI, 1, 0.011);
    VRNHDebug d; VRNH_GetDebug(&d);
    CHECK(d.gun.targetInstalled == 1, "one-hand hold established");
    CHECK(d.gun.twoHandState == VRNH_2H_LEFT_FREE, "left starts FREE");

    // Compute where the support segment's midpoint sits in the render space
    // under the CURRENT hold, and put the LEFT controller exactly there.
    // (Room->eyeish with an identity head reduces to quake->GL, which is
    // what the XR grip conversion produces -- so aim the XR grip by
    // inverting that mapping: eyeish e -> xr = (e2? ...). Easiest: search.)
    float supEyeish[3];
    {
        // production formula, replicated with the pair's published target
        const float sU[3] = { gun->supportM[0], gun->supportM[1], gun->supportM[2] };
        const float relM[3] = { (sU[0] - gun->rearGripM[0]) * 39.3701f,
                                (sU[1] - gun->rearGripM[1]) * 39.3701f,
                                (sU[2] - gun->rearGripM[2]) * 39.3701f };
        // applied axis == desired axis on the install pair; reconstruct it
        // from the published target grip and the authored relation is
        // overkill here -- read the rendered delta instead.
        // The hold pair's target axis equals the palm target axis composed
        // with the authored relation; for this test the head/calibration are
        // identity so we can take the axis from the recorded gun transform.
        RecReset();
        NHDriveGunDraw(gun, gAxis0, gPos0);
        const GLRec* dm = NthKind(REC_LOADMV, 0);
        if (!dm) { CHECK(0, "no gun transform recorded"); return; }
        // supEyeish = delta * authored support point.
        float authSup[3];
        {
            float r[3]; HpApply(gAxis0, relM, r);
            float authPivot[3];
            const float gU[3] = { gun->rearGripM[0]*39.3701f,
                                  gun->rearGripM[1]*39.3701f,
                                  gun->rearGripM[2]*39.3701f };
            float rp[3]; HpApply(gAxis0, gU, rp);
            for (int k = 0; k < 3; ++k) authPivot[k] = gPos0[k] + rp[k];
            for (int k = 0; k < 3; ++k) authSup[k] = authPivot[k] + r[k];
        }
        HpXform4(dm->m, authSup, supEyeish);
    }
    // eyeish -> XR grip with identity head: quake = (-e2, -e0, e1)/39.37;
    // xr = (-q1, q2, -q0).
    float xrLeft[3];
    {
        const float q[3] = { -supEyeish[2] / 39.37f, -supEyeish[0] / 39.37f,
                             supEyeish[1] / 39.37f };
        xrLeft[0] = -q[1]; xrLeft[1] = q[2]; xrLeft[2] = -q[0];
    }
    in.hand[VRI_LEFT] = NHHand(1, xrLeft, qIdentity);

    // Within radius, no squeeze: CANDIDATE.
    NHScene(0);
    NHDriveHandDraw();
    NHDriveGunDraw(gun, gAxis0, gPos0);
    VRNH_OnPairBegin(33, &in, headPos0, headAxisI, 1, 0.022);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_CANDIDATE,
          "left inside the grab radius -> SUPPORT_CANDIDATE (residual %.3f u)",
          d.gun.supportResidualU);

    // Squeeze: ATTACHED.
    in.hand[VRI_LEFT].squeezeDown = 1;
    NHScene(0);
    NHDriveHandDraw();
    NHDriveGunDraw(gun, gAxis0, gPos0);
    VRNH_OnPairBegin(34, &in, headPos0, headAxisI, 1, 0.033);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED,
          "deliberate squeeze at the fore-end -> SUPPORT_ATTACHED");
    const float tAtAttach = d.gun.supportT;

    // Slide: move the left hand ALONG THE BORE (its direction measured from
    // the recorded transform, not guessed); t follows, stays clamped.
    float xrSlid[3];
    {
        RecReset();
        NHDriveGunDraw(gun, gAxis0, gPos0);
        const GLRec* dm2 = NthKind(REC_LOADMV, 0);
        float rows[9] = { 1,0,0, 0,1,0, 0,0,1 };
        if (dm2) {
            rows[0] = dm2->m[0]; rows[1] = dm2->m[1]; rows[2] = dm2->m[2];
            rows[3] = dm2->m[4]; rows[4] = dm2->m[5]; rows[5] = dm2->m[6];
            rows[6] = dm2->m[8]; rows[7] = dm2->m[9]; rows[8] = dm2->m[10];
        }
        float bore[3];
        VRM_LocalToWorldVec(rows, gun->barrelFwd, bore);
        float e[3];
        for (int k = 0; k < 3; ++k)
            e[k] = supEyeish[k] + 3.0f * bore[k];
        const float q[3] = { -e[2] / 39.37f, -e[0] / 39.37f, e[1] / 39.37f };
        xrSlid[0] = -q[1]; xrSlid[1] = q[2]; xrSlid[2] = -q[0];
    }
    in.hand[VRI_LEFT] = NHHand(1, xrSlid, qIdentity);
    in.hand[VRI_LEFT].squeezeDown = 1;
    NHScene(0);
    NHDriveHandDraw();
    NHDriveGunDraw(gun, gAxis0, gPos0);
    VRNH_OnPairBegin(35, &in, headPos0, headAxisI, 1, 0.044);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED, "still attached while sliding");
    CHECK(d.gun.supportT >= 0.0f && d.gun.supportT <= 1.0f,
          "slide parameter stays in [0,1] (t=%.2f)", d.gun.supportT);
    CHECK(fabsf(d.gun.supportT - tAtAttach) > 1e-3f,
          "the contact actually SLID (t %.3f -> %.3f)", tAtAttach, d.gun.supportT);

    // Release: back to FREE, one-hand hold resumes (target still installed).
    in.hand[VRI_LEFT].squeezeDown = 0;
    NHScene(0);
    NHDriveHandDraw();
    NHDriveGunDraw(gun, gAxis0, gPos0);
    VRNH_OnPairBegin(36, &in, headPos0, headAxisI, 1, 0.055);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_LEFT_FREE, "grip released -> LEFT_FREE");
    CHECK(d.gun.targetInstalled == 1, "the one-hand hold carries on seamlessly");

    // Far away with squeeze held: never attaches (no magic grab).
    float xrFar[3] = { 1.0f, 1.0f, -2.0f };
    in.hand[VRI_LEFT] = NHHand(1, xrFar, qIdentity);
    in.hand[VRI_LEFT].squeezeDown = 1;
    NHScene(0);
    NHDriveHandDraw();
    NHDriveGunDraw(gun, gAxis0, gPos0);
    VRNH_OnPairBegin(37, &in, headPos0, headAxisI, 1, 0.066);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_LEFT_FREE,
          "squeezing far from the fore-end grabs nothing");

    VRNH_TestReset();
    VRHandPose_TestSetLandmarks(VRHP_TRIGGER, kNHRight, NH_N);
    VRHandPose_TestSetLandmarks(VRHP_SUPPORT, kNHLeft, NH_N);
}

// ===========================================================================
// PHYSICAL RELOAD, END TO END THROUGH THE PRODUCTION WIRING
// ===========================================================================

// Aim the LEFT XR grip so its eyeish position corresponds to body-frame
// point B on the CURRENT weapon hold (identity head).
static void NHAimLeftAtBody(VRInputState* in, const VRNHGunDebug& gd,
                            const float appliedAxis[9],
                            const CodvrGunModel* gun, const float bM[3])
{
    const float relU[3] = { (bM[0] - gun->rearGripM[0]) * 39.3701f,
                            (bM[1] - gun->rearGripM[1]) * 39.3701f,
                            (bM[2] - gun->rearGripM[2]) * 39.3701f };
    float w[3];
    VRM_LocalToWorldVec(appliedAxis, relU, w);
    float e[3];
    for (int k = 0; k < 3; ++k) e[k] = gd.desiredGripEyeish[k] + w[k];
    const float q[3] = { -e[2] / 39.37f, -e[0] / 39.37f, e[1] / 39.37f };
    const float xr[3] = { -q[1], q[2], -q[0] };
    const float qi[4] = { 0, 0, 0, 1 };
    const int squeeze = in->hand[VRI_LEFT].squeezeDown;
    in->hand[VRI_LEFT] = NHHand(1, xr, qi);
    in->hand[VRI_LEFT].squeezeDown = squeeze;
}

static void S_NativeReloadIntegration()
{
    printf("--- physical reload through the production wiring ---\n");
    const float headPos0[3] = { 0, 0, 0 };
    float headAxisI[9]; NHIdentityAxis(headAxisI);
    const float qIdentity[4] = { 0, 0, 0, 1 };
    const CodvrGunModel* gun = VRGunPose_ByVertexCount(326);   // thompson
    if (!gun) { CHECK(0, "thompson table missing"); return; }

    NHResetForCase();
    VRBinds_TestReset();
    VRReload_Reset();
    VRNH_SetEnabled(1);
    VRNH_SetTwoHandEnabled(0);          // isolate the gesture from the grab
    VRNH_SetReloadEnabled(1);

    float rAxis0[9], lAxis0[9], gAxis0[9];
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis0);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis0);
    NHIdentityAxis(gAxis0);
    const float rPos0[3] = { 0.20f, -0.05f, 0.55f };
    const float lPos0[3] = { -0.20f, -0.05f, 0.55f };
    const float gPos0[3] = { 5.0f, -4.0f, -13.0f };

    const float gripXr[3] = { 0.0f, 0.0f, -0.30f };
    VRInputState in; memset(&in, 0, sizeof(in));
    in.hand[VRI_RIGHT] = NHHand(1, gripXr, qIdentity);
    in.hand[VRI_LEFT]  = NHHand(1, gripXr, qIdentity);

    VRReloadProfile prof;
    double now = 0.0;
    unsigned serial = 41;
    VRNHDebug d;

    // One pair step: scene + draws + boundary.
    #define NH_RELOAD_PAIR() do {                                        \
        NHScene(0);                                                      \
        NHBuildHandVerts(rAxis0, rPos0, lAxis0, lPos0);                  \
        NHDriveHandDraw();                                               \
        NHDriveGunDraw(gun, gAxis0, gPos0);                              \
        now += 0.011;                                                    \
        VRNH_OnPairBegin(serial++, &in, headPos0, headAxisI, 1, now);    \
        VRNH_GetDebug(&d);                                               \
    } while (0)

    NH_RELOAD_PAIR();                    // seed
    NH_RELOAD_PAIR();                    // engage; weapon identified
    CHECK(d.gun.targetInstalled == 1, "hold established for the reload test");
    VRReload_Get(&prof);
    CHECK(prof.valid == 1 && prof.hand == VRRL_HAND_SUPPORT,
          "the thompson's measured profile selected itself off the live "
          "weapon identity (valid=%d hand=%d)", prof.valid, prof.hand);

    // The applied gun axis for aiming the left hand: with identity head and
    // identity gun-authored axis on the engage pair, the desired axis equals
    // the composed relation; read it from the recorded transform instead of
    // re-deriving: aim helper only needs axis rows, so recover them from a
    // fresh recorded draw.
    float applied[9];
    {
        RecReset();
        NHDriveGunDraw(gun, gAxis0, gPos0);
        const GLRec* dm = NthKind(REC_LOADMV, 0);
        if (!dm) { CHECK(0, "no transform to read the applied axis from"); return; }
        // rows of the applied basis = delta * authored rows (authored is
        // identity here), i.e. the rotation part of the recorded matrix,
        // column-major -> rows.
        applied[0] = dm->m[0]; applied[1] = dm->m[1]; applied[2] = dm->m[2];
        applied[3] = dm->m[4]; applied[4] = dm->m[5]; applied[5] = dm->m[6];
        applied[6] = dm->m[8]; applied[7] = dm->m[9]; applied[8] = dm->m[10];
    }

    // The gesture, exactly as the state machine defines it.
    in.hand[VRI_LEFT].squeezeDown = 0;
    NHAimLeftAtBody(&in, d.gun, applied, gun, prof.firingGripM);
    NH_RELOAD_PAIR();
    CHECK(d.gun.reloadState == VRRL_IDLE, "at the firing grip: IDLE");

    NHAimLeftAtBody(&in, d.gun, applied, gun, prof.loadZoneM);
    NH_RELOAD_PAIR();
    CHECK(d.gun.reloadState == VRRL_REACH, "left the grip: REACH");

    in.hand[VRI_LEFT].squeezeDown = 1;   // grab the magazine at the zone
    NH_RELOAD_PAIR();
    CHECK(d.gun.reloadState == VRRL_GRIPPED, "grip at the zone: GRIPPED");

    // Pull it out: 0.2 m straight away from the zone in body space.
    {
        float away[3];
        for (int k = 0; k < 3; ++k)
            away[k] = prof.loadZoneM[k]
                    + 0.2f * ((prof.loadZoneM[k] - prof.firingGripM[k] >= 0) ? 1.0f : -1.0f);
        NHAimLeftAtBody(&in, d.gun, applied, gun, away);
    }
    NH_RELOAD_PAIR();
    CHECK(d.gun.reloadState == VRRL_PULLED, "0.2 m out: PULLED (magazine free)");

    // To the pouch (head-relative quake fwd 0.15, below 0.45), grip released
    // on the way so the pouch press is an EDGE.
    in.hand[VRI_LEFT].squeezeDown = 0;
    {
        // Pouch, head-relative quake (+0.15 fwd, 0, -0.45 up). quake -> XR
        // inverts VRM_XrToQuakeVec's out = (-v2, -v0, v1): xr = (-q1, q2, -q0).
        const float q[3] = { 0.15f, 0.0f, -0.45f };
        const float xr[3] = { -q[1], q[2], -q[0] };
        in.hand[VRI_LEFT] = NHHand(1, xr, qIdentity);
    }
    NH_RELOAD_PAIR();
    CHECK(d.gun.reloadState == VRRL_POUCH, "at the pouch: POUCH");

    in.hand[VRI_LEFT].squeezeDown = 1;   // edge: take the fresh magazine
    NH_RELOAD_PAIR();
    CHECK(d.gun.reloadState == VRRL_ACQUIRED, "grip edge at the pouch: ACQUIRED");

    NHAimLeftAtBody(&in, d.gun, applied, gun, prof.loadZoneM);
    NH_RELOAD_PAIR();
    CHECK(d.gun.reloadState == VRRL_INSERT, "back at the zone: INSERT");

    const unsigned tapsBefore = VRBinds_TapCount(VRB_RELOAD);
    in.hand[VRI_LEFT].squeezeDown = 0;   // release AT the zone = insertion
    NH_RELOAD_PAIR();
    CHECK(VRBinds_TapCount(VRB_RELOAD) == tapsBefore + 1,
          "EXACTLY ONE engine reload was delivered (taps %u -> %u)",
          tapsBefore, VRBinds_TapCount(VRB_RELOAD));

    // Recover and idle: no second tap, ever, without a full new gesture.
    NHAimLeftAtBody(&in, d.gun, applied, gun, prof.firingGripM);
    NH_RELOAD_PAIR();
    NH_RELOAD_PAIR();
    CHECK(VRBinds_TapCount(VRB_RELOAD) == tapsBefore + 1,
          "no repeat delivery after recovery (still %u)",
          VRBinds_TapCount(VRB_RELOAD));

    // ---- cancellation: menu mid-gesture, zero deliveries -----------------
    NHAimLeftAtBody(&in, d.gun, applied, gun, prof.loadZoneM);
    NH_RELOAD_PAIR();                    // REACH again
    in.menuDown = 1;
    NH_RELOAD_PAIR();
    CHECK(d.gun.reloadState == VRRL_IDLE, "menu open cancels the gesture");
    CHECK(VRBinds_TapCount(VRB_RELOAD) == tapsBefore + 1,
          "a cancelled gesture delivers nothing");
    in.menuDown = 0;

    // ---- tracking loss mid-gesture cancels, and reacquisition cannot
    //      manufacture a commit -------------------------------------------
    NHAimLeftAtBody(&in, d.gun, applied, gun, prof.loadZoneM);
    NH_RELOAD_PAIR();
    in.hand[VRI_LEFT].trackState = VRI_TRACK_UNAVAILABLE;
    in.hand[VRI_LEFT].active = 0;
    in.hand[VRI_LEFT].gripValid = 0;
    NH_RELOAD_PAIR();
    CHECK(d.gun.reloadState == VRRL_IDLE, "tracking loss cancels the gesture");
    NHAimLeftAtBody(&in, d.gun, applied, gun, prof.loadZoneM);   // reacquire AT the zone
    in.hand[VRI_LEFT].squeezeDown = 1;
    NH_RELOAD_PAIR();
    NH_RELOAD_PAIR();
    CHECK(VRBinds_TapCount(VRB_RELOAD) == tapsBefore + 1,
          "reacquiring tracking mid-air manufactures NO commit");

    // The original native wiring measured trigger-hand rifle reloads
    // against a gun pinned to that same controller, so translation could
    // never move the loading hand away from its own gun. The Garand's
    // measured receiver can now be reached by the independent LEFT hand.
    gun = VRGunPose_ByVertexCount(494);
    CHECK(gun != nullptr, "Garand measured gun model exists for receiver integration");
    if (gun) {
        in.hand[VRI_LEFT].squeezeDown = 0;
        NH_RELOAD_PAIR(); NH_RELOAD_PAIR();
        VRReload_Get(&prof);
        CHECK(prof.valid && prof.hand == VRRL_HAND_SUPPORT && prof.directLoad,
              "Garand receiver uses the support controller, with no fake magazine extraction");
        {
            RecReset(); NHDriveGunDraw(gun, gAxis0, gPos0);
            const GLRec* dm = NthKind(REC_LOADMV, 0);
            if (dm) {
                applied[0] = dm->m[0]; applied[1] = dm->m[1]; applied[2] = dm->m[2];
                applied[3] = dm->m[4]; applied[4] = dm->m[5]; applied[5] = dm->m[6];
                applied[6] = dm->m[8]; applied[7] = dm->m[9]; applied[8] = dm->m[10];
            }
        }
        NHAimLeftAtBody(&in, d.gun, applied, gun, prof.firingGripM);
        NH_RELOAD_PAIR(); NH_RELOAD_PAIR();
        const float pouchXr[3] = { 0.0f, -0.45f, -0.15f };
        in.hand[VRI_LEFT] = NHHand(1, pouchXr, qIdentity);
        NH_RELOAD_PAIR(); NH_RELOAD_PAIR();
        CHECK(d.gun.reloadState == VRRL_POUCH,
              "support hand reaches Garand ammunition pouch while right hand keeps the weapon");
        in.hand[VRI_LEFT].squeezeDown = 1; NH_RELOAD_PAIR();
        CHECK(d.gun.reloadState == VRRL_ACQUIRED, "Garand pouch squeeze acquires ammunition");
        NHAimLeftAtBody(&in, d.gun, applied, gun, prof.loadZoneM);
        NH_RELOAD_PAIR();
        CHECK(d.gun.reloadState == VRRL_INSERT, "support hand reaches the actual Garand receiver");
        const unsigned receiverBefore = VRBinds_TapCount(VRB_RELOAD);
        in.hand[VRI_LEFT].squeezeDown = 0; NH_RELOAD_PAIR();
        CHECK(VRBinds_TapCount(VRB_RELOAD) == receiverBefore + 1,
              "Garand receiver release delivers exactly one stock reload");
    }

    #undef NH_RELOAD_PAIR
    VRNH_TestReset();
    VRHandPose_TestSetLandmarks(VRHP_TRIGGER, kNHRight, NH_N);
    VRHandPose_TestSetLandmarks(VRHP_SUPPORT, kNHLeft, NH_N);
}

// ===========================================================================
// THE hands4 RIG'S SINGLE DRAWS TAKE HAND TARGETS TOO
// ===========================================================================
static void S_NativeHands4Singles()
{
    printf("--- hands4 single draws: targets apply outside the merged path ---\n");
    VRHandPose_TestRestore();            // hands4 needs its REAL tables
    const CodvrHandRigDef* h4 = VRHandPose_Rig(2);
    if (!h4 || h4->splitVert != 0) { CHECK(0, "rig 2 is not the singles rig"); return; }

    NHResetForCase();
    VRHandPose_TestRestore();            // NHResetForCase re-injects; undo it

    // Build a 302-vert single-hand batch from the REAL trigger table.
    static float verts[512 * 3];
    static unsigned idx[GLS_HAND4_INDICES];
    float axis[9];
    HpAxisFromEuler(6.0f, -15.0f, 2.0f, axis);
    const float posU[3] = { 5.0f, -3.0f, -12.0f };
    for (int i = 0; i < h4->batchVerts * 3; ++i)
        verts[i] = 700.0f + (float)(i % 9);
    for (int i = 0; i < h4->nTrig; ++i) {
        const float bindU[3] = { h4->trig[i].bindM[0] * 39.3701f,
                                 h4->trig[i].bindM[1] * 39.3701f,
                                 h4->trig[i].bindM[2] * 39.3701f };
        float r[3];
        HpApply(axis, bindU, r);
        float* v = &verts[h4->trig[i].index * 3];
        v[0] = posU[0] + r[0]; v[1] = posU[1] + r[1]; v[2] = posU[2] + r[2];
    }
    for (int i = 0; i < GLS_HAND4_INDICES; ++i)
        idx[i] = (unsigned)(i % GLS_HAND4_VERTS);

    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, verts);
    // Solve-wanted seeds the baseline through the SINGLES path.
    GLSurf_SetHandSolveWanted(GLSC_HAND_R, 1);
    int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND4_INDICES,
                                      GLS_GL_UNSIGNED_INT, idx);
    CHECK(owned == 0, "no target yet: the single is left to the engine");
    GLSurfHandSolve hs;
    CHECK(GLSurf_GetHandSolve(GLSC_HAND_R, &hs) == 1 && hs.solves >= 1,
          "the hands4 single seeded a baseline (%u solves, rig %d)",
          hs.solves, hs.lastRig);
    CHECK(hs.lastRig == 2, "...identified as the hands4 rig");

    // A target moves the SINGLE draw, exactly like a merged half. A fresh
    // frame first: the singles classifier assigns sides by frame ordinal,
    // and a second hand single in the SAME frame would classify as the LEFT.
    const float off[3] = { 2.0f, -1.0f, 0.5f };
    const float ident[9] = { 1,0,0, 0,1,0, 0,0,1 };
    // The target is ABSOLUTE: the desired palm origin, here the authored one
    // displaced by `off`.
    const float wantAbs[3] = { posU[0] + off[0], posU[1] + off[1], posU[2] + off[2] };
    // Production installs BOTH of these at the pair boundary: the target
    // (vr_native_hands) and the pair-level stereo gate that authorises it.
    // A layer-level case has to do the wiring's job explicitly -- the gate
    // is fail-closed, so a target alone must NOT move anything.
    GLSurf_SetHandTarget(GLSC_HAND_R, 1, wantAbs, ident);
    GLSurf_SetHandPairEligible(GLSC_HAND_R, 1);
    NHScene(0);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, verts);
    RecReset();
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND4_INDICES,
                                  GLS_GL_UNSIGNED_INT, idx);
    CHECK(owned == 1, "with a target, the hands4 single is transformed");
    const GLRec* dm = NthKind(REC_LOADMV, 0);
    CHECK(dm != nullptr, "its matrix was recorded");
    if (dm) {
        float landed[3];
        HpXform4(dm->m, posU, landed);
        const float want[3] = { posU[0] + off[0], posU[1] + off[1], posU[2] + off[2] };
        CHECK_NEAR(HpDist(landed, want), 0.0, 1e-3,
                   "the hands4 palm lands on the offset target (%.4f u off)",
                   HpDist(landed, want));
    }
    GLSurf_SetHandTarget(GLSC_HAND_R, 0, nullptr, nullptr);
    GLSurf_SetHandSolveWanted(GLSC_HAND_R, 0);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_verts);
    VRHandPose_TestSetLandmarks(VRHP_TRIGGER, kNHRight, NH_N);
    VRHandPose_TestSetLandmarks(VRHP_SUPPORT, kNHLeft, NH_N);
}

// ===========================================================================
// 10,000 PAIRS THROUGH THE ACTUAL PRODUCTION FUNCTIONS
// ===========================================================================
static void S_NativeHandsProductionSoak()
{
    printf("--- production soak: 10k pairs, dropouts, switches, transitions ---\n");
    const float headPos0[3] = { 0, 0, 0 };
    float headAxisI[9]; NHIdentityAxis(headAxisI);
    const float qIdentity[4] = { 0, 0, 0, 1 };
    const CodvrGunModel* thompson = VRGunPose_ByVertexCount(326);
    const CodvrGunModel* bar      = VRGunPose_ByVertexCount(516);
    if (!thompson || !bar) { CHECK(0, "soak gun tables missing"); return; }

    NHResetForCase();
    VRBinds_TestReset();
    VRReload_Reset();
    VRNH_SetEnabled(1);

    float rAxis0[9], lAxis0[9], gAxis0[9];
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis0);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis0);
    NHIdentityAxis(gAxis0);
    const float rPos0[3] = { 0.20f, -0.05f, 0.55f };
    const float lPos0[3] = { -0.20f, -0.05f, 0.55f };
    const float gPos0[3] = { 5.0f, -4.0f, -13.0f };

    VRInputState in; memset(&in, 0, sizeof(in));
    double now = 0.0;
    int nanFaults = 0, stereoFaults = 0;
    const int kPairs = 10000;

    // ENGAGEMENT COUNTS ARE MANDATORY.
    //
    // The previous version of this soak passed while native engagement was
    // ZERO on every pair: the invariants it checked (no NaN, no stereo
    // mismatch, no re-entry) are all trivially satisfied by a build that never
    // engages anything. "10000 hostile pairs, all green" meant nothing. So the
    // soak now counts what actually happened and REQUIRES it to have happened.
    const CodvrGunModel* soakWeapons[6] = {
        VRGunPose_ByVertexCount(516),    // BAR
        VRGunPose_ByVertexCount(326),    // Thompson
        VRGunPose_ByVertexCount(344),    // Colt
        VRGunPose_ByVertexCount(494),    // M1 Garand
        VRGunPose_ByVertexCount(724),    // MP40
        VRGunPose_ByVertexCount(960),    // frag grenade: no usable rear grip
    };
    for (int i = 0; i < 6; ++i)
        if (!soakWeapons[i]) { CHECK(0, "soak: weapon %d missing", i); return; }
    // 2026-09-04d counters: a weapon switch may not move the palm, no
    // eye-scene may mix owners, and a weapon with a bore must use it.
    unsigned palmJumps = 0, switches = 0, ownerConflicts = 0, mixedOwners = 0;
    unsigned boreMismatches = 0, grenadePairs = 0, grenadeOwned = 0;
    float worstPalmJumpU = 0.0f, worst2hResidDeg = 0.0f;
    float prevPalm[3] = { 0, 0, 0 };
    int   prevPalmValid = 0, prevKey = 0;
    unsigned engR = 0, engL = 0, engGun = 0;
    unsigned failClosed = 0, stereoRefused = 0, leftFreeFar = 0;
    unsigned rigSwitches = 0, twoHandEntries = 0, twoHandExits = 0;
    unsigned halfNativePairs = 0;
    int      prevVerts = 0, prev2h = VRNH_2H_LEFT_FREE;

    for (int p = 0; p < kPairs; ++p) {
        now += 0.011;
        // Hostile schedule: tracking dropouts, weapon switches, grip churn,
        // occasional headless pairs, hands sometimes not drawn at all.
        const int rightUp = !((p % 613) < 9);
        const int leftUp  = !((p % 401) < 13);
        const int headOk  = !((p % 977) == 5);
        // BAR, Thompson, Colt, M1 Garand, MP40 and the frag grenade -- the
        // set the 2026-09-04d brief names, including the one weapon with no
        // usable rear grip, which must still be controller-owned.
        const CodvrGunModel* gun = soakWeapons[(p / 163) % 6];
        // Around every switch the controller is held STILL for two pairs, so
        // the palm may be compared across the switch with nothing else
        // changing: a weapon switch may never move the user's hand.
        const int freezeGrip = ((p % 163) < 2);
        const int drawHands = ((p % 17) != 3);
        const int drawGun   = ((p % 23) != 7);

        float gripXr[3];
        if (freezeGrip) {
            gripXr[0] = 0.10f; gripXr[1] = 0.02f; gripXr[2] = -0.28f;
        } else {
            gripXr[0] = 0.05f * (float)((p % 7) - 3);
            gripXr[1] = 0.02f * (float)(p % 5);
            gripXr[2] = -0.25f - 0.01f * (float)(p % 9);
        }
        // Every so often the LEFT controller is taken a full metre clear of
        // the weapon -- the automated form of "pull my left hand several feet
        // away". Nothing may drag it back or drag the weapon with it.
        const int leftFar = ((p % 137) < 20);
        const float gripLeftXr[3] = { leftFar ? 1.0f : gripXr[0], gripXr[1],
                                      gripXr[2] };
        in.hand[VRI_RIGHT] = NHHand(rightUp, gripXr, qIdentity);
        in.hand[VRI_LEFT]  = NHHand(leftUp, gripLeftXr, qIdentity);
        in.hand[VRI_LEFT].squeezeDown = ((p % 31) < 11);
        in.menuDown = ((p % 499) == 100);

        VRNH_OnPairBegin((unsigned)(100 + p), &in, headPos0, headAxisI,
                         headOk, now);

        for (int eye = 0; eye < 2; ++eye) {
            NHScene(eye);
            if (drawHands) {
                NHBuildHandVerts(rAxis0, rPos0, lAxis0, lPos0);
                NHDriveHandDraw();
            }
            if (drawGun) {
                NHDriveGunDraw(gun, gAxis0, gPos0);
                NHDriveOtherGunBatch();
            }
            VRObj_NoteEyeConsumed(eye);
        }

        VRNHDebug d; VRNH_GetDebug(&d);
        for (int k = 0; k < 3; ++k) {
            const float v = d.gun.desiredGripEyeish[k];
            if (v != v || fabsf(v) > 1e6f) ++nanFaults;
        }
        if (d.gun.supportT != d.gun.supportT) ++nanFaults;
        if (!VRObj_PairConsistent()) ++stereoFaults;

        // ---- 2026-09-04d: the palm, the owners, the bore ----------------
        {
            GLSurfGunScene sc;
            for (int e = 0; e < 2; ++e) {
                if (!GLSurf_GetGunScene(e, &sc)) continue;
                if (sc.weaponOwnerConflict) ++ownerConflicts;
                if (sc.weaponDrawVanilla > 0
                    && (sc.weaponDrawNative + sc.weaponDrawStale + sc.weaponDrawPartKeyed) > 0)
                    ++mixedOwners;
            }
            // A weapon switch with the controller held still must leave the
            // palm exactly where it was.
            if (freezeGrip && d.hand[VRI_RIGHT].engaged && d.hand[VRI_RIGHT].haveTarget) {
                if (prevPalmValid && d.gun.vertexCount != prevKey && prevKey != 0) {
                    ++switches;
                    const float jump = HpDist(prevPalm, d.hand[VRI_RIGHT].targetEyeish);
                    if (jump > worstPalmJumpU) worstPalmJumpU = jump;
                    if (jump > 1e-3f) ++palmJumps;
                }
                memcpy(prevPalm, d.hand[VRI_RIGHT].targetEyeish, sizeof(prevPalm));
                prevPalmValid = 1;
                if (d.gun.vertexCount) prevKey = d.gun.vertexCount;
            } else if (!freezeGrip) {
                prevPalmValid = 0;
            }
            // The grenade: no rear grip, and still controller-owned.
            if (gun == soakWeapons[5]) {
                ++grenadePairs;
                if (d.gun.targetInstalled) ++grenadeOwned;
            }
            // A weapon with a derived bore must be firing along it.
            if (d.gun.barrelValid) {
                const CodvrGunModel* m = d.gun.vertexCount
                    ? VRGunPose_ByVertexCount(d.gun.vertexCount) : 0;
                if (m && m->boreUsable && !d.gun.boreFromSightLine) ++boreMismatches;
            }
            if (d.gun.twoHandState == VRNH_2H_ATTACHED
                && d.gun.angularResidualDeg > worst2hResidDeg)
                worst2hResidDeg = d.gun.angularResidualDeg;
        }

        // ---- what actually happened this pair ---------------------------
        if (d.hand[VRI_RIGHT].engaged) ++engR;
        if (d.hand[VRI_LEFT].engaged)  ++engL;
        if (d.gun.targetInstalled)     ++engGun;
        for (int h = 0; h < VRI_HANDS; ++h) {
            if (!d.hand[h].engaged) ++failClosed;
            if (d.hand[h].reason == VRNH_HAND_STEREO_DISAGREE) ++stereoRefused;
        }
        if (leftFar && d.hand[VRI_LEFT].engaged
            && d.gun.twoHandState == VRNH_2H_LEFT_FREE) ++leftFreeFar;
        if (d.gun.vertexCount && d.gun.vertexCount != prevVerts) {
            if (prevVerts) ++rigSwitches;
            prevVerts = d.gun.vertexCount;
        }
        if (d.gun.twoHandState != prev2h) {
            if (d.gun.twoHandState == VRNH_2H_ATTACHED)      ++twoHandEntries;
            else if (prev2h == VRNH_2H_ATTACHED)             ++twoHandExits;
            prev2h = d.gun.twoHandState;
        }
        // THE HALF-NATIVE CHECK, every pair. An engaged hand whose pair gate
        // is shut, or a disengaged hand whose gate is open, is one eye's
        // decision leaking past the pair.
        for (int c = 0; c < 2; ++c) {
            const int cls = c ? GLSC_HAND_R : GLSC_HAND_L;
            const int hh  = c ? VRI_RIGHT   : VRI_LEFT;
            if (d.hand[hh].engaged != GLSurf_HandPairEligible(cls))
                ++halfNativePairs;
        }
    }

    unsigned pairs = 0, mismatches = 0, late = 0, splits = 0;
    VRObj_GetCounters(&pairs, &mismatches, &late, &splits);
    CHECK(nanFaults == 0, "%d pairs: zero NaN/absurd values in the weapon "
          "chain (%d)", kPairs, nanFaults);
    CHECK(stereoFaults == 0, "zero pair-consistency violations (%d)",
          stereoFaults);
    CHECK(mismatches == 0 && splits == 0,
          "vr_objects saw zero cross-eye mismatches (%u) and zero serial "
          "splits (%u)", mismatches, splits);
    CHECK(GLSurf_PairMismatchCount() == 0,
          "glsurf saw zero latch mismatches across %d hostile pairs", kPairs);
    GLSurfHandSolve hr;
    GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
    CHECK(hr.solves > 1000, "the palm solve ran throughout (%u solves)", hr.solves);
    GLSurfGunSolve gs;
    GLSurf_GetGunSolve(&gs);
    CHECK(gs.solves > 500, "the gun solve ran throughout (%u scene solves)",
          gs.solves);
    CHECK(GLSurf_ReentryCount() == 0, "zero draw re-entries");

    // ---- THE COUNTS. A soak that cannot say what engaged proves nothing --
    GLSurfHandSolve hl;
    GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
    printf("      soak counts: pairs %d | engaged R %u L %u gun %u | "
           "deltas R %u L %u gun %u | fail-closed %u (stereo %u) | "
           "LEFT_FREE-far %u | rig switches %u | 2H in/out %u/%u\n",
           kPairs, engR, engL, engGun, hr.deltasApplied, hl.deltasApplied,
           gs.batchesTransformed, failClosed, stereoRefused, leftFreeFar,
           rigSwitches, twoHandEntries, twoHandExits);

    CHECK(engR > 1000 && engL > 1000,
          "soak: native engagement ACTUALLY HAPPENED -- R %u, L %u pairs of "
          "%d (this is the check the old soak did not have)", engR, engL, kPairs);
    CHECK(engGun > 500, "soak: the gun was driven on %u pairs", engGun);
    CHECK(hr.deltasApplied > 1000 && hl.deltasApplied > 1000,
          "soak: RIGHT-owned and LEFT-owned draws were actually TRANSFORMED "
          "(%u / %u) -- engagement without deltas is the failure that looks "
          "identical in a headset", hr.deltasApplied, hl.deltasApplied);
    CHECK(gs.batchesTransformed > 500,
          "soak: gun-owned draws transformed (%u)", gs.batchesTransformed);
    CHECK(halfNativePairs == 0,
          "soak: ZERO half-native stereo pairs across %d pairs (%u)",
          kPairs, halfNativePairs);
    CHECK(failClosed > 0,
          "soak: the hostile schedule DID force fail-closed pairs (%u) -- a "
          "soak in which nothing ever refuses is not hostile", failClosed);
    CHECK(leftFreeFar > 100,
          "soak: LEFT_FREE stayed controller-owned a metre from the weapon on "
          "%u pairs, with no support-point capture", leftFreeFar);
    CHECK(rigSwitches > 0, "soak: weapon switches exercised (%u)", rigSwitches);
    printf("      2026-09-04d: switches measured %u | palm jumps %u (worst %.5f u) | "
           "owner conflicts %u | mixed-owner eye-scenes %u | bore mismatches %u | "
           "grenade pairs %u owned %u | worst two-hand residual %.3f deg\n",
           switches, palmJumps, worstPalmJumpU, ownerConflicts, mixedOwners,
           boreMismatches, grenadePairs, grenadeOwned, worst2hResidDeg);
    CHECK(switches >= 20 && palmJumps == 0,
          "soak: %u weapon switches with the controller held still, and the palm did not "
          "move once (worst %.5f u)", switches, worstPalmJumpU);
    CHECK(ownerConflicts == 0 && mixedOwners == 0,
          "soak: zero owner conflicts and zero mixed-owner eye-scenes");
    CHECK(boreMismatches == 0,
          "soak: every weapon that has a derived bore fired along it");
    CHECK(grenadePairs > 500 && grenadeOwned > grenadePairs / 2,
          "soak: the grenade was equipped on %u pairs and controller-owned on %u of them "
          "(never head-coupled beside a native hand)", grenadePairs, grenadeOwned);
    CHECK(hr.refusals + hl.refusals >= 0,
          "soak: refusals classified by site -- R idx/base/solve %u/%u/%u, "
          "L %u/%u/%u", hr.refusedIdx, hr.refusedBase, hr.refusedSolve,
          hl.refusedIdx, hl.refusedBase, hl.refusedSolve);

    VRNH_TestReset();
}

// ===========================================================================
// PRODUCTION POSE OWNERSHIP -- the 2026-08-28 hardware-run response
// ===========================================================================
//
// THIS SCENARIO STARTS AT THE REAL XR SEAM. Every other native-hands case
// above hands VRNH_OnPairBegin a VRHandState it built itself. That is exactly
// how the shipped engagement bug survived 1419 green tests: NHHand() set
//
//     hs.stable = VRI_TRACK_ACTIVE;          // 2   (field since DELETED)
//
// while the REAL producer (vr_input.cpp) only ever wrote
//
//     hs.stable = (... ACTIVE || ... LOST) ? 1 : 0;
//
// and the consumer asked `hs->stable == VRI_TRACK_ACTIVE`, i.e. 1 == 2, which
// is ALWAYS FALSE. The field is gone and trackState is an enum class now, so
// neither line above would compile today -- but the SEAM this case starts at
// is what makes that guarantee load-bearing rather than a comment. On hardware every pair logged "R{eng=0 TRACKING}" while
// both controllers tracked perfectly. The fixture agreed with the bug, so the
// suite could not see it.
//
// So this case constructs NO input state at all. It moves the MOCK OPENXR
// CONTROLLERS and lets the real chain run:
//
//   mock xrLocateSpace -> VRInput_Sync -> VRInput_State()
//     -> VR_BeginFrameEx's pair boundary -> VRNH_OnPairBegin
//     -> GLSurf_SetHandTarget / VRObj_SetHandPose -> BuildHandDelta -> draw
//
// and asserts on the PUBLISHED poses and the ACTUAL applied GL matrices.
// Against the pre-fix build every engagement assertion here fails.
//
// Fixed head + fixed authored viewmodel throughout, so any change in a
// published pose can only have come from a controller.

static double g_prodNow = 5000.0;

// THE REAL VIEWMODEL MODELVIEW, not the identity every other case uses.
//
// Measured (runs/vbogame-20260827-155348 capture, hand batch seq 8): the
// engine's modelview for the viewmodel is the Quake-view -> GL-eye
// permutation -- submitted (fwd, left, up) maps to eye (right, up, back), so
// submitted +x is eye -z. Driving the layer with an identity modelview makes
// "submitted space" and "eye space" the same space, which is precisely what
// hid the delta being built in one and applied in the other. A translation is
// included as well (the gun batches really do carry one, ~(0.72,-4.73,-10.58))
// so the full rigid term is exercised, not just the rotation.
static void MockRealViewmodelModelview()
{
    memset(g_mockModelview, 0, sizeof(float) * 16);
    g_mockModelview[2]  = -1.0f;    // col0: submitted +x -> eye -z (forward)
    g_mockModelview[4]  = -1.0f;    // col1: submitted +y -> eye -x (left)
    g_mockModelview[9]  =  1.0f;    // col2: submitted +z -> eye +y (up)
    g_mockModelview[12] =  0.72f;
    g_mockModelview[13] = -4.73f;
    g_mockModelview[14] = -10.58f;
    g_mockModelview[15] =  1.0f;
}

static void NHProdScene(int eye, const CodvrGunModel* gun,
                        const float gunAxis[9], const float gunPosU[3])
{
    NHScene(eye);
    NHDriveHandDraw();
    if (gun) NHDriveGunDraw(gun, gunAxis, gunPosU);
}

// One complete PRODUCTION XR pair: the two game frames of an F1 pair driven
// through VR_BeginFrameEx (which is what calls VRInput_Sync and, at the real
// pair boundary, VRNH_OnPairBegin), with each eye's viewmodel draws in between
// -- the production order, not a convenience order.
static void NHProdPair(const CodvrGunModel* gun, const float gunAxis[9],
                       const float gunPosU[3])
{
    g_prodNow += 1.0 / 120.0;
    CODVR_TestSetNow(g_prodNow);          // deterministic tracking-grace clock
    StepNativeGameFrame(0);               // eye 0 frame == the pair boundary
    NHProdScene(0, gun, gunAxis, gunPosU);
    StepNativeGameFrame(0);               // eye 1 frame, SAME XR pair
    NHProdScene(1, gun, gunAxis, gunPosU);
}

static void NHProdPairs(int n, const CodvrGunModel* gun, const float gunAxis[9],
                        const float gunPosU[3])
{
    for (int i = 0; i < n; ++i) NHProdPair(gun, gunAxis, gunPosU);
}

static float NHPoseDist(const VRObjPose& a, const VRObjPose& b)
{
    return HpDist(a.pos, b.pos);
}

// The angle, in degrees, between two rows-as-axes bases: acos((tr(A*B^T)-1)/2).
// Comparing a single axis ROW understates the rotation whenever that row lies
// near the rotation axis -- 25 degrees of controller yaw moved the gun's
// forward row by only 5.4 degrees, which would have let a badly-tracking
// weapon pass a row-wise check.
static float NHBasisAngleDeg(const float a[9], const float b[9])
{
    float tr = 0.0f;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k)
            tr += a[r*3 + k] * b[r*3 + k];
    float c = (tr - 1.0f) * 0.5f;
    if (c >  1.0f) c =  1.0f;
    if (c < -1.0f) c = -1.0f;
    return acosf(c) * 57.2957795f;
}

static int NHPoseIdentical(const VRObjPose& a, const VRObjPose& b)
{
    return memcmp(a.pos, b.pos, sizeof(a.pos)) == 0
        && memcmp(a.axis, b.axis, sizeof(a.axis)) == 0
        && a.valid == b.valid;
}

// ===========================================================================
// TRACKING-STATE TYPE SAFETY -- the structural close of the 2026-08-28 trap
// ===========================================================================
//
// The bug was NOT "a consumer used the wrong operator". It was that the data
// model made a nonsense comparison well-typed:
//
//     struct VRHandState { int stable; int trackState; ... };
//     if (h.stable == VRI_TRACK_ACTIVE)      // 1 == 2, always false
//
// Two changes make that unrepresentable rather than merely tested-against:
//   * `stable` is DELETED. It held exactly (ACTIVE || LOST), so it was
//     derivable; its only unique contribution was the collision.
//   * `trackState` is a VRITrack `enum class`, so no int in the struct can be
//     compared to a VRI_TRACK_* name without an explicit cast.
//
// The compile-time half is asserted below with static_assert -- if someone
// re-widens VRITrack to a plain enum or adds an int alias for it, the BUILD
// breaks. The runtime half walks the whole state space and then runs the
// MUTANT: the old expression, reconstructed from the values the deleted field
// would have held, must be unsatisfiable on every state a real producer can
// reach. That is what "always false" means, stated as a test.
static void S_TrackStateTypeSafety()
{
    printf("--- tracking-state type safety: the encoding trap is unrepresentable ---\n");

    // ---- compile-time: the enum cannot decay into the int namespace -------
    static_assert(!std::is_same<VRITrack, int>::value,
                  "VRITrack must not be a plain int alias");
    static_assert(!std::is_convertible<VRITrack, int>::value,
                  "VRITrack must not implicitly convert to int -- that is the "
                  "conversion that made `someInt == VRI_TRACK_ACTIVE` legal");
    static_assert(std::is_same<decltype(VRHandState::trackState), VRITrack>::value,
                  "VRHandState::trackState must be the enum class itself");
    static_assert(sizeof(VRITrack) == sizeof(int), "VRITrack stays int-sized");
    CHECK(1, "typesafe: VRITrack is a distinct, non-convertible type "
             "(4 static_asserts held at compile time)");

    // ---- the truth table, over the WHOLE state space ---------------------
    struct Row { VRITrack st; int active; int lost; int usable; const char* name; };
    const Row rows[] = {
        { VRI_TRACK_UNAVAILABLE, 0, 0, 0, "UNAVAILABLE" },
        { VRI_TRACK_ACQUIRING,   0, 0, 0, "ACQUIRING"   },
        { VRI_TRACK_ACTIVE,      1, 0, 1, "ACTIVE"      },
        { VRI_TRACK_LOST,        0, 1, 1, "LOST"        },
    };
    for (int i = 0; i < 4; ++i) {
        VRHandState h; memset(&h, 0, sizeof(h));
        h.trackState = rows[i].st;
        CHECK(VRI_HandTrackingActive(h) == rows[i].active
              && VRI_HandTrackingLost(h) == rows[i].lost
              && VRI_HandTrackingUsable(h) == rows[i].usable,
              "typesafe: %s -> active %d / lost %d / usable %d",
              rows[i].name, VRI_HandTrackingActive(h),
              VRI_HandTrackingLost(h), VRI_HandTrackingUsable(h));
        // USABLE is exactly ACTIVE-or-LOST: no third source of truth.
        CHECK(VRI_HandTrackingUsable(h)
                  == (VRI_HandTrackingActive(h) || VRI_HandTrackingLost(h)),
              "typesafe: %s -- USABLE is derived, not stored", rows[i].name);
    }

    // ---- THE MUTANT: restore the old comparison and require it to fail ---
    //
    // `oldStable` is precisely what the deleted field held. The shipped gate
    // compared it to the ENUM. Walk every state a producer can reach and show
    // the mutant gate is false in all of them -- including the two where
    // production must engage. A build that regressed to this expression would
    // disengage the hands on 100% of pairs, which is what hardware saw.
    int mutantEverTrue = 0, mutantDisagreements = 0;
    for (int i = 0; i < 4; ++i) {
        VRHandState h; memset(&h, 0, sizeof(h));
        h.trackState = rows[i].st;
        const int oldStable = VRI_HandTrackingUsable(h);          // 0 or 1
        const int mutantGate = (oldStable == VRI_TrackCode(VRI_TRACK_ACTIVE));
        const int productionGate = VRI_HandTrackingUsable(h);
        if (mutantGate) ++mutantEverTrue;
        if (mutantGate != productionGate) ++mutantDisagreements;
    }
    CHECK(mutantEverTrue == 0,
          "typesafe: MUTANT `stable == VRI_TRACK_ACTIVE` is UNSATISFIABLE over "
          "the entire state space (fired %d/4 -- must be 0)", mutantEverTrue);
    CHECK(mutantDisagreements == 2,
          "typesafe: MUTANT disagrees with production on exactly the two "
          "engaging states (ACTIVE, LOST) -- %d found", mutantDisagreements);

    // ---- a fixture cannot construct the illegal state --------------------
    //
    // The old fixture wrote `hs.stable = VRI_TRACK_ACTIVE`. The nearest thing
    // that still compiles is assigning the enum to trackState, which is the
    // CORRECT thing -- so the fixture is now forced into production shape.
    {
        VRHandState h; memset(&h, 0, sizeof(h));
        h.trackState = VRI_TRACK_ACTIVE;
        CHECK(VRI_HandTrackingUsable(h) == 1 && VRI_TrackCode(h) == 2,
              "typesafe: the only assignment that compiles is the correct one");
    }
}

static void S_ProductionPoseOwnership()
{
    printf("--- production pose ownership: real XR controllers -> rendered hands/gun ---\n");

    // 0.10 m of controller travel, in the units the render space uses.
    const float MOVE_M = 0.10f;
    const float MOVE_U = MOVE_M * 39.37f;          // 3.937 u

    const CodvrGunModel* gun = VRGunPose_ByVertexCount(516);   // the BAR, as on hardware
    CHECK(gun && gun->rearGripUsable,
          "prod: the BAR body (key 516) is a STRONG-grip model, as used on hardware");
    if (!gun) return;
    float gunAxis[9];
    HpAxisFromEuler(4.0f, -12.0f, 2.0f, gunAxis);
    const float gunPosU[3] = { 6.0f, -4.0f, -14.0f };

    // ---- bring the REAL stack up ----------------------------------------
    ResetAll();
    NHResetForCase();                       // GL layer + synthetic landmarks
    MockRealViewmodelModelview();           // ...and the REAL viewmodel matrix
    VRNH_SetEnabled(1);
    VRNH_SetTwoHandEnabled(0);              // isolate pose ownership
    VRNH_SetReloadEnabled(0);
    float rAxis0[9], lAxis0[9];
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis0);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis0);
    const float rPos0[3] = { 0.20f, -0.05f, 0.55f }, lPos0[3] = { -0.20f, -0.05f, 0.55f };
    NHBuildHandVerts(rAxis0, rPos0, lAxis0, lPos0);   // FIXED authored pose

    SetMockHead(0, 0, 0, 0);                // FIXED head for the whole case
    CHECK(BringUp(), "prod: real mocked-XR session up");
    SetHandGripPos(VRI_RIGHT, 0.20f, -0.10f, -0.35f);
    SetHandGripPos(VRI_LEFT, -0.20f, -0.10f, -0.35f);
    SetHandAim(VRI_RIGHT, 0, 0);
    SetHandAim(VRI_LEFT, 0, 0);

    // Warm-up: tracking must pass the acquire hysteresis AND the draws must
    // seed the authored baselines. Both happen through the production path.
    NHProdPairs(VRI_ACQUIRE_SYNCS + 6, gun, gunAxis, gunPosU);

    // ---- A/B: the production chain ENGAGES from real controller state ----
    //
    // THE REGRESSION ASSERTION. Pre-fix this reads eng=0 / TRACKING, exactly
    // as the hardware log did, because `stable`(1) == VRI_TRACK_ACTIVE(2) is
    // false. Nothing downstream can be tested until this passes.
    {
        VRNHDebug d; VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].engaged == 1,
              "prod: RIGHT engages from REAL controller state (reason %s)",
              VRNH_HandReasonName(d.hand[VRI_RIGHT].reason));
        CHECK(d.hand[VRI_LEFT].engaged == 1,
              "prod: LEFT engages from REAL controller state (reason %s)",
              VRNH_HandReasonName(d.hand[VRI_LEFT].reason));
        CHECK(d.hand[VRI_RIGHT].trackState == VRI_TrackCode(VRI_TRACK_ACTIVE),
              "prod: the telemetry trackState carries the ENUM code (%d)",
              d.hand[VRI_RIGHT].trackState);
        CHECK(d.gun.targetInstalled == 1,
              "prod: the gun target installs too (reason %s, verts %d)",
              VRNH_GunReasonName(d.gun.reason), d.gun.vertexCount);
    }

    // The producer's actual encoding, asserted against the REAL layer -- the
    // fact the old fixture got wrong.
    {
        const VRInputState* in = VRInput_State();
        CHECK(in->hand[VRI_RIGHT].trackState == VRI_TRACK_ACTIVE,
              "prod: vr_input publishes trackState as the VRITrack enum (got %d)",
              VRI_TrackCode(in->hand[VRI_RIGHT]));
        CHECK(VRI_HandTrackingActive(in->hand[VRI_RIGHT]) == 1
              && VRI_HandTrackingUsable(in->hand[VRI_RIGHT]) == 1
              && VRI_HandTrackingLost(in->hand[VRI_RIGHT]) == 0,
              "prod: the named predicates agree with the real producer");

        // FIXTURE FIDELITY. Every other native-hands case builds its input
        // with NHHand(). Pin that fixture against the LIVE producer here, in
        // the one scenario where both exist at once: if they ever disagree
        // about the encoding again, this fails instead of 1400 tests quietly
        // agreeing with a bug.
        const float gp[3] = { 0, 0, 0 };
        const float gq[4] = { 0, 0, 0, 1 };
        const VRHandState fake = NHHand(1, gp, gq);
        CHECK(fake.trackState == in->hand[VRI_RIGHT].trackState
              && fake.active == in->hand[VRI_RIGHT].active
              && VRI_HandTrackingUsable(fake)
                     == VRI_HandTrackingUsable(in->hand[VRI_RIGHT]),
              "prod: the NHHand() fixture encodes tracking EXACTLY as the real "
              "producer does (fixture %d/%d vs live %d/%d)",
              fake.active, VRI_TrackCode(fake),
              in->hand[VRI_RIGHT].active, VRI_TrackCode(in->hand[VRI_RIGHT]));
    }

    // ---- C: RIGHT moves alone; LEFT must not budge ----------------------
    VRObjPose r0 = VRObj_Set()->hand[VROBJ_HAND_R];
    VRObjPose l0 = VRObj_Set()->hand[VROBJ_HAND_L];
    VRObjPose g0 = VRObj_Set()->gun;
    const unsigned serial0 = VRObj_Set()->pairSerial;
    CHECK(r0.valid && l0.valid, "prod: both hand poses published valid");

    // Camera baseline, to prove controller motion never reaches it.
    float cam0[3]; memcpy(cam0, g_out[0], sizeof(cam0));
    float cam1[3]; memcpy(cam1, g_out[1], sizeof(cam1));

    SetHandGripPos(VRI_RIGHT, 0.20f + MOVE_M, -0.10f, -0.35f);
    NHProdPairs(3, gun, gunAxis, gunPosU);

    {
        const VRObjectSet* s = VRObj_Set();
        const float dR = NHPoseDist(s->hand[VROBJ_HAND_R], r0);
        CHECK_NEAR(dR, MOVE_U, 0.02,
                   "prod: 10 cm of RIGHT controller travel moves the published "
                   "RIGHT hand %.3f u (want %.3f, 1:1)", dR, MOVE_U);
        CHECK(NHPoseIdentical(s->hand[VROBJ_HAND_L], l0),
              "prod: the LEFT published pose is BYTE-IDENTICAL (independence)");
        CHECK(s->pairSerial > serial0,
              "prod: the pair serial advanced (%u -> %u)", serial0, s->pairSerial);

        // D: the gun rides the RIGHT controller 1:1 (orientation unchanged).
        const float dG = NHPoseDist(s->gun, g0);
        CHECK(s->gun.valid == 1, "prod: the gun pose is published valid");
        CHECK_NEAR(dG, MOVE_U, 0.02,
                   "prod: and the published GUN rear grip moved the same %.3f u "
                   "(want %.3f)", dG, MOVE_U);

        // G: camera untouched by controller motion.
        CHECK(memcmp(cam0, g_out[0], sizeof(cam0)) == 0
              && memcmp(cam1, g_out[1], sizeof(cam1)) == 0,
              "prod: the rendered eye origins did NOT move with the controller");
    }

    // ---- C (mirror): LEFT moves alone; RIGHT and the gun must not budge --
    {
        const VRObjPose rA = VRObj_Set()->hand[VROBJ_HAND_R];
        const VRObjPose lA = VRObj_Set()->hand[VROBJ_HAND_L];
        const VRObjPose gA = VRObj_Set()->gun;
        SetHandGripPos(VRI_LEFT, -0.20f, -0.10f + MOVE_M, -0.35f);
        NHProdPairs(3, gun, gunAxis, gunPosU);
        const VRObjectSet* s = VRObj_Set();
        const float dL = NHPoseDist(s->hand[VROBJ_HAND_L], lA);
        CHECK_NEAR(dL, MOVE_U, 0.02,
                   "prod: 10 cm of LEFT controller travel moves the published "
                   "LEFT hand %.3f u (want %.3f, 1:1)", dL, MOVE_U);
        CHECK(NHPoseIdentical(s->hand[VROBJ_HAND_R], rA),
              "prod: the RIGHT published pose is BYTE-IDENTICAL");
        CHECK(NHPoseIdentical(s->gun, gA),
              "prod: and the one-hand GUN pose is BYTE-IDENTICAL "
              "(the left hand does not own the weapon)");
    }

    // ---- RIGHT rotation turns the hand AND the weapon --------------------
    {
        const VRObjPose rA = VRObj_Set()->hand[VROBJ_HAND_R];
        const VRObjPose gA = VRObj_Set()->gun;
        const VRObjPose lA = VRObj_Set()->hand[VROBJ_HAND_L];
        SetHandAim(VRI_RIGHT, 25.0f, 0.0f);          // 25 deg of controller yaw
        NHProdPairs(3, gun, gunAxis, gunPosU);
        const VRObjectSet* s = VRObj_Set();
        const float handTurn = NHBasisAngleDeg(s->hand[VROBJ_HAND_R].axis, rA.axis);
        CHECK_NEAR(handTurn, 25.0, 0.5,
                   "prod: 25 deg of RIGHT controller yaw turns the published "
                   "RIGHT hand %.2f deg (1:1)", handTurn);
        const float gunTurn = NHBasisAngleDeg(s->gun.axis, gA.axis);
        CHECK_NEAR(gunTurn, 25.0, 0.5,
                   "prod: ...and turns the published GUN the same %.2f deg -- "
                   "the weapon rides the controller's ORIENTATION, not just its "
                   "position", gunTurn);
        // The left hand is a bystander to a right-hand rotation.
        CHECK(NHPoseIdentical(s->hand[VROBJ_HAND_L], lA),
              "prod: the LEFT pose is BYTE-IDENTICAL through a RIGHT rotation");
    }

    // ---- E: the per-eye GL deltas really change ---------------------------
    //
    // Not "a pose was published" -- the matrix the DRAW actually received.
    {
        SetHandAim(VRI_RIGHT, 0, 0);
        SetHandGripPos(VRI_RIGHT, 0.20f, -0.10f, -0.35f);
        NHProdPairs(3, gun, gunAxis, gunPosU);

        // Eye 0 of the next pair, with the hand where it is now.
        g_prodNow += 1.0 / 120.0; CODVR_TestSetNow(g_prodNow);
        StepNativeGameFrame(0);
        NHScene(0);
        RecReset();
        NHDriveHandDraw();
        // Both hands are engaged, so the split issues two transformed halves,
        // each as loadMatrixf(wanted) + loadMatrixf(restore). MergedHalves
        // draws the LEFT half first, so index 2 is the RIGHT half's matrix --
        // reading index 0 measures the LEFT hand, which is exactly the mistake
        // that made this assertion read 0.000 u the first time it ran.
        const int nmv = CountKind(REC_LOADMV);
        CHECK(nmv == 4,
              "prod: both halves transformed -- wanted+restore each (%d matrices)",
              nmv);
        const GLRec* mR0 = NthKind(REC_LOADMV, 2);
        CHECK(mR0 != nullptr, "prod: the RIGHT half's applied matrix was recorded");
        float landedA[3] = { 0, 0, 0 };
        if (mR0) {
            HpXform4(mR0->m, rPos0, landedA);
            // The matrix the DRAW received puts the authored palm exactly on
            // the pose that was published for this pair -- expressed in THIS
            // eye's frame: the published pose is head-midpoint, the draw is
            // eye 0, and the two differ by eye 0's own offset (real IPD in
            // this scenario, so the difference is 1.26 u, not 0).
            VRNHDebug dd; VRNH_GetDebug(&dd);
            const float* pub = VRObj_Set()->hand[VROBJ_HAND_R].pos;
            const float wantEye0[3] = { pub[0] - dd.eyeOffEyeish[0][0],
                                        pub[1] - dd.eyeOffEyeish[0][1],
                                        pub[2] - dd.eyeOffEyeish[0][2] };
            CHECK_NEAR(HpDist(landedA, wantEye0), 0.0, 1e-3,
                       "prod: the received GL delta lands the authored palm ON "
                       "the published RIGHT pose in eye 0's frame (%.4f u off; "
                       "eye offset %.3f u)",
                       HpDist(landedA, wantEye0), HpDist(pub, wantEye0));
        }
        StepNativeGameFrame(0);
        NHProdScene(1, gun, gunAxis, gunPosU);

        // Move the controller, run a pair, re-measure the SAME authored point.
        SetHandGripPos(VRI_RIGHT, 0.20f + MOVE_M, -0.10f, -0.35f);
        NHProdPairs(2, gun, gunAxis, gunPosU);
        g_prodNow += 1.0 / 120.0; CODVR_TestSetNow(g_prodNow);
        StepNativeGameFrame(0);
        NHScene(0);
        RecReset();
        NHDriveHandDraw();
        const GLRec* mR1 = NthKind(REC_LOADMV, 2);
        CHECK(mR1 != nullptr, "prod: the second RIGHT-half matrix was recorded");
        if (mR0 && mR1) {
            float landedB[3];
            HpXform4(mR1->m, rPos0, landedB);
            const float moved = HpDist(landedA, landedB);
            CHECK_NEAR(moved, MOVE_U, 0.02,
                       "prod: the RENDERED right palm moved %.3f u in the GL "
                       "delta the draw received (want %.3f)", moved, MOVE_U);
        }
        StepNativeGameFrame(0);
        NHProdScene(1, gun, gunAxis, gunPosU);
    }

    // ---- pair discipline held throughout ---------------------------------
    {
        unsigned pairs = 0, mism = 0, late = 0, splits = 0;
        VRObj_GetCounters(&pairs, &mism, &late, &splits);
        CHECK(VRObj_PairConsistent() == 1, "prod: the published set is pair-consistent");
        CHECK(mism == 0 && splits == 0,
              "prod: zero object pair mismatches / eye-serial splits (%u/%u)",
              mism, splits);
        CHECK(GLSurf_PairMismatchCount() == 0,
              "prod: zero glsurf latch mismatches (%u)", GLSurf_PairMismatchCount());
    }

    // ---- F: an invalid controller returns the VANILLA viewmodel ----------
    {
        g_mock.handActive[VRI_RIGHT] = false;
        g_mock.handActive[VRI_LEFT] = false;
        // Past the tracking-loss window AND the 0.5 s engagement grace.
        for (int i = 0; i < 240; ++i)
            NHProdPair(gun, gunAxis, gunPosU);
        g_prodNow += 3.0;                       // well beyond the grace
        CODVR_TestSetNow(g_prodNow);
        NHProdPairs(4, gun, gunAxis, gunPosU);

        VRNHDebug d; VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].engaged == 0 && d.hand[VRI_LEFT].engaged == 0,
              "prod: an untracked controller DISENGAGES both hands");
        CHECK(d.gun.targetInstalled == 0,
              "prod: ...and releases the gun (reason %s)",
              VRNH_GunReasonName(d.gun.reason));
        const VRObjectSet* s = VRObj_Set();
        CHECK(s->hand[VROBJ_HAND_R].valid == 0 && s->hand[VROBJ_HAND_L].valid == 0
              && s->gun.valid == 0,
              "prod: the published poses say ABSENT, not stale");

        // And the draw itself is left to the engine, untransformed.
        g_prodNow += 1.0 / 120.0; CODVR_TestSetNow(g_prodNow);
        StepNativeGameFrame(0);
        NHScene(0);
        RecReset();
        const int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES,
                                                GLS_HAND_INDICES * 2,
                                                GLS_GL_UNSIGNED_INT, g_handIdx);
        CHECK(owned == 0 && CountKind(REC_LOADMV) == 0,
              "prod: MUTATION-FREE FALLBACK -- the vanilla hand batch is not "
              "claimed and not transformed (owned %d, %d matrices)",
              owned, CountKind(REC_LOADMV));
        StepNativeGameFrame(0);
        g_mock.handActive[VRI_RIGHT] = true;
        g_mock.handActive[VRI_LEFT] = true;
    }

    CHECK(g_mock.violations == 0, "prod: zero XR protocol violations");
    CODVR_TestSetNow(-1.0);
    VRHandPose_TestRestore();
    VRNH_TestReset();
    ResetLayerForCase();
}
