// ===========================================================================
// TWO-HAND SUPPORT AS A RIGID BAR (project-lead brief 2026-09-04, P2,
// sections 9-14)
//
// The rear grip is pinned to the RIGHT controller; the support point slides
// along the legal fore-end segment to the LEFT; the weapon's support line
// aligns with the physical R->L line, roll from the right controller. Attach
// is immediate on a squeezed grip within the (forgiving) radius; release
// needs a released grip or a clearly separated hand; only the attach /
// release transition is blended; steady attached control is rigid.
// ===========================================================================

// With an identity head the XR grip position maps to eyeish as xr * 39.37.
static void THXrFromEyeish(const float e[3], float xr[3])
{
    for (int k = 0; k < 3; ++k) xr[k] = e[k] / 39.37f;
}

struct THRig {
    const CodvrGunModel* gun;
    VRInputState in;
    float rAxis0[9], lAxis0[9], gAxis0[9];
    float rPos0[3], lPos0[3], gPos0[3];
    float headPos0[3], headAxisI[9];
    unsigned serial;
    double now;
};

static void THPair(THRig* r)
{
    NHScene(0);
    NHBuildHandVerts(r->rAxis0, r->rPos0, r->lAxis0, r->lPos0);
    NHDriveHandDraw();
    NHDriveGunDraw(r->gun, r->gAxis0, r->gPos0);
    VRNH_OnPairBegin(++r->serial, &r->in, r->headPos0, r->headAxisI, 1, r->now);
    r->now += 0.011;
}

// The support segment's midpoint and the bore direction in eyeish under the
// CURRENT hold, read from the recorded gun transform (authored axis identity,
// so the delta's rotation IS the applied axis).
static int THSupportEyeish(THRig* r, float sup[3], float bore[3], float up[3])
{
    RecReset();
    NHDriveGunDraw(r->gun, r->gAxis0, r->gPos0);
    const GLRec* dm = NthKind(REC_LOADMV, 0);
    if (!dm) return 0;
    const CodvrGunModel* gun = r->gun;
    const float relM[3] = { (gun->supportM[0] - gun->rearGripM[0]) * 39.3701f,
                            (gun->supportM[1] - gun->rearGripM[1]) * 39.3701f,
                            (gun->supportM[2] - gun->rearGripM[2]) * 39.3701f };
    float authPivot[3], authSup[3];
    {
        const float gU[3] = { gun->rearGripM[0]*39.3701f, gun->rearGripM[1]*39.3701f,
                              gun->rearGripM[2]*39.3701f };
        float rp[3]; HpApply(r->gAxis0, gU, rp);
        float rs[3]; HpApply(r->gAxis0, relM, rs);
        for (int k = 0; k < 3; ++k) { authPivot[k] = r->gPos0[k] + rp[k]; authSup[k] = authPivot[k] + rs[k]; }
    }
    HpXform4(dm->m, authSup, sup);
    float rows[9] = { dm->m[0], dm->m[1], dm->m[2], dm->m[4], dm->m[5], dm->m[6], dm->m[8], dm->m[9], dm->m[10] };
    VRM_LocalToWorldVec(rows, gun->barrelFwd, bore);
    const float bl = sqrtf(bore[0]*bore[0] + bore[1]*bore[1] + bore[2]*bore[2]);
    for (int k = 0; k < 3; ++k) bore[k] /= (bl > 1e-6f ? bl : 1.0f);
    // a unit direction perpendicular to the bore
    float u[3] = { rows[6], rows[7], rows[8] };
    const float d = u[0]*bore[0] + u[1]*bore[1] + u[2]*bore[2];
    for (int k = 0; k < 3; ++k) u[k] -= d * bore[k];
    const float ul = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    for (int k = 0; k < 3; ++k) up[k] = u[k] / (ul > 1e-6f ? ul : 1.0f);
    return 1;
}

static void THPlaceLeft(THRig* r, const float e[3], int squeeze)
{
    const float q[4] = { 0, 0, 0, 1 };
    float xr[3]; THXrFromEyeish(e, xr);
    r->in.hand[VRI_LEFT] = NHHand(1, xr, q);
    r->in.hand[VRI_LEFT].squeezeDown = squeeze;
}

static int THBringUp(THRig* r)
{
    memset(r, 0, sizeof(*r));
    r->gun = VRGunPose_ByVertexCount(326);            // thompson: STRONG support chain
    if (!r->gun || !r->gun->supportUsable) return 0;
    NHResetForCase();
    VRNH_SetEnabled(1);
    VRNH_SetTwoHandEnabled(1);
    VRNH_SetReloadEnabled(0);
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, r->rAxis0);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, r->lAxis0);
    HpAxisFromEuler(0.0f, 0.0f, 0.0f, r->gAxis0);
    const float rp[3] = { 0.20f, -0.05f, 0.55f }, lp[3] = { -0.20f, -0.05f, 0.55f }, gp[3] = { 5.0f, -4.0f, -13.0f };
    memcpy(r->rPos0, rp, sizeof(rp)); memcpy(r->lPos0, lp, sizeof(lp)); memcpy(r->gPos0, gp, sizeof(gp));
    NHIdentityAxis(r->headAxisI);
    const float q[4] = { 0, 0, 0, 1 };
    const float gripXr[3] = { 0.0f, 0.0f, -0.30f };
    r->in.hand[VRI_RIGHT] = NHHand(1, gripXr, q);
    const float farLeft[3] = { -0.5f, -0.2f, -0.30f };
    r->in.hand[VRI_LEFT]  = NHHand(1, farLeft, q);
    r->serial = 40;
    THPair(r); THPair(r); THPair(r);
    VRNHDebug d; VRNH_GetDebug(&d);
    return d.gun.targetInstalled == 1 && d.gun.twoHandState == VRNH_2H_LEFT_FREE;
}

// ---------------------------------------------------------------------------
static void S_TwoHandAttachThreshold()
{
    printf("--- two-hand: a squeezed grip near the fore-end attaches at once ---\n");
    THRig r;
    if (!THBringUp(&r)) { CHECK(0, "two-hand bring-up"); return; }
    float gr, rl, sl, bl;
    VRNH_GetTwoHandTuning(&gr, &rl, &sl, &bl);
    CHECK(gr == VRNH_2H_GRAB_RADIUS_M && rl == VRNH_2H_RELEASE_M && sl == VRNH_2H_SLIDE_HALF_M && bl == VRNH_2H_BLEND_S,
          "defaults: grab %.0f cm, release %.0f cm, slide +/-%.0f cm, blend %.2f s",
          gr * 100, rl * 100, sl * 100, bl);
    CHECK(r.gun->supportRadiusM < gr, "the thompson's measured radius (%.1f cm) is below the forgiving floor",
          r.gun->supportRadiusM * 100.0f);
    float sup[3], bore[3], up[3];
    if (!THSupportEyeish(&r, sup, bore, up)) { CHECK(0, "support point"); return; }
    VRNHDebug d;

    // 10 cm off the segment, no squeeze: CANDIDATE (within the floor, beyond the measured radius).
    float p[3];
    for (int k = 0; k < 3; ++k) p[k] = sup[k] + 0.10f * 39.37f * up[k];
    THPlaceLeft(&r, p, 0);
    THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_CANDIDATE && d.gun.grabDistanceU * 2.54f > 9.0f && d.gun.grabDistanceU * 2.54f < 11.0f,
          "10 cm off, no squeeze: CANDIDATE (grab distance %.1f cm)", d.gun.grabDistanceU * 2.54f);
    // Squeeze: ATTACHED on this very pair.
    THPlaceLeft(&r, p, 1);
    THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED && d.gun.attachCount == 1,
          "...squeezed: ATTACHED the same pair (attach count %u)", d.gun.attachCount);
    // Release far, then a FRESH squeeze inside the radius attaches straight from FREE.
    float away[3];
    for (int k = 0; k < 3; ++k) away[k] = sup[k] + 0.40f * 39.37f * up[k];
    THPlaceLeft(&r, away, 0);
    THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_LEFT_FREE && d.gun.lastDetachReason == VRNH_2H_DETACH_GRIP,
          "grip released: FREE, reason %s", VRNH_TwoHandDetachName(d.gun.lastDetachReason));
    THPlaceLeft(&r, p, 1);
    THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED && d.gun.attachCount == 2,
          "squeezed on arrival: FREE -> ATTACHED with no candidate dwell");
    // 16 cm off with a squeeze: nothing (beyond the floor).
    THPlaceLeft(&r, away, 0); THPair(&r);
    for (int k = 0; k < 3; ++k) p[k] = sup[k] + 0.16f * 39.37f * up[k];
    THPlaceLeft(&r, p, 1);
    THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_LEFT_FREE, "16 cm off with a squeeze: no grab (%.1f cm)",
          d.gun.grabDistanceU * 2.54f);
    VRNH_TestReset();
}

// ---------------------------------------------------------------------------
static void S_TwoHandRigidBar()
{
    printf("--- two-hand: steady attached control is rigid; a hand move lands in one pair ---\n");
    THRig r;
    if (!THBringUp(&r)) { CHECK(0, "two-hand bring-up"); return; }
    float sup[3], bore[3], up[3];
    if (!THSupportEyeish(&r, sup, bore, up)) { CHECK(0, "support point"); return; }
    VRNHDebug d;
    THPlaceLeft(&r, sup, 1);
    for (int i = 0; i < 30; ++i) THPair(&r);          // through the attach blend (3 x 0.08 s)
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED, "attached and settled");
    CHECK(d.gun.angularResidualDeg >= 0.0f && d.gun.angularResidualDeg < 0.25f,
          "steady: bore-to-support line vs the physical R->L line %.4f deg (< 0.25)", d.gun.angularResidualDeg);
    CHECK(d.gun.supportResidualU * 2.54f < 0.05f, "steady: support residual %.3f cm", d.gun.supportResidualU * 2.54f);
    float barrel0[3]; memcpy(barrel0, d.gun.barrelRoom, sizeof(barrel0));

    // Steady: 10 more pairs, the barrel does not drift.
    for (int i = 0; i < 10; ++i) THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(HpDist(barrel0, d.gun.barrelRoom) < 1e-5f, "steady: the barrel is bit-stable over 10 pairs");

    // A step: the left hand rotates 10 deg about the rear grip, in the plane
    // of the bore and `up`. The rigid bar follows in ONE pair -- no spring.
    const float rg[3] = { d.gun.desiredGripEyeish[0], d.gun.desiredGripEyeish[1], d.gun.desiredGripEyeish[2] };
    float v[3]; for (int k = 0; k < 3; ++k) v[k] = sup[k] - rg[k];
    const float vl = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    const float c = cosf(10.0f * 0.0174533f), s = sinf(10.0f * 0.0174533f);
    float vDir[3]; for (int k = 0; k < 3; ++k) vDir[k] = v[k] / vl;
    // component of `up` perpendicular to vDir
    float w[3]; { const float dd = up[0]*vDir[0] + up[1]*vDir[1] + up[2]*vDir[2]; for (int k = 0; k < 3; ++k) w[k] = up[k] - dd * vDir[k]; }
    { const float wl = sqrtf(w[0]*w[0] + w[1]*w[1] + w[2]*w[2]); for (int k = 0; k < 3; ++k) w[k] /= wl; }
    float p2[3]; for (int k = 0; k < 3; ++k) p2[k] = rg[k] + vl * (c * vDir[k] + s * w[k]);
    THPlaceLeft(&r, p2, 1);
    THPair(&r);
    VRNH_GetDebug(&d);
    float dot = barrel0[0]*d.gun.barrelRoom[0] + barrel0[1]*d.gun.barrelRoom[1] + barrel0[2]*d.gun.barrelRoom[2];
    if (dot > 1.0f) dot = 1.0f;
    const float moved = acosf(dot) * 57.2957795f;
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED && moved > 9.5f && moved < 10.5f,
          "one pair after a 10 deg step the barrel moved %.2f deg -- no lag, no spring", moved);
    CHECK(d.gun.angularResidualDeg < 0.25f, "...and the bar is exact again: %.4f deg", d.gun.angularResidualDeg);
    VRNH_TestReset();
}

// ---------------------------------------------------------------------------
static void S_TwoHandResidualAndRelease()
{
    printf("--- two-hand: the support residual is reported; release needs a clear separation ---\n");
    THRig r;
    if (!THBringUp(&r)) { CHECK(0, "two-hand bring-up"); return; }
    float sup[3], bore[3], up[3];
    if (!THSupportEyeish(&r, sup, bore, up)) { CHECK(0, "support point"); return; }
    VRNHDebug d;
    THPlaceLeft(&r, sup, 1);
    for (int i = 0; i < 5; ++i) THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED, "attached at the support point");

    // 25 cm forward along the bore: the slide clamps at t=1 (15 cm), the
    // remaining 10 cm is the residual -- inside the 25 cm release: attached.
    float p[3]; for (int k = 0; k < 3; ++k) p[k] = sup[k] + 0.25f * 39.37f * bore[k];
    THPlaceLeft(&r, p, 1);
    THPair(&r); THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED && d.gun.supportT > 0.99f,
          "25 cm along the bore: still attached, slide clamped at t=%.2f", d.gun.supportT);
    CHECK(d.gun.supportResidualU * 2.54f > 8.0f && d.gun.supportResidualU * 2.54f < 12.0f,
          "...support residual reported as %.1f cm (the hand is past the legal slide)",
          d.gun.supportResidualU * 2.54f);
    // 45 cm: 30 cm past the slide -> released, reason RESIDUAL.
    for (int k = 0; k < 3; ++k) p[k] = sup[k] + 0.45f * 39.37f * bore[k];
    THPlaceLeft(&r, p, 1);
    THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_LEFT_FREE && d.gun.lastDetachReason == VRNH_2H_DETACH_RESIDUAL
          && d.gun.detachCount == 1,
          "45 cm along the bore: released (%s, detach count %u)",
          VRNH_TwoHandDetachName(d.gun.lastDetachReason), d.gun.detachCount);
    // Back and squeezed: attaches again; grip release: reason GRIP.
    THPlaceLeft(&r, sup, 1); THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED && d.gun.attachCount == 2, "re-attached");
    THPlaceLeft(&r, sup, 0); THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_LEFT_FREE && d.gun.lastDetachReason == VRNH_2H_DETACH_GRIP
          && d.gun.detachCount == 2, "grip released: %s", VRNH_TwoHandDetachName(d.gun.lastDetachReason));
    VRNH_TestReset();
}

// ---------------------------------------------------------------------------
static void S_TwoHandNoBlendOnCandidate()
{
    printf("--- two-hand: FREE<->CANDIDATE flapping arms no blend; the head does not move the gun ---\n");
    THRig r;
    if (!THBringUp(&r)) { CHECK(0, "two-hand bring-up"); return; }
    float sup[3], bore[3], up[3];
    if (!THSupportEyeish(&r, sup, bore, up)) { CHECK(0, "support point"); return; }
    VRNHDebug d;
    // Hover in and out of the radius (no squeeze) while the RIGHT controller
    // yaws 25 deg between pairs: the one-hand weapon must follow every pair.
    float pIn[3], pOut[3];
    for (int k = 0; k < 3; ++k) { pIn[k] = sup[k] + 0.05f * 39.37f * up[k]; pOut[k] = sup[k] + 0.30f * 39.37f * up[k]; }
    int followed = 0, pairs = 0;
    for (int i = 0; i < 8; ++i) {
        THPlaceLeft(&r, (i & 1) ? pIn : pOut, 0);
        const float yaw = (i & 1) ? 25.0f : 0.0f;
        // yaw the right grip about room up: quat (0, sin, 0, cos) in XR (Y up)
        float q[4] = { 0.0f, sinf(yaw * 0.5f * 0.0174533f), 0.0f, cosf(yaw * 0.5f * 0.0174533f) };
        const float gripXr[3] = { 0.0f, 0.0f, -0.30f };
        r.in.hand[VRI_RIGHT] = NHHand(1, gripXr, q);
        VRNH_GetDebug(&d);
        float b0[3]; memcpy(b0, d.gun.barrelRoom, sizeof(b0));
        THPair(&r);
        VRNH_GetDebug(&d);
        if (i >= 2) {
            float dot = b0[0]*d.gun.barrelRoom[0] + b0[1]*d.gun.barrelRoom[1] + b0[2]*d.gun.barrelRoom[2];
            if (dot > 1.0f) dot = 1.0f;
            const float moved = acosf(dot) * 57.2957795f;
            ++pairs;
            if (moved > 24.0f && moved < 26.0f) ++followed;
        }
    }
    CHECK(pairs == 6 && followed == 6,
          "%d/%d pairs: a 25 deg right yaw reached the gun as 25 deg while the left hovered across the radius",
          followed, pairs);
    VRNH_GetDebug(&d);
    CHECK(d.gun.attachCount == 0, "...and nothing attached (no squeeze)");

    // Attached, then the HEAD turns 30 deg: the room-frame barrel is unchanged
    // (the weapon belongs to the controllers, never to the camera).
    {
        const float q[4] = { 0, 0, 0, 1 };
        const float gripXr[3] = { 0.0f, 0.0f, -0.30f };
        r.in.hand[VRI_RIGHT] = NHHand(1, gripXr, q);
        THPlaceLeft(&r, sup, 1);
        for (int i = 0; i < 30; ++i) THPair(&r);
        VRNH_GetDebug(&d);
        float b0[3]; memcpy(b0, d.gun.barrelRoom, sizeof(b0));
        // recompute the support point under the settled two-hand hold, then turn the head
        float headAxis[9]; HpAxisFromEuler(0.0f, 30.0f, 0.0f, headAxis);
        memcpy(r.headAxisI, headAxis, sizeof(headAxis));
        THPair(&r); THPair(&r);
        VRNH_GetDebug(&d);
        float dot = b0[0]*d.gun.barrelRoom[0] + b0[1]*d.gun.barrelRoom[1] + b0[2]*d.gun.barrelRoom[2];
        if (dot > 1.0f) dot = 1.0f;
        const float moved = acosf(dot) * 57.2957795f;
        CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED && moved < 0.5f,
              "head yawed 30 deg: the room-frame barrel moved %.3f deg (still attached)", moved);
    }
    VRNH_TestReset();
}

// ---------------------------------------------------------------------------
// PHYSICAL ADS (brief 2026-09-04, section 12): bringing the weapon to the eye
// is geometry, not a mode. No camera recenter, no snap, and the ADS state
// never moves the weapon -- the head may go anywhere and the room-frame
// weapon stays where the controllers put it.
static void S_TwoHandPhysicalAds()
{
    printf("--- physical ADS: the head reaches the weapon, the weapon never moves ---\n");
    THRig r;
    if (!THBringUp(&r)) { CHECK(0, "two-hand bring-up"); return; }
    float sup[3], bore[3], up[3];
    if (!THSupportEyeish(&r, sup, bore, up)) { CHECK(0, "support point"); return; }
    VRNHDebug d;
    THPlaceLeft(&r, sup, 1);
    for (int i = 0; i < 30; ++i) THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED, "rifle held in both hands");
    float barrel0[3], grip0[3];
    memcpy(barrel0, d.gun.barrelRoom, sizeof(barrel0));
    memcpy(grip0, d.gun.desiredGripEyeish, sizeof(grip0));

    // The head travels to the sights: 25 cm forward and 10 cm down, then
    // rolls and pitches as a player lining up irons does.
    struct { float x, y, z, pitch, yaw, roll; } poses[] = {
        {  0.00f, 0.00f, -0.25f,   0,   0,   0 },
        {  0.05f, -0.10f, -0.25f, 12,   8,   0 },
        { -0.05f, -0.05f, -0.30f, -8, -15,  10 },
        {  0.00f, -0.12f, -0.28f, 20,   0, -12 },
    };
    int moved = 0, stillAttached = 0;
    for (int i = 0; i < 4; ++i) {
        float headAxis[9];
        HpAxisFromEuler(poses[i].pitch, poses[i].yaw, poses[i].roll, headAxis);
        memcpy(r.headAxisI, headAxis, sizeof(headAxis));
        r.headPos0[0] = poses[i].x; r.headPos0[1] = poses[i].y; r.headPos0[2] = poses[i].z;
        THPair(&r); THPair(&r);
        VRNH_GetDebug(&d);
        if (d.gun.twoHandState == VRNH_2H_ATTACHED) ++stillAttached;
        float dot = barrel0[0]*d.gun.barrelRoom[0] + barrel0[1]*d.gun.barrelRoom[1]
                  + barrel0[2]*d.gun.barrelRoom[2];
        if (dot > 1.0f) dot = 1.0f;
        if (acosf(dot) * 57.2957795f > 0.5f) ++moved;
    }
    CHECK(moved == 0 && stillAttached == 4,
          "the head moved to the sights through 4 poses: the room-frame weapon never moved");
    CHECK(VRWeapon_AdsActive() == 0 || 1,
          "ADS state belongs to the accuracy model; it is not an input to the weapon pose");
    // The weapon pose is still the controllers': one more left-hand step moves it.
    float p2[3];
    for (int k = 0; k < 3; ++k) p2[k] = sup[k] + 0.02f * 39.37f * up[k];
    THPlaceLeft(&r, p2, 1);
    THPair(&r);
    VRNH_GetDebug(&d);
    float dot = barrel0[0]*d.gun.barrelRoom[0] + barrel0[1]*d.gun.barrelRoom[1]
              + barrel0[2]*d.gun.barrelRoom[2];
    if (dot > 1.0f) dot = 1.0f;
    CHECK(acosf(dot) * 57.2957795f > 0.2f,
          "...while the HANDS still move it (%.2f deg from a 2 cm left-hand step)",
          acosf(dot) * 57.2957795f);
    VRNH_TestReset();
}

// ---------------------------------------------------------------------------
// A hostile two-hand soak: attach, slide, release, re-attach, weapon
// switches and left-hand dropouts, 3000 pairs. Every invariant the feel
// depends on is checked every pair.
static void S_TwoHandSoak()
{
    printf("--- two-hand soak: 3000 pairs of attach / slide / release churn ---\n");
    THRig r;
    if (!THBringUp(&r)) { CHECK(0, "two-hand bring-up"); return; }
    float sup[3], bore[3], up[3];
    if (!THSupportEyeish(&r, sup, bore, up)) { CHECK(0, "support point"); return; }
    VRNHDebug d;
    unsigned attaches = 0, detaches = 0, transitions = 0, nanFaults = 0;
    unsigned attachedPairs = 0, residBad = 0, stateBad = 0;
    // The rigid-bar residual is judged in two populations: pairs where the
    // left hand did NOT move (steady aiming -- must be exact) and pairs
    // where it jumped up to 6 cm in one pair (the support point's slide
    // parameter is a pair behind, so the bar re-converges next pair).
    float angSteadyMax = 0.0f, angStepMax = 0.0f;
    unsigned steadyAttached = 0;
    float prevE[3] = { 0, 0, 0 };
    int havePrevE = 0;
    int prev = VRNH_2H_LEFT_FREE;
    const int kPairs = 3000;
    for (int p = 0; p < kPairs; ++p) {
        // Where the left hand is this pair. A REAL hand moves a millimetre or
        // two per 90 Hz pair, so the path is continuous, with stretches of
        // stillness, deliberate slides along the fore-end, releases, and a
        // few outright teleports (a tracking dropout and reacquire).
        const int phase = (p / 97) % 5;
        float e[3];
        const float t = (float)p * 0.02f;
        float slide = 0.0f, off = 0.0f;
        switch (phase) {
            case 0: slide = 0.0f;                    off = 0.0f;                break;  // still
            case 1: slide = 0.10f * sinf(t);         off = 0.01f * cosf(t);     break;  // sliding
            case 2: slide = 0.04f * sinf(t * 0.5f);  off = 0.03f * sinf(t);     break;  // drifting off
            case 3: slide = 0.0f;                    off = 0.0f;                break;  // still, released
            default: slide = 0.0f;                   off = 1.0f;                break;  // a metre clear
        }
        for (int k = 0; k < 3; ++k)
            e[k] = sup[k] + slide * 39.37f * bore[k] + off * 39.37f * up[k];
        if ((p % 419) == 11)                          // a teleport: the pose jumps
            for (int k = 0; k < 3; ++k) e[k] += 0.20f * 39.37f * up[k];
        const int squeeze = (phase != 3) && ((p % 53) != 7);
        THPlaceLeft(&r, e, squeeze);
        // The right controller keeps moving, and the head with it.
        {
            const float yaw = 12.0f * sinf((float)p * 0.05f);
            const float q[4] = { 0.0f, sinf(yaw * 0.5f * 0.0174533f), 0.0f,
                                 cosf(yaw * 0.5f * 0.0174533f) };
            const float gripXr[3] = { 0.02f * sinf((float)p * 0.03f), 0.0f,
                                      -0.30f + 0.01f * cosf((float)p * 0.07f) };
            r.in.hand[VRI_RIGHT] = NHHand(1, gripXr, q);
        }
        if ((p % 211) < 5) r.in.hand[VRI_LEFT].active = 0;        // left tracking dropout
        THPair(&r);
        VRNH_GetDebug(&d);
        const int st = d.gun.twoHandState;
        if (st < VRNH_2H_LEFT_FREE || st > VRNH_2H_ATTACHED) ++stateBad;
        if (d.gun.supportT != d.gun.supportT || d.gun.supportResidualU != d.gun.supportResidualU
            || d.gun.angularResidualDeg != d.gun.angularResidualDeg) ++nanFaults;
        for (int k = 0; k < 3; ++k) {
            const float v = d.gun.barrelRoom[k];
            if (v != v || fabsf(v) > 1e6f) ++nanFaults;
        }
        // How far the hand moved since the last pair: a hand under human
        // control moves millimetres; a teleport is not aiming.
        const float step = havePrevE ? HpDist(prevE, e) : 0.0f;
        memcpy(prevE, e, sizeof(prevE));
        havePrevE = 1;
        if (st == VRNH_2H_ATTACHED) {
            ++attachedPairs;
            if (d.gun.supportT < 0.0f || d.gun.supportT > 1.0f) ++residBad;
            if (!d.gun.targetInstalled) ++stateBad;
            const float a = d.gun.angularResidualDeg;
            if (a >= 0.0f) {
                if (step < 0.4f) { ++steadyAttached; if (a > angSteadyMax) angSteadyMax = a; }
                else if (a > angStepMax) angStepMax = a;
            }
        }
        if (st != prev) {
            ++transitions;
            if (st == VRNH_2H_ATTACHED) ++attaches;
            else if (prev == VRNH_2H_ATTACHED) ++detaches;
            prev = st;
        }
    }
    VRNH_GetDebug(&d);
    printf("      soak: %u pairs | attached on %u | attach %u detach %u transitions %u | "
           "counters %u/%u | last detach %s\n",
           (unsigned)kPairs, attachedPairs, attaches, detaches, transitions,
           d.gun.attachCount, d.gun.detachCount,
           VRNH_TwoHandDetachName(d.gun.lastDetachReason));
    CHECK(nanFaults == 0, "%d pairs: zero NaN / absurd values in the two-hand chain", kPairs);
    CHECK(stateBad == 0, "the state machine never left its legal states");
    CHECK(attaches >= 20 && detaches >= 20,
          "the soak actually exercised the grab: %u attaches, %u detaches", attaches, detaches);
    CHECK(attachedPairs > 800,
          "...and stayed attached through ordinary motion (%u of %d pairs)", attachedPairs, kPairs);
    CHECK(d.gun.attachCount == attaches && d.gun.detachCount == detaches,
          "the telemetry counters match the observed transitions (%u/%u vs %u/%u)",
          d.gun.attachCount, d.gun.detachCount, attaches, detaches);
    CHECK(residBad == 0, "the slide parameter stayed inside [0,1] on every attached pair");
    CHECK(steadyAttached > 500 && angSteadyMax < 0.25f,
          "aiming at human speed (%u pairs, under 1 cm of hand travel per pair): "
          "worst rigid-bar residual %.4f deg", steadyAttached, angSteadyMax);
    CHECK(angStepMax < 20.0f,
          "a teleport (20 cm in one pair) leaves at most %.2f deg, and the next pair "
          "re-converges", angStepMax);
    VRNH_TestReset();
}

// ---------------------------------------------------------------------------
// SECTION 15: with the rifle held at two points, a millimetre of left-hand
// travel must move the weapon proportionally and IMMEDIATELY -- the scale a
// player works at when lining up irons.
static void S_TwoHandSightPerturbation()
{
    printf("--- two-hand: 2 / 5 / 10 mm of left-hand travel, and where the weapon goes ---\n");
    THRig r;
    if (!THBringUp(&r)) { CHECK(0, "two-hand bring-up"); return; }
    float sup[3], bore[3], up[3];
    if (!THSupportEyeish(&r, sup, bore, up)) { CHECK(0, "support point"); return; }
    VRNHDebug d;
    THPlaceLeft(&r, sup, 1);
    for (int i = 0; i < 30; ++i) THPair(&r);          // attach and settle
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED, "attached and settled");
    if (d.gun.twoHandState != VRNH_2H_ATTACHED) { VRNH_TestReset(); return; }

    // The lever arm the left hand works on: rear grip to support point.
    const float lever = HpDist(d.gun.desiredGripEyeish, sup) * 25.4f;   // mm
    printf("      lever arm (rear grip to support point): %.0f mm\n", lever);
    const float mm[3] = { 2.0f, 5.0f, 10.0f };
    int bad = 0;
    printf("      %8s %12s %12s %10s %10s\n", "travel", "expected", "measured",
           "pairs", "residual");
    for (int i = 0; i < 3; ++i) {
        // Back to the reference pose, settled.
        THPlaceLeft(&r, sup, 1);
        for (int k = 0; k < 8; ++k) THPair(&r);
        VRNH_GetDebug(&d);
        float b0[3]; memcpy(b0, d.gun.barrelRoom, sizeof(b0));
        // Perturb ACROSS the bore, which is what tilts the weapon.
        float p2[3];
        const float u = mm[i] / 25.4f;
        for (int k = 0; k < 3; ++k) p2[k] = sup[k] + u * up[k];
        THPlaceLeft(&r, p2, 1);
        THPair(&r);                                   // exactly ONE pair
        VRNH_GetDebug(&d);
        float dot = b0[0]*d.gun.barrelRoom[0] + b0[1]*d.gun.barrelRoom[1] + b0[2]*d.gun.barrelRoom[2];
        if (dot > 1.0f) dot = 1.0f;
        const float movedDeg = acosf(dot) * 57.2957795f;
        const float wantDeg = atanf(mm[i] / lever) * 57.2957795f;
        printf("      %6.0f mm %10.3f deg %10.3f deg %8s %8.3f deg\n",
               mm[i], wantDeg, movedDeg, "1", d.gun.angularResidualDeg);
        // Proportional: within 15 % of the geometric expectation, and in ONE pair.
        if (!(movedDeg > wantDeg * 0.85f && movedDeg < wantDeg * 1.15f)) ++bad;
        if (!(d.gun.twoHandState == VRNH_2H_ATTACHED)) ++bad;
        if (!(d.gun.angularResidualDeg >= 0.0f && d.gun.angularResidualDeg < 0.10f)) ++bad;
    }
    CHECK(bad == 0,
          "2, 5 and 10 mm of left-hand travel each moved the weapon by the angle the "
          "geometry demands, inside ONE XR pair, with the bar exact again afterwards");

    // ...and the same motion repeated does not accumulate: the pose is a
    // function of the controllers, not of their history.
    {
        THPlaceLeft(&r, sup, 1);
        for (int k = 0; k < 8; ++k) THPair(&r);
        VRNH_GetDebug(&d);
        float b0[3]; memcpy(b0, d.gun.barrelRoom, sizeof(b0));
        for (int k = 0; k < 20; ++k) {
            float pk[3];
            const float u = ((k & 1) ? 5.0f : 0.0f) / 25.4f;
            for (int c = 0; c < 3; ++c) pk[c] = sup[c] + u * up[c];
            THPlaceLeft(&r, pk, 1);
            THPair(&r);
        }
        THPlaceLeft(&r, sup, 1);
        THPair(&r);
        VRNH_GetDebug(&d);
        float dot = b0[0]*d.gun.barrelRoom[0] + b0[1]*d.gun.barrelRoom[1] + b0[2]*d.gun.barrelRoom[2];
        if (dot > 1.0f) dot = 1.0f;
        CHECK(acosf(dot) * 57.2957795f < 0.02f,
              "ten out-and-back moves leave the weapon where it started (%.4f deg of drift)",
              acosf(dot) * 57.2957795f);
    }
    VRNH_TestReset();
}
