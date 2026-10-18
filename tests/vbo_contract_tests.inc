// ===========================================================================
// THE VBO VALIDITY CONTRACT (project-lead review 2026-08-29, section 2)
// ===========================================================================
//
// BufferData(NULL) must NOT create valid zero bytes. The shadow tracks, per
// buffer, the allocation generation and the intervals WRITTEN in it; a palm
// reader whose landmarks touch an UNKNOWN byte is refused BEFORE the solver
// with its own reason (VBO_BYTES_UNKNOWN), and Horn's method never runs on a
// mixed cloud. These cases drive the REAL production path -- a VBO-backed
// merged hand draw through GLSurf_OnDrawElements with the solve armed -- and
// read the attributed counters and the palm-attempt telemetry.
//
//   A. orphan NULL -> landmark read refuses
//   B. partial SubData not covering the landmarks -> refuses
//   C. all landmark ranges written -> succeeds (per-landmark, not whole-span)
//   D. a second orphan invalidates the previous good generation
//   E. old-generation data can never satisfy the new generation
//   F. a mixed valid/unknown cloud never reaches Kabsch/Horn
//
// Included AFTER nativehands_tests.inc: it borrows the engine-unit synthetic
// hand batch (NHBuildHandVerts at the real index ranges) and the VBO fixture
// (VBOT_*, g_vboStage) from vbo_safety_tests.inc.

// Position bytes of one landmark table inside the packed store. kNHRight sits
// at [837, 837+70], kNHLeft at [0, 70].
static size_t VCLmLo(const CodvrPalmLandmark* lm, int n)
{
    int lo = lm[0].index;
    for (int i = 1; i < n; ++i) if (lm[i].index < lo) lo = lm[i].index;
    return (size_t)VBOT_HAND_OFF + (size_t)lo * VBOT_STRIDE;
}
static size_t VCLmHi(const CodvrPalmLandmark* lm, int n)
{
    int hi = lm[0].index;
    for (int i = 1; i < n; ++i) if (lm[i].index > hi) hi = lm[i].index;
    return (size_t)VBOT_HAND_OFF + (size_t)(hi + 1) * VBOT_STRIDE;
}

static void VCOrphan()
{
    VBO_NoteBindBuffer(VBO_GL_ARRAY_BUFFER, VBOT_ARRAY_ID);
    VBO_NoteBufferData(VBO_GL_ARRAY_BUFFER, VBOT_SIZE, nullptr, 0x88E0);
}

static void VCSubData(size_t off, size_t len)
{
    VBO_NoteBufferSubData(VBO_GL_ARRAY_BUFFER, (ptrdiff_t)off, (ptrdiff_t)len,
                          g_vboStage + off);
}

// Fresh layer, synthetic hand at known poses staged into the packed store
// (NOT uploaded), the buffer bound, the viewmodel pass entered with the real
// byte offset, and the solve armed on both hands with no target.
static void VCSetup(const float rAxis[9], const float rPos[3],
                    const float lAxis[9], const float lPos[3])
{
    NHResetForCase();
    NHBuildHandVerts(rAxis, rPos, lAxis, lPos);
    memset(g_vboStage, 0, sizeof(g_vboStage));
    memcpy(g_vboStage + VBOT_HAND_OFF, g_verts, VBOT_VERTS * VBOT_STRIDE);
    VCOrphan();                                  // bind + BufferData(NULL)
    EnterViewmodelPassVboBacked();               // pointer latches the buffer
    GLSurf_SetHandSolveWanted(GLSC_HAND_R, 1);
    GLSurf_SetHandSolveWanted(GLSC_HAND_L, 1);
    VRHandPose_ResetStats();
}

static void VCDrain()
{
    GLSurfPalmTry pt;
    while (GLSurf_PopPalmTry(&pt)) {}
}

static void S_VboOrphanContract()
{
    printf("--- vbo contract: orphan bytes are UNKNOWN until written ---\n");

    float rAxis[9], lAxis[9];
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, rAxis);
    HpAxisFromEuler(-8.0f, 35.0f, 4.0f, lAxis);
    const float rPos[3] = { 6.0f, -2.0f, -12.0f };
    const float lPos[3] = { -6.0f, -2.0f, -12.0f };
    const size_t rLo = VCLmLo(kNHRight, NH_N), rHi = VCLmHi(kNHRight, NH_N);
    const size_t lLo = VCLmLo(kNHLeft, NH_N),  lHi = VCLmHi(kNHLeft, NH_N);
    const size_t posLo = VBOT_HAND_OFF, posHi = VBOT_HAND_OFF + (size_t)VBOT_VERTS * VBOT_STRIDE;

    // ---- A: orphan -> every landmark read refuses, BEFORE the solver ------
    {
        VCSetup(rAxis, rPos, lAxis, lPos);
        VRHandPoseStats hp0; VRHandPose_GetStats(&hp0);
        CHECK(!DrawFaulted(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                           GLS_GL_UNSIGNED_INT, g_handIdx),
              "A: a hand draw against a freshly orphaned store does not fault");
        GLSurfHandSolve hr, hl;
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
        CHECK(hr.solves == 0 && hl.solves == 0 && hr.refusals == 1 && hl.refusals == 1,
              "A: both hands REFUSED (R %u/%u, L %u/%u)",
              hr.solves, hr.refusals, hl.solves, hl.refusals);
        CHECK(hr.refusedUnknownBytes == 1 && hl.refusedUnknownBytes == 1
              && hr.refusedSolve == 0 && hr.refusedBase == 0 && hr.refusedIdx == 0,
              "A: ...attributed to VBO_BYTES_UNKNOWN, not to the solver, the "
              "base or the indices (R unk %u solve %u base %u idx %u)",
              hr.refusedUnknownBytes, hr.refusedSolve, hr.refusedBase, hr.refusedIdx);
        CHECK(hr.lastRefSite == 4 && hr.lastRefReason == VRHP_REASON_BYTES_UNKNOWN
              && hr.lastRefUnknownBytes > 0,
              "A: the refusal telemetry names site 4 / %s with %u unknown bytes",
              VRHandPose_ReasonName(hr.lastRefReason), hr.lastRefUnknownBytes);
        VRHandPoseStats hp1; VRHandPose_GetStats(&hp1);
        CHECK(hp1.solverRuns == hp0.solverRuns,
              "A/F: Horn's method did NOT run on undefined bytes (runs %u -> %u)",
              hp0.solverRuns, hp1.solverRuns);
        CHECK(hp1.refusedUnknownBytes[VRHP_TRIGGER] == 1
              && hp1.refusedUnknownBytes[VRHP_SUPPORT] == 1,
              "A: the solver's own counters say unknown-bytes for both hands");
        VBOStats v; VBO_GetStats(&v);
        CHECK(v.orphans == 1, "A: exactly one orphan recorded (%u)", v.orphans);
        // The attempt telemetry carries the sourcing.
        GLSurfPalmTry pt;
        int got = 0, allUnknown = 1;
        while (GLSurf_PopPalmTry(&pt)) {
            ++got;
            if (pt.ok || pt.validRange || pt.unknownBytes == 0
                || pt.reason != VRHP_REASON_BYTES_UNKNOWN || pt.site != 4
                || pt.vbo != VBOT_ARRAY_ID || pt.allocGen != 1)
                allUnknown = 0;
        }
        CHECK(got == 2 && allUnknown,
              "A: palmtry records both attempts as REFUSE/VBO_BYTES_UNKNOWN with "
              "validRange=NO on buffer %u, allocation generation 1 (%d records)",
              VBOT_ARRAY_ID, got);
    }

    // ---- B: partial SubData NOT covering the landmarks -> refuses --------
    {
        VCSetup(rAxis, rPos, lAxis, lPos);
        // The engine's first two chunks (the tex/normal blocks in the real
        // layout): everything BELOW the position block. No landmark byte.
        VCSubData(0, posLo);
        VRHandPoseStats hp0; VRHandPose_GetStats(&hp0);
        NHDriveHandDraw();
        GLSurfHandSolve hr, hl;
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
        CHECK(hr.solves == 0 && hl.solves == 0
              && hr.refusedUnknownBytes == 1 && hl.refusedUnknownBytes == 1,
              "B: a SubData that misses the landmarks leaves both hands "
              "refused as VBO_BYTES_UNKNOWN");
        VRHandPoseStats hp1; VRHandPose_GetStats(&hp1);
        CHECK(hp1.solverRuns == hp0.solverRuns, "B/F: the solver still never ran");

        // Now ONLY the LEFT landmarks' bytes: L solves, R still refuses --
        // and exactly one table ran the solver (L's).
        VCSubData(lLo, lHi - lLo);
        VRHandPose_GetStats(&hp0);
        VCDrain();
        NHDriveHandDraw();
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
        CHECK(hl.solves == 1 && hl.valid == 1,
              "B: writing exactly the LEFT landmark range lets LEFT solve (%u)",
              hl.solves);
        CHECK(hr.solves == 0 && hr.refusedUnknownBytes == 2,
              "B: ...while RIGHT, whose bytes are still unknown, refuses (%u unk)",
              hr.refusedUnknownBytes);
        VRHandPose_GetStats(&hp1);
        CHECK(hp1.solverRuns == hp0.solverRuns + 1,
              "B/F: Horn's method ran EXACTLY once -- for the defined hand "
              "(runs %u -> %u)", hp0.solverRuns, hp1.solverRuns);
        CHECK_NEAR(HpDist(hl.pos, lPos), 0.0, 1e-3,
                   "B: and LEFT's recovered origin is the authored one (%.4f u off)",
                   HpDist(hl.pos, lPos));
    }

    // ---- C: all landmark ranges written -> succeeds ----------------------
    //
    // Per-LANDMARK gating, not whole-span: the span still holds unknown bytes
    // (the filler vertices between the two landmark blocks), yet every byte
    // the solver reads is defined, so both hands solve.
    {
        VCSetup(rAxis, rPos, lAxis, lPos);
        VCSubData(lLo, lHi - lLo);
        VCSubData(rLo, rHi - rLo);
        VCDrain();
        NHDriveHandDraw();
        GLSurfHandSolve hr, hl;
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
        CHECK(hr.solves == 1 && hl.solves == 1 && hr.refusals == 0 && hl.refusals == 0,
              "C: with every landmark range written, BOTH hands solve "
              "(R %u/%u, L %u/%u)", hr.solves, hr.refusals, hl.solves, hl.refusals);
        CHECK_NEAR(HpDist(hr.pos, rPos), 0.0, 1e-3,
                   "C: RIGHT recovered its authored origin (%.4f u off)",
                   HpDist(hr.pos, rPos));
        GLSurfPalmTry pt;
        int got = 0, spanHadUnknown = 0, allOk = 1;
        while (GLSurf_PopPalmTry(&pt)) {
            ++got;
            if (!pt.validRange && pt.unknownBytes > 0) spanHadUnknown = 1;
            if (!pt.ok || pt.reason != VRHP_REASON_OK) allOk = 0;
        }
        CHECK(got == 2 && allOk && spanHadUnknown,
              "C: palmtry shows PASS with validRange=NO -- the span had unknown "
              "filler bytes, the landmarks did not (%d records)", got);
        CHECK(VBO_RangeDefined(VBOT_ARRAY_ID, posLo, posHi - posLo) == 0
              && VBO_RangeDefined(VBOT_ARRAY_ID, rLo, rHi - rLo) == 1,
              "C: the shadow agrees: whole span undefined, landmark range defined");
        CHECK(VBO_ResolveRange(VBOT_ARRAY_ID, posLo, posHi - posLo) == nullptr,
              "C: a STRICT whole-span read (pivots, scratch transform) still "
              "refuses -- no substitute bytes for readers that touch everything");
    }

    // ---- D: a second orphan invalidates the previous good generation ----
    {
        VCSetup(rAxis, rPos, lAxis, lPos);
        VCSubData(posLo, posHi - posLo);          // the whole position block
        NHDriveHandDraw();
        GLSurfHandSolve hr;
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        CHECK(hr.solves == 1, "D: generation 1 fully written -> solves");
        const unsigned gen1 = VBO_AllocGeneration(VBOT_ARRAY_ID);
        CHECK(VBO_RangeDefined(VBOT_ARRAY_ID, rLo, rHi - rLo) == 1,
              "D: the landmark bytes are defined in generation %u", gen1);

        VCOrphan();                                // BufferData(NULL) again
        CHECK(VBO_AllocGeneration(VBOT_ARRAY_ID) == gen1 + 1,
              "D: the allocation generation advanced (%u -> %u)",
              gen1, VBO_AllocGeneration(VBOT_ARRAY_ID));
        CHECK(VBO_RangeDefined(VBOT_ARRAY_ID, rLo, rHi - rLo) == 0,
              "D: ...and the previously good bytes are UNKNOWN again");
        NHScene(0);
        EnterViewmodelPassVboBacked();
        VCDrain();
        NHDriveHandDraw();
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        CHECK(hr.solves == 1 && hr.refusedUnknownBytes == 1,
              "D: the same draw after the second orphan REFUSES as unknown "
              "bytes (solves still %u, unk %u)", hr.solves, hr.refusedUnknownBytes);
        GLSurfPalmTry pt; int last = 0;
        while (GLSurf_PopPalmTry(&pt)) last = (int)pt.allocGen;
        CHECK(last == (int)(gen1 + 1),
              "D: palmtry carries the new allocation generation (%d)", last);
    }

    // ---- E: old-generation data can never satisfy the new generation ----
    //
    // The bytes of generation 1 are physically gone (the store was
    // reallocated), but the CONTRACT is stronger than that: even the
    // interval bookkeeping is cleared, so nothing written before a
    // BufferData can vouch for anything after it. Writing only LEFT in
    // generation 2 must leave RIGHT unknown although RIGHT was fully valid in
    // generation 1.
    {
        VCSetup(rAxis, rPos, lAxis, lPos);
        VCSubData(posLo, posHi - posLo);
        NHDriveHandDraw();                         // gen 1: both solve
        VCOrphan();                                // gen 2
        VCSubData(lLo, lHi - lLo);                 // only LEFT this generation
        NHScene(0);
        EnterViewmodelPassVboBacked();
        NHDriveHandDraw();
        GLSurfHandSolve hr, hl;
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        GLSurf_GetHandSolve(GLSC_HAND_L, &hl);
        CHECK(hl.solves == 2, "E: LEFT, rewritten in generation 2, solves (%u)",
              hl.solves);
        CHECK(hr.solves == 1 && hr.refusedUnknownBytes == 1,
              "E: RIGHT, valid only in generation 1, is REFUSED in generation 2 "
              "(solves %u, unknown-bytes refusals %u)",
              hr.solves, hr.refusedUnknownBytes);
        VBOResolveInfo info;
        const void* p = VBO_ResolveRangeEx(VBOT_ARRAY_ID, rLo, rHi - rLo, &info);
        CHECK(p == nullptr && info.reason == VBO_RES_BYTES_UNKNOWN
              && info.allocGen == 2 && info.unknownBytes == rHi - rLo,
              "E: the resolve reports %s, allocation generation %u, %u unknown "
              "bytes of %u", VBO_ReasonName(info.reason), info.allocGen,
              (unsigned)info.unknownBytes, (unsigned)(rHi - rLo));
    }

    // ---- F: a mixed valid/unknown cloud never reaches the solver --------
    //
    // Half the RIGHT landmarks written, half not. The table is refused as a
    // whole; the solver-run counter does not move; the residual counters do
    // not move; nothing about the fit is reported, because no fit was made.
    {
        VCSetup(rAxis, rPos, lAxis, lPos);
        const size_t rMid = rLo + ((rHi - rLo) / 2 / VBOT_STRIDE) * VBOT_STRIDE;
        VCSubData(rLo, rMid - rLo);                // first half of RIGHT's landmarks
        VRHandPoseStats hp0; VRHandPose_GetStats(&hp0);
        VCDrain();
        NHDriveHandDraw();
        VRHandPoseStats hp1; VRHandPose_GetStats(&hp1);
        GLSurfHandSolve hr;
        GLSurf_GetHandSolve(GLSC_HAND_R, &hr);
        CHECK(hr.solves == 0 && hr.refusedUnknownBytes == 1 && hr.refusedSolve == 0,
              "F: a half-written landmark set is refused as unknown bytes, not "
              "as a bad fit (unk %u, solve %u)", hr.refusedUnknownBytes, hr.refusedSolve);
        CHECK(hp1.solverRuns == hp0.solverRuns,
              "F: Horn's method never ran on the mixed cloud (runs %u -> %u)",
              hp0.solverRuns, hp1.solverRuns);
        CHECK(hr.lastRefBestRmsM < 0.0f,
              "F: no residual exists for the refusal (bestRms %.4f) -- there was "
              "no fit to have a residual", hr.lastRefBestRmsM);
        GLSurfPalmTry pt; int got = 0, mixed = 0;
        while (GLSurf_PopPalmTry(&pt))
            if (pt.cls == GLSC_HAND_R) { ++got; if (pt.unknownBytes > 0 && pt.rmsM < 0.0f) mixed = 1; }
        CHECK(got == 1 && mixed,
              "F: palmtry records the RIGHT attempt with unknown bytes and no rms");

        // The whole-store strict readers agree: a strict resolve of the span
        // (what the pivot and scratch paths use) refuses and is counted.
        VBOStats v0; VBO_GetStats(&v0);
        CHECK(VBO_ResolveRange(VBOT_ARRAY_ID, posLo, posHi - posLo) == nullptr,
              "F: a strict whole-span resolve refuses the half-written store");
        VBOStats v; VBO_GetStats(&v);
        CHECK(v.resolveMissUndefined == v0.resolveMissUndefined + 1,
              "F: ...counted as undefined content (%u)", v.resolveMissUndefined);
    }

    // The same contract for the GUN reader: an orphaned keyed batch refuses
    // as unknown bytes and never runs the body solve.
    {
        const CodvrGunModel* m = VRGunPose_ByVertexCount(326);
        if (m) {
            NHResetForCase();
            GLSurf_SetGunSolveWanted(1);
            // Stage a real body batch, orphan a store of the right size,
            // point the vertex array at offset 0 of it.
            float axis[9]; HpAxisFromEuler(4.0f, -12.0f, 2.0f, axis);
            const float pos[3] = { 6.0f, -4.0f, -14.0f };
            const int ic = NHBuildGunBatch(m, axis, pos);
            VBO_NoteBindBuffer(VBO_GL_ARRAY_BUFFER, VBOT_ARRAY_ID + 1);
            VBO_NoteBufferData(VBO_GL_ARRAY_BUFFER, (ptrdiff_t)m->vertexCount * 12,
                               nullptr, 0x88E0);
            GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, (const void*)0);
            VRHandPoseStats hp0; VRHandPose_GetStats(&hp0);
            GLSurf_OnDrawElements(GLS_GL_TRIANGLES, ic, GLS_GL_UNSIGNED_INT, g_gunIdxBuf);
            VRHandPoseStats hp1; VRHandPose_GetStats(&hp1);
            GLSurfGunSolve gs; GLSurf_GetGunSolve(&gs);
            CHECK(gs.haveBody == 0 && gs.refusedUnknownBytes == 1
                  && gs.lastReason == VRHP_REASON_BYTES_UNKNOWN,
                  "gun: an orphaned keyed batch is refused as %s (unk %u)",
                  VRHandPose_ReasonName(gs.lastReason), gs.refusedUnknownBytes);
            CHECK(hp1.solverRuns == hp0.solverRuns,
                  "gun: ...and the body solver never ran");
            // Fill it: the same draw solves.
            VBO_NoteBufferSubData(VBO_GL_ARRAY_BUFFER, 0, (ptrdiff_t)m->vertexCount * 12,
                                  g_gunVertsBuf);
            NHScene(0);
            GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, (const void*)0);
            GLSurf_OnDrawElements(GLS_GL_TRIANGLES, ic, GLS_GL_UNSIGNED_INT, g_gunIdxBuf);
            GLSurf_GetGunSolve(&gs);
            CHECK(gs.haveBody == 1 && gs.lastReason == VRHP_REASON_OK,
                  "gun: once written, the body solves (%s)",
                  VRHandPose_ReasonName(gs.lastReason));
            GLSurf_SetGunSolveWanted(0);
            GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_verts);
        }
    }

    ResetLayerForCase();
    VRHandPose_TestSetLandmarks(VRHP_TRIGGER, kNHRight, NH_N);
    VRHandPose_TestSetLandmarks(VRHP_SUPPORT, kNHLeft, NH_N);
}
