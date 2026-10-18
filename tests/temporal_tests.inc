// ===========================================================================
// TEMPORAL STEREO -- the real F1 order: eye 0 renders, eye 1 renders, the
// PAIR is judged before xrEndFrame (project-lead review 2026-08-29, 4-6, 10)
// ===========================================================================
//
// Every earlier stereo case drove both eyes and asked the NEXT pair boundary
// for a verdict -- one-pair-late detection. Production cannot work that way:
// at the instant eye 0's hand draw is transformed, eye 1 has not rendered.
// These cases step the EXACT production order through the mocked runtime --
//
//   VR_BeginFrameEx        (pair boundary: locate, sync, VRNH_OnPairBeginEyes)
//   eye 0 scene draws      (the layer transforms PROVISIONALLY)
//   VR_SubmitFrame(eye 0)  (capture only; the XR frame stays open)
//   VR_BeginFrameEx        (eye 1 of the same pair: no locate, no sync)
//   eye 1 scene draws
//   VR_SubmitFrame(eye 1)  (SubmitBothEyesAndEnd -> VRNH_PairPreSubmit
//                           -> xrEndFrame with layers, or with none)
//
// -- and assert on what the MOCK RUNTIME was actually handed. A refused pair
// is one the runtime received with zero layers. Nothing here asks the layer
// what it would have done; it asks the runtime what it got.

struct TmpScene {
    float rAxis[9], lAxis[9], gAxis[9];
    float rPos[3], lPos[3], gPos[3];
    const CodvrGunModel* gun;
    int   corruptHands;        // 1 = whole hand batch unrecoverable
    int   corruptLeftOnly;     // 1 = only the LEFT landmarks unrecoverable
    int   swapped;             // 1 = the engine packed the hand surfaces the other way round
    int   singles;             // 1 = the hands arrive as two single-surface draws
    int   sleeves;             // 1 = draw the merged sleeve batch after the hands
    int   gunExtra;            // 1 = a second (non-keyed) gun batch after the body
};

static float g_tmpHandM[2][16];     // the RIGHT half's applied matrix per eye
static int   g_tmpHandMValid[2];
static int   g_tmpZeroOffsets;      // MUTANT: hand both eyes the same eye-space target

static void TmpDefaultScene(TmpScene* s, const CodvrGunModel* gun)
{
    memset(s, 0, sizeof(*s));
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, s->rAxis);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, s->lAxis);
    HpAxisFromEuler(4.0f, -12.0f, 2.0f, s->gAxis);
    // Palms a hand's width apart, as on the real rig (the sleeve attribution
    // is a cuff-to-palm proximity rule and needs distinct palms to attribute
    // to; with coincident palms it correctly falls back to the ordinal).
    const float rp[3] = { 6.0f, -2.0f, -12.0f };
    const float lp[3] = { -6.0f, -2.0f, -12.0f };
    const float gp[3] = { 6.0f, -4.0f, -14.0f };
    memcpy(s->rPos, rp, sizeof(rp));
    memcpy(s->lPos, lp, sizeof(lp));
    memcpy(s->gPos, gp, sizeof(gp));
    s->gun = gun;
}

// The animation advanced between the two game frames: a different authored
// pose for eye 1 than for eye 0.
static void TmpAdvance(TmpScene* s, float du, float ddeg)
{
    s->rPos[0] += du; s->rPos[1] -= du * 0.6f; s->rPos[2] += du * 0.4f;
    s->lPos[0] -= du; s->lPos[1] += du * 0.3f;
    float r[9];
    HpAxisFromEuler(5.0f + ddeg, 20.0f - ddeg * 0.7f, -3.0f + ddeg * 0.4f, r);
    memcpy(s->rAxis, r, sizeof(r));
    HpAxisFromEuler(-8.0f - ddeg * 0.5f, 35.0f + ddeg, 4.0f, r);
    memcpy(s->lAxis, r, sizeof(r));
}

static void SOBuildSwappedHandVerts(const float rightAxis[9], const float rightPos[3],
                                    const float leftAxis[9], const float leftPos[3]);
static int  TmpGameFrame(const TmpScene* s);
static void TmpPair(const TmpScene* s0, const TmpScene* s1);
static int  TmpBringUp(const CodvrGunModel* gun, float ipd);
static void SODriveHalfDraw(int half);
static void SOBuildSleeveVerts(int swapped, const float rPalm[3], const float lPalm[3]);
static void SODriveSleeveDraw();

static void TmpDrawScene(int eye, const TmpScene* s)
{
    NHScene(eye);
    if (s->corruptHands) {
        for (int i = 0; i < 4096 * 3; ++i) g_verts[i] = 12345.0f;
    } else {
        if (s->swapped) SOBuildSwappedHandVerts(s->rAxis, s->rPos, s->lAxis, s->lPos);
        else            NHBuildHandVerts(s->rAxis, s->rPos, s->lAxis, s->lPos);
        if (s->corruptLeftOnly)
            for (int i = 0; i < NH_N; ++i) {
                const int vi = s->swapped ? kNHLeft[i].index + GLS_HAND_VERTS : kNHLeft[i].index;
                float* v = &g_verts[vi * 3];
                v[0] = 12345.0f; v[1] = -777.0f; v[2] = 9.0f * i;
            }
    }
    RecReset();
    if (s->singles) {
        SODriveHalfDraw(0);
        SODriveHalfDraw(1);
    } else {
        NHDriveHandDraw();
    }
    // Both hands engaged: the halves are drawn in batch order, so the RIGHT
    // half's applied matrix is the third loadMatrixf (wanted+restore per
    // half) in the file order and the first in the swapped order.
    const GLRec* m = NthKind(REC_LOADMV, s->swapped ? 0 : 2);
    g_tmpHandMValid[eye & 1] = (m != nullptr);
    if (m) memcpy(g_tmpHandM[eye & 1], m->m, sizeof(g_tmpHandM[0]));
    if (s->sleeves && !s->corruptHands) {
        SOBuildSleeveVerts(s->swapped, s->rPos, s->lPos);
        SODriveSleeveDraw();
    }
    if (s->gun) {
        NHDriveGunDraw(s->gun, s->gAxis, s->gPos);
        if (s->gunExtra) NHDriveOtherGunBatch();
    }
}

// ===========================================================================
// THE VANILLA-FALLBACK POLICY (brief 2026-09-03, section 6): an engaged pair
// that both eyes could only render vanilla is refused (the compositor holds
// the previous native pair) up to the budget, then accepted as coherent
// vanilla so a hand that really stopped solving degrades instead of freezing.
// ===========================================================================
static void S_TemporalVanillaFallback()
{
    printf("--- temporal: engaged-but-vanilla pairs are refused within budget, then degrade ---\n");
    const CodvrGunModel* gun = VRGunPose_ByVertexCount(516);
    if (!gun) { CHECK(0, "BAR body table missing"); return; }
    if (!TmpBringUp(gun, 0.064f)) { CHECK(0, "fallback: bring-up failed"); return; }
    VRNHDebug d; VRNH_GetDebug(&d);
    CHECK(d.hand[VRI_RIGHT].engaged && d.hand[VRI_LEFT].engaged, "fallback: engaged");

    // A pair whose hand batch cannot be recovered in EITHER eye while the
    // hand is engaged. Two mechanisms then act, in order: the pre-submit
    // policy refuses the engaged-but-vanilla pair (the compositor holds the
    // previous native image), and the engagement gate -- a participating eye
    // with no fresh baseline, then a stale baseline -- disengages the hand,
    // after which the vanilla pairs are plain vanilla (nothing engaged, so
    // nothing to refuse) and are accepted. That is the degradation path: an
    // aborted pair or two, then coherent vanilla, never a stale pose.
    VRNHPairStats ps0; VRNH_GetPairStats(&ps0);
    TmpScene bad; TmpDefaultScene(&bad, gun);
    bad.corruptHands = 1;
    int aborted = 0, acceptedVanilla = 0, acceptedEngaged = 0;
    for (int i = 0; i < 6; ++i) {
        const int layers0 = g_mock.endsWithLayers, empty0 = g_mock.endsEmpty;
        TmpPair(&bad, &bad);
        VRNHPairVerdict pv; VRNH_GetLastVerdict(&pv);
        // The debug block reflects THIS pair's boundary decision (nothing
        // runs between the pair's own OnPairBegin and here).
        VRNH_GetDebug(&d);
        const int engagedThis = d.hand[VRI_RIGHT].engaged || d.hand[VRI_LEFT].engaged;
        if (g_mock.endsEmpty == empty0 + 1 && g_mock.endsWithLayers == layers0) {
            ++aborted;
            CHECK(!pv.ok && pv.reason == VRNH_VERDICT_VANILLA_FALLBACK && engagedThis,
                  "fallback/%d: an ENGAGED pair both eyes could only render vanilla is refused as %s",
                  i, VRNH_VerdictName(pv.reason));
        } else if (g_mock.endsWithLayers == layers0 + 1) {
            if (engagedThis) ++acceptedEngaged; else ++acceptedVanilla;
            CHECK(pv.ok && !pv.obj[VRNH_OBJ_R].applied[0] && !pv.obj[VRNH_OBJ_R].applied[1],
                  "fallback/%d: accepted with nothing applied (R %s, L %s)", i,
                  VRNH_HandReasonName(d.hand[VRI_RIGHT].reason),
                  VRNH_HandReasonName(d.hand[VRI_LEFT].reason));
        }
    }
    VRNHPairStats ps1; VRNH_GetPairStats(&ps1);
    CHECK(aborted >= 1 && aborted <= VRNH_FALLBACK_ABORT_BUDGET,
          "fallback: %d pair(s) refused (budget %d) before the hand disengaged", aborted, VRNH_FALLBACK_ABORT_BUDGET);
    CHECK(acceptedEngaged == 0,
          "fallback: NO engaged-but-vanilla pair was ever shown (%d)", acceptedEngaged);
    CHECK(acceptedVanilla >= 1,
          "fallback: then the display degrades to coherent vanilla with the hand disengaged (%d pairs)", acceptedVanilla);
    CHECK(ps1.vanillaFallbackAborted - ps0.vanillaFallbackAborted == (unsigned)aborted,
          "fallback: the counter agrees (%u)", ps1.vanillaFallbackAborted - ps0.vanillaFallbackAborted);
    // Recovery: recoverable pairs re-seed the baseline and native resumes
    // within two pairs; the abort budget is reset by the native pair.
    TmpScene ok; TmpDefaultScene(&ok, gun);
    int resumedAt = -1;
    for (int i = 0; i < 3 && resumedAt < 0; ++i) {
        TmpPair(&ok, &ok);
        VRNHPairVerdict pv; VRNH_GetLastVerdict(&pv);
        if (pv.ok && pv.obj[VRNH_OBJ_R].applied[0] && pv.obj[VRNH_OBJ_R].applied[1]) resumedAt = i;
    }
    CHECK(resumedAt >= 0 && resumedAt <= 1,
          "fallback: native resumed on recoverable pair %d (seed, then engage)", resumedAt);
    {
        const int empty0 = g_mock.endsEmpty;
        TmpPair(&bad, &bad);
        VRNHPairVerdict pv; VRNH_GetLastVerdict(&pv);
        CHECK(g_mock.endsEmpty == empty0 + 1 && pv.reason == VRNH_VERDICT_VANILLA_FALLBACK,
              "fallback: after recovery the next refusal is aborted again (budget reset)");
    }
    CHECK(g_mock.violations == 0, "fallback: zero XR protocol violations");
    CODVR_TestSetNow(-1.0);
    VRHandPose_TestRestore();
    VRNH_TestReset();
    ResetLayerForCase();
}

// ===========================================================================
// THE PHYSICAL AIM SEAM (brief 2026-09-03, sections 10-15)
// ===========================================================================
//
// The rendered barrel (the native gun object) -> yaw/pitch relative to the
// body -> the game's own view angles through the ordinary aim input -> the
// existing fire path. The camera is composed on the body basis and never
// sees those angles. The mocked "game" applies injected mouse to its aim
// (g_steerRespond), exactly as the other native scenarios do.
static float TmpGameYawDelta(float a, float b) { return VRM_WrapDeg(a - b); }

// THE ORACLE: the fire direction must be the initial barrel rotated rigidly
// by the controller's own rotation (the gun is a rigid object in the hand),
// re-expressed as the game's yaw/pitch relative to the body facing. Computed
// from the mock's grip quaternions and vr_math alone -- nothing of the
// weapon layer's own chain is reused.
static void TmpExpectedAim(const float barrel0Room[3], const float q0[4],
                           const float q1[4], float bodyYawDeg,
                           float* outYaw, float* outPitch, float outBarrel[3])
{
    float a0[9], a1[9], local[3], rotated[3];
    VRM_QuatToQuakeAxis(q0, a0);
    VRM_QuatToQuakeAxis(q1, a1);
    VRM_WorldToLocalVec(a0, barrel0Room, local);
    VRM_LocalToWorldVec(a1, local, rotated);
    memcpy(outBarrel, rotated, 3 * sizeof(float));
    float y = 0, p = 0;
    const float rows[9] = { rotated[0], rotated[1], rotated[2], 0, 0, 0, 0, 0, 0 };
    VRM_AxisYawPitchDeg(rows, &y, &p);
    *outYaw = VRM_WrapDeg(y + bodyYawDeg);     // neutral yaw is 0 in this fixture
    *outPitch = p;
}

static void TmpGripQuat(float q[4])
{
    q[0] = g_mock.handGrip[VRI_RIGHT].orientation.x;
    q[1] = g_mock.handGrip[VRI_RIGHT].orientation.y;
    q[2] = g_mock.handGrip[VRI_RIGHT].orientation.z;
    q[3] = g_mock.handGrip[VRI_RIGHT].orientation.w;
}

static void S_PhysicalAimSeam()
{
    printf("--- physical aim: rendered barrel -> game aim state, camera untouched ---\n");
    const CodvrGunModel* gun = VRGunPose_ByVertexCount(516);
    if (!gun) { CHECK(0, "BAR body table missing"); return; }
    if (!TmpBringUp(gun, 0.064f)) { CHECK(0, "aim: bring-up failed"); return; }
    // Arm the seam the way codvr.cfg's vr_physical_aim does, with the
    // production palm calibration in force.
    VRBinds_SetExperimentalWeapon(1);
    VRWeapon_SetEnabled(1);
    VRNH_SetCalibrationTable(1);
    TmpScene s; TmpDefaultScene(&s, gun);
    for (int i = 0; i < 400; ++i) TmpPair(&s, &s);      // engage + converge

    VRNHDebug d; VRNH_GetDebug(&d);
    VRWeaponDebug wd; VRWeapon_GetDebug(&wd);
    CHECK(d.gun.targetInstalled && d.gun.barrelValid,
          "aim: the native gun is driving and publishes a barrel (%+.3f %+.3f %+.3f room)",
          d.gun.barrelRoom[0], d.gun.barrelRoom[1], d.gun.barrelRoom[2]);
    CHECK(wd.decoupled && wd.barrelOverride,
          "aim: the aim model is decoupled and its basis IS the rendered barrel");
    const float body0 = VRWeapon_BodyYawDeg();
    float base0 = 0; unsigned ls = 0;
    CHECK(VR_GetPairBaseYawDeg(&base0, &ls) && fabsf(TmpGameYawDelta(base0, body0)) < 0.01f,
          "aim: the latched camera base IS the body yaw (%.2f vs %.2f)", base0, body0);
    const float target0 = wd.targetYawDeg, tp0 = wd.targetPitchDeg;
    CHECK(fabsf(TmpGameYawDelta(wd.gameYawDeg, target0)) < 0.5f
          && fabsf(wd.gamePitchDeg - tp0) < 0.5f,
          "aim: the game's aim converged onto the barrel (err %.2f/%.2f deg)",
          TmpGameYawDelta(wd.gameYawDeg, target0), wd.gamePitchDeg - tp0);
    VRBindsDebug bd; VRBinds_GetDebug(&bd);
    const int leaks0 = bd.steerLeakCount;
    float barrel0[3]; memcpy(barrel0, d.gun.barrelRoom, sizeof(barrel0));
    float q0[4]; TmpGripQuat(q0);
    printf("      aim: initial barrel (room) %+.3f %+.3f %+.3f  desired yaw %.2f pitch %.2f  body %.2f\n",
           barrel0[0], barrel0[1], barrel0[2], target0, tp0, body0);

    // (1) gun 20 deg right: the shot goes with the rotated barrel; the camera does not move.
    struct { const char* name; float yaw, pitch; } moves[3] = {
        { "aim/1: gun 20 deg right", -20.0f, 0.0f },
        { "aim/2: gun 15 deg up",      0.0f, 15.0f },
        { "aim/3: gun diagonal",     -20.0f, 15.0f },
    };
    for (int m = 0; m < 3; ++m) {
        SetHandAim(VRI_RIGHT, moves[m].yaw, moves[m].pitch);
        for (int i = 0; i < 300; ++i) TmpPair(&s, &s);
        VRWeapon_GetDebug(&wd);
        VRNH_GetDebug(&d);
        float q1[4]; TmpGripQuat(q1);
        float expYaw = 0, expPitch = 0, expBarrel[3];
        TmpExpectedAim(barrel0, q0, q1, VRWeapon_BodyYawDeg(), &expYaw, &expPitch, expBarrel);
        const float dotB = d.gun.barrelRoom[0]*expBarrel[0] + d.gun.barrelRoom[1]*expBarrel[1] + d.gun.barrelRoom[2]*expBarrel[2];
        CHECK(dotB > 0.9999f,
              "%s -> the rendered barrel is the initial barrel rigidly rotated by the controller (dot %.5f)",
              moves[m].name, dotB);
        CHECK(fabsf(TmpGameYawDelta(wd.targetYawDeg, expYaw)) < 0.5f && fabsf(wd.targetPitchDeg - expPitch) < 0.5f,
              "%s -> desired aim yaw %.2f pitch %.2f == oracle %.2f / %.2f (body-relative offset %+.1f / %+.1f)",
              moves[m].name, wd.targetYawDeg, wd.targetPitchDeg, expYaw, expPitch,
              TmpGameYawDelta(wd.targetYawDeg, VRWeapon_BodyYawDeg()), wd.targetPitchDeg);
        CHECK(fabsf(TmpGameYawDelta(wd.gameYawDeg, wd.targetYawDeg)) < 0.5f
              && fabsf(wd.gamePitchDeg - wd.targetPitchDeg) < 0.5f,
              "%s -> the GAME's aim followed to within %.2f/%.2f deg -- shots go where the barrel points",
              moves[m].name, TmpGameYawDelta(wd.gameYawDeg, wd.targetYawDeg), wd.gamePitchDeg - wd.targetPitchDeg);
        float base1 = 0;
        VR_GetPairBaseYawDeg(&base1, &ls);
        CHECK(fabsf(TmpGameYawDelta(base1, base0)) < 0.01f && fabsf(VRWeapon_BodyYawDeg() - body0) < 0.01f,
              "%s -> the camera base did NOT move (%.3f deg) -- aim never rotates the HMD camera",
              moves[m].name, TmpGameYawDelta(base1, base0));
        if (m == 0)
            CHECK_NEAR(TmpGameYawDelta(wd.targetYawDeg, target0), -20.0, 0.75,
                       "aim/1: a pure 20 deg controller yaw is 20 deg of aim yaw (%.2f)",
                       TmpGameYawDelta(wd.targetYawDeg, target0));
    }

    // (4) the head turns 30 deg while the gun stays put: the shot stays gun-owned.
    const float tgt3 = wd.targetYawDeg, tp3 = wd.targetPitchDeg;
    SetMockHead(30.0f, 0.0f, 0.0f, 0.0f, 0.064f);
    for (int i = 0; i < 120; ++i) TmpPair(&s, &s);
    VRWeapon_GetDebug(&wd);
    float viewYaw = 0;
    VR_GetRenderedViewYawDeg(&viewYaw);
    CHECK(fabsf(TmpGameYawDelta(wd.targetYawDeg, tgt3)) < 0.5f && fabsf(wd.targetPitchDeg - tp3) < 0.5f,
          "aim/4: head turned, gun fixed -> desired aim unchanged (%.2f/%.2f deg)",
          TmpGameYawDelta(wd.targetYawDeg, tgt3), wd.targetPitchDeg - tp3);
    CHECK(fabsf(TmpGameYawDelta(viewYaw, body0 + 30.0f)) < 1.0f,
          "aim/4: ...while the rendered view turned with the head (%.1f deg)", TmpGameYawDelta(viewYaw, body0));
    CHECK(fabsf(TmpGameYawDelta(wd.gameYawDeg, wd.targetYawDeg)) < 0.5f,
          "aim/4: the game aim stayed on the barrel");
    SetMockHead(0.0f, 0.0f, 0.0f, 0.0f, 0.064f);
    for (int i = 0; i < 60; ++i) TmpPair(&s, &s);

    // (5) a RIGHT-stick turn: body, camera base and aim all turn together.
    VRWeapon_GetDebug(&wd);
    const float body4 = VRWeapon_BodyYawDeg(), tgt4 = wd.targetYawDeg;
    g_mock.stickX[VRI_RIGHT] = 1.0f;
    for (int i = 0; i < 30; ++i) TmpPair(&s, &s);
    g_mock.stickX[VRI_RIGHT] = 0.0f;
    for (int i = 0; i < 200; ++i) TmpPair(&s, &s);
    VRWeapon_GetDebug(&wd);
    float base5 = 0;
    VR_GetPairBaseYawDeg(&base5, &ls);
    const float turned = TmpGameYawDelta(VRWeapon_BodyYawDeg(), body4);
    CHECK(fabsf(turned) > 5.0f, "aim/5: the right stick turned the body %.1f deg", turned);
    CHECK(fabsf(TmpGameYawDelta(TmpGameYawDelta(wd.targetYawDeg, tgt4), turned)) < 0.75f,
          "aim/5: the desired aim turned by the same %.1f deg (coherent)", TmpGameYawDelta(wd.targetYawDeg, tgt4));
    CHECK(fabsf(TmpGameYawDelta(base5, VRWeapon_BodyYawDeg())) < 0.01f,
          "aim/5: the camera base followed the BODY (%.2f vs %.2f)", base5, VRWeapon_BodyYawDeg());
    CHECK(fabsf(TmpGameYawDelta(wd.gameYawDeg, wd.targetYawDeg)) < 0.5f,
          "aim/5: the game aim reconverged (err %.2f)", TmpGameYawDelta(wd.gameYawDeg, wd.targetYawDeg));

    VRBinds_GetDebug(&bd);
    CHECK(bd.steerLeakCount == leaks0,
          "aim: ZERO steering leaks into the rendered camera across the run (%d)", bd.steerLeakCount - leaks0);
    CHECK(CODVR_CollisionHookCameraMutationCount() == 0, "aim: zero camera mutations");
    CHECK(g_mock.violations == 0, "aim: zero XR protocol violations");

    VRBinds_SetExperimentalWeapon(0);
    VRWeapon_SetEnabled(0);
    CODVR_TestSetNow(-1.0);
    VRHandPose_TestRestore();
    VRNH_TestReset();
    ResetLayerForCase();
}

// One GAME frame in dllmain's true order: boundary/camera, scene draws,
// submit. Returns the eye rendered, or -1.
static int TmpGameFrame(const TmpScene* s)
{
    int rendered = -1;
    VRFrameDisposition d = VR_BeginFrameEx();
    if (d == VR_FRAME_RENDER) {
        // "The game" consumes last frame's injected mouse, as the other
        // native scenarios do, so the steering loop stays closed.
        if (g_steerRespond) {
            g_gameYawDeg = VRM_WrapDeg(g_gameYawDeg + g_pendDx * -0.11f);
            g_gamePitchDeg += g_pendDy * -0.11f;
            if (g_gamePitchDeg >  85.0f) g_gamePitchDeg =  85.0f;
            if (g_gamePitchDeg < -85.0f) g_gamePitchDeg = -85.0f;
        }
        g_pendDx = g_pendDy = 0;
        const int eye = VR_ExpectedEye();
        float axis[9];
        MakeAxis(g_gameYawDeg, g_gamePitchDeg, 0, axis);
        VRWeapon_NoteGameAim(axis, g_gameOrigin, g_now);
        float base[9];
        const float* camBase = axis;
        if (VRWeapon_GetViewBasisRows(base))
            camBase = base;
        float fx = 0, fy = 0;
        VR_GetEyeCamera(eye, g_gameOrigin, camBase, 1440, 1440,
                        g_out[eye], g_outAxis[eye], &fx, &fy);
        if (g_tmpZeroOffsets) {
            float z[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
            GLSurf_SetEyeOffsets(z);
        }
        if (s) TmpDrawScene(eye, s);
        VR_SubmitFrame(640, 480, true, eye);
        rendered = eye;
    }
    VRWeapon_Update(VRInput_State(), g_now, 0);
    VRBinds_Update(g_now, 0);
    g_now += 1.0 / 240.0;
    g_prodNow += 1.0 / 240.0;
    CODVR_TestSetNow(g_prodNow);
    return rendered;
}

static void TmpPair(const TmpScene* s0, const TmpScene* s1)
{
    TmpGameFrame(s0);
    TmpGameFrame(s1);
}

static int TmpBringUp(const CodvrGunModel* gun, float ipd)
{
    ResetAll();
    NHResetForCase();
    MockRealViewmodelModelview();
    VRNH_SetEnabled(1);
    VRNH_SetTwoHandEnabled(0);
    VRNH_SetReloadEnabled(0);
    SetMockHead(0, 0, 0, 0, ipd);
    if (!BringUp()) return 0;
    SetHandGripPos(VRI_RIGHT, 0.20f, -0.10f, -0.35f);
    SetHandGripPos(VRI_LEFT, -0.20f, -0.10f, -0.35f);
    SetHandAim(VRI_RIGHT, 0, 0);
    SetHandAim(VRI_LEFT, 0, 0);
    g_tmpZeroOffsets = 0;
    TmpScene s; TmpDefaultScene(&s, gun);
    for (int i = 0; i < VRI_ACQUIRE_SYNCS + 8; ++i) TmpPair(&s, &s);
    return 1;
}

static float TmpDist3(const float a[3], const float b[3])
{
    return HpDist(a, b);
}

// ===========================================================================
static void S_TemporalPairVerdict()
{
    printf("--- temporal stereo: eye 0 provisional, eye 1, pre-submit verdict ---\n");
    const CodvrGunModel* gun = VRGunPose_ByVertexCount(516);
    if (!gun) { CHECK(0, "BAR body table missing"); return; }
    if (!TmpBringUp(gun, 0.064f)) { CHECK(0, "temporal: bring-up failed"); return; }

    VRNHDebug d; VRNH_GetDebug(&d);
    CHECK(d.hand[VRI_RIGHT].engaged && d.hand[VRI_LEFT].engaged && d.gun.targetInstalled,
          "temporal: warmed up engaged (R %s, L %s, gun %s)",
          VRNH_HandReasonName(d.hand[VRI_RIGHT].reason),
          VRNH_HandReasonName(d.hand[VRI_LEFT].reason),
          VRNH_GunReasonName(d.gun.reason));
    const float ipdU = TmpDist3(d.eyeOffEyeish[0], d.eyeOffEyeish[1]);
    CHECK_NEAR(ipdU, 0.064f * 39.37f, 0.02,
               "temporal: the pair's eye offsets are %.3f u apart (64 mm IPD)", ipdU);

    // ---- A: TRUE ORDER, animation advancing between the eyes ------------
    {
        TmpScene s0, s1;
        TmpDefaultScene(&s0, gun);
        s1 = s0;
        TmpAdvance(&s1, 0.05f, 1.5f);           // eye 1 sees a later frame
        const int layers0 = g_mock.endsWithLayers, empty0 = g_mock.endsEmpty;

        const int e0 = TmpGameFrame(&s0);
        const unsigned serial = VR_TestFrameSerial();
        CHECK(e0 == 0, "A: the first game frame of the pair rendered eye 0");
        // NO FUTURE INFORMATION: at this instant eye 1 has drawn nothing for
        // this pair, and eye 0 has already applied its delta.
        GLSurfEyeApply a0, a1;
        GLSurf_GetEyeApply(GLSC_HAND_R, 0, &a0);
        GLSurf_GetEyeApply(GLSC_HAND_R, 1, &a1);
        CHECK(a0.serial == serial && a0.drew && a0.applied,
              "A: eye 0 applied its RIGHT delta under serial %u before eye 1 existed", serial);
        CHECK(a1.serial != serial,
              "A: ...and eye 1 holds NO record for this pair yet (its record is "
              "serial %u) -- eye 0 needed nothing from the future", a1.serial);
        CHECK(g_mock.endsWithLayers == layers0 && g_mock.endsEmpty == empty0,
              "A: the XR frame is still OPEN after eye 0 (nothing submitted)");

        const int e1 = TmpGameFrame(&s1);
        CHECK(e1 == 1, "A: the second game frame rendered eye 1");
        CHECK(g_mock.endsWithLayers == layers0 + 1 && g_mock.endsEmpty == empty0,
              "A: the pair was SUBMITTED with layers (%d -> %d, empty %d -> %d)",
              layers0, g_mock.endsWithLayers, empty0, g_mock.endsEmpty);
        VRNHPairVerdict pv; VRNH_GetLastVerdict(&pv);
        CHECK(pv.pairSerial == serial && pv.ok,
              "A: the pre-submit verdict for pair %u is OK", serial);
        for (int o = 0; o < VRNH_OBJS; ++o) {
            const VRNHObjVerdict& ov = pv.obj[o];
            CHECK(ov.applied[0] && ov.applied[1],
                  "A: %s applied in BOTH eyes", VRNH_ObjName(o));
            CHECK(ov.posErrMm >= 0.0f && ov.posErrMm <= VRNH_PAIRWORLD_POS_TOL_MM
                  && ov.rotErrDeg >= 0.0f && ov.rotErrDeg <= VRNH_PAIRWORLD_ROT_TOL_DEG,
                  "A: %s final objects reconstruct to ONE PairWorld pose: %.4f mm, "
                  "%.4f deg", VRNH_ObjName(o), ov.posErrMm, ov.rotErrDeg);
        }
        // Per-eye deltas DIFFER (different authored poses, different eye
        // offsets) while the finals agree -- the section-5 rule, measured.
        CHECK(g_tmpHandMValid[0] && g_tmpHandMValid[1]
              && memcmp(g_tmpHandM[0], g_tmpHandM[1], sizeof(g_tmpHandM[0])) != 0,
              "A: the two eyes' applied RIGHT-hand matrices differ (each eye's own "
              "authored pose and offset)");
        GLSurf_GetEyeApply(GLSC_HAND_R, 0, &a0);
        GLSurf_GetEyeApply(GLSC_HAND_R, 1, &a1);
        const float eyeSep = TmpDist3(a0.finalPos, a1.finalPos);
        CHECK_NEAR(eyeSep, ipdU, 0.02,
                   "A: in eye space the two finals sit %.3f u apart -- exactly the "
                   "eye separation, i.e. one physical object seen from two eyes",
                   eyeSep);
        CHECK(g_mock.lastEnd.layerCount == 1 && g_mock.lastEnd.hasProjection,
              "A: the runtime received one projection layer with both views");
    }

    // ---- B: THE TEMPORAL MUTANT -- eye 0 PASS, eye 1 FAIL ----------------
    {
        TmpScene s0, s1;
        TmpDefaultScene(&s0, gun);
        s1 = s0;
        s1.corruptHands = 1;
        const int layers0 = g_mock.endsWithLayers, empty0 = g_mock.endsEmpty;
        const unsigned rejects0 = VR_NativePairRejectCount();
        VRNHPairStats ps0; VRNH_GetPairStats(&ps0);

        TmpGameFrame(&s0);                        // eye 0: native, provisional
        const unsigned serial = VR_TestFrameSerial();
        GLSurfEyeApply a0;
        GLSurf_GetEyeApply(GLSC_HAND_R, 0, &a0);
        CHECK(a0.serial == serial && a0.applied,
              "B: eye 0 rendered the RIGHT hand natively (provisionally)");

        TmpGameFrame(&s1);                        // eye 1: solve refuses
        GLSurfEyeApply a1;
        GLSurf_GetEyeApply(GLSC_HAND_R, 1, &a1);
        CHECK(a1.serial == serial && a1.drew && !a1.applied,
              "B: eye 1 drew the hand and could NOT recover it -- the half-native setup");
        CHECK(g_mock.endsWithLayers == layers0 && g_mock.endsEmpty == empty0 + 1
              && g_mock.lastEnd.layerCount == 0,
              "B: MIXED PAIR NEVER REACHED XR SUBMISSION -- the runtime got the "
              "frame with ZERO layers (layers %d -> %d, empty %d -> %d)",
              layers0, g_mock.endsWithLayers, empty0, g_mock.endsEmpty);
        VRNHPairVerdict pv; VRNH_GetLastVerdict(&pv);
        CHECK(pv.pairSerial == serial && !pv.ok && pv.reason == VRNH_VERDICT_HALF_NATIVE,
              "B: the verdict is %s on %s", VRNH_VerdictName(pv.reason),
              VRNH_ObjName(pv.objIndex));
        CHECK(VR_NativePairRejectCount() == rejects0 + 1,
              "B: vr.cpp counted the refused pair (%u)", VR_NativePairRejectCount());
        VRNHPairStats ps1; VRNH_GetPairStats(&ps1);
        CHECK(ps1.rejectHalfNative == ps0.rejectHalfNative + 1,
              "B: the pair statistics say half-native (%u)", ps1.rejectHalfNative);
        VRPerfStats perf; VR_GetPerfStats(&perf);
        CHECK(perf.abortedPairs >= 1, "B: ...and the pair is an aborted pair (%u)",
              perf.abortedPairs);

        // The NEXT pair: both eyes vanilla (cooldown), submitted normally.
        TmpScene ok; TmpDefaultScene(&ok, gun);
        const int layers1 = g_mock.endsWithLayers;
        TmpPair(&ok, &ok);
        VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].engaged == 0
              && d.hand[VRI_RIGHT].reason == VRNH_HAND_PAIR_COOLDOWN,
              "B: the following pair holds the RIGHT hand vanilla on BOTH eyes "
              "(reason %s)", VRNH_HandReasonName(d.hand[VRI_RIGHT].reason));
        CHECK(g_mock.endsWithLayers == layers1 + 1,
              "B: ...and that consistent vanilla pair IS submitted");
        VRNH_GetLastVerdict(&pv);
        CHECK(pv.ok && !pv.obj[VRNH_OBJ_R].applied[0] && !pv.obj[VRNH_OBJ_R].applied[1],
              "B: its verdict is OK with nothing applied");

        // Recovery: the pair after that engages and submits natively again.
        TmpPair(&ok, &ok);
        VRNH_GetDebug(&d);
        VRNH_GetLastVerdict(&pv);
        CHECK(d.hand[VRI_RIGHT].engaged && pv.ok
              && pv.obj[VRNH_OBJ_R].applied[0] && pv.obj[VRNH_OBJ_R].applied[1],
              "B: one pair later native ownership resumes on both eyes");
    }

    // ---- B': the mirror -- eye 0 FAIL, eye 1 PASS -----------------------
    {
        TmpScene s0, s1;
        TmpDefaultScene(&s0, gun);
        s1 = s0;
        s0.corruptHands = 1;
        const int layers0 = g_mock.endsWithLayers, empty0 = g_mock.endsEmpty;
        TmpPair(&s0, &s1);
        CHECK(g_mock.endsWithLayers == layers0 && g_mock.endsEmpty == empty0 + 1,
              "B': eye 0 refusing while eye 1 applies is ALSO refused at submit");
        VRNHPairVerdict pv; VRNH_GetLastVerdict(&pv);
        CHECK(!pv.ok && pv.reason == VRNH_VERDICT_HALF_NATIVE
              && !pv.obj[VRNH_OBJ_R].applied[0] && pv.obj[VRNH_OBJ_R].applied[1],
              "B': verdict %s, applied 0/1", VRNH_VerdictName(pv.reason));
        TmpScene ok; TmpDefaultScene(&ok, gun);
        TmpPair(&ok, &ok);                       // cooldown
        TmpPair(&ok, &ok);                       // resume
    }

    // ---- B'': per-object attribution -- only the LEFT hand fails --------
    {
        TmpScene s0, s1;
        TmpDefaultScene(&s0, gun);
        s1 = s0;
        s1.corruptLeftOnly = 1;
        const int layers0 = g_mock.endsWithLayers;
        TmpPair(&s0, &s1);
        VRNHPairVerdict pv; VRNH_GetLastVerdict(&pv);
        CHECK(!pv.ok && pv.objIndex == VRNH_OBJ_L
              && pv.obj[VRNH_OBJ_R].verdict == VRNH_VERDICT_OK
              && pv.obj[VRNH_OBJ_L].verdict == VRNH_VERDICT_HALF_NATIVE,
              "B'': a LEFT-only refusal is attributed to LEFT (RIGHT OK) and "
              "still refuses the whole pair");
        CHECK(g_mock.endsWithLayers == layers0, "B'': not submitted");
        TmpScene ok; TmpDefaultScene(&ok, gun);
        TmpPair(&ok, &ok);
        VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_LEFT].reason == VRNH_HAND_PAIR_COOLDOWN
              && d.hand[VRI_RIGHT].engaged == 1,
              "B'': only LEFT is held vanilla afterwards; RIGHT keeps driving");
        TmpPair(&ok, &ok);
    }

    // ---- C: THE SPACE MUTANT -- both eyes given the SAME eye-space target -
    //
    // Two finals identical in eye space are two DIFFERENT physical objects,
    // IPD apart. The verdict measures PairWorld, so this is a DISAGREE, and
    // it is what proves the eye offsets are load-bearing rather than
    // cosmetic.
    {
        TmpScene s; TmpDefaultScene(&s, gun);
        const int layers0 = g_mock.endsWithLayers, empty0 = g_mock.endsEmpty;
        g_tmpZeroOffsets = 1;
        TmpPair(&s, &s);
        g_tmpZeroOffsets = 0;
        VRNHPairVerdict pv; VRNH_GetLastVerdict(&pv);
        CHECK(!pv.ok && pv.reason == VRNH_VERDICT_DISAGREE,
              "C/MUTANT: identical eye-space targets are refused as %s",
              VRNH_VerdictName(pv.reason));
        CHECK_NEAR(pv.obj[VRNH_OBJ_R].posErrMm, 64.0, 1.5,
                   "C/MUTANT: the RIGHT finals disagree by %.2f mm -- the IPD",
                   pv.obj[VRNH_OBJ_R].posErrMm);
        CHECK(g_mock.endsWithLayers == layers0 && g_mock.endsEmpty == empty0 + 1,
              "C/MUTANT: the runtime received zero layers");
        TmpPair(&s, &s);                          // cooldown pair
        TmpPair(&s, &s);                          // resume
        VRNH_GetLastVerdict(&pv);
        CHECK(pv.ok, "C: with the real offsets back, the pair is accepted again");
    }

    CHECK(g_mock.violations == 0, "temporal: zero XR protocol violations");
    CODVR_TestSetNow(-1.0);
    VRHandPose_TestRestore();
    VRNH_TestReset();
    ResetLayerForCase();
}

// ===========================================================================
// >= 10k pairs in TRUE F1 ORDER through the production submit path
// ===========================================================================
static void S_TemporalSoak()
{
    printf("--- temporal soak: 10k pairs, real order, pre-submit verdicts ---\n");
    const CodvrGunModel* thompson = VRGunPose_ByVertexCount(326);
    const CodvrGunModel* bar      = VRGunPose_ByVertexCount(516);
    if (!thompson || !bar) { CHECK(0, "soak gun tables missing"); return; }
    if (!TmpBringUp(bar, 0.064f)) { CHECK(0, "temporal soak: bring-up failed"); return; }

    const int kPairs = 10000;
    unsigned begun = 0, eye0 = 0, eye1 = 0;
    unsigned engR = 0, engL = 0, engGun = 0;
    unsigned halfNativeSubmitted = 0, nan = 0, staleValid = 0, ownershipCollisions = 0;
    unsigned injectedEye1 = 0, injectedEye0 = 0, injectedLeft = 0, expectedRejects = 0;
    unsigned leftFarPairs = 0, rigSwitches = 0, turnEvents = 0, dropouts = 0;
    unsigned swappedFrames = 0;
    int missedDiag = 0;
    GLSurfOwnerStats own0; GLSurf_GetOwnerStats(&own0);
    const int layersStart = g_mock.endsWithLayers;
    const unsigned rejectsStart = VR_NativePairRejectCount();
    VRNHPairStats psStart; VRNH_GetPairStats(&psStart);
    GLSurfHandSolve hr0, hl0; GLSurf_GetHandSolve(GLSC_HAND_R, &hr0); GLSurf_GetHandSolve(GLSC_HAND_L, &hl0);
    GLSurfGunSolve gs0; GLSurf_GetGunSolve(&gs0);
    int prevGunVerts = 0;
    float prevCam0[3]; memcpy(prevCam0, g_out[0], sizeof(prevCam0));

    for (int p = 0; p < kPairs; ++p) {
        const CodvrGunModel* gun = ((p / 977) % 2) ? thompson : bar;
        TmpScene s0, s1;
        TmpDefaultScene(&s0, gun);
        // Authored animation: slowly varying base, plus a per-frame advance
        // between the two eyes of every pair.
        const float ph = (float)p * 0.013f;
        s0.rPos[0] += 0.10f * sinf(ph); s0.rPos[2] += 0.08f * cosf(ph * 0.7f);
        s0.lPos[1] += 0.06f * sinf(ph * 1.3f);
        s1 = s0;
        TmpAdvance(&s1, 0.02f + 0.01f * sinf(ph * 3.0f), 0.4f);
        // The engine's surface order, per GAME FRAME (the 2026-09-03 root
        // cause): a pseudo-random ~50 % swap rate, independent per eye, plus
        // single-surface draws and the sleeve batch on some frames.
        s0.swapped = ((p * 7919u + 13u) % 100u) < 50u;
        s1.swapped = ((p * 104729u + 7u) % 100u) < 50u;
        s0.singles = ((p % 11) == 4);
        s1.singles = ((p % 13) == 6);
        s0.sleeves = s1.sleeves = ((p % 3) != 1);
        if (s0.swapped) ++swappedFrames;
        if (s1.swapped) ++swappedFrames;
        // Controller motion, head motion, game turning, weapon switches.
        const int leftFar = ((p % 137) < 20);
        SetHandGripPos(VRI_RIGHT, 0.20f + 0.05f * sinf(ph * 0.5f), -0.10f + 0.03f * cosf(ph),
                       -0.35f - 0.02f * sinf(ph * 0.3f));
        SetHandGripPos(VRI_LEFT, leftFar ? -1.20f : -0.20f + 0.04f * sinf(ph * 0.8f),
                       -0.10f, -0.35f);
        SetHandAim(VRI_RIGHT, 15.0f * sinf(ph * 0.2f), 5.0f * cosf(ph * 0.25f));
        if ((p % 3) == 0)
            SetMockHead(8.0f * sinf(ph * 0.1f), 0.02f * sinf(ph * 0.4f),
                        0.015f * cosf(ph * 0.3f), 0.01f * sinf(ph * 0.2f), 0.064f);
        if ((p % 400) == 200) { g_gameYawDeg = VRM_WrapDeg(g_gameYawDeg + 5.0f); ++turnEvents; }
        // Tracking dropouts.
        const bool rUp = !((p % 613) < 9);
        const bool lUp = !((p % 401) < 13);
        if (!rUp || !lUp) ++dropouts;
        g_mock.handActive[VRI_RIGHT] = rUp;
        g_mock.handActive[VRI_LEFT]  = lUp;
        // Injected solve refusals, in both temporal directions and per object.
        if ((p % 97) == 5)  { s1.corruptHands = 1;    ++injectedEye1; }
        if ((p % 233) == 9) { s0.corruptHands = 1;    ++injectedEye0; }
        if ((p % 151) == 7) { s1.corruptLeftOnly = 1; ++injectedLeft; }

        const int layersBefore = g_mock.endsWithLayers;
        ++begun;
        if (TmpGameFrame(&s0) == 0) ++eye0;
        if (TmpGameFrame(&s1) == 1) ++eye1;

        VRNHDebug d; VRNH_GetDebug(&d);
        VRNHPairVerdict pv; VRNH_GetLastVerdict(&pv);
        // An injected refusal can only produce a half-native pair when the
        // affected hand was ENGAGED this pair (a dropout or a cooldown pair
        // renders vanilla on both eyes, and there is nothing to refuse).
        if ((s0.corruptHands || s1.corruptHands)
            && (d.hand[VRI_RIGHT].engaged || d.hand[VRI_LEFT].engaged))
            ++expectedRejects;
        else if (s1.corruptLeftOnly && d.hand[VRI_LEFT].engaged)
            ++expectedRejects;
        if ((s0.corruptHands || s1.corruptHands || s1.corruptLeftOnly)
            && (d.hand[VRI_RIGHT].engaged || d.hand[VRI_LEFT].engaged)
            && g_mock.endsWithLayers > layersBefore && missedDiag < 12) {
            ++missedDiag;
            printf("      UNREJECTED injection pair %d: corrupt s0 %d s1 %d L %d swapped %d/%d singles %d/%d "
                   "engR %d engL %d verdict %s R applied %d/%d drew %d/%d L applied %d/%d drew %d/%d\n",
                   p, s0.corruptHands, s1.corruptHands, s1.corruptLeftOnly, s0.swapped, s1.swapped,
                   s0.singles, s1.singles, d.hand[VRI_RIGHT].engaged, d.hand[VRI_LEFT].engaged,
                   VRNH_VerdictName(pv.reason),
                   pv.obj[VRNH_OBJ_R].applied[0], pv.obj[VRNH_OBJ_R].applied[1],
                   pv.obj[VRNH_OBJ_R].drew[0], pv.obj[VRNH_OBJ_R].drew[1],
                   pv.obj[VRNH_OBJ_L].applied[0], pv.obj[VRNH_OBJ_L].applied[1],
                   pv.obj[VRNH_OBJ_L].drew[0], pv.obj[VRNH_OBJ_L].drew[1]);
        }
        if (d.hand[VRI_RIGHT].engaged) ++engR;
        if (d.hand[VRI_LEFT].engaged)  ++engL;
        if (d.gun.targetInstalled)     ++engGun;
        if (leftFar && d.hand[VRI_LEFT].engaged && d.gun.twoHandState == VRNH_2H_LEFT_FREE)
            ++leftFarPairs;
        if (d.gun.vertexCount && d.gun.vertexCount != prevGunVerts) {
            if (prevGunVerts) ++rigSwitches;
            prevGunVerts = d.gun.vertexCount;
        }
        // THE INVARIANT: a pair the runtime received WITH layers must have
        // passed the verdict, and every object in it must be all-or-nothing.
        if (g_mock.endsWithLayers > layersBefore) {
            if (!pv.ok) ++halfNativeSubmitted;
            for (int o = 0; o < VRNH_OBJS; ++o)
                if (pv.obj[o].applied[0] != pv.obj[o].applied[1]
                    && pv.obj[o].drew[0] && pv.obj[o].drew[1])
                    ++halfNativeSubmitted;
        }
        // NaN / absurd values anywhere in the published set or the verdict.
        const VRObjectSet* os = VRObj_Set();
        for (int h = 0; h < VROBJ_HANDS; ++h)
            for (int k = 0; k < 3; ++k) {
                const float v = os->hand[h].pos[k];
                if (v != v || fabsf(v) > 1e6f) ++nan;
            }
        for (int k = 0; k < 3; ++k) {
            const float v = os->gun.pos[k];
            if (v != v || fabsf(v) > 1e6f) ++nan;
        }
        for (int o = 0; o < VRNH_OBJS; ++o)
            for (int e = 0; e < 2; ++e)
                for (int k = 0; k < 3; ++k) {
                    const float v = pv.obj[o].pairWorldPosM[e][k];
                    if (v != v) ++nan;
                }
        // A disengaged hand must publish ABSENT, never a stale valid pose.
        if (!d.hand[VRI_RIGHT].engaged && os->hand[VROBJ_HAND_R].valid) ++staleValid;
        if (!d.hand[VRI_LEFT].engaged  && os->hand[VROBJ_HAND_L].valid)  ++staleValid;
        // Ownership: the two hands are two distinct anchors whenever both
        // reconstructed in PairWorld (both eyes applied for both hands); a
        // hand inheriting the other's matrix would coincide with it.
        if (pv.obj[VRNH_OBJ_R].applied[0] && pv.obj[VRNH_OBJ_R].applied[1]
            && pv.obj[VRNH_OBJ_L].applied[0] && pv.obj[VRNH_OBJ_L].applied[1]) {
            const float dRL = TmpDist3(pv.obj[VRNH_OBJ_R].pairWorldPosM[0],
                                       pv.obj[VRNH_OBJ_L].pairWorldPosM[0]);
            if (dRL < 0.05f) ++ownershipCollisions;
        }
        // Diagnostics for any DISAGREE: which object, by how much, under
        // what schedule -- printed, so a failure names its cause.
        if (!pv.ok && pv.reason == VRNH_VERDICT_DISAGREE) {
            for (int o = 0; o < VRNH_OBJS; ++o)
                if (pv.obj[o].verdict == VRNH_VERDICT_DISAGREE)
                    printf("      DISAGREE pair %d obj %s pos %.3f mm rot %.4f deg "
                           "(leftFar %d rUp %d lUp %d corrupt s0 %d s1 %d L %d "
                           "gun %d turn %d)\n",
                           p, VRNH_ObjName(o), pv.obj[o].posErrMm, pv.obj[o].rotErrDeg,
                           leftFar, rUp ? 1 : 0, lUp ? 1 : 0, s0.corruptHands,
                           s1.corruptHands, s1.corruptLeftOnly, d.gun.vertexCount,
                           (p % 400) == 200);
        }
        if (g_mock.violations) break;
    }
    g_mock.handActive[VRI_RIGHT] = true;
    g_mock.handActive[VRI_LEFT] = true;

    const unsigned submitted = (unsigned)(g_mock.endsWithLayers - layersStart);
    const unsigned rejected  = VR_NativePairRejectCount() - rejectsStart;
    VRNHPairStats ps; VRNH_GetPairStats(&ps);
    GLSurfHandSolve hr, hl; GLSurf_GetHandSolve(GLSC_HAND_R, &hr); GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
    GLSurfGunSolve gs; GLSurf_GetGunSolve(&gs);
    GLSurfOwnerStats own; GLSurf_GetOwnerStats(&own);
    printf("      temporal soak: surface order swapped on %u of %u game frames | "
           "merged batches normal %u swapped %u unknown %u | singles by content %u ordinal %u | "
           "sleeves proximity %u ordinal %u | primaryNativeDraw %u primaryVanillaFallbackDraw %u "
           "duplicateOwnerPrevented %u | pairNative %u pairFailClosed %u mixedEyeRejected %u "
           "vanillaFallback aborted %u accepted %u | swappedSolves R %u L %u\n",
           swappedFrames, 2u * (unsigned)kPairs,
           own.orderNormal - own0.orderNormal, own.orderSwapped - own0.orderSwapped,
           own.orderUnknown - own0.orderUnknown,
           own.singleByContent - own0.singleByContent, own.singleByOrdinal - own0.singleByOrdinal,
           own.sleeveByProximity - own0.sleeveByProximity, own.sleeveByOrdinal - own0.sleeveByOrdinal,
           own.primaryNativeDraw - own0.primaryNativeDraw,
           own.primaryVanillaFallbackDraw - own0.primaryVanillaFallbackDraw,
           own.duplicateOwnerPrevented - own0.duplicateOwnerPrevented,
           ps.pairNative - psStart.pairNative, ps.pairFailClosed - psStart.pairFailClosed,
           ps.mixedEyeRejected - psStart.mixedEyeRejected,
           ps.vanillaFallbackAborted - psStart.vanillaFallbackAborted,
           ps.vanillaFallbackAccepted - psStart.vanillaFallbackAccepted,
           hr.swappedSolves - hr0.swappedSolves, hl.swappedSolves - hl0.swappedSolves);
    printf("      temporal soak: pairs begun %u | eye0 %u eye1 %u | submitted %u "
           "aborted/fail-closed %u (halfNative %u disagree %u) cooldown %u | "
           "engaged R %u L %u gun %u | transformed R %u L %u gun %u | "
           "bothApplied R %u L %u gun %u | refusals VBO-unknown R %u L %u, "
           "RMS R %u L %u | injected eye1 %u eye0 %u leftOnly %u | dropouts %u "
           "rig switches %u turns %u LEFT_FREE-far %u | halfNativeSubmitted %u "
           "NaN %u staleValid %u collisions %u\n",
           begun, eye0, eye1, submitted, rejected,
           ps.rejectHalfNative - psStart.rejectHalfNative,
           ps.rejectDisagree - psStart.rejectDisagree,
           ps.cooldownPairs - psStart.cooldownPairs,
           engR, engL, engGun,
           hr.deltasApplied - hr0.deltasApplied, hl.deltasApplied - hl0.deltasApplied,
           gs.batchesTransformed - gs0.batchesTransformed,
           ps.objBothApplied[VRNH_OBJ_R] - psStart.objBothApplied[VRNH_OBJ_R],
           ps.objBothApplied[VRNH_OBJ_L] - psStart.objBothApplied[VRNH_OBJ_L],
           ps.objBothApplied[VRNH_OBJ_GUN] - psStart.objBothApplied[VRNH_OBJ_GUN],
           hr.refusedUnknownBytes - hr0.refusedUnknownBytes,
           hl.refusedUnknownBytes - hl0.refusedUnknownBytes,
           hr.refusedSolve - hr0.refusedSolve, hl.refusedSolve - hl0.refusedSolve,
           injectedEye1, injectedEye0, injectedLeft, dropouts, rigSwitches,
           turnEvents, leftFarPairs, halfNativeSubmitted, nan, staleValid,
           ownershipCollisions);

    CHECK(begun == (unsigned)kPairs && eye0 == begun && eye1 == begun,
          "soak: every pair rendered eye 0 then eye 1 (%u / %u / %u)", begun, eye0, eye1);
    CHECK(halfNativeSubmitted == 0,
          "soak: ZERO half-native pairs SUBMITTED across %d pairs (%u)",
          kPairs, halfNativeSubmitted);
    CHECK(rejected == expectedRejects,
          "soak: EVERY injected refusal on an engaged hand was caught at "
          "pre-submit, and nothing else was refused (%u rejected, %u expected "
          "of %u injected)", rejected, expectedRejects,
          injectedEye1 + injectedEye0 + injectedLeft);
    CHECK(ps.rejectDisagree - psStart.rejectDisagree == 0,
          "soak: zero PairWorld disagreements among %u both-eye applications -- "
          "per-eye deltas absorbed the animation advance every time (%u)",
          ps.objBothApplied[VRNH_OBJ_R] - psStart.objBothApplied[VRNH_OBJ_R],
          ps.rejectDisagree - psStart.rejectDisagree);
    CHECK(submitted + rejected == begun,
          "soak: every pair was either submitted or refused (%u + %u = %u)",
          submitted, rejected, begun);
    CHECK(engR > 8000 && engL > 7000 && engGun > 8000,
          "soak: native engagement ACTUALLY HAPPENED (R %u L %u gun %u)", engR, engL, engGun);
    CHECK(hr.deltasApplied - hr0.deltasApplied > 15000
          && hl.deltasApplied - hl0.deltasApplied > 13000
          && gs.batchesTransformed - gs0.batchesTransformed > 8000,
          "soak: RIGHT/LEFT/gun draws were TRANSFORMED (%u / %u / %u)",
          hr.deltasApplied - hr0.deltasApplied, hl.deltasApplied - hl0.deltasApplied,
          gs.batchesTransformed - gs0.batchesTransformed);
    CHECK(nan == 0, "soak: zero NaN (%u)", nan);
    CHECK(staleValid == 0, "soak: zero stale-valid published poses (%u)", staleValid);
    CHECK(ownershipCollisions == 0, "soak: zero ownership collisions (%u)", ownershipCollisions);
    CHECK(CODVR_CollisionHookCameraMutationCount() == 0,
          "soak: zero camera mutations by the object layer");
    CHECK(GLSurf_PairMismatchCount() == 0 && GLSurf_ReentryCount() == 0,
          "soak: zero latch mismatches, zero re-entries");
    {
        unsigned pairs = 0, mism = 0, late = 0, splits = 0;
        VRObj_GetCounters(&pairs, &mism, &late, &splits);
        CHECK(mism == 0 && splits == 0, "soak: zero object pair mismatches (%u/%u)", mism, splits);
    }
    CHECK(rigSwitches > 0 && dropouts > 0 && leftFarPairs > 100 && turnEvents > 0,
          "soak: the schedule was hostile (switches %u dropouts %u LEFT_FREE-far %u turns %u)",
          rigSwitches, dropouts, leftFarPairs, turnEvents);
    CHECK(swappedFrames > 8000 && own.orderSwapped - own0.orderSwapped > 3000
          && own.orderUnknown - own0.orderUnknown <= injectedEye1 + injectedEye0 + 64,
          "soak: the surface order swapped on %u frames; every recoverable merged batch "
          "resolved its order from content (unknown %u, all on injected corruption)",
          swappedFrames, own.orderUnknown - own0.orderUnknown);
    CHECK(own.singleByOrdinal - own0.singleByOrdinal <= injectedEye1 + injectedEye0 + 64
          && own.singleByContent - own0.singleByContent > 1000,
          "soak: single-surface hand draws identified by content (%u), ordinal only on corruption (%u)",
          own.singleByContent - own0.singleByContent, own.singleByOrdinal - own0.singleByOrdinal);
    CHECK(own.duplicateOwnerPrevented - own0.duplicateOwnerPrevented == 0,
          "soak: ZERO duplicate native/vanilla owners (%u)", own.duplicateOwnerPrevented - own0.duplicateOwnerPrevented);
    CHECK(hr.refusedSolve - hr0.refusedSolve <= injectedEye1 + injectedEye0
          && hl.refusedSolve - hl0.refusedSolve <= injectedEye1 + injectedEye0 + injectedLeft,
          "soak: every solve refusal was an injected corruption -- the swapped order never "
          "refused (R %u L %u vs injected %u/%u/%u)",
          hr.refusedSolve - hr0.refusedSolve, hl.refusedSolve - hl0.refusedSolve,
          injectedEye1, injectedEye0, injectedLeft);
    CHECK(ps.vanillaFallbackAccepted - psStart.vanillaFallbackAccepted == 0,
          "soak: no engaged pair ever degraded to accepted vanilla (%u)",
          ps.vanillaFallbackAccepted - psStart.vanillaFallbackAccepted);
    CHECK(g_mock.violations == 0, "soak: zero XR protocol violations");

    CODVR_TestSetNow(-1.0);
    VRHandPose_TestRestore();
    VRNH_TestReset();
    ResetLayerForCase();
}
