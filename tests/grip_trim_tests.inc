// grip_trim_tests.inc -- the shipped whole-palm trim (2026-09-06) against the
// hardware run's own numbers.
//
// Oracles (logs/hw-2026-09-06/snapshots, right grip frame, XR axes):
//   * the Thompson's BORE as drawn, constant to 0.02 deg over 7 snapshots:
//     (0.0724 -0.9871 0.1425) = 9.2 deg from the grip's -Y toward the
//     little finger;
//   * the Touch controller's AIM -Z in the grip frame, constant to 0.01 deg:
//     (0 -0.866 -0.5) = 30 deg toward the thumb;
//   * the direction the user called "straight" (trigger pulls 34-36, the
//     bore LEVEL at the aim's yaw): (0.0386 -0.9655 -0.2576) = 15 deg
//     toward the thumb -- halfway between the grip and the aim.
// The trim must turn the drawn bore onto that third direction with the hand
// and the weapon moving as one (a rotation of the whole palm target about
// the palm origin, which is what vr_native_hands_r_pitch/yaw/roll are).

// Shipped values (dist/codvr.cfg). Solved in the palm bone's own frame from
// the 23.2 deg grip-frame rotation; see the phase doc, 2026-09-06.
static const float kTrimR[3] = { -22.66f, -3.18f, -3.30f };
static const float kTrimL[3] = { 22.71f, -2.91f, 3.21f };

// XR grip-frame vector -> the grip's labelled rows (fwd,left,up) = (-Z,-X,+Y).
static void GTXrToRows(const float v[3], float c[3]) { c[0] = -v[2]; c[1] = -v[0]; c[2] = v[1]; }

// The bore's direction in the world for a grip axis and a trim: the bore is
// rigid in the palm bone, so take its bone coordinates from the UNTRIMMED
// base and re-express them on the trimmed rows.
static void GTBoreWorld(const float gripAxis[9], const float rows[9], const float trim[3],
                        const float boreGripRows[3], float out[3])
{
    float pos[3], base[9], trimmed[9];
    const float zero[3] = { 0, 0, 0 };
    VRHandPose_DesiredFromGripCal(zero, gripAxis, rows, nullptr, nullptr, pos, base);
    VRHandPose_DesiredFromGripCal(zero, gripAxis, rows, nullptr, trim, pos, trimmed);
    // bone coords: project the bore (in grip-row coords, i.e. the world of an
    // identity grip) on the identity-grip base rows = the cal rows.
    float ident[9] = { 1,0,0, 0,1,0, 0,0,1 };
    float basePos[3], baseI[9];
    VRHandPose_DesiredFromGripCal(zero, ident, rows, nullptr, nullptr, basePos, baseI);
    float bone[3];
    for (int r = 0; r < 3; ++r)
        bone[r] = baseI[r*3]*boreGripRows[0] + baseI[r*3+1]*boreGripRows[1] + baseI[r*3+2]*boreGripRows[2];
    for (int k = 0; k < 3; ++k)
        out[k] = bone[0]*trimmed[k] + bone[1]*trimmed[3+k] + bone[2]*trimmed[6+k];
    (void)base;
}
static float GTAngle(const float a[3], const float b[3])
{
    const float d = (a[0]*b[0] + a[1]*b[1] + a[2]*b[2]) / sqrtf((a[0]*a[0]+a[1]*a[1]+a[2]*a[2])*(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]));
    return (float)(acos(d > 1 ? 1 : d < -1 ? -1 : d) * 57.2957795);
}

static void S_GripTrimFeltStraight()
{
    printf("--- grip trim: the bore is level when the controller feels level (hw 2026-09-06) ---\n");
    NHResetForCase();
    VRNH_SetCalibrationTable(1);
    float rowsR[9], offR[3], rowsL[9], offL[3];
    CHECK(VRNH_GetEffectiveCalibration(VRI_RIGHT, -1, rowsR, offR)
          && VRNH_GetEffectiveCalibration(VRI_LEFT, -1, rowsL, offL),
          "the shipped hands_new4 calibration rows are in effect");

    const float boreXr[3] = { 0.0724f, -0.9871f, 0.1425f };
    const float aimXr[3]  = { 0.0f, -0.866f, -0.5f };
    const float desXr[3]  = { 0.0386f, -0.9655f, -0.2576f };
    float bore[3], aim[3], des[3];
    GTXrToRows(boreXr, bore); GTXrToRows(aimXr, aim); GTXrToRows(desXr, des);
    const float minusY[3] = { 0, 0, -1 };   // the grip's -Y in (fwd,left,up)

    // 1. In the grip frame (identity grip axis): the trimmed bore lands on
    //    the felt-straight direction; untrimmed it is the drawn 9.2 deg.
    const float ident[9] = { 1,0,0, 0,1,0, 0,0,1 };
    float b0[3], b1[3];
    GTBoreWorld(ident, rowsR, nullptr, bore, b0);
    GTBoreWorld(ident, rowsR, kTrimR, bore, b1);
    CHECK(GTAngle(b0, minusY) > 8.5f && GTAngle(b0, minusY) < 10.0f,
          "untrimmed: the drawn bore is 9.2 deg from the grip's -Y (got %.1f)", GTAngle(b0, minusY));
    CHECK(GTAngle(b1, des) < 0.3f, "trimmed: the bore lands on the felt-straight direction (%.2f deg off)", GTAngle(b1, des));
    CHECK(fabsf(GTAngle(b1, minusY) - 15.1f) < 0.5f && fabsf(GTAngle(b1, aim) - 15.2f) < 0.5f,
          "trimmed: halfway between the grip (%.1f) and the aim (%.1f)", GTAngle(b1, minusY), GTAngle(b1, aim));

    // 2. The full chain on the trigger pull the user called straight
    //    (snapshot 35): the bore was 23.3 deg low; with the trim it is level
    //    at the aim's yaw.
    const float gq35[4] = { -0.6089f, 0.0377f, 0.0599f, -0.7901f };
    const float aq35[4] = { -0.1322f, 0.0027f, 0.0707f, -0.9887f };
    float gripAxis[9], aimAxis[9];
    VRM_QuatToQuakeAxis(gq35, gripAxis);
    VRM_QuatToQuakeAxis(aq35, aimAxis);
    const float aimYaw = (float)(atan2(aimAxis[1], aimAxis[0]) * 57.2957795);
    float r0[3], r1[3];
    GTBoreWorld(gripAxis, rowsR, nullptr, bore, r0);
    GTBoreWorld(gripAxis, rowsR, kTrimR, bore, r1);
    const float p0 = (float)(asin(r0[2]) * 57.2957795), p1 = (float)(asin(r1[2]) * 57.2957795);
    const float y1 = (float)(atan2(r1[1], r1[0]) * 57.2957795);
    CHECK(fabsf(p0 - (-23.3f)) < 1.0f, "snapshot 35 untrimmed: bore pitch %.1f (hardware -23.3)", p0);
    CHECK(fabsf(p1) < 1.0f, "snapshot 35 trimmed: bore pitch %.1f (level)", p1);
    CHECK(fabsf(y1 - aimYaw) < 2.0f, "snapshot 35 trimmed: bore yaw %.1f at the aim's yaw %.1f", y1, aimYaw);
    // Mutants: the pitch sign flipped, or pitch only, do not level it.
    {
        const float flip[3] = { -kTrimR[0], kTrimR[1], kTrimR[2] };
        const float pitchOnly[3] = { 23.2f, 0, 0 };
        float m1[3], m2[3];
        GTBoreWorld(gripAxis, rowsR, flip, bore, m1);
        GTBoreWorld(gripAxis, rowsR, pitchOnly, bore, m2);
        const float pm1 = (float)(asin(m1[2]) * 57.2957795), pm2 = (float)(asin(m2[2]) * 57.2957795);
        CHECK(fabsf(pm1) > 10.0f, "mutant: pitch sign flipped leaves the bore at %.1f deg", pm1);
        CHECK(fabsf(pm2) > 10.0f, "mutant: a bone-pitch of 23.2 leaves the bore at %.1f deg", pm2);
    }

    // 3. The LEFT trim is the mirror image: the left hand's pointing (-Y)
    //    ends up where the right one's does, mirrored across the sagittal
    //    plane (the 'left' component flips).
    float pR[3], pL[3];
    GTBoreWorld(ident, rowsR, kTrimR, minusY, pR);
    GTBoreWorld(ident, rowsL, kTrimL, minusY, pL);
    CHECK(fabsf(pR[0] - pL[0]) < 0.01f && fabsf(pR[1] + pL[1]) < 0.01f && fabsf(pR[2] - pL[2]) < 0.01f,
          "left trim mirrors the right: R (%.3f %.3f %.3f) L (%.3f %.3f %.3f)",
          pR[0], pR[1], pR[2], pL[0], pL[1], pL[2]);
    CHECK(fabsf(GTAngle(pR, minusY) - 23.2f) < 0.5f && fabsf(GTAngle(pL, minusY) - 23.2f) < 0.5f,
          "both hands turn by the same 23.2 deg (R %.1f, L %.1f)", GTAngle(pR, minusY), GTAngle(pL, minusY));

    // 4. The trim is a rotation about the palm origin: the position is untouched.
    {
        float pos0[3], pos1[3], ax[9];
        const float gp[3] = { 3.0f, -9.8f, -16.7f };
        VRHandPose_DesiredFromGripCal(gp, gripAxis, rowsR, offR, nullptr, pos0, ax);
        VRHandPose_DesiredFromGripCal(gp, gripAxis, rowsR, offR, kTrimR, pos1, ax);
        CHECK(fabsf(pos0[0]-pos1[0]) + fabsf(pos0[1]-pos1[1]) + fabsf(pos0[2]-pos1[2]) < 1e-5f,
              "the trim moves no palm origin (the hand and the weapon turn in place)");
    }
}
