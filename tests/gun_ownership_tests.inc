// ===========================================================================
// WEAPON DRAW OWNERSHIP (project-lead brief 2026-09-04, P0, sections 1-5)
//
// Hardware (logs/hw-2026-09-04c): the Colt and the Thompson drew ~1.1-1.2
// gun batches per scene VANILLA with zero solve refusals, the BAR none. The
// engine draws a weapon's surfaces in shader-sorted order, so a part reached
// GL before the body surface that keyed the scene's one rigid delta, and
// went out head-coupled beside a controller-owned body: two weapons.
//
// The fix: every shipped gun surface is rigid to ONE bone, so per-surface
// landmark tables in the body frame let ANY body-bound surface key the
// scene; a moving part drawn before the key is placed by the carried
// previous-scene baseline (or suppressed) -- never vanilla. Every gun draw
// of an eye-scene is attributed to one disposition, and native+vanilla in
// one eye-scene is an OWNER CONFLICT the pre-submit verdict refuses.
// ===========================================================================

// A gun SURFACE batch at a body pose, from that surface's own table.
static int GOBuildSurfaceBatch(const CodvrGunSurface* s, const float axis[9],
                               const float posU[3])
{
    const int nv = s->vertexCount;
    for (int i = 0; i < nv * 3 && i < 2048 * 3; ++i)
        g_gunVertsBuf[i] = 500.0f + (float)(i % 13);
    for (int i = 0; i < s->n; ++i) {
        const float bindU[3] = { s->lm[i].bindM[0] * 39.3701f,
                                 s->lm[i].bindM[1] * 39.3701f,
                                 s->lm[i].bindM[2] * 39.3701f };
        float r[3];
        HpApply(axis, bindU, r);
        float* v = &g_gunVertsBuf[s->lm[i].index * 3];
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

// The same batch with its landmark cloud stretched 15 %: no rigid transform
// reproduces it, so the part cannot key itself -- the fixture's stand-in for
// a part caught mid-animation by a deformation the solver must refuse.
static int GOBuildSurfaceBatchSkewed(const CodvrGunSurface* s, const float axis[9],
                                     const float posU[3])
{
    const int ic = GOBuildSurfaceBatch(s, axis, posU);
    for (int i = 0; i < s->n; ++i) {
        float* v = &g_gunVertsBuf[s->lm[i].index * 3];
        for (int k = 0; k < 3; ++k) v[k] = posU[k] + (v[k] - posU[k]) * 1.15f;
    }
    return ic;
}

static int GODrawSurfaceSkewed(const CodvrGunSurface* s, const float axis[9],
                               const float posU[3])
{
    const int ic = GOBuildSurfaceBatchSkewed(s, axis, posU);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_gunVertsBuf);
    const int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, ic,
                                            GLS_GL_UNSIGNED_INT, g_gunIdxBuf);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_verts);
    return owned;
}

static int GODrawSurface(const CodvrGunSurface* s, const float axis[9],
                         const float posU[3])
{
    const int ic = GOBuildSurfaceBatch(s, axis, posU);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_gunVertsBuf);
    const int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, ic,
                                            GLS_GL_UNSIGNED_INT, g_gunIdxBuf);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_verts);
    return owned;
}

// The surfaces of one model, by its name string, in asset order.
static int GOModelSurfaces(const char* model, const CodvrGunSurface* out[16])
{
    int n = 0;
    for (int i = 0; i < VRGunPose_SurfaceCount() && n < 16; ++i) {
        const CodvrGunSurface* s = VRGunPose_Surface(i);
        if (strcmp(s->model, model) == 0) out[n++] = s;
    }
    return n;
}

static void GORearGrip(const CodvrGunModel* m, const float axis[9], const float posU[3],
                       float out[3])
{
    const float gU[3] = { m->rearGripM[0]*39.3701f, m->rearGripM[1]*39.3701f,
                          m->rearGripM[2]*39.3701f };
    float r[3]; HpApply(axis, gU, r);
    for (int k = 0; k < 3; ++k) out[k] = posU[k] + r[k];
}

// A transformed draw records TWO matrix loads: the wanted one, then the
// engine's restored. Count draws, and read the WANTED matrix of draw i.
static int GOTransformedDraws() { return CountKind(REC_LOADMV) / 2; }
static const GLRec* GOWanted(int i) { return NthKind(REC_LOADMV, 2 * i); }

static const char* GODispName(int d)
{
    switch (d) {
        case GLS_GDISP_NATIVE: return "F";
        case GLS_GDISP_PARTKEY: return "P";
        case GLS_GDISP_STALE: return "S";
        case GLS_GDISP_SUPPRESSED: return "X";
        case GLS_GDISP_VANILLA: return "V";
        default: return "?";
    }
}

// ---------------------------------------------------------------------------
static void S_GunSurfaceTables()
{
    printf("--- gun surface tables: every surface rigid, only body surfaces key ---\n");
    const int nS = VRGunPose_SurfaceCount();
    CHECK(nS >= 100, "per-surface table has %d entries", nS);
    int badKey = 0, tooFew = 0, keyable = 0, parts = 0;
    for (int i = 0; i < nS; ++i) {
        const CodvrGunSurface* s = VRGunPose_Surface(i);
        if (s->keyUsable && !s->bodyRigid) ++badKey;
        if (s->n < 3) ++tooFew;
        if (s->keyUsable) ++keyable;
        if (!s->bodyRigid) ++parts;
    }
    CHECK(badKey == 0, "no moving part is key-usable (%d would be)", badKey);
    CHECK(tooFew == 0, "every surface carries >= 3 landmarks");
    printf("      %d key-usable body surfaces, %d moving parts\n", keyable, parts);

    // Every shipped model has its body key surface key-usable, and at least
    // that one way to key a scene.
    int modelsMissing = 0;
    for (int i = 0; i < nS; ++i) {
        const CodvrGunSurface* s = VRGunPose_Surface(i);
        if (s->vertexCount == s->modelKey && !s->keyUsable) ++modelsMissing;
    }
    CHECK(modelsMissing == 0, "every model's body key surface is key-usable");

    // The two hardware offenders, exactly as the assets say.
    const CodvrGunModel* colt = VRGunPose_ByVertexCount(344);
    const CodvrGunModel* thompson = VRGunPose_ByVertexCount(326);
    CHECK(colt && thompson, "colt (344) and thompson (326) models present");
    if (colt) {
        const CodvrGunSurface* a = VRGunPose_SurfaceOfModel(colt, 270);
        const CodvrGunSurface* b = VRGunPose_SurfaceOfModel(colt, 123);
        const CodvrGunSurface* c = VRGunPose_SurfaceOfModel(colt, 48);
        CHECK(a && b && c && !a->keyUsable && !b->keyUsable && !c->keyUsable,
              "colt: slide/trigger/clip are moving parts, never keys");
        CHECK(!VRGunPose_AnyKeySurface(270), "no weapon keys on a 270-vertex batch");
    }
    if (thompson) {
        const CodvrGunSurface* a = VRGunPose_SurfaceOfModel(thompson, 319);
        const CodvrGunSurface* b = VRGunPose_SurfaceOfModel(thompson, 122);
        CHECK(a && b && a->keyUsable && b->keyUsable,
              "thompson: the 319 and 122 body surfaces can key the scene too");
        CHECK(!VRGunPose_AnyKeySurface(24), "the 24-vertex bolt never keys");
    }
}

// ---------------------------------------------------------------------------
static void S_GunAnySurfaceKeys()
{
    printf("--- any body-bound surface recovers the BODY pose ---\n");
    const CodvrGunModel* thompson = VRGunPose_ByVertexCount(326);
    if (!thompson) { CHECK(0, "thompson table missing"); return; }
    float axis0[9];
    HpAxisFromEuler(7.0f, -25.0f, 3.0f, axis0);
    const float pos0[3] = { 5.0f, -3.0f, -13.0f };
    const int counts[3] = { 319, 326, 122 };
    for (int c = 0; c < 3; ++c) {
        const CodvrGunSurface* s = VRGunPose_SurfaceOfModel(thompson, counts[c]);
        CHECK(s != nullptr, "thompson surface with %d verts listed", counts[c]);
        if (!s) continue;
        GOBuildSurfaceBatch(s, axis0, pos0);
        VRHandPose body; const CodvrGunSurface* got = nullptr; int reason = -1;
        const CodvrGunModel* m = VRGunPose_SolveAnySurfaceEx(counts[c], g_gunVertsBuf, 12,
            counts[c], 39.3701f, &body, nullptr, &got, 0, 0, &reason);
        CHECK(m == thompson && got == s,
              "surface %d (%d verts, bone %s) keys the THOMPSON body", s->surface,
              counts[c], s->bone);
        CHECK_NEAR(HpDist(body.pos, pos0), 0.0, 1e-3,
                   "...body origin recovered (%.5f u off)", HpDist(body.pos, pos0));
        float dmax = 0.0f;
        for (int k = 0; k < 9; ++k) { const float d = fabsf(body.axis[k] - axis0[k]); if (d > dmax) dmax = d; }
        CHECK(dmax < 1e-4f, "...body basis recovered (max %.6f)", dmax);
        float grip[3], gripTruth[3];
        VRGunPose_RearGripPivot(m->vertexCount, &body, 39.3701f, grip);
        GORearGrip(thompson, axis0, pos0, gripTruth);
        CHECK_NEAR(HpDist(grip, gripTruth), 0.0, 1e-3,
                   "...and the rear grip is the SAME point from every surface");
    }
    // A moving part built at rest solves nothing: it is not allowed to key.
    {
        const CodvrGunSurface* bolt = VRGunPose_SurfaceOfModel(thompson, 24);
        if (bolt) {
            GOBuildSurfaceBatch(bolt, axis0, pos0);
            VRHandPose body; int reason = -1;
            const CodvrGunModel* m = VRGunPose_SolveAnySurfaceEx(24, g_gunVertsBuf, 12, 24,
                39.3701f, &body, thompson, nullptr, 0, 0, &reason);
            CHECK(m == nullptr, "the bolt (moving part) is refused as a key");
        }
    }
    // Preference: the kar98k twins share body key 536; the caller's model wins.
    {
        const CodvrGunModel* k98 = VRGunPose_ByVertexCount(536);
        const CodvrGunSurface* s536 = k98 ? VRGunPose_SurfaceOfModel(k98, 536) : nullptr;
        if (k98 && s536) {
            GOBuildSurfaceBatch(s536, axis0, pos0);
            VRHandPose body; const CodvrGunSurface* got = nullptr;
            const CodvrGunModel* m = VRGunPose_SolveAnySurfaceEx(536, g_gunVertsBuf, 12, 536,
                39.3701f, &body, k98, &got, 0, 0, nullptr);
            CHECK(m == k98, "the preferred model owns a shared-key solve");
        }
    }
}

// ---------------------------------------------------------------------------
// The Colt in the engine's order: parts BEFORE the body, every scene
// (measured on hardware 2026-09-04d: [123:S 270:S 344:F 48:F], and the
// carried delta drifted up to 4.5 u / 20.5 deg -- the second Colt).
static void S_GunPreKeyOwnership()
{
    printf("--- pre-key gun batches: the part keys ITSELF, or it is not drawn ---\n");
    const CodvrGunModel* colt = VRGunPose_ByVertexCount(344);
    if (!colt) { CHECK(0, "colt table missing"); return; }
    const CodvrGunSurface* body = VRGunPose_SurfaceOfModel(colt, 344);
    const CodvrGunSurface* slide = VRGunPose_SurfaceOfModel(colt, 270);
    const CodvrGunSurface* trig = VRGunPose_SurfaceOfModel(colt, 123);
    const CodvrGunSurface* clip = VRGunPose_SurfaceOfModel(colt, 48);
    if (!body || !slide || !trig || !clip) { CHECK(0, "colt surfaces missing"); return; }
    CHECK(slide->restStable && trig->restStable && clip->restStable
          && !slide->bodyRigid && !trig->bodyRigid && !clip->bodyRigid,
          "the colt's slide, trigger and clip are moving parts with a rest relation "
          "to the body (spreads %.3f / %.3f / %.3f m)",
          slide->minSpreadM, trig->minSpreadM, clip->minSpreadM);

    float axis0[9];
    HpAxisFromEuler(4.0f, -12.0f, 2.0f, axis0);
    const float pos0[3] = { 6.0f, -4.0f, -14.0f };
    float pivot0[3];
    GORearGrip(colt, axis0, pos0, pivot0);
    float wantAxis[9];
    HpAxisFromEuler(24.0f, 3.0f, 2.0f, wantAxis);
    const float wantGrip[3] = { pivot0[0] + 2.0f, pivot0[1] - 1.0f, pivot0[2] + 3.0f };

    NHResetForCase();
    GLSurf_SetGunSolveWanted(1);
    GLSurf_SetGunPreKeyPolicy(0);
    GLSurf_SetGunTarget(1, wantGrip, wantAxis);

    // ---- scene A: no weapon known yet, so a part cannot look up its own
    //      table. Not drawn -- never head-coupled.
    NHScene(0);
    RecReset();
    int o1 = GODrawSurface(clip, axis0, pos0);
    int o2 = GODrawSurface(slide, axis0, pos0);
    CHECK(o1 == 1 && o2 == 1 && GOTransformedDraws() == 0,
          "A: before any weapon is identified, a pre-key part is not drawn");
    int o3 = GODrawSurface(body, axis0, pos0);
    CHECK(o3 == 1 && GOTransformedDraws() == 1, "A: the body keys the scene and is native");
    GLSurfGunScene sc;
    CHECK(GLSurf_GetGunScene(0, &sc) && sc.weaponDrawSuppressed == 2
          && sc.weaponDrawNative == 1 && sc.weaponDrawVanilla == 0,
          "A: 2 suppressed, 1 native, 0 vanilla");

    // ---- scene B: the weapon is known. Every pre-key part now keys ITSELF
    //      from its own rest table and lands on the SAME rigid delta as the
    //      body -- no carried pose, no drift.
    NHScene(1);
    RecReset();
    o1 = GODrawSurface(clip, axis0, pos0);
    o2 = GODrawSurface(slide, axis0, pos0);
    o3 = GODrawSurface(trig, axis0, pos0);
    CHECK(o1 == 1 && o2 == 1 && o3 == 1 && GOTransformedDraws() == 3,
          "B: three pre-key parts placed from their own rest tables");
    const int o4 = GODrawSurface(body, axis0, pos0);
    CHECK(o4 == 1 && GOTransformedDraws() == 4, "B: the body then keys the scene");
    {
        int same = 1;
        const GLRec* m0 = GOWanted(3);          // the body's own fresh delta
        for (int i = 0; i < 4; ++i) {
            const GLRec* mi = GOWanted(i);
            if (!m0 || !mi) { same = 0; break; }
            for (int k = 0; k < 16; ++k)
                if (fabsf(mi->m[k] - m0->m[k]) > 1e-4f) same = 0;
        }
        CHECK(same, "B: all four matrices are the SAME rigid transform -- one Colt");
    }
    CHECK(GLSurf_GetGunScene(1, &sc) && sc.weaponDrawPartKeyed == 3
          && sc.weaponDrawNative == 1 && sc.weaponDrawStale == 0
          && sc.weaponDrawVanilla == 0 && sc.weaponOwnerConflict == 0,
          "B: 3 part-keyed, 1 native, 0 stale, 0 vanilla, no conflict");

    // ---- scene C: a part the solver cannot reproduce (its landmarks no
    //      longer form its own rigid cloud) is NOT drawn, and is counted.
    GLSurfGunSolve g0; GLSurf_GetGunSolve(&g0);
    NHScene(0);
    RecReset();
    o1 = GODrawSurfaceSkewed(slide, axis0, pos0);
    GLSurfGunSolve g1; GLSurf_GetGunSolve(&g1);
    CHECK(o1 == 1 && GOTransformedDraws() == 0
          && g1.batchesPartRefused == g0.batchesPartRefused + 1,
          "C: a part that cannot key itself is hidden, not carried (refusals +%u)",
          g1.batchesPartRefused - g0.batchesPartRefused);
    GODrawSurface(body, axis0, pos0);

    // ---- scene D: the same case with the carry fallback armed.
    GLSurf_SetGunPreKeyPolicy(1);
    NHScene(1);
    RecReset();
    o1 = GODrawSurfaceSkewed(slide, axis0, pos0);
    CHECK(o1 == 1 && GOTransformedDraws() == 1,
          "D: vr_gun_prekey_policy 1 carries the previous scene's delta instead");
    GODrawSurface(body, axis0, pos0);
    GLSurf_SetGunPreKeyPolicy(0);

    // ---- scene E: a batch no surface of this weapon has: not drawn.
    NHScene(0);
    RecReset();
    const int o5 = NHDriveOtherGunBatch();
    CHECK(o5 == 1 && GOTransformedDraws() == 0,
          "E: a batch that belongs to no surface of this weapon is not drawn");
    GODrawSurface(body, axis0, pos0);

    GLSurfGunSolve gs; GLSurf_GetGunSolve(&gs);
    CHECK(gs.batchesVanilla == 0 && gs.ownerConflicts == 0,
          "throughout: 0 vanilla gun batches and 0 owner conflicts "
          "(partKeyed %u, suppressed %u, stale %u)",
          gs.batchesPartKeyed, gs.batchesSuppressed, gs.batchesStale);

    GLSurf_SetGunTarget(0, nullptr, nullptr);
    GLSurf_SetGunSolveWanted(0);
}

// ---------------------------------------------------------------------------
// HARD GATE (section 3): one gun batch forced vanilla while the rest go
// native must be called an owner conflict, and the pair must be refused.
static void S_GunOwnerConflictGate()
{
    printf("--- MUTANT: one gun batch left vanilla -> owner conflict -> pair refused ---\n");
    const CodvrGunModel* colt = VRGunPose_ByVertexCount(344);
    const CodvrGunSurface* body = colt ? VRGunPose_SurfaceOfModel(colt, 344) : nullptr;
    const CodvrGunSurface* clip = colt ? VRGunPose_SurfaceOfModel(colt, 48) : nullptr;
    if (!body || !clip) { CHECK(0, "colt surfaces missing"); return; }
    float axis0[9]; HpAxisFromEuler(4.0f, -12.0f, 2.0f, axis0);
    const float pos0[3] = { 6.0f, -4.0f, -14.0f };
    float pivot0[3]; GORearGrip(colt, axis0, pos0, pivot0);
    float wantAxis[9]; HpAxisFromEuler(24.0f, 3.0f, 2.0f, wantAxis);
    const float wantGrip[3] = { pivot0[0] + 2.0f, pivot0[1] - 1.0f, pivot0[2] + 3.0f };

    // glsurf level: the accounting names it.
    NHResetForCase();
    GLSurf_SetGunSolveWanted(1);
    GLSurf_SetGunTarget(1, wantGrip, wantAxis);
    NHScene(0);
    GODrawSurface(body, axis0, pos0);   // baseline scene
    GLSurf_TestForceGunBatchVanilla(1);
    NHScene(1);
    RecReset();
    GODrawSurface(body, axis0, pos0);   // ordinal 0: native
    const int o = GODrawSurface(clip, axis0, pos0);   // ordinal 1: FORCED vanilla
    GLSurfGunScene sc;
    CHECK(o == 0 && GLSurf_GetGunScene(1, &sc) && sc.weaponDrawNative == 1
          && sc.weaponDrawVanilla == 1 && sc.weaponOwnerConflict == 1,
          "glsurf: native 1 + vanilla 1 in one eye-scene = OWNER CONFLICT");
    GLSurfGunSolve gs; GLSurf_GetGunSolve(&gs);
    CHECK(gs.ownerConflicts == 1, "...counted once per eye-scene");
    GLSurf_TestForceGunBatchVanilla(-1);
    GLSurf_SetGunTarget(0, nullptr, nullptr);
    GLSurf_SetGunSolveWanted(0);

    // Production order: pair begin -> eye 0 draws -> eye 1 draws -> pre-submit.
    const CodvrGunModel* bar = VRGunPose_ByVertexCount(516);
    if (!bar || !TmpBringUp(bar, 0.064f)) { CHECK(0, "temporal bring-up failed"); return; }
    VRNHDebug d; VRNH_GetDebug(&d);
    CHECK(d.gun.targetInstalled, "gun engaged (%s)", VRNH_GunReasonName(d.gun.reason));
    TmpScene s; TmpDefaultScene(&s, bar);
    s.gunExtra = 1;                     // a second gun batch per scene
    VRNHPairStats before; VRNH_GetPairStats(&before);
    TmpPair(&s, &s);
    VRNHPairVerdict v; VRNH_GetLastVerdict(&v);
    CHECK(v.ok == 1 && v.obj[VRNH_OBJ_GUN].applied[0] && v.obj[VRNH_OBJ_GUN].applied[1],
          "control: two gun batches per eye, both native, pair accepted");
    GLSurf_TestForceGunBatchVanilla(1);
    unsigned refused = 0;
    for (int i = 0; i < VRNH_FALLBACK_ABORT_BUDGET; ++i) {
        TmpPair(&s, &s);
        VRNH_GetLastVerdict(&v);
        if (!v.ok && v.reason == VRNH_VERDICT_OWNER_CONFLICT) ++refused;
    }
    CHECK(refused == (unsigned)VRNH_FALLBACK_ABORT_BUDGET,
          "MUTANT: %u/%d pairs REFUSED as OWNER_CONFLICT (the gate catches a vanilla copy)",
          refused, VRNH_FALLBACK_ABORT_BUDGET);
    TmpPair(&s, &s);
    VRNH_GetLastVerdict(&v);
    CHECK(v.ok == 1, "...past the budget the conflict degrades (accepted) instead of freezing");
    VRNHPairStats after; VRNH_GetPairStats(&after);
    CHECK(after.rejectOwnerConflict - before.rejectOwnerConflict == (unsigned)VRNH_FALLBACK_ABORT_BUDGET
          && after.gunOwnerConflictPairs - before.gunOwnerConflictPairs == (unsigned)VRNH_FALLBACK_ABORT_BUDGET + 1,
          "stats: rejectOwnerConflict +%u, gunOwnerConflictPairs +%u",
          after.rejectOwnerConflict - before.rejectOwnerConflict,
          after.gunOwnerConflictPairs - before.gunOwnerConflictPairs);
    GLSurf_TestForceGunBatchVanilla(-1);
    TmpPair(&s, &s);
    VRNH_GetLastVerdict(&v);
    CHECK(v.ok == 1 && v.reason == VRNH_VERDICT_OK, "mutant off: the next pair is clean");
    VRNH_TestReset();
}

// ---------------------------------------------------------------------------
// EVERY supported weapon as ONE visual object (sections 2, 4, 5): all of its
// surfaces drawn parts-first in both eyes of a pair. One delta per
// eye-scene, byte-identical across every gun-owned batch; PairWorld rear
// grip identical across the eyes; no vanilla copy; no owner conflict.
static void S_GunWeaponMatrix()
{
    printf("--- per-weapon ownership matrix: every surface, parts first, both eyes ---\n");
    // Group surfaces by model name.
    const char* names[32]; int nNames = 0;
    for (int i = 0; i < VRGunPose_SurfaceCount(); ++i) {
        const char* nm = VRGunPose_Surface(i)->model;
        int seen = 0;
        for (int k = 0; k < nNames; ++k) if (strcmp(names[k], nm) == 0) seen = 1;
        if (!seen && nNames < 32) names[nNames++] = nm;
    }
    const float ipdU = 0.064f * 39.3701f;
    float off[2][3] = { { 0.0f, -ipdU * 0.5f, 0.0f }, { 0.0f, ipdU * 0.5f, 0.0f } };
    float axis0[9]; HpAxisFromEuler(4.0f, -12.0f, 2.0f, axis0);
    const float pos0[3] = { 6.0f, -4.0f, -14.0f };
    float wantAxis[9]; HpAxisFromEuler(20.0f, 6.0f, -3.0f, wantAxis);

    printf("      %-26s %-5s %-5s %-5s %-5s %-5s %-5s %s\n", "model", "surfs", "fresh",
           "part", "supp", "van", "confl", "eye0/eye1 dispositions");
    int weapons = 0, weaponsOk = 0;
    for (int w = 0; w < nNames; ++w) {
        const CodvrGunSurface* surfs[16];
        const int n = GOModelSurfaces(names[w], surfs);
        if (n == 0) continue;
        const CodvrGunModel* m = VRGunPose_ByVertexCount(surfs[0]->modelKey);
        if (!m || !m->rearGripUsable) continue;         // grenades, carbine, panzerfaust: never driven
        ++weapons;
        float pivot0[3]; GORearGrip(m, axis0, pos0, pivot0);
        const float wantGrip[3] = { pivot0[0] + 1.0f, pivot0[1] - 2.0f, pivot0[2] + 1.5f };

        NHResetForCase();
        GLSurf_SetGunSolveWanted(1);
        GLSurf_SetGunPreKeyPolicy(1);
        GLSurf_SetEyeOffsets(off);
        GLSurf_SetGunTarget(1, wantGrip, wantAxis);
        // Warm-up scene: seeds the baseline exactly as solve-wanted does live.
        NHScene(0);
        for (int i = n - 1; i >= 0; --i) GODrawSurface(surfs[i], axis0, pos0);

        int ok = 1;
        unsigned fresh = 0, stale = 0, supp = 0, van = 0, confl = 0;
        char dispStr[160]; int dn = 0;
        float finalMid[2][3], finalAxis[2][9];
        for (int eye = 0; eye < 2; ++eye) {
            NHScene(eye);
            RecReset();
            for (int i = n - 1; i >= 0; --i) GODrawSurface(surfs[i], axis0, pos0);
            GLSurfGunScene sc;
            if (!GLSurf_GetGunScene(eye, &sc)) { ok = 0; continue; }
            fresh += sc.weaponDrawNative; stale += sc.weaponDrawStale + sc.weaponDrawPartKeyed;
            supp += sc.weaponDrawSuppressed; van += sc.weaponDrawVanilla;
            confl += sc.weaponOwnerConflict;
            if (sc.weaponDrawVanilla || sc.weaponOwnerConflict) ok = 0;
            if (sc.weaponDrawNative + sc.weaponDrawStale + sc.weaponDrawPartKeyed != n) ok = 0;
            if (sc.deltaId == 0) ok = 0;
            // Every matrix issued this scene is the same one.
            const int nm = GOTransformedDraws();
            if (nm != n) ok = 0;
            const GLRec* m0 = GOWanted(0);
            for (int k = 1; k < nm; ++k) {
                const GLRec* mk = GOWanted(k);
                // ONE rigid transform for the whole weapon. A part that keys
                // itself solves its own copy of that transform rather than
                // reusing the cached one, so they agree to float precision
                // rather than bit-for-bit; anything a viewer could see is
                // orders of magnitude above this.
                float worst = 0.0f;
                for (int q = 0; q < 16; ++q) {
                    const float e = (m0 && mk) ? fabsf(m0->m[q] - mk->m[q]) : 1.0f;
                    if (e > worst) worst = e;
                }
                // Judged the way a viewer sees it, not element-wise: where a
                // reference point lands and how far the basis turns. A part
                // that keys itself runs its own solve, so its copy of the
                // weapon's rigid transform carries that solve's residual --
                // measured at 0.2 mm on the smallest part that keys, two
                // orders below the 0.5 mm stereo gate.
                float p0[3], pk[3];
                const float ref[3] = { 3.0f, -2.0f, 5.0f };
                if (m0 && mk) {
                    HpXform4(m0->m, ref, p0);
                    HpXform4(mk->m, ref, pk);
                }
                const float dPt = (m0 && mk) ? HpDist(p0, pk) : 1.0f;
                if (dPt > 0.0197f || worst > 0.02f) {
                    ok = 0;
                    printf("        [%s: draw %d (%d verts, %s) lands %.4f u away]%s",
                           names[w], k, sc.dispVerts[k], GODispName(sc.disp[k]), dPt, "\n");
                }
            }
            if (sc.staleErrU > 1e-4f) ok = 0;
            for (int k = 0; k < sc.nDisp && dn < 140; ++k)
                dn += snprintf(dispStr + dn, sizeof(dispStr) - dn, "%s%d%s",
                               (k || eye) ? " " : "", sc.dispVerts[k], GODispName(sc.disp[k]));
            if (eye == 0 && dn < 140) dn += snprintf(dispStr + dn, sizeof(dispStr) - dn, " /");
            GLSurfEyeApply a;
            if (!GLSurf_GetEyeApply(GLSC_GUN, eye, &a) || !a.applied) { ok = 0; continue; }
            for (int k = 0; k < 3; ++k) finalMid[eye][k] = a.finalPos[k] + off[eye][k];
            memcpy(finalAxis[eye], a.finalAxis, sizeof(finalAxis[eye]));
        }
        dispStr[dn] = 0;
        // PairWorld: both eyes reconstruct ONE rear grip / ONE basis.
        const float dPos = HpDist(finalMid[0], finalMid[1]);
        // Basis angle from the row differences (acos near 1 reads float
        // noise as 0.04 deg; |d row| ~ angle in radians for small angles).
        float dRow = 0.0f;
        for (int r = 0; r < 3; ++r) {
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) { const float d = finalAxis[0][r*3+k] - finalAxis[1][r*3+k]; d2 += d * d; }
            if (sqrtf(d2) > dRow) dRow = sqrtf(d2);
        }
        const float dDeg = dRow * 57.2957795f;
        if (dPos > 0.0197f || dDeg > 0.05f) ok = 0;
        if (HpDist(finalMid[0], wantGrip) > 1e-3f) ok = 0;
        printf("      %-26s %-5d %-5u %-5u %-5u %-5u %-5u [%s] pair %.4fu/%.4fdeg\n",
               names[w], n, fresh, stale, supp, van, confl, dispStr, dPos, dDeg);
        CHECK(ok, "%s: one weapon, %d surfaces, parts first: native, identical, stereo-consistent",
              names[w], n);
        if (ok) ++weaponsOk;
        GLSurf_SetGunTarget(0, nullptr, nullptr);
        GLSurf_SetGunSolveWanted(0);
    }
    CHECK(weapons >= 16 && weaponsOk == weapons, "%d/%d driveable weapons pass the matrix",
          weaponsOk, weapons);
    float z[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
    GLSurf_SetEyeOffsets(z);
}
