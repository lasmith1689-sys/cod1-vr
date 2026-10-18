// ===========================================================================
// SURFACE ORDER, ONE VISUAL OWNER, AND THE ANATOMICAL PALM CALIBRATION
// (project-lead brief 2026-09-03, sections 2-3, 6-9)
// ===========================================================================
//
// docs/FLICKER-ROOT-CAUSE-2026-09-03.md: the engine packs the two hand
// surfaces of the merged batch in EITHER order from frame to frame. Every
// case here drives the real production draw path (GLSurf_OnDrawElements)
// with batches laid out both ways and asks the layer what it did.

// The synthetic merged batch with the halves EXCHANGED: the RIGHT landmarks
// land at index - 837 (first half), the LEFT at index + 837 (second half) --
// exactly the layout the 2026-09-03 log's raw sample showed (vertex 909 held
// what vertex 72 holds in the file order).
static void SOBuildSwappedHandVerts(const float rightAxis[9], const float rightPos[3],
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
        float* v = &g_verts[(kNHRight[i].index - GLS_HAND_VERTS) * 3];
        v[0] = rightPos[0] + r[0]; v[1] = rightPos[1] + r[1]; v[2] = rightPos[2] + r[2];
    }
    for (int i = 0; i < NH_N; ++i) {
        const float bindU[3] = { kNHLeft[i].bindM[0] * CODVR_UNITS_PER_M,
                                 kNHLeft[i].bindM[1] * CODVR_UNITS_PER_M,
                                 kNHLeft[i].bindM[2] * CODVR_UNITS_PER_M };
        float r[3];
        HpApply(leftAxis, bindU, r);
        float* v = &g_verts[(kNHLeft[i].index + GLS_HAND_VERTS) * 3];
        v[0] = leftPos[0] + r[0]; v[1] = leftPos[1] + r[1]; v[2] = leftPos[2] + r[2];
    }
}

static void SOBuildHandVerts(int swapped, const float rA[9], const float rP[3],
                             const float lA[9], const float lP[3])
{
    if (swapped) SOBuildSwappedHandVerts(rA, rP, lA, lP);
    else         NHBuildHandVerts(rA, rP, lA, lP);
}

// One half of the merged batch as its own draw (the 3912-index single draws
// the log shows): half 0 = indices [0,837), half 1 = [837,1674).
static void SODriveHalfDraw(int half)
{
    GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES, GLS_GL_UNSIGNED_INT,
                          g_handIdx + (half ? GLS_HAND_INDICES : 0));
}

// Sleeve vertices in their own array: 120 per arm, each arm's cuff placed
// within 1.5 in of its palm and the rest trailing 6 in behind it. `swapped`
// puts the LEFT arm's vertices in the FIRST half.
static float g_soSleeveVerts[GLS_SLEEVE_VERTS * 2 * 3];
static void SOBuildSleeveVerts(int swapped, const float rPalm[3], const float lPalm[3])
{
    for (int arm = 0; arm < 2; ++arm) {
        const int first = (arm == 0) != (swapped != 0) ? 0 : GLS_SLEEVE_VERTS;
        const float* palm = (arm == 0) ? rPalm : lPalm;
        for (int i = 0; i < GLS_SLEEVE_VERTS; ++i) {
            const float t = (float)i / (float)(GLS_SLEEVE_VERTS - 1);   // 0 cuff .. 1 elbow
            float* v = &g_soSleeveVerts[(first + i) * 3];
            v[0] = palm[0] - 1.0f - 6.0f * t;
            v[1] = palm[1] + 0.5f * sinf(i * 0.7f);
            v[2] = palm[2] - 0.5f - 0.5f * cosf(i * 0.9f);
        }
    }
}

static void SODriveSleeveDraw()
{
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_soSleeveVerts);
    GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_SLEEVE_INDICES * 2,
                          GLS_GL_UNSIGNED_INT, g_sleeveIdx);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_verts);
}

static int SOMatEq(const float* a, const float* b, float tol)
{
    for (int i = 0; i < 16; ++i)
        if (fabsf(a[i] - b[i]) > tol) return 0;
    return 1;
}

// ===========================================================================
static void S_SurfaceOrderContract()
{
    printf("--- surface order: hand identity from content, never from draw order ---\n");

    float rAxis[9], lAxis[9];
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis);
    const float rPos[3] = { 6.0f, -2.0f, -12.0f };
    const float lPos[3] = { -6.0f, -2.0f, -12.0f };

    // ---- A: the SWAPPED batch solves, and says so -----------------------
    {
        NHResetForCase();
        GLSurf_SetHandSolveWanted(GLSC_HAND_R, 1);
        GLSurf_SetHandSolveWanted(GLSC_HAND_L, 1);
        SOBuildSwappedHandVerts(rAxis, rPos, lAxis, lPos);
        NHDriveHandDraw();
        GLSurfHandSolve hr, hl;
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
        CHECK(hr.solves == 1 && hl.solves == 1 && hr.refusals == 0 && hl.refusals == 0,
              "A: swapped halves -> BOTH hands solve, zero refusals (R %u/%u L %u/%u)",
              hr.solves, hr.refusals, hl.solves, hl.refusals);
        CHECK(hr.lastSwapped == 1 && hl.lastSwapped == 1
              && hr.swappedSolves == 1 && hl.swappedSolves == 1,
              "A: ...and both report the SWAPPED order");
        CHECK(HpDist(hr.submPos, rPos) < 1e-2f && HpDist(hl.submPos, lPos) < 1e-2f,
              "A: the recovered palms are the RIGHT hand at (%.1f %.1f %.1f) and the "
              "LEFT at (%.1f %.1f %.1f) -- laterality follows the content",
              hr.submPos[0], hr.submPos[1], hr.submPos[2],
              hl.submPos[0], hl.submPos[1], hl.submPos[2]);
        GLSurfPalmTry pt; int n = 0, sw = 0;
        while (GLSurf_PopPalmTry(&pt)) { ++n; if (pt.ok && pt.swapped) ++sw; }
        CHECK(n == 2 && sw == 2, "A: palmtry records both attempts as PASS/swapped");
        GLSurfOwnerStats os; GLSurf_GetOwnerStats(&os);
        CHECK(os.orderSwapped == 0 && os.orderUnknown == 0,
              "A: an unsplit batch (no targets) records no order verdict yet");
    }

    // ---- B: MUTANT -- the pre-2026-09-03 solver (file order only) --------
    {
        NHResetForCase();
        GLSurf_SetHandSolveWanted(GLSC_HAND_R, 1);
        GLSurf_SetHandSolveWanted(GLSC_HAND_L, 1);
        SOBuildSwappedHandVerts(rAxis, rPos, lAxis, lPos);
        VRHandPose_TestSetSwapAllowed(0);
        NHDriveHandDraw();
        VRHandPose_TestSetSwapAllowed(1);
        GLSurfHandSolve hr, hl;
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
        CHECK(hr.solves == 0 && hl.solves == 0 && hr.refusedSolve == 1 && hl.refusedSolve == 1
              && hr.lastRefSite == 3 && hr.lastRefReason == VRHP_REASON_RMS,
              "B/MUTANT: a file-order-only solver REFUSES the swapped batch at site 3 "
              "LANDMARK_RMS (best rms %.4f m) -- the hardware fingerprint",
              hr.lastRefBestRmsM);
        CHECK(hr.lastRefBestRmsM > 0.01f,
              "B/MUTANT: the wrong-hand residual is centimetres, not a gate margin");
        // The same batch, file order: solves either way.
        NHResetForCase();
        GLSurf_SetHandSolveWanted(GLSC_HAND_R, 1);
        GLSurf_SetHandSolveWanted(GLSC_HAND_L, 1);
        NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
        NHDriveHandDraw();
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        CHECK(hr.solves == 1 && hr.lastSwapped == 0 && hr.swappedSolves == 0,
              "B: the file-order batch solves in the file order");
    }

    // ---- C: ENGAGED, the order alternating every pair -------------------
    //
    // The production path: targets installed, the merged batch split, each
    // half solved and transformed. Whatever order the engine packs, the
    // RIGHT controller's target must land on the RIGHT hand's shell and the
    // pair verdict must be OK with both eyes applied for both hands.
    {
        NHResetForCase();
        VRNH_SetEnabled(1);
        VRNH_SetTwoHandEnabled(0);
        VRNH_SetReloadEnabled(0);
        VRNH_SetCalibrationTable(1);              // the production calibration
        const float headPos0[3] = { 0, 0, 0 };
        float headAxisI[9]; NHIdentityAxis(headAxisI);
        const float qI[4] = { 0, 0, 0, 1 };
        const float gripR[3] = { 0.20f, -0.10f, -0.35f };
        const float gripL[3] = { -0.20f, -0.10f, -0.35f };
        VRInputState in; memset(&in, 0, sizeof(in));
        in.hand[VRI_RIGHT] = NHHand(1, gripR, qI);
        in.hand[VRI_LEFT]  = NHHand(1, gripL, qI);
        unsigned serial = 10;
        // Seed and engage on file-order pairs.
        for (int p = 0; p < 3; ++p) {
            VRNH_OnPairBegin(++serial, &in, headPos0, headAxisI, 1, p * 0.01);
            for (int eye = 0; eye < 2; ++eye) {
                NHScene(eye);
                NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
                NHDriveHandDraw();
            }
        }
        VRNHDebug d; VRNH_GetDebug(&d);
        CHECK(d.hand[VRI_RIGHT].engaged && d.hand[VRI_LEFT].engaged,
              "C: both hands engaged on file-order pairs");
        GLSurfOwnerStats os0; GLSurf_GetOwnerStats(&os0);

        int okPairs = 0, bothApplied = 0, rOnTarget = 0, lOnTarget = 0;
        const int kPairs = 12;
        for (int p = 0; p < kPairs; ++p) {
            const int swapped = (p % 3 != 0);          // ~2/3 swapped, mixed within pairs too
            const int swappedEye1 = ((p % 5) == 2) ? !swapped : swapped;
            VRNH_OnPairBegin(++serial, &in, headPos0, headAxisI, 1, 1.0 + p * 0.01);
            VRNH_GetDebug(&d);
            for (int eye = 0; eye < 2; ++eye) {
                NHScene(eye);
                SOBuildHandVerts(eye ? swappedEye1 : swapped, rAxis, rPos, lAxis, lPos);
                NHDriveHandDraw();
            }
            VRNHPairVerdict pv;
            const int ok = VRNH_PairPreSubmit(serial, &pv);
            if (ok) ++okPairs;
            const VRNHObjVerdict& r = pv.obj[VRNH_OBJ_R];
            const VRNHObjVerdict& l = pv.obj[VRNH_OBJ_L];
            if (r.applied[0] && r.applied[1] && l.applied[0] && l.applied[1]) ++bothApplied;
            // The finals sit on the controller targets (monoscopic wrapper:
            // zero eye offsets, so eye space == the midpoint frame).
            GLSurfEyeApply aR, aL;
            GLSurf_GetEyeApply(GLSC_HAND_R, 1, &aR);
            GLSurf_GetEyeApply(GLSC_HAND_L, 1, &aL);
            if (aR.applied && HpDist(aR.finalPos, d.hand[VRI_RIGHT].targetEyeish) < 0.05f) ++rOnTarget;
            if (aL.applied && HpDist(aL.finalPos, d.hand[VRI_LEFT].targetEyeish) < 0.05f) ++lOnTarget;
        }
        GLSurfOwnerStats os; GLSurf_GetOwnerStats(&os);
        VRNHPairStats ps; VRNH_GetPairStats(&ps);
        CHECK(okPairs == kPairs && bothApplied == kPairs,
              "C: every pair OK with BOTH hands applied in BOTH eyes across %d pairs "
              "of alternating/mixed surface order (ok %d, bothApplied %d)",
              kPairs, okPairs, bothApplied);
        CHECK(rOnTarget == kPairs && lOnTarget == kPairs,
              "C: the RIGHT hand lands on the RIGHT controller and the LEFT on the "
              "LEFT in every pair (%d / %d)", rOnTarget, lOnTarget);
        CHECK(os.orderSwapped - os0.orderSwapped > 0 && os.orderNormal - os0.orderNormal > 0
              && os.orderUnknown == os0.orderUnknown,
              "C: the order was resolved from content every time (normal %u, "
              "swapped %u, unknown %u)", os.orderNormal - os0.orderNormal,
              os.orderSwapped - os0.orderSwapped, os.orderUnknown - os0.orderUnknown);
        CHECK(os.primaryVanillaFallbackDraw == 0 && os.duplicateOwnerPrevented == 0
              && ps.rejected == 0,
              "C: zero vanilla fallbacks, zero duplicate owners, zero refused pairs");
        GLSurfHandSolve hr, hl;
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
        CHECK(hr.refusals == 0 && hl.refusals == 0,
              "C: zero solve refusals on real hand draws (R %u L %u)", hr.refusals, hl.refusals);
        VRNH_TestReset();
    }

    // ---- D: single-surface hand draws, either half, either order --------
    {
        NHResetForCase();
        GLSurf_SetHandSolveWanted(GLSC_HAND_R, 1);
        GLSurf_SetHandSolveWanted(GLSC_HAND_L, 1);
        int cls[2][2];
        for (int sw = 0; sw < 2; ++sw) {
            SOBuildHandVerts(sw, rAxis, rPos, lAxis, lPos);
            for (int half = 0; half < 2; ++half) {
                NHScene(0);
                SODriveHalfDraw(half);
                cls[sw][half] = GLSurf_TestClassOfLastDraw();
            }
        }
        CHECK(cls[0][0] == GLSC_HAND_L && cls[0][1] == GLSC_HAND_R,
              "D: file order singles: first half is LEFT, second is RIGHT");
        CHECK(cls[1][0] == GLSC_HAND_R && cls[1][1] == GLSC_HAND_L,
              "D: swapped singles: first half is RIGHT, second is LEFT -- identity from content");
        GLSurfOwnerStats os; GLSurf_GetOwnerStats(&os);
        CHECK(os.singleByContent == 4 && os.singleByOrdinal == 0,
              "D: all four single draws identified by content (%u/%u)",
              os.singleByContent, os.singleByOrdinal);
        GLSurfHandSolve hr, hl;
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
        CHECK(hr.solves == 2 && hl.solves == 2 && hr.refusals == 0 && hl.refusals == 0,
              "D: each single draw solved under its own hand (R %u/%u L %u/%u)",
              hr.solves, hr.refusals, hl.solves, hl.refusals);
    }

    // ---- E: sleeves follow the palm their cuff is nearest ---------------
    {
        NHResetForCase();
        VRNH_SetEnabled(1);
        VRNH_SetTwoHandEnabled(0);
        VRNH_SetReloadEnabled(0);
        const float headPos0[3] = { 0, 0, 0 };
        float headAxisI[9]; NHIdentityAxis(headAxisI);
        const float qI[4] = { 0, 0, 0, 1 };
        const float gripR[3] = { 0.20f, -0.10f, -0.35f };
        const float gripL[3] = { -0.20f, -0.10f, -0.35f };
        VRInputState in; memset(&in, 0, sizeof(in));
        in.hand[VRI_RIGHT] = NHHand(1, gripR, qI);
        in.hand[VRI_LEFT]  = NHHand(1, gripL, qI);
        unsigned serial = 40;
        for (int p = 0; p < 3; ++p) {
            VRNH_OnPairBegin(++serial, &in, headPos0, headAxisI, 1, p * 0.01);
            for (int eye = 0; eye < 2; ++eye) {
                NHScene(eye);
                NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
                NHDriveHandDraw();
            }
        }
        int matched = 0, total = 0;
        GLSurf_SetSleeveVisible(2);     // this case is about attribution, not policy
        for (int sw = 0; sw < 2; ++sw) {
            VRNH_OnPairBegin(++serial, &in, headPos0, headAxisI, 1, 1.0 + sw * 0.01);
            for (int eye = 0; eye < 2; ++eye) {
                NHScene(eye);
                NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
                RecReset();
                NHDriveHandDraw();
                // File-order hand batch: loads are [L, restore, R, restore].
                // Copied out: the recorder ring is reused by the next draw.
                float mL[16], mR[16];
                const GLRec* lL = NthKind(REC_LOADMV, 0);
                const GLRec* lR = NthKind(REC_LOADMV, 2);
                const int haveHands = (lL && lR);
                if (lL) memcpy(mL, lL->m, sizeof(mL));
                if (lR) memcpy(mR, lR->m, sizeof(mR));
                SOBuildSleeveVerts(sw, rPos, lPos);
                float dR = -1, dL = -1;
                GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_soSleeveVerts);
                const int have = GLSurf_TestSleeveNearest(GLS_GL_UNSIGNED_INT, g_sleeveIdx,
                                                          GLS_SLEEVE_INDICES, &dR, &dL);
                GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_verts);
                RecReset();
                SODriveSleeveDraw();
                const GLRec* s0 = NthKind(REC_LOADMV, 0);   // first sleeve half
                const GLRec* s1 = NthKind(REC_LOADMV, 2);   // second sleeve half
                ++total;
                int ok = 0;
                if (haveHands && s0 && s1) {
                    // sw=0: first half is the RIGHT arm -> the R hand's matrix.
                    const float* wantFirst  = sw ? mL : mR;
                    const float* wantSecond = sw ? mR : mL;
                    ok = SOMatEq(s0->m, wantFirst, 1e-4f) && SOMatEq(s1->m, wantSecond, 1e-4f);
                }
                if (ok) ++matched;
                else printf("      E sw=%d eye=%d: have=%d dR=%.2f dL=%.2f hands=%d sleeveLoads=%d\n",
                            sw, eye, have, dR, dL, haveHands, CountKind(REC_LOADMV));
            }
        }
        GLSurfOwnerStats os; GLSurf_GetOwnerStats(&os);
        CHECK(matched == total && total == 4,
              "E: each sleeve half took ITS OWN arm's delta in both sleeve orders "
              "(%d/%d eye-scenes)", matched, total);
        CHECK(os.sleeveByProximity >= 4 && os.sleeveByOrdinal == 0,
              "E: sleeves attributed by cuff-to-palm proximity, never by ordinal (%u/%u)",
              os.sleeveByProximity, os.sleeveByOrdinal);
        VRNH_TestReset();
    }

    // ---- G: ONE VISUAL OWNER within an eye-scene ------------------------
    {
        NHResetForCase();
        VRNH_SetEnabled(1);
        VRNH_SetTwoHandEnabled(0);
        VRNH_SetReloadEnabled(0);
        const float headPos0[3] = { 0, 0, 0 };
        float headAxisI[9]; NHIdentityAxis(headAxisI);
        const float qI[4] = { 0, 0, 0, 1 };
        const float gripR[3] = { 0.20f, -0.10f, -0.35f };
        const float gripL[3] = { -0.20f, -0.10f, -0.35f };
        VRInputState in; memset(&in, 0, sizeof(in));
        in.hand[VRI_RIGHT] = NHHand(1, gripR, qI);
        in.hand[VRI_LEFT]  = NHHand(1, gripL, qI);
        unsigned serial = 60;
        for (int p = 0; p < 3; ++p) {
            VRNH_OnPairBegin(++serial, &in, headPos0, headAxisI, 1, p * 0.01);
            for (int eye = 0; eye < 2; ++eye) {
                NHScene(eye);
                NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
                NHDriveHandDraw();
            }
        }
        // G1: native first, then a copy that cannot be recovered: the copy
        // is NOT drawn; the eye's record stays native.
        VRNH_OnPairBegin(++serial, &in, headPos0, headAxisI, 1, 1.0);
        GLSurfOwnerStats os0; GLSurf_GetOwnerStats(&os0);
        for (int eye = 0; eye < 2; ++eye) {
            NHScene(eye);
            NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
            NHDriveHandDraw();
            for (int i = 0; i < 4096 * 3; ++i) g_verts[i] = 12345.0f;
            RecReset();
            NHDriveHandDraw();
            CHECK(CountKind(REC_DRAW) == 0,
                  "G1/eye%d: the unrecoverable second copy after a native draw is NOT drawn", eye);
        }
        GLSurfOwnerStats os1; GLSurf_GetOwnerStats(&os1);
        VRNHPairVerdict pv;
        CHECK(VRNH_PairPreSubmit(serial, &pv) == 1
              && pv.obj[VRNH_OBJ_R].applied[0] && pv.obj[VRNH_OBJ_R].applied[1],
              "G1: the pair stays native on both eyes (verdict %s)", VRNH_VerdictName(pv.reason));
        CHECK(os1.duplicateOwnerPrevented - os0.duplicateOwnerPrevented == 4,
              "G1: four would-be second owners prevented (%u)",
              os1.duplicateOwnerPrevented - os0.duplicateOwnerPrevented);
        // G2: vanilla first (unrecoverable), then a recoverable copy: the
        // copy renders VANILLA, the record says vanilla, the pair is judged.
        VRNH_OnPairBegin(++serial, &in, headPos0, headAxisI, 1, 1.02);
        for (int eye = 0; eye < 2; ++eye) {
            NHScene(eye);
            for (int i = 0; i < 4096 * 3; ++i) g_verts[i] = 12345.0f;
            NHDriveHandDraw();
            NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
            RecReset();
            NHDriveHandDraw();
            CHECK(CountKind(REC_LOADMV) == 0 && CountKind(REC_DRAW) >= 1,
                  "G2/eye%d: a recoverable copy after a vanilla draw renders VANILLA "
                  "(no matrix load), not as a second, native owner", eye);
            GLSurfEyeApply a; GLSurf_GetEyeApply(GLSC_HAND_R, eye, &a);
            CHECK(a.drew && !a.applied, "G2/eye%d: the eye's record says vanilla", eye);
        }
        GLSurfOwnerStats os2; GLSurf_GetOwnerStats(&os2);
        CHECK(os2.duplicateOwnerPrevented - os1.duplicateOwnerPrevented == 4,
              "G2: four more prevented (%u)", os2.duplicateOwnerPrevented - os1.duplicateOwnerPrevented);
        CHECK(VRNH_PairPreSubmit(serial, &pv) == 0 && pv.reason == VRNH_VERDICT_VANILLA_FALLBACK,
              "G2: an engaged pair that both eyes could only render vanilla is refused "
              "as %s -- never shown", VRNH_VerdictName(pv.reason));
        VRNH_TestReset();
    }

    VRHandPose_TestRestore();
    ResetLayerForCase();
}

// ===========================================================================
// THE ANATOMICAL PALM CALIBRATION FIXTURE (brief sections 7-8)
// ===========================================================================
//
// Grip frames are stated as the OpenXR axes (X, Y, Z) of the grip in the
// room, Quake convention (+X fwd, +Y left, +Z up). The runtime reads the
// grip as Quake rows (fwd, left, up) = (-Z, -X, +Y), which is what
// VRM_QuatToQuakeAxis produces. Expected anatomy: knuckles k = +Y, thumb
// side t = +Z, palm-out n = handSign * X (RIGHT -1, LEFT +1).
struct SOGripPose {
    const char* name;
    float X[3], Y[3], Z[3];
    float pos[3];
};

static void SOGripRows(const SOGripPose& g, float rows[9])
{
    for (int k = 0; k < 3; ++k) {
        rows[0 + k] = -g.Z[k];     // fwd  = -Z
        rows[3 + k] = -g.X[k];     // left = -X
        rows[6 + k] =  g.Y[k];     // up   = +Y
    }
}

static float SODot(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

static void S_PalmCalibrationFixture()
{
    printf("--- palm calibration fixture: grip -> anatomical palm, both hands ---\n");
    VRNH_TestReset();
    VRNH_SetCalibrationTable(1);

    // Representative physical poses (brief section 8), right-handed sets.
    // A: pistol hold, straight forward, neutral wrist: Y fwd, Z up, X right.
    // B: palm-down: X up (into a right palm facing down), Y fwd, Z left.
    // C: palm-up: X down, Y fwd, Z right.
    // D: wrist rolled 45 deg from A about the forward axis.
    // E: pointing vertically up: Y up, Z back, X right.
    // F: A translated 30 cm forward, 10 cm left, 10 cm down.
    const float c45 = 0.70710678f;
    const SOGripPose poses[6] = {
        { "A neutral fwd", { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0, 0 } },
        { "B palm-down",   { 0, 0, 1 },  { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 0 } },
        { "C palm-up",     { 0, 0, -1 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 0, 0 } },
        { "D roll 45",     { 0, -c45, c45 }, { 1, 0, 0 }, { 0, c45, c45 }, { 0, 0, 0 } },
        { "E vertical up", { 0, -1, 0 }, { 0, 0, 1 }, { -1, 0, 0 }, { 0, 0, 0 } },
        { "F translated",  { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0.30f * 39.37f, 0.10f * 39.37f, -0.10f * 39.37f } },
    };
    float rowsCal[VRI_HANDS][9], offCal[VRI_HANDS][3];
    for (int h = 0; h < VRI_HANDS; ++h) {
        const int have = VRNH_GetEffectiveCalibration(h, -1, rowsCal[h], offCal[h]);
        const float* r = rowsCal[h];
        const float det = r[0]*(r[4]*r[8]-r[5]*r[7]) - r[1]*(r[3]*r[8]-r[5]*r[6]) + r[2]*(r[3]*r[7]-r[4]*r[6]);
        CHECK(have && fabsf(det - 1.0f) < 1e-3f,
              "cal/%s: table calibration present, proper rotation (det %.4f)",
              h == VRI_RIGHT ? "RIGHT" : "LEFT", det);
        printf("      %s ControllerGripToPalm rows (bone axis in grip fwd/left/up): "
               "(%+.3f %+.3f %+.3f) (%+.3f %+.3f %+.3f) (%+.3f %+.3f %+.3f)  offset m (%+.4f %+.4f %+.4f)\n",
               h == VRI_RIGHT ? "RIGHT" : "LEFT", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8],
               offCal[h][0], offCal[h][1], offCal[h][2]);
    }
    // Mirrored, not identical: the two hands' rows differ.
    {
        float diff = 0.0f;
        for (int i = 0; i < 9; ++i) diff += fabsf(rowsCal[VRI_RIGHT][i] - rowsCal[VRI_LEFT][i]);
        CHECK(diff > 0.5f, "cal: RIGHT and LEFT calibration frames are distinct (mirrored), sum|diff| %.3f", diff);
    }

    const float zero3[3] = { 0, 0, 0 };
    float posA[VRI_HANDS][3];
    for (int h = 0; h < VRI_HANDS; ++h) {
        const float handSign = (h == VRI_RIGHT) ? -1.0f : 1.0f;
        float kB[3], tB[3], nB[3];
        VRNH_GetCalibrationAnatomy(h, -1, kB, tB, nB);
        for (int p = 0; p < 6; ++p) {
            const SOGripPose& g = poses[p];
            float gripRows[9];
            SOGripRows(g, gripRows);
            float pos[3], axis[9];
            VRHandPose_DesiredFromGripCal(g.pos, gripRows, rowsCal[h], offCal[h], zero3, pos, axis);
            // Anatomical directions of the RENDERED palm, in the room.
            float kW[3], tW[3], nW[3];
            HpApply(axis, kB, kW);
            HpApply(axis, tB, tW);
            HpApply(axis, nB, nW);
            // THE OPENXR GRIP CONVENTION, as specified (corrected 2026-09-04d;
            // this fixture previously asserted the same swapped Y/Z the
            // generator used, which is how a wrong palm frame passed):
            //   knuckles = -Z,  thumb = +Y,  palm-out = -X RIGHT / +X LEFT.
            const float nExp[3] = { handSign * g.X[0], handSign * g.X[1], handSign * g.X[2] };
            // OpenXR grip pose, verbatim: -Z is little finger -> thumb (the
            // THUMB side), +X the palm normal (into the palm on the right),
            // +Y = Z x X, which is the WRIST side: the hand points along -Y.
            const float kExp[3] = { -g.Y[0], -g.Y[1], -g.Y[2] };
            const float tExp[3] = { -g.Z[0], -g.Z[1], -g.Z[2] };
            const float dk = SODot(kW, kExp), dt = SODot(tW, tExp), dn = SODot(nW, nExp);
            CHECK(dk > 0.999f && dt > 0.999f && dn > 0.999f,
                  "cal/%s/%s: rendered knuckles along grip -Y (%.4f), thumb side along -Z "
                  "(%.4f), palm-out along %cX (%.4f)",
                  h == VRI_RIGHT ? "RIGHT" : "LEFT", g.name, dk, dt, handSign < 0 ? '-' : '+', dn);
            printf("      %s %-14s palm basis (rows fwd/left/up): (%+.2f %+.2f %+.2f) (%+.2f %+.2f %+.2f) (%+.2f %+.2f %+.2f)  origin (%+.2f %+.2f %+.2f) u\n",
                   h == VRI_RIGHT ? "R" : "L", g.name,
                   axis[0], axis[1], axis[2], axis[3], axis[4], axis[5], axis[6], axis[7], axis[8],
                   pos[0], pos[1], pos[2]);
            if (p == 0) memcpy(posA[h], pos, sizeof(pos));
            if (p == 5) {
                const float d[3] = { pos[0] - posA[h][0], pos[1] - posA[h][1], pos[2] - posA[h][2] };
                CHECK(fabsf(d[0] - g.pos[0]) < 1e-3f && fabsf(d[1] - g.pos[1]) < 1e-3f
                      && fabsf(d[2] - g.pos[2]) < 1e-3f,
                      "cal/%s/F: translation without rotation moves the palm by exactly the "
                      "controller's displacement (%.2f %.2f %.2f u)",
                      h == VRI_RIGHT ? "RIGHT" : "LEFT", d[0], d[1], d[2]);
            }
        }
        // The bone origin sits behind the grip origin (toward the wrist)
        // along the KNUCKLE direction, which in the corrected convention is
        // the offset's FORWARD component, and it is a hand's length of a few
        // centimetres -- an anatomy check the old convention could not make
        // (it read 1.3 cm, far too short for a palm centroid to wrist).
        // ...which under the real convention is row 2 (+Y, the wrist side),
        // and POSITIVE: the bone origin lies toward the wrist.
        CHECK(offCal[h][2] > 0.03f && offCal[h][2] < 0.08f,
              "cal/%s: bone origin is %.1f cm behind the palm centroid along the knuckles",
              h == VRI_RIGHT ? "RIGHT" : "LEFT", offCal[h][2] * 100.0f);
    }
    // The table is off -> the raw grip frame (pre-2026-09-03 behaviour).
    VRNH_SetCalibrationTable(0);
    {
        float r[9], o[3];
        const int have = VRNH_GetEffectiveCalibration(VRI_RIGHT, -1, r, o);
        CHECK(!have && fabsf(r[0] - 1) < 1e-6f && fabsf(r[4] - 1) < 1e-6f && o[0] == 0.0f,
              "cal: with the table off the calibration is identity/zero");
    }
    VRNH_SetCalibrationTable(1);
    VRNH_TestReset();
}

// ===========================================================================
// THE VISUAL LIMB CORRECTION (2026-09-04, second cut)
// ===========================================================================
//
// Driven by the REAL runtime grip: the RIGHT grip quaternion the 3352E385
// hardware log recorded with the trigger pulled while the user pointed the
// controller at the start menu (logs/hw-2026-09-04b, pairs 1-5), and its
// sagittal mirror for the LEFT hand. Through the production chain the
// corrected hand must render knuckles FORWARD, thumb UP, palm-out toward
// the body's midline; the uncorrected chain renders it backward with the
// thumb down (the defect both hardware runs reported). The whole limb turns
// about its existing palm pivot (position unchanged), the sleeve takes the
// same rigid motion, and the weapon is byte-identical (frozen) unless the
// operator opts it into the corrected basis.
static const float kHwGripR[4] = { -0.738f, -0.287f, -0.244f, -0.560f };
static const float kHwGripL[4] = { -0.738f, +0.287f, +0.244f, -0.560f };   // mirror x=0

static void SOEyeToRoom(const float e[3], float out[3])
{ out[0] = -e[2]; out[1] = -e[0]; out[2] = e[1]; }

// An anatomy direction (bone frame) of a rendered hand axis, in ROOM.
static void SOAnatRoom(const float axisEyeish[9], const float v[3], float outRoom[3])
{
    float e[3];
    for (int c = 0; c < 3; ++c)
        e[c] = v[0]*axisEyeish[c] + v[1]*axisEyeish[3+c] + v[2]*axisEyeish[6+c];
    SOEyeToRoom(e, outRoom);
}

static float SODot3(const float a[3], const float b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }

struct SOLimbCap {
    int   ok;
    float handPos[2][3], handAxis[2][9];      // [VROBJ_HAND_R, VROBJ_HAND_L]
    VRObjPose gun;
    float gunGrip[3], barrelRoom[3];
    float handM[2][16], sleeveM[2][16];       // R/L wanted matrices, last eye-scene
    int   haveM;
};

static void SORunLimb(int flip, int gunFollow, SOLimbCap* cap)
{
    const CodvrGunModel* gun = VRGunPose_ByVertexCount(516);
    float rAxis[9], lAxis[9], gAxis[9];
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis);
    HpAxisFromEuler(2.0f, 14.0f, 1.0f, gAxis);
    const float rPos[3] = { 6.0f, -2.0f, -12.0f };
    const float lPos[3] = { -6.0f, -2.0f, -12.0f };
    const float gPos[3] = { 5.0f, -4.0f, -13.0f };
    const float headPos0[3] = { 0, 0, 0 };
    float headAxisI[9]; NHIdentityAxis(headAxisI);
    const float gripR[3] = { 0.20f, -0.10f, -0.35f };
    const float gripL[3] = { -0.20f, -0.10f, -0.35f };

    memset(cap, 0, sizeof(*cap));
    NHResetForCase();
    VRNH_SetEnabled(1);
    VRNH_SetTwoHandEnabled(0);
    VRNH_SetReloadEnabled(0);
    VRNH_SetCalibrationTable(1);
    VRNH_SetPalmFlip(flip);
    VRNH_SetGunFollowsLimbFix(gunFollow);
    VRInputState in; memset(&in, 0, sizeof(in));
    in.hand[VRI_RIGHT] = NHHand(1, gripR, kHwGripR);
    in.hand[VRI_LEFT]  = NHHand(1, gripL, kHwGripL);
    unsigned serial = 30;
    for (int p = 0; p < 4; ++p) {
        VRNH_OnPairBegin(++serial, &in, headPos0, headAxisI, 1, p * 0.01);
        for (int eye = 0; eye < 2; ++eye) {
            NHScene(eye);
            NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
            RecReset();
            NHDriveHandDraw();
            const GLRec* mL = NthKind(REC_LOADMV, 0);   // file order: LEFT half first
            const GLRec* mR = NthKind(REC_LOADMV, 2);
            cap->haveM = (mL && mR);
            if (mL) memcpy(cap->handM[1], mL->m, sizeof(cap->handM[1]));
            if (mR) memcpy(cap->handM[0], mR->m, sizeof(cap->handM[0]));
            SOBuildSleeveVerts(0, rPos, lPos);           // first sleeve half = RIGHT arm
            RecReset();
            SODriveSleeveDraw();
            const GLRec* sR = NthKind(REC_LOADMV, 0);
            const GLRec* sL = NthKind(REC_LOADMV, 2);
            if (!sR || !sL) cap->haveM = 0;
            if (sR) memcpy(cap->sleeveM[0], sR->m, sizeof(cap->sleeveM[0]));
            if (sL) memcpy(cap->sleeveM[1], sL->m, sizeof(cap->sleeveM[1]));
            if (gun) NHDriveGunDraw(gun, gAxis, gPos);
        }
    }
    VRNHDebug d; VRNH_GetDebug(&d);
    cap->ok = d.hand[VRI_RIGHT].engaged && d.hand[VRI_LEFT].engaged && d.gun.targetInstalled;
    for (int h = 0; h < 2; ++h) {
        memcpy(cap->handPos[h], VRObj_Set()->hand[h].pos, sizeof(cap->handPos[h]));
        memcpy(cap->handAxis[h], VRObj_Set()->hand[h].axis, sizeof(cap->handAxis[h]));
    }
    cap->gun = VRObj_Set()->gun;
    memcpy(cap->gunGrip, d.gun.desiredGripEyeish, sizeof(cap->gunGrip));
    memcpy(cap->barrelRoom, d.gun.barrelRoom, sizeof(cap->barrelRoom));
}

// (S_PalmOrientationFix lived here until 2026-09-04d. It asserted the
// behaviour of a 180 deg runtime flip that has been removed: the flip was
// compensating for a palm calibration whose grip Y and Z axes were swapped,
// and it could not -- the flipped frame was still 90 deg from the OpenXR
// grip convention. The palm anchor is now measured against the spec in
// tests/palm_anchor_tests.inc, and the limb capture helpers above are used
// from there.)
