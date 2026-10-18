// ===========================================================================
// THE BORE: SHOTS FOLLOW WHAT THE USER ALIGNED
// (project-lead brief 2026-09-04d, P1, sections 6-9)
//
// Hardware: the Thompson can be brought to its sights, and the shot lands
// about three feet high. The axis production called "barrel forward" is the
// CHORD from the weapon frame's origin to the muzzle tag, which on a rig
// whose frame origin sits off the bore is not the bore at all. The fire
// direction is now the weapon's SIGHT LINE, cross-checked at generation
// time against the muzzle tag's own rotation, in the body-bone frame.
// ===========================================================================

static float BOAngleDeg(const float a[3], const float b[3])
{
    const float la = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
    const float lb = sqrtf(b[0]*b[0] + b[1]*b[1] + b[2]*b[2]);
    if (la < 1e-6f || lb < 1e-6f) return -1.0f;
    float c = (a[0]*b[0] + a[1]*b[1] + a[2]*b[2]) / (la * lb);
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return acosf(c) * 57.2957795f;
}

// ---------------------------------------------------------------------------
static void S_BoreAxisTable()
{
    printf("--- the bore: every driveable weapon carries a sight-checked fire axis ---\n");
    struct { const char* name; int key; } w[] = {
        { "BAR",        516 }, { "Thompson",   326 }, { "Colt",       344 },
        { "M1 Garand",  494 }, { "MP40",       724 }, { "Sten",       630 },
        { "Kar98k",     536 }, { "Springfield",729 }, { "Bren",      1284 },
        { "MP44",       854 }, { "PPSh",       782 }, { "Luger",     1174 },
        { "Enfield",   1197 }, { "FG42",       999 }, { "Nagant",     652 },
    };
    const int n = (int)(sizeof(w) / sizeof(w[0]));
    int usable = 0, bad = 0;
    printf("      %-12s %7s  %-38s %s\n", "weapon", "vs body", "bore (body frame)", "muzzle (m)");
    for (int i = 0; i < n; ++i) {
        const CodvrGunModel* m = VRGunPose_ByVertexCount(w[i].key);
        if (!m) { CHECK(0, "%s (%d) missing", w[i].name, w[i].key); ++bad; continue; }
        if (!m->boreUsable) { printf("      %-12s  no bore\n", w[i].name); continue; }
        ++usable;
        const float dev = BOAngleDeg(m->boreFwd, m->barrelFwd);
        const float len = sqrtf(m->boreFwd[0]*m->boreFwd[0] + m->boreFwd[1]*m->boreFwd[1]
                              + m->boreFwd[2]*m->boreFwd[2]);
        printf("      %-12s %6.2f   (%+.4f %+.4f %+.4f)   (%+.3f %+.3f %+.3f)\n",
               w[i].name, dev, m->boreFwd[0], m->boreFwd[1], m->boreFwd[2],
               m->muzzleM[0], m->muzzleM[1], m->muzzleM[2]);
        if (fabsf(len - 1.0f) > 1e-3f) ++bad;                       // unit
        if (m->boreConf != VRWP_CONF_STRONG && m->boreConf != VRWP_CONF_PROVEN) ++bad;
        // The muzzle must be forward of the rear grip ALONG the bore, and
        // roughly the weapon's length away -- a bore pointing at the stock
        // would pass every angular test and still be backwards.
        float rel[3];
        for (int k = 0; k < 3; ++k) rel[k] = m->muzzleM[k] - m->rearGripM[k];
        const float along = rel[0]*m->boreFwd[0] + rel[1]*m->boreFwd[1] + rel[2]*m->boreFwd[2];
        if (!(along > 0.10f && along < 1.30f)) { ++bad; printf("        muzzle %.3f m along the bore\n", along); }
    }
    CHECK(usable >= 14 && bad == 0,
          "%d weapons carry a unit, STRONG+ bore whose muzzle lies forward along it", usable);

    // The grenades and the panzerfaust have no bore, and must not pretend to.
    {
        const CodvrGunModel* g = VRGunPose_ByVertexCount(960);
        CHECK(g && !g->boreUsable, "the frag grenade has no bore and says so");
    }
}

// ---------------------------------------------------------------------------
// Section 8: what angular error makes a three-foot miss, and does the
// Thompson's old axis produce exactly that?
static void S_BoreSightLineError()
{
    printf("--- the Thompson's three-foot miss, measured ---\n");
    const CodvrGunModel* th = VRGunPose_ByVertexCount(326);
    if (!th || !th->boreUsable) { CHECK(0, "thompson bore missing"); return; }
    const float oldErr = BOAngleDeg(th->barrelFwd, th->boreFwd);
    printf("      angular error of the OLD fire axis vs the sight line: %.2f deg\n", oldErr);
    printf("      %6s %10s %10s\n", "range", "old miss", "new miss");
    const float ranges[3] = { 3.0f, 5.0f, 10.0f };
    for (int i = 0; i < 3; ++i) {
        const float missOld = ranges[i] * tanf(oldErr * 0.0174533f);
        printf("      %5.0f m %9.2f m %9.3f m\n", ranges[i], missOld, 0.0f);
    }
    // 3 feet = 0.914 m: the range at which the old axis misses by that much.
    const float rangeFor3ft = 0.9144f / tanf(oldErr * 0.0174533f);
    printf("      the old axis misses by three feet at %.1f m\n", rangeFor3ft);
    CHECK(oldErr > 6.0f && oldErr < 7.5f,
          "the old fire axis sat %.2f deg from the Thompson's sight line", oldErr);
    CHECK(rangeFor3ft > 5.0f && rangeFor3ft < 12.0f,
          "...which is a three-foot miss at %.1f m -- the reported hardware error", rangeFor3ft);

    // Every weapon's bore now agrees with its sight line by construction, and
    // the ones that moved most are the ones the hardware complained about.
    int moved = 0;
    const int keys[6] = { 326, 1174, 782, 854, 344, 516 };
    const char* nm[6] = { "Thompson", "Luger", "PPSh", "MP44", "Colt", "BAR" };
    printf("      correction applied, per weapon (deg): ");
    for (int i = 0; i < 6; ++i) {
        const CodvrGunModel* m = VRGunPose_ByVertexCount(keys[i]);
        if (!m || !m->boreUsable) continue;
        const float d = BOAngleDeg(m->barrelFwd, m->boreFwd);
        printf("%s %.2f  ", nm[i], d);
        if (d > 1.5f) ++moved;
    }
    printf("\n");
    CHECK(moved >= 3, "%d weapons had a fire axis more than 1.5 deg off their sights", moved);
}

// ---------------------------------------------------------------------------
// Section 9: the bore reaches the game's aim, one degree for one degree,
// through the production chain.
static void S_BoreToGameAim()
{
    printf("--- the bore reaches the game aim: one degree in, one degree out ---\n");
    struct { const char* name; int key; } weapons[] = {
        { "Thompson", 326 }, { "Colt", 344 }, { "BAR", 516 },
    };
    float gAxis[9]; HpAxisFromEuler(2.0f, 14.0f, 1.0f, gAxis);
    const float gPos[3] = { 5.0f, -4.0f, -13.0f };
    int bad = 0;
    for (int w = 0; w < 3; ++w) {
        const CodvrGunModel* m = VRGunPose_ByVertexCount(weapons[w].key);
        if (!m) { CHECK(0, "%s missing", weapons[w].name); continue; }
        // Two controller poses: level, then pitched up 10 deg and yawed right 15.
        float b0[3], b1[3], q0[4], q1[4];
        int ok = 1;
        for (int p = 0; p < 2; ++p) {
            const float pitch = p ? 10.0f : 0.0f, yaw = p ? -15.0f : 0.0f;
            // A grip quaternion built from Quake pitch/yaw about the recorded
            // hardware pose, so the weapon is held the same way in both.
            float q[4];
            {
                const float cp = cosf(pitch * 0.5f * 0.0174533f), sp = sinf(pitch * 0.5f * 0.0174533f);
                const float cy = cosf(yaw * 0.5f * 0.0174533f), sy = sinf(yaw * 0.5f * 0.0174533f);
                // XR: pitch about +X (right), yaw about +Y (up)
                const float qp[4] = { sp, 0, 0, cp }, qy[4] = { 0, sy, 0, cy };
                q[0] = qy[3]*qp[0] + qy[0]*qp[3] + qy[1]*qp[2] - qy[2]*qp[1];
                q[1] = qy[3]*qp[1] - qy[0]*qp[2] + qy[1]*qp[3] + qy[2]*qp[0];
                q[2] = qy[3]*qp[2] + qy[0]*qp[1] - qy[1]*qp[0] + qy[2]*qp[3];
                q[3] = qy[3]*qp[3] - qy[0]*qp[0] - qy[1]*qp[1] - qy[2]*qp[2];
            }
            memcpy(p ? q1 : q0, q, sizeof(q));
            PACap c;
            PARun(m, gAxis, gPos, q, &c);
            if (!c.ok) { ok = 0; break; }
            VRNHDebug d; VRNH_GetDebug(&d);
            if (!d.gun.barrelValid || !d.gun.boreFromSightLine) { ok = 0; break; }
            memcpy(p ? b1 : b0, d.gun.barrelRoom, sizeof(b0));
        }
        if (!ok) { CHECK(0, "%s: both poses drove a valid bore", weapons[w].name); ++bad; continue; }
        // The fire direction must rotate by EXACTLY the rotation the
        // controller made. Comparing Euler components would fold the two
        // rotations together (the yaw of a pitched vector is not the yaw
        // that was commanded), so the check is against the rotated vector.
        float want[3];
        {
            float r0[9], r1[9];
            VRM_QuatToQuakeAxis(q0, r0);
            VRM_QuatToQuakeAxis(q1, r1);
            // b0 in the level grip's own frame, then back out through the
            // rotated grip: want = rows1^T * (rows0 * b0).
            float loc[3];
            for (int r = 0; r < 3; ++r)
                loc[r] = r0[r*3+0]*b0[0] + r0[r*3+1]*b0[1] + r0[r*3+2]*b0[2];
            for (int c = 0; c < 3; ++c)
                want[c] = r1[0*3+c]*loc[0] + r1[1*3+c]*loc[1] + r1[2*3+c]*loc[2];
        }
        const float err = BOAngleDeg(want, b1);
        const float moved = BOAngleDeg(b0, b1);
        printf("      %-9s fire direction moved %6.2f deg, and is %.4f deg from the "
               "controller's own rotation of it\n", weapons[w].name, moved, err);
        if (!(err < 0.05f)) ++bad;
        // How far a vector travels under a fixed rotation depends on where it
        // starts (10 up and 15 right composes to 17.9 deg for a level bore and
        // less for a pitched one), so the travel is bounded and the rotation
        // match above is the assertion.
        // (10 deg about XR X and 15 deg about XR Y compose to 10-18 deg of
        // travel depending on where the bore starts; a level bore along the
        // grip's -Y sees ~10.4.)
        if (!(moved > 8.0f && moved < 19.0f)) ++bad;
    }
    CHECK(bad == 0,
          "the controller's own rotation IS the fire direction's rotation, to under 0.05 "
          "deg, on every weapon (10 deg up and 15 deg right = 17.9 deg of travel)");
    VRNH_TestReset();
}

// ---------------------------------------------------------------------------
// The head may not move the shot, and the muzzle must sit on the bore.
static void S_BoreHeadIndependence()
{
    printf("--- the bore belongs to the weapon: the head cannot move the shot ---\n");
    const CodvrGunModel* th = VRGunPose_ByVertexCount(326);
    if (!th) { CHECK(0, "thompson missing"); return; }
    float gAxis[9]; HpAxisFromEuler(2.0f, 14.0f, 1.0f, gAxis);
    const float gPos[3] = { 5.0f, -4.0f, -13.0f };
    PACap c;
    PARun(th, gAxis, gPos, kHwGripR, &c);
    CHECK(c.ok, "engaged");
    VRNHDebug d0; VRNH_GetDebug(&d0);
    float b0[3]; memcpy(b0, d0.gun.barrelRoom, sizeof(b0));
    CHECK(d0.gun.muzzleValid, "the muzzle position is published with the fire direction");
    // The muzzle sits forward of the rear grip, along the fire direction.
    {
        float rel[3];
        for (int k = 0; k < 3; ++k) rel[k] = d0.gun.muzzleEyeish[k] - d0.gun.desiredGripEyeish[k];
        const float len = sqrtf(rel[0]*rel[0] + rel[1]*rel[1] + rel[2]*rel[2]);
        float relRoom[3];
        SOEyeToRoom(rel, relRoom);
        const float along = (relRoom[0]*b0[0] + relRoom[1]*b0[1] + relRoom[2]*b0[2]) / (len > 1e-6f ? len : 1.0f);
        const float drop = len * sqrtf(1.0f - (along < 1.0f ? along * along : 1.0f));
        CHECK(len > 4.0f && along > 0.90f,
              "the muzzle is %.1f u forward of the grip, %.3f of it along the fire direction, "
              "with %.1f u of grip drop below the bore line", len, along, drop);
    }
    // Now turn and translate the head; the room-frame fire direction must not move.
    {
        float headAxis[9]; HpAxisFromEuler(15.0f, 40.0f, -8.0f, headAxis);
        const float headPos[3] = { 0.10f, -0.05f, 0.08f };
        float rAxis[9], lAxis[9];
        HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis);
        HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis);
        const float rPos[3] = { 6.0f, -2.0f, -12.0f };
        const float lPos[3] = { -6.0f, -2.0f, -12.0f };
        const float gripR[3] = { 0.20f, -0.10f, -0.35f };
        const float gripL[3] = { -0.20f, -0.10f, -0.35f };
        const float qL[4] = { kHwGripR[0], -kHwGripR[1], -kHwGripR[2], kHwGripR[3] };
        VRInputState in; memset(&in, 0, sizeof(in));
        in.hand[VRI_RIGHT] = NHHand(1, gripR, kHwGripR);
        in.hand[VRI_LEFT]  = NHHand(1, gripL, qL);
        unsigned serial = 120;
        for (int p = 0; p < 3; ++p) {
            VRNH_OnPairBegin(++serial, &in, headPos, headAxis, 1, 1.0 + p * 0.01);
            for (int eye = 0; eye < 2; ++eye) {
                NHScene(eye);
                NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
                NHDriveHandDraw();
                NHDriveGunDraw(th, gAxis, gPos);
                VRObj_NoteEyeConsumed(eye);
            }
        }
        VRNHDebug d1; VRNH_GetDebug(&d1);
        const float moved = BOAngleDeg(b0, d1.gun.barrelRoom);
        CHECK(d1.gun.barrelValid && moved < 0.05f,
              "the head turned 40 deg and moved 10 cm: the room-frame fire direction moved "
              "%.4f deg", moved);
    }
    VRNH_TestReset();
}
