// ===========================================================================
// THE SLEEVE FOLLOWS THE WRIST RIGIDLY (project-lead brief 2026-09-04, P1,
// sections 6-8)
//
// The sleeve surface is skinned (forearm / upper arm / cuff / wrist / hand),
// so it is placed by ONE rigid motion about the anatomical wrist: the
// authored wrist point (the palm solve's origin, exact) goes to the desired
// palm origin, and the drawn forearm's orientation (solved from the
// forearm-dominated sleeve vertices) is turned to DesiredPalm * Rest, the
// forearm continuing the knuckle line. Never the sleeve's own centroid,
// never the camera, never the gun frame.
// ===========================================================================

static float g_slVerts[GLS_SLEEVE_VERTS * 2 * 3];

static int SLRigIndex(const char* name)
{
    for (int i = 0; i < VRHandPose_RigCount(); ++i)
        if (strcmp(VRHandPose_Rig(i)->name, name) == 0) return i;
    return -1;
}

// The merged sleeve batch in file order: RIGHT half first, then LEFT. Each
// half's forearm-dominated landmarks sit at an authored forearm pose whose
// wrist end is the palm origin; the other vertices run wrist -> elbow so
// the cuff-to-palm attribution finds them near their own palm.
static void SLBuildSleeveVerts(const float rFAxis[9], const float rWrist[3],
                               const float lFAxis[9], const float lWrist[3],
                               int corruptLeft)
{
    for (int arm = 0; arm < 2; ++arm) {
        const int first = (arm == 0) ? 0 : GLS_SLEEVE_VERTS;
        const float* wrist = (arm == 0) ? rWrist : lWrist;
        const float* faxis = (arm == 0) ? rFAxis : lFAxis;
        const CodvrSleeveDef* d = VRSleevePose_DefFor(SLRigIndex("viewmodel_hands_new4"),
                                                      arm == 0 ? VRHP_TRIGGER : VRHP_SUPPORT);
        const float lenU = d ? d->forearmLenM * CODVR_UNITS_PER_M : 11.57f;
        float elbow[3];
        for (int k = 0; k < 3; ++k) elbow[k] = wrist[k] - lenU * faxis[k];
        for (int i = 0; i < GLS_SLEEVE_VERTS; ++i) {
            const float t = (float)i / (float)(GLS_SLEEVE_VERTS - 1);
            float* v = &g_slVerts[(first + i) * 3];
            for (int k = 0; k < 3; ++k)
                v[k] = wrist[k] + (elbow[k] - wrist[k]) * t + 0.3f * faxis[3 + k] * sinf(i * 0.7f);
        }
        if (!d) continue;
        for (int i = 0; i < d->n; ++i) {
            const float bindU[3] = { d->lm[i].bindM[0] * CODVR_UNITS_PER_M,
                                     d->lm[i].bindM[1] * CODVR_UNITS_PER_M,
                                     d->lm[i].bindM[2] * CODVR_UNITS_PER_M };
            float r[3];
            HpApply(faxis, bindU, r);
            float* v = &g_slVerts[(first + d->lm[i].index) * 3];
            if (arm == 1 && corruptLeft) { v[0] = 777.0f + i; v[1] = -555.0f; v[2] = 9.0f * i; continue; }
            for (int k = 0; k < 3; ++k) v[k] = elbow[k] + r[k];
        }
    }
}

static void SLDriveSleeveDraw()
{
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_slVerts);
    GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_SLEEVE_INDICES * 2,
                          GLS_GL_UNSIGNED_INT, g_sleeveIdx);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_verts);
}

// Rotate a direction by a column-major 4x4's rotation part.
static void SLRot(const float m[16], const float v[3], float out[3])
{
    out[0] = m[0] * v[0] + m[4] * v[1] + m[8]  * v[2];
    out[1] = m[1] * v[0] + m[5] * v[1] + m[9]  * v[2];
    out[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2];
}

static float SLDet3(const float m[16])
{
    return m[0] * (m[5] * m[10] - m[9] * m[6])
         - m[4] * (m[1] * m[10] - m[9] * m[2])
         + m[8] * (m[1] * m[6]  - m[5] * m[2]);
}

// Both hands engaged at the glsurf level, right and left targets given.
static void SLEngage(const float rTPos[3], const float rTAxis[9],
                     const float lTPos[3], const float lTAxis[9])
{
    GLSurf_SetHandSolveWanted(GLSC_HAND_R, 1);
    GLSurf_SetHandSolveWanted(GLSC_HAND_L, 1);
    GLSurf_SetHandPairEligible(GLSC_HAND_R, 1);
    GLSurf_SetHandPairEligible(GLSC_HAND_L, 1);
    GLSurf_SetHandTarget(GLSC_HAND_R, 1, rTPos, rTAxis);
    GLSurf_SetHandTarget(GLSC_HAND_L, 1, lTPos, lTAxis);
}

struct SLScene {
    float rAxis[9], lAxis[9];      // authored palms
    float rPos[3], lPos[3];
    float rFAxis[9], lFAxis[9];    // authored forearms (with the animation's wrist bend)
    float rTAxis[9], lTAxis[9];    // desired palms
    float rTPos[3], lTPos[3];
    int   corruptLeftSleeve;
};

static void SLDefaultScene(SLScene* s)
{
    memset(s, 0, sizeof(*s));
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, s->rAxis);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, s->lAxis);
    HpAxisFromEuler(5.0f + 25.0f, 20.0f - 10.0f, -3.0f, s->rFAxis);   // bent wrists
    HpAxisFromEuler(-8.0f + 30.0f, 35.0f + 12.0f, 4.0f, s->lFAxis);
    const float rp[3] = { 6.0f, -2.0f, -12.0f };
    const float lp[3] = { -6.0f, -2.0f, -12.0f };
    memcpy(s->rPos, rp, sizeof(rp));
    memcpy(s->lPos, lp, sizeof(lp));
    HpAxisFromEuler(2.0f, 10.0f, 1.0f, s->rTAxis);
    HpAxisFromEuler(0.0f, 0.0f, 0.0f, s->lTAxis);
    const float rt[3] = { 7.0f, -3.0f, -13.0f };
    const float lt[3] = { -5.0f, -1.0f, -14.0f };
    memcpy(s->rTPos, rt, sizeof(rt));
    memcpy(s->lTPos, lt, sizeof(lt));
}

// Draw one eye-scene; returns the LEFT sleeve's wanted matrix (1) or 0 when
// it was not transformed (suppressed / vanilla). The RIGHT sleeve's matrix
// is returned through rightM when it was transformed.
static int SLDrawScene(int eye, const SLScene* s, float leftM[16], float rightM[16],
                       int* rightDrawn)
{
    NHScene(eye);
    SLEngage(s->rTPos, s->rTAxis, s->lTPos, s->lTAxis);
    NHBuildHandVerts(s->rAxis, s->rPos, s->lAxis, s->lPos);
    RecReset();
    NHDriveHandDraw();
    const int handDraws = CountKind(REC_LOADMV) / 2;
    SLBuildSleeveVerts(s->rFAxis, s->rPos, s->lFAxis, s->lPos, s->corruptLeftSleeve);
    SLDriveSleeveDraw();
    const int sleeveDraws = CountKind(REC_LOADMV) / 2 - handDraws;
    if (rightDrawn) *rightDrawn = 0;
    // Both arms are targeted, so the RIGHT half (first in file order) is
    // always transformed; the LEFT half follows it unless it was suppressed.
    if (sleeveDraws >= 1) {
        const GLRec* r = NthKind(REC_LOADMV, 2 * handDraws);
        if (r && rightM) { memcpy(rightM, r->m, 16 * sizeof(float)); if (rightDrawn) *rightDrawn = 1; }
    }
    if (sleeveDraws == 2) {
        const GLRec* l = NthKind(REC_LOADMV, 2 * handDraws + 2);
        if (l && leftM)  { memcpy(leftM, l->m, 16 * sizeof(float)); return 1; }
    }
    return 0;
}

// The five acceptance facts for one placement of the LEFT sleeve.
static int SLCheckPlacement(const char* tag, const SLScene* s, const float M[16])
{
    const CodvrSleeveDef* d = VRSleevePose_DefFor(SLRigIndex("viewmodel_hands_new4"), VRHP_SUPPORT);
    if (!d) { CHECK(0, "%s: left sleeve def missing", tag); return 0; }
    int ok = 1;
    // 1. the wrist stays connected: authored wrist -> desired palm origin
    float w[3]; HpXform4(M, s->lPos, w);
    const float dW = HpDist(w, s->lTPos);
    if (dW > 1e-3f) ok = 0;
    // 2. the forearm exits the wrist along the anatomical line:
    //    authored forearm X -> DesiredPalm * rest X
    float wantRows[9], gotRows[9];
    for (int r = 0; r < 3; ++r) {
        HpApply(s->lTAxis, d->restRows + r * 3, wantRows + r * 3);
        SLRot(M, s->lFAxis + r * 3, gotRows + r * 3);
    }
    float rowErr = 0.0f;
    for (int k = 0; k < 9; ++k) { const float e = fabsf(gotRows[k] - wantRows[k]); if (e > rowErr) rowErr = e; }
    if (rowErr > 2e-3f) ok = 0;
    // 3. the elbow lands one forearm length back along that line
    const float lenU = d->forearmLenM * CODVR_UNITS_PER_M;
    float aElbow[3], gElbow[3], wElbow[3];
    for (int k = 0; k < 3; ++k) { aElbow[k] = s->lPos[k] - lenU * s->lFAxis[k]; wElbow[k] = s->lTPos[k] - lenU * wantRows[k]; }
    HpXform4(M, aElbow, gElbow);
    const float dE = HpDist(gElbow, wElbow);
    if (dE > 1e-2f) ok = 0;
    // 4. a proper rotation: det +1, orthonormal, no scaling / mirroring
    const float det = SLDet3(M);
    float ortho = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float n = sqrtf(M[c*4]*M[c*4] + M[c*4+1]*M[c*4+1] + M[c*4+2]*M[c*4+2]);
        if (fabsf(n - 1.0f) > ortho) ortho = fabsf(n - 1.0f);
    }
    if (fabsf(det - 1.0f) > 1e-4f || ortho > 1e-4f) ok = 0;
    // 5. the elbow is nowhere near the eye (it is the visibility policy's job to say when it is)
    const float dEye = sqrtf(wElbow[0]*wElbow[0] + wElbow[1]*wElbow[1] + wElbow[2]*wElbow[2]);
    CHECK(ok, "%s: wrist %.4fu, forearm rows %.5f, elbow %.4fu, det %.5f, ortho %.1e, elbow %.1fu from eye",
          tag, dW, rowErr, dE, det, ortho, dEye);
    return ok;
}

// ---------------------------------------------------------------------------
static void S_SleeveTables()
{
    printf("--- sleeve tables: forearm landmarks and the rest relation per rig ---\n");
    CHECK(VRSleevePose_DefCount() >= 4, "%d sleeve defs (new4 + sten4, both sides)", VRSleevePose_DefCount());
    for (int i = 0; i < VRSleevePose_DefCount(); ++i) {
        const CodvrSleeveDef* d = VRSleevePose_Def(i);
        // rest rows: a proper right-handed orthonormal basis, X ~ +X of the
        // palm bone (the knuckle line), length the shipped forearm bone.
        float dot01 = 0, dot02 = 0, dot12 = 0, n0 = 0, n1 = 0, n2 = 0;
        for (int k = 0; k < 3; ++k) {
            dot01 += d->restRows[k] * d->restRows[3+k];
            dot02 += d->restRows[k] * d->restRows[6+k];
            dot12 += d->restRows[3+k] * d->restRows[6+k];
            n0 += d->restRows[k]*d->restRows[k]; n1 += d->restRows[3+k]*d->restRows[3+k]; n2 += d->restRows[6+k]*d->restRows[6+k];
        }
        float cr[3];
        cr[0] = d->restRows[1]*d->restRows[5] - d->restRows[2]*d->restRows[4];
        cr[1] = d->restRows[2]*d->restRows[3] - d->restRows[0]*d->restRows[5];
        cr[2] = d->restRows[0]*d->restRows[4] - d->restRows[1]*d->restRows[3];
        const float handed = cr[0]*d->restRows[6] + cr[1]*d->restRows[7] + cr[2]*d->restRows[8];
        CHECK(fabsf(dot01) < 1e-4f && fabsf(dot02) < 1e-4f && fabsf(dot12) < 1e-4f
              && fabsf(n0-1) < 1e-4f && fabsf(n1-1) < 1e-4f && fabsf(n2-1) < 1e-4f && handed > 0.999f,
              "%s %s: rest rows orthonormal, right-handed (X.palmX %.4f)", d->rig,
              d->hand == VRHP_TRIGGER ? "RIGHT" : "LEFT", d->restRows[0]);
        CHECK(d->restRows[0] > 0.99f, "...the forearm continues the knuckle line (straight wrist)");
        CHECK(d->forearmLenM > 0.28f && d->forearmLenM < 0.31f && d->n >= 20 && d->surfaceVerts == 120,
              "...forearm %.4f m, %d landmarks on the %d-vertex sleeve, gate %.0f mm",
              d->forearmLenM, d->n, d->surfaceVerts, d->rmsGateM * 1000.0f);
    }
}

// ---------------------------------------------------------------------------
static void S_SleeveFollowsWrist()
{
    printf("--- the LEFT sleeve exits the wrist along the knuckle line, pivot at the wrist ---\n");
    NHResetForCase();
    GLSurf_SetSleeveAnatomical(1);
    GLSurf_SetSleeveHideNearHead(1);
    GLSurf_SetSleeveVisible(2);        // this case is about placement, not policy
    SLScene s; SLDefaultScene(&s);
    float mL[16], mR[16]; int rDrawn = 0;

    // ---- neutral
    int got = SLDrawScene(0, &s, mL, mR, &rDrawn);
    CHECK(got == 1 && rDrawn == 1, "neutral: both sleeve halves transformed");
    if (got) SLCheckPlacement("neutral", &s, mL);
    GLSurfSleeveStats st; GLSurf_GetSleeveStats(&st);
    CHECK(st.followedForearm == 2 && st.followedPalm == 0,
          "both sleeves placed by the forearm solve (forearm %u, palm-fallback %u, rms %.4f m)",
          st.followedForearm, st.followedPalm, st.lastRmsM);
    CHECK(st.lastRmsM >= 0.0f && st.lastRmsM < 1e-4f, "exact fixture: residual %.6f m", st.lastRmsM);
    float mR0[16]; memcpy(mR0, mR, sizeof(mR0));

    // ---- the section-8 poses of the LEFT hand; the RIGHT sleeve must not move
    struct { const char* name; float pitch, yaw, roll; } poses[] = {
        { "palm up",    0,   0, 180 }, { "palm down",  0,   0,   0 },
        { "roll +90",   0,   0,  90 }, { "roll -90",   0,   0, -90 },
        { "pitch +60", 60,   0,   0 }, { "pitch -60", -60,  0,   0 },
        { "yaw +60",    0,  60,   0 }, { "yaw -60",    0, -60,   0 },
    };
    for (int p = 0; p < 8; ++p) {
        HpAxisFromEuler(poses[p].pitch, poses[p].yaw, poses[p].roll, s.lTAxis);
        got = SLDrawScene(p & 1, &s, mL, mR, &rDrawn);
        CHECK(got == 1, "%s: left sleeve transformed", poses[p].name);
        if (got) SLCheckPlacement(poses[p].name, &s, mL);
        CHECK(rDrawn && memcmp(mR, mR0, sizeof(mR0)) == 0,
              "%s: the RIGHT sleeve is byte-identical (the left target does not touch it)", poses[p].name);
    }

    // ---- the LEFT controller 1 m away: hand and sleeve stay one limb
    HpAxisFromEuler(0, 0, 0, s.lTAxis);
    s.lTPos[0] = -4.0f; s.lTPos[1] = 2.0f; s.lTPos[2] = -40.0f;
    got = SLDrawScene(0, &s, mL, mR, &rDrawn);
    CHECK(got == 1, "1 m away: transformed");
    if (got) SLCheckPlacement("1 m away", &s, mL);

    // ---- the gun is untouched by any of it
    {
        const CodvrGunModel* gun = VRGunPose_ByVertexCount(516);
        float gAxis[9]; HpAxisFromEuler(2.0f, 14.0f, 1.0f, gAxis);
        const float gPos[3] = { 5.0f, -4.0f, -13.0f };
        float want[9]; HpAxisFromEuler(10.0f, 5.0f, 0.0f, want);
        const float wantGrip[3] = { 4.0f, -3.0f, -12.0f };
        GLSurf_SetGunSolveWanted(1);
        GLSurf_SetGunTarget(1, wantGrip, want);
        float gM[2][16]; int have = 0;
        for (int v = 0; v < 2; ++v) {
            HpAxisFromEuler(v ? 40.0f : -20.0f, v ? -30.0f : 25.0f, v ? 70.0f : -50.0f, s.lTAxis);
            s.lTPos[2] = v ? -16.0f : -11.0f;
            SLDrawScene(v, &s, mL, mR, &rDrawn);
            const int before = CountKind(REC_LOADMV);
            NHDriveGunDraw(gun, gAxis, gPos);
            const GLRec* g = NthKind(REC_LOADMV, before);
            if (g) { memcpy(gM[v], g->m, sizeof(gM[v])); ++have; }
        }
        CHECK(have == 2 && memcmp(gM[0], gM[1], sizeof(gM[0])) == 0,
              "the gun's matrix is byte-identical across two very different left limbs");
        GLSurf_SetGunTarget(0, nullptr, nullptr);
        GLSurf_SetGunSolveWanted(0);
    }
    GLSurf_ClearHandTargets();
}

// ---------------------------------------------------------------------------
static void S_SleeveVisibilityAndFallback()
{
    printf("--- sleeve: hidden when its elbow reaches the head; fallback to the hand's delta ---\n");
    NHResetForCase();
    GLSurf_SetSleeveVisible(2);
    SLScene s; SLDefaultScene(&s);
    float mL[16], mR[16]; int rDrawn = 0;

    // The forearm line from a wrist 20 cm in front of the eyes, pointing
    // back at them: the elbow lands behind the eye plane -> not drawn.
    {
        const float wrist[3] = { -1.0f, -0.5f, -8.0f };
        const float len = sqrtf(wrist[0]*wrist[0] + wrist[1]*wrist[1] + wrist[2]*wrist[2]);
        float x[3] = { wrist[0] / len, wrist[1] / len, wrist[2] / len };   // eye -> wrist
        // rows: X = that line (so the elbow = wrist - L*X heads back to the eye)
        float y[3] = { 0, 1, 0 };
        float z[3] = { x[1]*y[2]-x[2]*y[1], x[2]*y[0]-x[0]*y[2], x[0]*y[1]-x[1]*y[0] };
        const float zn = sqrtf(z[0]*z[0]+z[1]*z[1]+z[2]*z[2]);
        for (int k = 0; k < 3; ++k) z[k] /= zn;
        y[0] = z[1]*x[2]-z[2]*x[1]; y[1] = z[2]*x[0]-z[0]*x[2]; y[2] = z[0]*x[1]-z[1]*x[0];
        memcpy(s.lTAxis, x, sizeof(x)); memcpy(s.lTAxis + 3, y, sizeof(y)); memcpy(s.lTAxis + 6, z, sizeof(z));
        memcpy(s.lTPos, wrist, sizeof(wrist));
        GLSurfStats st0; GLSurf_GetStats(&st0);
        const int got = SLDrawScene(0, &s, mL, mR, &rDrawn);
        GLSurfSleeveStats ss; GLSurf_GetSleeveStats(&ss);
        GLSurfStats st1; GLSurf_GetStats(&st1);
        CHECK(got == 0 && rDrawn == 1 && ss.hiddenNearHead == 1 && st1.suppressed == st0.suppressed + 1,
              "elbow into the head: the LEFT sleeve is not drawn (hidden %u, suppressed +%u), the RIGHT is",
              ss.hiddenNearHead, st1.suppressed - st0.suppressed);
        CHECK(ss.lastElbowEye[2] > -3.0f,
              "...its elbow would have been at z %.1f u (the eye plane is z 0)", ss.lastElbowEye[2]);
        // policy off: drawn, placed by the same rule
        GLSurf_SetSleeveHideNearHead(0);
        const int got2 = SLDrawScene(1, &s, mL, mR, &rDrawn);
        CHECK(got2 == 1, "policy off: drawn");
        if (got2) SLCheckPlacement("policy off, elbow at the head", &s, mL);
        GLSurf_SetSleeveHideNearHead(1);
    }

    // Fallback: a sleeve whose forearm cannot be solved takes its HAND's
    // delta (connected, the animation's wrist bend kept) -- never vanilla.
    SLDefaultScene(&s);
    s.corruptLeftSleeve = 1;
    {
        GLSurfSleeveStats s0; GLSurf_GetSleeveStats(&s0);
        const int got = SLDrawScene(0, &s, mL, mR, &rDrawn);
        GLSurfSleeveStats s1; GLSurf_GetSleeveStats(&s1);
        const GLRec* hand = NthKind(REC_LOADMV, 0);       // LEFT hand half first in file order
        CHECK(got == 1 && s1.followedPalm == s0.followedPalm + 1 && s1.solveRefusals == s0.solveRefusals + 1,
              "unsolvable sleeve: placed by the hand's delta (fallback +%u, refusal +%u)",
              s1.followedPalm - s0.followedPalm, s1.solveRefusals - s0.solveRefusals);
        CHECK(got == 1 && hand && memcmp(hand->m, mL, sizeof(mL)) == 0,
              "...byte-identical to the hand's matrix: still one limb");
    }

    // Config off: every sleeve takes the hand's delta (the previous behaviour).
    s.corruptLeftSleeve = 0;
    GLSurf_SetSleeveAnatomical(0);
    {
        GLSurfSleeveStats s0; GLSurf_GetSleeveStats(&s0);
        const int got = SLDrawScene(1, &s, mL, mR, &rDrawn);
        GLSurfSleeveStats s1; GLSurf_GetSleeveStats(&s1);
        const GLRec* hand = NthKind(REC_LOADMV, 0);
        CHECK(got == 1 && hand && memcmp(hand->m, mL, sizeof(mL)) == 0
              && s1.followedPalm == s0.followedPalm + 2 && s1.followedForearm == s0.followedForearm,
              "vr_sleeve_anatomical 0: sleeve == hand delta, no forearm solve");
    }
    GLSurf_SetSleeveAnatomical(1);
    GLSurf_ClearHandTargets();
}

// ---------------------------------------------------------------------------
// THE SHIPPED POLICY (brief 2026-09-04d, P5): the LEFT forearm still read as
// disconnected on hardware, so it is not drawn -- and the LEFT HAND is not
// touched by that decision. One config line brings it back.
static void S_SleeveVisibilityPolicy()
{
    printf("--- sleeve policy: the left forearm is hidden, the left HAND is not ---\n");
    NHResetForCase();
    CHECK(GLSurf_SleeveVisible() == 1,
          "the shipped default draws the RIGHT sleeve only (mode %d)", GLSurf_SleeveVisible());
    SLScene s; SLDefaultScene(&s);
    float mL[16], mR[16]; int rDrawn = 0;

    GLSurfSleeveStats before; GLSurf_GetSleeveStats(&before);
    int got = SLDrawScene(0, &s, mL, mR, &rDrawn);
    GLSurfSleeveStats after; GLSurf_GetSleeveStats(&after);
    CHECK(got == 0 && rDrawn == 1
          && after.hiddenByPolicy == before.hiddenByPolicy + 1,
          "default: the LEFT sleeve is not drawn, the RIGHT one is");
    // The hands themselves are still controller-owned and transformed.
    {
        GLSurfEyeApply aR, aL;
        CHECK(GLSurf_GetEyeApply(GLSC_HAND_R, 0, &aR) && aR.applied
              && GLSurf_GetEyeApply(GLSC_HAND_L, 0, &aL) && aL.applied,
              "...and BOTH hands are still native in that eye-scene");
    }
    // Mode 0 hides both; mode 2 restores both.
    GLSurf_SetSleeveVisible(0);
    got = SLDrawScene(1, &s, mL, mR, &rDrawn);
    CHECK(got == 0 && rDrawn == 0, "vr_sleeve_visible 0: neither sleeve is drawn");
    GLSurf_SetSleeveVisible(2);
    got = SLDrawScene(0, &s, mL, mR, &rDrawn);
    CHECK(got == 1 && rDrawn == 1, "vr_sleeve_visible 2: both are drawn again");
    if (got) SLCheckPlacement("restored", &s, mL);
    GLSurf_SetSleeveVisible(1);
    GLSurf_ClearHandTargets();
}
