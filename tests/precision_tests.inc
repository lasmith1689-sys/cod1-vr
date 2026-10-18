// precision_tests.inc -- the 2026-09-06 afternoon batch: HMD brightness,
// the crosshair mirror into autoexec, and the support pose lock (the LEFT
// hand rides the weapon while the two-hand support is attached).

static void S_CrosshairLineSync()
{
    printf("--- crosshair: the cg_drawCrosshair line is set in place, once, and only that line ---\n");
    char t[2048];
    // 1. replace in place, keep everything else byte-identical
    strcpy_s(t, "// header\r\nseta r_gamma \"0.85\"\r\nseta cg_drawCrosshair \"1\"\r\nseta r_picmip \"0\"\r\n");
    int r = CodvrSetCvarLine(t, sizeof(t), "cg_drawCrosshair", "0");
    CHECK(r == 1 && !strcmp(t, "// header\r\nseta r_gamma \"0.85\"\r\nseta cg_drawCrosshair \"0\"\r\nseta r_picmip \"0\"\r\n"),
          "existing line replaced in place, CRLF kept, neighbours untouched");
    // 2. idempotent
    r = CodvrSetCvarLine(t, sizeof(t), "cg_drawCrosshair", "0");
    CHECK(r == 0, "already set: reports no change");
    // 3. absent: appended
    strcpy_s(t, "seta r_gamma \"0.85\"\n");
    r = CodvrSetCvarLine(t, sizeof(t), "cg_drawCrosshair", "1");
    CHECK(r == 1 && !strcmp(t, "seta r_gamma \"0.85\"\nseta cg_drawCrosshair \"1\"\n"), "absent: appended on its own line");
    // 4. a bare `set` form and duplicates collapse to one line; a longer name is not it
    strcpy_s(t, "set cg_drawCrosshair 1\nseta cg_drawCrosshairAlpha \"1.0\"\nseta cg_drawCrosshair \"1\"\n");
    r = CodvrSetCvarLine(t, sizeof(t), "cg_drawCrosshair", "0");
    CHECK(r == 1 && !strcmp(t, "seta cg_drawCrosshair \"0\"\nseta cg_drawCrosshairAlpha \"1.0\"\n"),
          "set/seta forms found, duplicate dropped, cg_drawCrosshairAlpha left alone (got: %s)", t);
    // 5. no room -> refused, text untouched
    char tiny[24]; strcpy_s(tiny, "seta x \"1\"\n");
    r = CodvrSetCvarLine(tiny, sizeof(tiny), "cg_drawCrosshair", "0");
    CHECK(r == -1 && !strcmp(tiny, "seta x \"1\"\n"), "no room: refused and untouched");
}

static void S_Brightness()
{
    printf("--- brightness: a pure gain on the finished eye, vignette off, state restored ---\n");
    if (!tg_getIntegerv || !tg_getFloatv || !tg_isEnabled) { printf("  (no GL introspection; skipping)\n"); return; }
    ResetAll();
    SetMockHead(0, 0, 0, 0);
    ClearBackBuffer(1.0f, 0.5f, 0.0f);   // red 255, green ~128
    CHECK(BringUp() && VR_BeginFrameEx() == VR_FRAME_RENDER, "session up");
    DriveEye(0);
    Comfort_TestSetVignette(0.0f);
    Comfort_SetBrightness(1.0f);
    VR_SubmitFrame(640, 480, true, 0);
    const int cx = 3072 / 2, cy = 3264 / 2;
    const int full = VR_TestSampleEyeTexture(0, cx, cy);
    CHECK(full > 240, "brightness 1.0: the scene is untouched (r=%d)", full);
    // dim to 50 %: red halves, the corner too (no vignette involved)
    Comfort_SetBrightness(0.5f);
    GLSnap before; TakeSnap(&before);
    CHECK(Comfort_DrawVignette(0), "the pass draws with the vignette fully open");
    GLSnap after; TakeSnap(&after);
    char diff[64] = "";
    const int bad = CompareSnap(before, after, diff, sizeof(diff));
    CHECK(bad == 0, "every saved GL state restored exactly (%d differ%s%s)", bad, bad ? ", e.g. " : "", diff);
    const int half = VR_TestSampleEyeTexture(0, cx, cy);
    const int corner = VR_TestSampleEyeTexture(0, 10, 10);
    CHECK(half > 115 && half < 140, "brightness 0.5: centre red is halved (r=%d)", half);
    CHECK(corner > 115 && corner < 140, "brightness 0.5: the corner is halved too, no tunnel (r=%d)", corner);
    // a second application multiplies again (proves it is a gain, not a set)
    CHECK(Comfort_DrawVignette(0), "second pass");
    const int quarter = VR_TestSampleEyeTexture(0, cx, cy);
    CHECK(quarter > 52 && quarter < 76, "applied twice: a quarter (r=%d) -- multiplicative, black stays black", quarter);
    Comfort_SetBrightness(1.0f);
    CHECK(Comfort_Brightness() == 1.0f, "back to 1.0");
    Comfort_SetBrightness(0.0f);
    CHECK(Comfort_Brightness() >= 0.1f, "clamped: never fully black (%.2f)", Comfort_Brightness());
    Comfort_SetBrightness(1.0f);
}

// Records one THRig scene the way THPair does: the merged hand batch (split
// into its RIGHT and LEFT halves by the proxy), then the gun. Hands are told
// apart by which delta lands the authored LEFT palm on the LEFT class's
// applied anchor for this eye -- never by draw order.
static int PRScene(THRig* r, float leftM[16], float rightM[16], float gunM[16])
{
    NHScene(0);
    NHBuildHandVerts(r->rAxis0, r->rPos0, r->lAxis0, r->lPos0);
    RecReset();
    NHDriveHandDraw();
    const int handDraws = CountKind(REC_LOADMV) / 2;
    if (handDraws != 2) return 0;
    GLSurfEyeApply aL;
    if (!GLSurf_GetEyeApply(GLSC_HAND_L, 0, &aL) || !aL.applied) return 0;
    int li = -1;
    for (int i = 0; i < 2; ++i) {
        const GLRec* m = NthKind(REC_LOADMV, 2 * i);
        if (!m) return 0;
        float p[3]; HpXform4(m->m, r->lPos0, p);
        if (HpDist(p, aL.finalPos) < 1e-2f) li = i;
    }
    if (li < 0) return 0;
    memcpy(leftM, NthKind(REC_LOADMV, 2 * li)->m, 16 * sizeof(float));
    memcpy(rightM, NthKind(REC_LOADMV, 2 * (1 - li))->m, 16 * sizeof(float));
    RecReset();
    NHDriveGunDraw(r->gun, r->gAxis0, r->gPos0);
    const GLRec* g = NthKind(REC_LOADMV, 0);
    if (!g) return 0;
    memcpy(gunM, g->m, 16 * sizeof(float));
    VRNH_OnPairBegin(++r->serial, &r->in, r->headPos0, r->headAxisI, 1, r->now);
    r->now += 0.011;
    return 1;
}
static float PRMatDiff(const float a[16], const float b[16])
{
    float m = 0; for (int i = 0; i < 16; ++i) { const float d = fabsf(a[i] - b[i]); if (d > m) m = d; }
    return m;
}

static void S_SupportHandRidesGun()
{
    printf("--- support pose lock: the LEFT hand rides the weapon's rigid transform while attached ---\n");
    THRig r;
    if (!THBringUp(&r)) { CHECK(0, "two-hand bring-up"); return; }
    VRNH_SetSupportPoseLock(1);
    float sup[3], bore[3], up[3];
    if (!THSupportEyeish(&r, sup, bore, up)) { CHECK(0, "support point"); return; }
    float L0[16], R0[16], G0[16];
    CHECK(PRScene(&r, L0, R0, G0), "free scene recorded");
    VRNHDebug d; VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_LEFT_FREE && !d.gun.leftRidesGun, "free: the ride is off");
    CHECK(PRMatDiff(L0, G0) > 0.05f, "free: the left hand's delta is its own, not the weapon's (%.3f)", PRMatDiff(L0, G0));

    // Attach: the left controller on the support point, squeezing.
    THPlaceLeft(&r, sup, 1);
    for (int i = 0; i < 30; ++i) THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED && d.gun.leftRidesGun, "attached: the ride is on");
    float L1[16], R1[16], G1[16];
    CHECK(PRScene(&r, L1, R1, G1), "attached scene recorded");
    CHECK(PRMatDiff(L1, G1) < 2e-3f,
          "attached: the LEFT hand's delta IS the weapon's rigid transform (max diff %.1e) -- no clipping possible", PRMatDiff(L1, G1));
    // The published left pose is the authored left palm carried by the weapon.
    {
        float want[3]; HpXform4(G1, r.lPos0, want);
        const float* got = VRObj_Set()->hand[VROBJ_HAND_L].pos;
        CHECK(HpDist(want, got) < 1e-2f, "attached: the published LEFT pose is the authored palm on the gun (%.4f u)", HpDist(want, got));
    }
    // The lock changes nothing else: weapon and RIGHT hand are byte-stable.
    VRNH_SetSupportPoseLock(0);
    THPair(&r);
    float L2[16], R2[16], G2[16];
    CHECK(PRScene(&r, L2, R2, G2), "attached, lock off, recorded");
    CHECK(PRMatDiff(G1, G2) < 1e-5f, "lock on/off: the weapon's delta is byte-stable (%.1e)", PRMatDiff(G1, G2));
    CHECK(PRMatDiff(R1, R2) < 1e-5f, "lock on/off: the RIGHT hand's delta is byte-stable (%.1e)", PRMatDiff(R1, R2));
    VRNH_GetDebug(&d);
    CHECK(!d.gun.leftRidesGun && PRMatDiff(L2, G2) > 0.05f,
          "lock off: the left hand is back on its controller (%.3f from the weapon's transform)", PRMatDiff(L2, G2));
    VRNH_SetSupportPoseLock(1);
    THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.leftRidesGun, "lock on again: rides");
    // Release: squeeze off -> detached -> ride off within the pair.
    THPlaceLeft(&r, sup, 0);
    THPair(&r); THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState != VRNH_2H_ATTACHED && !d.gun.leftRidesGun, "released: the ride is off (state %d)", d.gun.twoHandState);
    float L3[16], R3[16], G3[16];
    CHECK(PRScene(&r, L3, R3, G3), "released scene recorded");
    CHECK(PRMatDiff(L3, G3) > 0.05f, "released: the left hand follows its controller again (%.3f)", PRMatDiff(L3, G3));
    VRNH_TestReset();
}

static void S_TriggerHandRidesGun()
{
    printf("--- trigger hand lock: the RIGHT hand rides the weapon as applied; the weapon and aim never see it ---\n");
    THRig r;
    if (!THBringUp(&r)) { CHECK(0, "two-hand bring-up"); return; }
    VRNH_SetSupportPoseLock(1);
    VRNH_SetTriggerHandLock(1);
    float sup[3], bore[3], up[3];
    if (!THSupportEyeish(&r, sup, bore, up)) { CHECK(0, "support point"); return; }
    // One-handed: the weapon hangs off the palm by the authored relation, so
    // the ridden hand IS the palm target -- a no-op, proven, not assumed.
    float L0[16], R0[16], G0[16];
    CHECK(PRScene(&r, L0, R0, G0), "one-handed scene recorded (lock on)");
    VRNHDebug d; VRNH_GetDebug(&d);
    CHECK(d.gun.rightRidesGun, "one-handed: the right hand rides");
    CHECK(PRMatDiff(R0, G0) < 2e-3f, "one-handed: right hand delta == weapon delta (%.1e)", PRMatDiff(R0, G0));
    VRNH_SetTriggerHandLock(0);
    THPair(&r);
    float L1[16], R1[16], G1[16];
    CHECK(PRScene(&r, L1, R1, G1), "one-handed scene recorded (lock off)");
    VRNH_GetDebug(&d);
    CHECK(!d.gun.rightRidesGun, "lock off: no ride");
    CHECK(PRMatDiff(R0, R1) < 2e-3f, "one-handed: lock on/off draws the right hand in the same place (%.1e) -- consistent relation", PRMatDiff(R0, R1));
    CHECK(PRMatDiff(G0, G1) < 1e-5f, "one-handed: the weapon is byte-stable across the lock (%.1e)", PRMatDiff(G0, G1));

    // Two-handed: the bar re-aims the weapon away from the palm's own basis.
    // Without the lock the right hand stays on the controller and leaves the
    // grip; with it the hand follows the weapon exactly.
    THPlaceLeft(&r, sup, 1);
    for (int i = 0; i < 30; ++i) THPair(&r);
    // rotate the left hand 10 deg about the rear grip so the bar axis differs from the palm axis
    VRNH_GetDebug(&d);
    {
        const float rg[3] = { d.gun.desiredGripEyeish[0], d.gun.desiredGripEyeish[1], d.gun.desiredGripEyeish[2] };
        float v[3]; for (int k = 0; k < 3; ++k) v[k] = sup[k] - rg[k];
        const float vl = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        const float c = cosf(10.0f * 0.0174533f), s = sinf(10.0f * 0.0174533f);
        float vDir[3]; for (int k = 0; k < 3; ++k) vDir[k] = v[k] / vl;
        float w[3]; { const float dd = up[0]*vDir[0] + up[1]*vDir[1] + up[2]*vDir[2]; for (int k = 0; k < 3; ++k) w[k] = up[k] - dd * vDir[k]; }
        { const float wl = sqrtf(w[0]*w[0] + w[1]*w[1] + w[2]*w[2]); for (int k = 0; k < 3; ++k) w[k] /= wl; }
        float p2[3]; for (int k = 0; k < 3; ++k) p2[k] = rg[k] + vl * (c * vDir[k] + s * w[k]);
        THPlaceLeft(&r, p2, 1);
    }
    for (int i = 0; i < 30; ++i) THPair(&r);
    VRNH_GetDebug(&d);
    CHECK(d.gun.twoHandState == VRNH_2H_ATTACHED, "attached after the bar step");
    float barrelOff[3]; memcpy(barrelOff, d.gun.barrelRoom, sizeof(barrelOff));
    float L2[16], R2[16], G2[16];
    CHECK(PRScene(&r, L2, R2, G2), "two-handed scene recorded (lock off)");
    const float offGap = PRMatDiff(R2, G2);
    CHECK(offGap > 0.02f, "lock off: the right hand's delta differs from the re-aimed weapon's (%.3f) -- it would leave the grip", offGap);
    VRNH_SetTriggerHandLock(1);
    THPair(&r);
    VRNH_GetDebug(&d);
    float L3[16], R3[16], G3[16];
    CHECK(PRScene(&r, L3, R3, G3), "two-handed scene recorded (lock on)");
    CHECK(d.gun.rightRidesGun && PRMatDiff(R3, G3) < 2e-3f,
          "lock on: the right hand rides the re-aimed weapon exactly (%.1e)", PRMatDiff(R3, G3));
    CHECK(PRMatDiff(G2, G3) < 1e-5f, "the weapon is byte-stable across the lock while two-handed (%.1e)", PRMatDiff(G2, G3));
    VRNH_GetDebug(&d);
    CHECK(HpDist(barrelOff, d.gun.barrelRoom) < 1e-5f, "the fire direction is byte-stable across the lock");
    CHECK(d.gun.leftRidesGun && PRMatDiff(L3, G3) < 2e-3f, "...and the left hand rides too (%.1e)", PRMatDiff(L3, G3));
    // Ten more pairs: the ridden hand does not drift or feed back into the gun.
    float G4[16], R4[16], L4[16];
    for (int i = 0; i < 10; ++i) THPair(&r);
    CHECK(PRScene(&r, L4, R4, G4), "ten pairs later");
    CHECK(PRMatDiff(G3, G4) < 1e-4f && PRMatDiff(R4, G4) < 2e-3f, "no feedback: weapon steady (%.1e), hand still on it (%.1e)", PRMatDiff(G3, G4), PRMatDiff(R4, G4));
    VRNH_TestReset();
}
