// ===========================================================================
// THE PALM ANCHOR (project-lead brief 2026-09-04d, P0, sections 2-5)
//
// The RIGHT controller owns the user's RIGHT PALM. The chain is
//     RawRightGrip -> ControllerGripToAnatomicalPalm -> DesiredRightPalm
// and the weapon is then placed RELATIVE TO THAT PALM by the shipped
// authored holding relation. Nothing is anchored to the weapon's body
// centre, model origin or any other pivot.
//
// THE ORACLE IS THE SPEC, NOT OUR OWN FIXTURE (brief section 20). OpenXR
// 1.0 "Grip pose": origin at the palm centroid; +X normal to the palm
// (into the palm on the RIGHT, away on the LEFT); +Y through the closed
// fingers, little finger -> thumb; +Z from the fingertip side toward the
// wrist. In the Quake rows the runtime uses, (fwd,left,up) = (-Z,-X,+Y),
// that reads: fwd = knuckles, left = palm-out (RIGHT) / palm-in (LEFT),
// up = thumb. So the rendered palm's anatomical axes must coincide with
// the CONTROLLER's own axes, to within the small tilt the shipped hand
// asset actually has. That is what "the controller is inside my hand"
// means numerically, and it is what this file measures.
// ===========================================================================

static const float PA_FWD[3] = { 1, 0, 0 }, PA_LEFT[3] = { 0, 1, 0 }, PA_UP[3] = { 0, 0, 1 };

static float PADot(const float a[3], const float b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

// A bone-frame direction of a rendered hand, in ROOM coordinates.
static void PAAnatRoom(const float axisEyeish[9], const float v[3], float outRoom[3])
{
    SOAnatRoom(axisEyeish, v, outRoom);
}

// One driven pair with a chosen weapon, capturing everything the audit needs.
struct PACap {
    int   ok;
    int   weaponKey;
    float gripEyeish[3];          // RAW right controller grip origin
    float gripAxisEyeish[9];      // ...and its rows (fwd,left,up)
    float palmEyeish[3];          // DesiredRightPalm origin
    float palmAxisEyeish[9];
    float renderedPalmRoom[3];    // where the palm actually landed (PairWorld)
    float gunRearGripEyeish[3];   // desired rear grip (where the gun is pinned)
    float gunRearGripRoom[3];
    float authoredBodyEyeish[3];  // the weapon body bone's authored origin
    float authoredGripEyeish[3];  // the weapon's authored rear grip
    float authoredPalmEyeish[3];  // the authored RIGHT palm (the hand baseline)
    float authoredPalmAxisEyeish[9];
    float barrelRoom[3];
};

static void PARun(const CodvrGunModel* gun, const float gAxis[9], const float gPos[3],
                  const float gripQuatR[4], PACap* cap)
{
    memset(cap, 0, sizeof(*cap));
    float rAxis[9], lAxis[9];
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis);
    const float rPos[3] = { 6.0f, -2.0f, -12.0f };
    const float lPos[3] = { -6.0f, -2.0f, -12.0f };
    const float headPos0[3] = { 0, 0, 0 };
    float headAxisI[9]; NHIdentityAxis(headAxisI);
    const float gripR[3] = { 0.20f, -0.10f, -0.35f };
    const float gripL[3] = { -0.20f, -0.10f, -0.35f };
    const float qL[4] = { gripQuatR[0], -gripQuatR[1], -gripQuatR[2], gripQuatR[3] };

    NHResetForCase();
    VRNH_SetEnabled(1);
    VRNH_SetTwoHandEnabled(0);
    VRNH_SetReloadEnabled(0);
    VRNH_SetCalibrationTable(1);
    VRInputState in; memset(&in, 0, sizeof(in));
    in.hand[VRI_RIGHT] = NHHand(1, gripR, gripQuatR);
    in.hand[VRI_LEFT]  = NHHand(1, gripL, qL);
    unsigned serial = 60;
    for (int p = 0; p < 4; ++p) {
        VRNH_OnPairBegin(++serial, &in, headPos0, headAxisI, 1, p * 0.01);
        for (int eye = 0; eye < 2; ++eye) {
            NHScene(eye);
            NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
            NHDriveHandDraw();
            NHDriveGunDraw(gun, gAxis, gPos);
            VRObj_NoteEyeConsumed(eye);
        }
    }
    VRNHDebug d; VRNH_GetDebug(&d);
    GLSurfGunSolve gs; GLSurf_GetGunSolve(&gs);
    cap->ok = d.hand[VRI_RIGHT].engaged && d.gun.targetInstalled;
    cap->weaponKey = gs.lastVertexCount;
    memcpy(cap->gripEyeish, d.hand[VRI_RIGHT].gripEyeish, sizeof(cap->gripEyeish));
    memcpy(cap->palmEyeish, d.hand[VRI_RIGHT].targetEyeish, sizeof(cap->palmEyeish));
    memcpy(cap->gunRearGripEyeish, d.gun.desiredGripEyeish, sizeof(cap->gunRearGripEyeish));
    memcpy(cap->authoredBodyEyeish, gs.bodyPosU, sizeof(cap->authoredBodyEyeish));
    memcpy(cap->authoredGripEyeish, gs.pivotU, sizeof(cap->authoredGripEyeish));
    {
        GLSurfHandSolve rhs;
        GLSurf_GetHandBaseline(GLSC_HAND_R, &rhs);
        memcpy(cap->authoredPalmEyeish, rhs.pos, sizeof(cap->authoredPalmEyeish));
        memcpy(cap->authoredPalmAxisEyeish, rhs.axis, sizeof(cap->authoredPalmAxisEyeish));
    }
    memcpy(cap->barrelRoom, d.gun.barrelRoom, sizeof(cap->barrelRoom));
    memcpy(cap->palmAxisEyeish, VRObj_Set()->hand[VROBJ_HAND_R].axis, sizeof(cap->palmAxisEyeish));
    memcpy(cap->renderedPalmRoom, VRObj_Set()->hand[VROBJ_HAND_R].pos, sizeof(cap->renderedPalmRoom));
    memcpy(cap->gunRearGripRoom, VRObj_Set()->gun.pos, sizeof(cap->gunRearGripRoom));
    // The raw grip rows, in the same eyeish frame the target lives in.
    {
        float quakeRows[9], eyeRows[9];
        VRM_QuatToQuakeAxis(gripQuatR, quakeRows);
        VRM_QuakeToGLEyeAxis(quakeRows, eyeRows);
        memcpy(cap->gripAxisEyeish, eyeRows, sizeof(cap->gripAxisEyeish));
    }
}

// ---------------------------------------------------------------------------
static void S_PalmAnchorConvention()
{
    printf("--- palm anchor: the rendered palm's anatomy IS the controller's own frame ---\n");
    const CodvrGunModel* bar = VRGunPose_ByVertexCount(516);
    if (!bar) { CHECK(0, "BAR table missing"); return; }
    float gAxis[9]; HpAxisFromEuler(2.0f, 14.0f, 1.0f, gAxis);
    const float gPos[3] = { 5.0f, -4.0f, -13.0f };
    PACap cap;
    PARun(bar, gAxis, gPos, kHwGripR, &cap);
    CHECK(cap.ok, "engaged with the recorded hardware grip");
    if (!cap.ok) return;

    // The controller's own axes in ROOM, from the recorded hardware quaternion.
    float gFwd[3], gLeft[3], gUp[3];
    SOEyeToRoom(cap.gripAxisEyeish + 0, gFwd);
    SOEyeToRoom(cap.gripAxisEyeish + 3, gLeft);
    SOEyeToRoom(cap.gripAxisEyeish + 6, gUp);
    for (int h = 0; h < 2; ++h) {
        const int xr = (h == 0) ? VRI_RIGHT : VRI_LEFT;
        const char* nm = (h == 0) ? "RIGHT" : "LEFT";
        float kB[3], tB[3], nB[3];
        VRNH_GetCalibrationAnatomy(xr, -1, kB, tB, nB);
        const float* axis = (h == 0) ? cap.palmAxisEyeish
                                     : VRObj_Set()->hand[VROBJ_HAND_L].axis;
        float k[3], t[3], n[3];
        PAAnatRoom(axis, kB, k); PAAnatRoom(axis, tB, t); PAAnatRoom(axis, nB, n);
        // The hand's own controller axes (the left controller mirrors x).
        float lFwd[3], lLeft[3], lUp[3];
        if (h == 0) { memcpy(lFwd, gFwd, sizeof(lFwd)); memcpy(lLeft, gLeft, sizeof(lLeft)); memcpy(lUp, gUp, sizeof(lUp)); }
        else {
            const float qL[4] = { kHwGripR[0], -kHwGripR[1], -kHwGripR[2], kHwGripR[3] };
            float qr[9], er[9];
            VRM_QuatToQuakeAxis(qL, qr);
            VRM_QuakeToGLEyeAxis(qr, er);
            SOEyeToRoom(er + 0, lFwd); SOEyeToRoom(er + 3, lLeft); SOEyeToRoom(er + 6, lUp);
        }
        // OpenXR grip pose, verbatim: -Z = little finger -> thumb (the thumb
        // side), +X = palm normal into the palm on the RIGHT, +Y = Z x X (the
        // wrist side). In the runtime's rows labelled (fwd,left,up) =
        // (-Z,-X,+Y): thumb = row0, palm-out = +/-row1, knuckles = -row2.
        const float palmSign = (h == 0) ? 1.0f : -1.0f;   // row1 = palm-out RIGHT, palm-in LEFT
        const float pointing[3] = { -lUp[0], -lUp[1], -lUp[2] };
        const float dk = PADot(k, pointing), dt = PADot(t, lFwd), dn = palmSign * PADot(n, lLeft);
        printf("      %s: knuckles.(-Y) %+.3f   thumb.(-Z) %+.3f   palm-out.(-X) %+.3f\n",
               nm, dk, dt, dn);
        CHECK(dk > 0.97f && dt > 0.90f && dn > 0.90f,
              "%s: the rendered palm's knuckles / thumb / palm-out sit on the controller's "
              "-Y / -Z / -X, as the OpenXR grip pose defines them", nm);
    }

    // ...and the palm ORIGIN is the controller's grip origin plus one small
    // anatomical offset, not a weapon-sized lever.
    const float lever = HpDist(cap.palmEyeish, cap.gripEyeish);
    CHECK(lever < 3.0f, "the controller-to-palm offset is %.2f u (%.1f cm), an anatomical "
          "offset and nothing more", lever, lever * 2.54f);

    // THE WEAPON'S BORE points where the controller points: along the
    // grip's -Y. This is the assertion that would have caught the
    // 2026-09-04d table (bore pitched up to +86 deg on hardware).
    {
        float pointing[3] = { -gUp[0], -gUp[1], -gUp[2] };
        const float db = PADot(cap.barrelRoom, pointing);
        CHECK(db > 0.85f,
              "the weapon's bore lies along the controller's pointing axis (%.3f); it is "
              "level when the controller is level", db);
    }

    // MUTANTS: the two tables that shipped before this one, rebuilt from the
    // same anatomy. Each is caught by the knuckle check above.
    {
        float kB[3], tB[3], nB[3];
        VRNH_GetCalibrationAnatomy(VRI_RIGHT, -1, kB, tB, nB);
        struct { const char* name; float r0, r2; int kIn0, kIn2, tIn0, tIn2; } muts[2] = {
            // rows[i] = (-t, n, k): knuckles on row2 (+Y = wrist side) -> backwards
            { "pre-2026-09-04 rows (-t, n, k): hands backwards/upside down", -1.0f, 1.0f, 0, 1, 1, 0 },
            // rows[i] = (k, n, t): knuckles on row0 (-Z = thumb side) -> vertically up
            { "2026-09-04d rows (k, n, t): hand vertically up 90 deg",       1.0f, 1.0f, 1, 0, 0, 1 },
        };
        float pointing[3] = { -gUp[0], -gUp[1], -gUp[2] };
        for (int m = 0; m < 2; ++m) {
            float rows[9];
            for (int i = 0; i < 3; ++i) {
                rows[i * 3 + 0] = muts[m].kIn0 ? kB[i] : muts[m].r0 * tB[i];
                rows[i * 3 + 1] = nB[i];
                rows[i * 3 + 2] = muts[m].kIn2 ? kB[i] : muts[m].r2 * tB[i];
            }
            float axis[9], pos[3];
            VRHandPose_DesiredFromGripCal(cap.gripEyeish, cap.gripAxisEyeish, rows, 0, 0, pos, axis);
            float k[3];
            PAAnatRoom(axis, kB, k);
            const float dk = PADot(k, pointing);
            CHECK(dk < 0.5f, "MUTANT %s: knuckles at %+.3f of the pointing axis -- caught",
                  muts[m].name, dk);
        }
    }

    // SECTION 9 POSES: the same holds with the wrist rolled +/-90 and the
    // weapon pitched / yawed +/-45 -- the anatomy tracks the controller.
    {
        struct { const char* name; float ax[3]; float deg; } poses[6] = {
            { "roll +90",  { 0, 0, 1 },  90 }, { "roll -90",  { 0, 0, 1 }, -90 },
            { "pitch +45", { 1, 0, 0 },  45 }, { "pitch -45", { 1, 0, 0 }, -45 },
            { "yaw +45",   { 0, 1, 0 },  45 }, { "yaw -45",   { 0, 1, 0 }, -45 },
        };
        int bad = 0;
        for (int p = 0; p < 6; ++p) {
            // q = qDelta * kHwGripR (a rotation about an XR world axis)
            const float half = poses[p].deg * 0.5f * 0.0174533f;
            const float qd[4] = { poses[p].ax[0] * sinf(half), poses[p].ax[1] * sinf(half),
                                  poses[p].ax[2] * sinf(half), cosf(half) };
            const float* b = kHwGripR;
            const float q[4] = {
                qd[3]*b[0] + qd[0]*b[3] + qd[1]*b[2] - qd[2]*b[1],
                qd[3]*b[1] - qd[0]*b[2] + qd[1]*b[3] + qd[2]*b[0],
                qd[3]*b[2] + qd[0]*b[1] - qd[1]*b[0] + qd[2]*b[3],
                qd[3]*b[3] - qd[0]*b[0] - qd[1]*b[1] - qd[2]*b[2] };
            PACap c;
            PARun(bar, gAxis, gPos, q, &c);
            if (!c.ok) { ++bad; continue; }
            float kB[3], tB[3], nB[3];
            VRNH_GetCalibrationAnatomy(VRI_RIGHT, -1, kB, tB, nB);
            float k[3], t[3], n[3], f[3], l[3], u[3];
            PAAnatRoom(c.palmAxisEyeish, kB, k); PAAnatRoom(c.palmAxisEyeish, tB, t);
            PAAnatRoom(c.palmAxisEyeish, nB, n);
            SOEyeToRoom(c.gripAxisEyeish + 0, f); SOEyeToRoom(c.gripAxisEyeish + 3, l);
            SOEyeToRoom(c.gripAxisEyeish + 6, u);
            const float pointing[3] = { -u[0], -u[1], -u[2] };
            const float dk = PADot(k, pointing), dt = PADot(t, f), dn = PADot(n, l);
            const float db = PADot(c.barrelRoom, pointing);
            const float lever = HpDist(c.palmEyeish, c.gripEyeish);
            printf("      %-9s knuckles %+.3f thumb %+.3f palm-out %+.3f bore %+.3f lever %.2f u\n",
                   poses[p].name, dk, dt, dn, db, lever);
            if (!(dk > 0.97f && dt > 0.90f && dn > 0.90f && db > 0.85f && lever < 3.0f)) ++bad;
        }
        CHECK(bad == 0, "roll +/-90, pitch +/-45, yaw +/-45: the hand stays on the controller "
              "and the weapon stays along its pointing axis");
    }
    VRNH_TestReset();
}

// ---------------------------------------------------------------------------
// The brief's section 4 table, through production code.
static void S_PalmAnchorAudit()
{
    printf("--- palm anchor audit: what actually sits on the controller ---\n");
    struct { const char* name; int key; } weapons[] = {
        { "BAR",      516 }, { "Thompson", 326 }, { "Colt",     344 },
    };
    float gAxis[9]; HpAxisFromEuler(2.0f, 14.0f, 1.0f, gAxis);
    const float gPos[3] = { 5.0f, -4.0f, -13.0f };
    printf("      %-9s %8s %8s %8s %9s %9s %s\n", "weapon", "grip->", "grip->", "grip->",
           "palm->", "palm->", "(inches)");
    printf("      %-9s %8s %8s %8s %9s %9s\n", "", "palm", "rearGrip", "body", "rearGrip", "body");
    int bad = 0;
    for (int w = 0; w < 3; ++w) {
        const CodvrGunModel* m = VRGunPose_ByVertexCount(weapons[w].key);
        if (!m) { CHECK(0, "%s table missing", weapons[w].name); continue; }
        PACap c;
        PARun(m, gAxis, gPos, kHwGripR, &c);
        if (!c.ok) { CHECK(0, "%s: engaged", weapons[w].name); continue; }
        // The authored body origin carried onto the controller-owned pose:
        // the weapon body's DESIRED origin is its authored origin moved by
        // the same rigid delta that puts the authored rear grip on the palm.
        const float dPalm = HpDist(c.gripEyeish, c.palmEyeish);
        const float dGrip = HpDist(c.gripEyeish, c.gunRearGripEyeish);
        const float authoredBodyFromGrip = HpDist(c.authoredGripEyeish, c.authoredBodyEyeish);
        const float dBody = dGrip + authoredBodyFromGrip;   // upper bound in the worst orientation
        const float palmToGrip = HpDist(c.palmEyeish, c.gunRearGripEyeish);
        printf("      %-9s %8.2f %8.2f %8.2f %9.2f %9.2f\n", weapons[w].name,
               dPalm, dGrip, dBody, palmToGrip, authoredBodyFromGrip);
        // The physical invariant: the controller sits at the PALM (a hand's
        // offset away, never a weapon's length), and the weapon is placed by
        // the AUTHORED palm->rear-grip relation, so the rendered relation
        // must equal the authored one. (The absolute palm->grip distance is
        // whatever this fixture's synthetic rig authored; the RELATION is
        // the invariant, and it is what the shipped assets decide.)
        if (!(dPalm < 3.0f)) ++bad;
        if (!(authoredBodyFromGrip > 1.0f)) ++bad;   // the body centre is NOT the grip
        {
            float relAuthored[3], relRendered[3];
            const float dA[3] = { c.authoredGripEyeish[0] - c.authoredPalmEyeish[0],
                                  c.authoredGripEyeish[1] - c.authoredPalmEyeish[1],
                                  c.authoredGripEyeish[2] - c.authoredPalmEyeish[2] };
            const float dR[3] = { c.gunRearGripEyeish[0] - c.palmEyeish[0],
                                  c.gunRearGripEyeish[1] - c.palmEyeish[1],
                                  c.gunRearGripEyeish[2] - c.palmEyeish[2] };
            VRM_WorldToLocalVec(c.authoredPalmAxisEyeish, dA, relAuthored);
            VRM_WorldToLocalVec(c.palmAxisEyeish, dR, relRendered);
            const float relErr = HpDist(relAuthored, relRendered);
            printf("      %-9s authored palm->rearGrip (%+.2f %+.2f %+.2f) u, rendered relation "
                   "error %.5f u\n", weapons[w].name, relAuthored[0], relAuthored[1],
                   relAuthored[2], relErr);
            if (!(relErr < 1e-2f)) ++bad;
        }
    }
    CHECK(bad == 0,
          "for every weapon: the controller is within 3 u of the rendered palm, the weapon "
          "keeps its AUTHORED palm-to-rear-grip relation, and the weapon BODY sits away "
          "from both -- the controller is in the hand, not in the rifle");
    VRNH_TestReset();
}

// ---------------------------------------------------------------------------
// The brief's section 5 gate: a weapon switch may not move the physical hand.
static void S_PalmInvariantAcrossWeapons()
{
    printf("--- HARD GATE: the same controller pose gives the same palm on every weapon ---\n");
    const int keys[5] = { 516, 326, 344, 494, 724 };          // BAR, Thompson, Colt, M1, MP40
    const char* names[5] = { "BAR", "Thompson", "Colt", "M1 Garand", "MP40" };
    float gAxis[9]; HpAxisFromEuler(2.0f, 14.0f, 1.0f, gAxis);
    const float gPos[3] = { 5.0f, -4.0f, -13.0f };
    PACap caps[5];
    int have = 0;
    for (int i = 0; i < 5; ++i) {
        const CodvrGunModel* m = VRGunPose_ByVertexCount(keys[i]);
        if (!m) { CHECK(0, "%s table missing", names[i]); continue; }
        PARun(m, gAxis, gPos, kHwGripR, &caps[i]);
        if (caps[i].ok) ++have;
    }
    CHECK(have == 5, "all five weapons drove the hand (%d/5)", have);
    if (have != 5) return;
    float worstPos = 0.0f, worstDeg = 0.0f;
    for (int i = 1; i < 5; ++i) {
        const float dp = HpDist(caps[0].palmEyeish, caps[i].palmEyeish);
        float dmax = 0.0f;
        for (int k = 0; k < 9; ++k) {
            const float e = fabsf(caps[0].palmAxisEyeish[k] - caps[i].palmAxisEyeish[k]);
            if (e > dmax) dmax = e;
        }
        const float deg = dmax * 57.2957795f;
        if (dp > worstPos) worstPos = dp;
        if (deg > worstDeg) worstDeg = deg;
        CHECK(caps[i].weaponKey == keys[i], "%s: the fixture really drove key %d (got %d)",
              names[i], keys[i], caps[i].weaponKey);
    }
    CHECK(worstPos < 1e-4f && worstDeg < 1e-3f,
          "BAR / Thompson / Colt / M1 / MP40 give the SAME right palm: worst %.6f u and "
          "%.6f deg apart", worstPos, worstDeg);
    // ...while the weapon's own placement relative to that palm DOES differ.
    {
        int differing = 0;
        for (int i = 1; i < 5; ++i) {
            float rel0[3], reli[3];
            const float d0[3] = { caps[0].gunRearGripEyeish[0] - caps[0].palmEyeish[0],
                                  caps[0].gunRearGripEyeish[1] - caps[0].palmEyeish[1],
                                  caps[0].gunRearGripEyeish[2] - caps[0].palmEyeish[2] };
            const float di[3] = { caps[i].gunRearGripEyeish[0] - caps[i].palmEyeish[0],
                                  caps[i].gunRearGripEyeish[1] - caps[i].palmEyeish[1],
                                  caps[i].gunRearGripEyeish[2] - caps[i].palmEyeish[2] };
            VRM_WorldToLocalVec(caps[0].palmAxisEyeish, d0, rel0);
            VRM_WorldToLocalVec(caps[i].palmAxisEyeish, di, reli);
            if (HpDist(rel0, reli) > 1e-4f) ++differing;
        }
        printf("      %d of 4 weapons place their rear grip differently in the palm frame\n", differing);
    }

    // MUTANT: anchor the controller to the weapon BODY instead of the palm.
    // The body origin sits a DIFFERENT distance from the grip on every
    // weapon, so the user's hand would jump by that difference on every
    // switch -- exactly the defect this gate forbids.
    {
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 5; ++i) {
            const float d = HpDist(caps[i].authoredGripEyeish, caps[i].authoredBodyEyeish);
            if (d < lo) lo = d;
            if (d > hi) hi = d;
        }
        CHECK(hi - lo > 1.0f,
              "MUTANT: the weapon body sits %.2f to %.2f u from the grip depending on the "
              "weapon, so anchoring there would jump the hand by up to %.1f cm on a switch "
              "-- the gate would catch it", lo, hi, (hi - lo) * 2.54f);
    }
    VRNH_TestReset();
}

// ---------------------------------------------------------------------------
// SECTION 6: the hand-only visual trim turns the RENDERED right hand about
// its palm pivot and nothing else: weapon, bore, aim and the LEFT hand are
// byte-identical with it on or off.
static void S_HandVisualTrim()
{
    printf("--- the right-hand VISUAL trim moves the rendered hand and nothing else ---\n");
    const CodvrGunModel* bar = VRGunPose_ByVertexCount(516);
    if (!bar) { CHECK(0, "BAR table missing"); return; }
    float gAxis[9]; HpAxisFromEuler(2.0f, 14.0f, 1.0f, gAxis);
    const float gPos[3] = { 5.0f, -4.0f, -13.0f };
    PACap off, on;
    PARun(bar, gAxis, gPos, kHwGripR, &off);
    VRObjPose gunOff = VRObj_Set()->gun;
    float leftPosOff[3], leftAxisOff[9], barrelOff[3];
    memcpy(leftPosOff, VRObj_Set()->hand[VROBJ_HAND_L].pos, sizeof(leftPosOff));
    memcpy(leftAxisOff, VRObj_Set()->hand[VROBJ_HAND_L].axis, sizeof(leftAxisOff));
    memcpy(barrelOff, off.barrelRoom, sizeof(barrelOff));
    CHECK(off.ok, "reference pair engaged");

    // The trim is NOT reset by the fixture's per-run reset (it is config).
    const float trim[3] = { 0.0f, 0.0f, 30.0f };      // 30 deg of roll about the palm's own axis
    PARun(bar, gAxis, gPos, kHwGripR, &on);
    VRNH_SetHandVisualTrim(VRI_RIGHT, trim);
    // re-drive the pairs with the trim installed
    {
        float rAxis[9], lAxis[9];
        HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis);
        HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis);
        const float rPos[3] = { 6.0f, -2.0f, -12.0f }, lPos[3] = { -6.0f, -2.0f, -12.0f };
        const float headPos0[3] = { 0, 0, 0 };
        float headAxisI[9]; NHIdentityAxis(headAxisI);
        const float gripR[3] = { 0.20f, -0.10f, -0.35f }, gripL[3] = { -0.20f, -0.10f, -0.35f };
        const float qL[4] = { kHwGripR[0], -kHwGripR[1], -kHwGripR[2], kHwGripR[3] };
        VRInputState in; memset(&in, 0, sizeof(in));
        in.hand[VRI_RIGHT] = NHHand(1, gripR, kHwGripR);
        in.hand[VRI_LEFT]  = NHHand(1, gripL, qL);
        unsigned serial = 80;
        for (int p = 0; p < 3; ++p) {
            VRNH_OnPairBegin(++serial, &in, headPos0, headAxisI, 1, 2.0 + p * 0.01);
            for (int eye = 0; eye < 2; ++eye) {
                NHScene(eye);
                NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
                NHDriveHandDraw();
                NHDriveGunDraw(bar, gAxis, gPos);
                VRObj_NoteEyeConsumed(eye);
            }
        }
    }
    VRNHDebug d; VRNH_GetDebug(&d);
    const auto* set = VRObj_Set();
    // The hand turned about its pivot...
    const float pivotMove = HpDist(set->hand[VROBJ_HAND_R].pos, off.renderedPalmRoom);
    float ang = 0.0f;
    {
        float worst = 0.0f;
        for (int r = 0; r < 3; ++r) {
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) {
                const float e = set->hand[VROBJ_HAND_R].axis[r*3+k] - off.palmAxisEyeish[r*3+k];
                d2 += e * e;
            }
            if (sqrtf(d2) > worst) worst = sqrtf(d2);
        }
        ang = 2.0f * asinf(worst * 0.5f) * 57.2957795f;
    }
    CHECK(pivotMove < 1e-4f && ang > 25.0f && ang < 35.0f,
          "trim (0,0,30): the rendered right hand turned %.1f deg about a pivot that moved %.6f u",
          ang, pivotMove);
    // ...and NOTHING else moved.
    float dGunPos = 0, dGunAxis = 0, dBarrel = 0, dLeftPos = 0, dLeftAxis = 0;
    for (int k = 0; k < 3; ++k) {
        dGunPos += fabsf(set->gun.pos[k] - gunOff.pos[k]);
        dBarrel += fabsf(d.gun.barrelRoom[k] - barrelOff[k]);
        dLeftPos += fabsf(set->hand[VROBJ_HAND_L].pos[k] - leftPosOff[k]);
    }
    for (int k = 0; k < 9; ++k) {
        dGunAxis += fabsf(set->gun.axis[k] - gunOff.axis[k]);
        dLeftAxis += fabsf(set->hand[VROBJ_HAND_L].axis[k] - leftAxisOff[k]);
    }
    CHECK(dGunPos == 0.0f && dGunAxis == 0.0f && dBarrel == 0.0f,
          "the weapon's PairWorld pose and the bore are BYTE-IDENTICAL (%.6f / %.6f / %.6f)",
          dGunPos, dGunAxis, dBarrel);
    CHECK(dLeftPos == 0.0f && dLeftAxis == 0.0f,
          "the LEFT hand is BYTE-IDENTICAL (%.6f / %.6f)", dLeftPos, dLeftAxis);
    VRNH_TestReset();
    float z[3];
    VRNH_GetHandVisualTrim(VRI_RIGHT, z);
    CHECK(z[0] == 0.0f && z[1] == 0.0f && z[2] == 0.0f, "the trim ships at identity");
}
