// ===========================================================================
// THREE-OBJECT RENDER OWNERSHIP -- the central architectural gate
// ===========================================================================
//
// The project-lead requirement is that GUN, RIGHT HAND/ARM and LEFT HAND/ARM
// be three INDEPENDENTLY OWNED rendered objects: the gun must not carry the
// hands, and neither hand may inherit the other's or the weapon's transform.
//
// WHY THIS FILE EXISTS SEPARATELY FROM THE POSE TESTS.
//
// vr_objects/vr_native_hands already prove that the PUBLISHED POSES are
// independent. That is not the same claim. A build can publish three perfectly
// independent poses and still render one rigid viewmodel, because publishing a
// pose and moving geometry are different things -- and the 2026-08-28 hardware
// run is exactly what that looks like from the outside. So every assertion
// here goes through the REAL production path
//
//     GLSurf_OnDrawElements -> ClassifyBatch -> MergedHalves / BatchPrimaryClass
//         -> VerifyMidpointSplit -> DrawOneHalf -> TransformDraw*
//
// and then reads the ACTUAL GEOMETRY the layer handed the driver: for every
// issued draw, every referenced vertex is pushed through the matrix that draw
// was rendered under. Nothing here inspects a pose struct.
//
// THE SUBMITTED LAYOUT IS THE REAL ONE. The engine rebinds glVertexPointer per
// batch and indexes 0-based within it (the live capture records span=1674 for
// the merged hand batch), so the fixture does the same, with a disjoint vertex
// block per object so "which geometry moved" is unambiguous.

#include "../src/vr_glsurf.h"

// The asset-derived ownership table, compiled into the TEST as well as into
// the layer. Asserting the two agree is what stops the classifier drifting
// away from the shipped rig -- every laterality error in this project came
// from a human reading a screenshot instead of the skin weights.
#include "../src/vr_render_ownership.inc"

enum {
    OWN_LEFT_VERTS   = GLS_HAND_VERTS,      // 837, submitted [0,837)
    OWN_RIGHT_VERTS  = GLS_HAND_VERTS,      // 837, submitted [837,1674)
    OWN_SLEEVE_VERTS = GLS_SLEEVE_VERTS,    // 120 each
    OWN_GUN_VERTS    = 486,
};

// Separate vertex blocks, so a transform applied to the wrong object is
// visible as geometry rather than as a matrix comparison.
static float    g_ownHandVerts[GLS_HAND_VERTS * 2 * 3];
static float    g_ownSleeveVerts[GLS_SLEEVE_VERTS * 2 * 3];
static float    g_ownGunVerts[OWN_GUN_VERTS * 3];
static float    g_ownNorms[GLS_HAND_VERTS * 2 * 3];
static unsigned g_ownHandIdx[GLS_HAND_INDICES * 2];
static unsigned g_ownSleeveIdx[GLS_SLEEVE_INDICES * 2];
static unsigned g_ownGunIdx[OWN_GUN_VERTS];

// CoD submits 1 unit = 1 inch (proven live), so the brief's centimetres land
// here.
static float OwnCm(float cm) { return cm * 0.393701f; }

static void OwnBuildGeometry()
{
    for (int i = 0; i < GLS_HAND_INDICES; ++i) {
        g_ownHandIdx[i] = (unsigned)(i % GLS_HAND_VERTS);
        g_ownHandIdx[GLS_HAND_INDICES + i] =
            (unsigned)(GLS_HAND_VERTS + (i % GLS_HAND_VERTS));
    }
    for (int i = 0; i < GLS_SLEEVE_INDICES; ++i) {
        g_ownSleeveIdx[i] = (unsigned)(i % GLS_SLEEVE_VERTS);
        g_ownSleeveIdx[GLS_SLEEVE_INDICES + i] =
            (unsigned)(GLS_SLEEVE_VERTS + (i % GLS_SLEEVE_VERTS));
    }
    for (int i = 0; i < OWN_GUN_VERTS; ++i) g_ownGunIdx[i] = (unsigned)i;

    // Distinct, well-separated clouds: a wrong owner cannot coincidentally
    // produce the right centroid.
    for (int i = 0; i < GLS_HAND_VERTS * 2; ++i) {
        const float t = (float)i * 0.01f;
        const int right = (i >= GLS_HAND_VERTS);
        g_ownHandVerts[i*3+0] = (right ? 6.0f : -6.0f) + (float)(i % 7) * 0.1f;
        g_ownHandVerts[i*3+1] = -3.0f + (float)(i % 5) * 0.2f;
        g_ownHandVerts[i*3+2] = -12.0f + t * 0.05f;
        g_ownNorms[i*3+0] = 1.0f; g_ownNorms[i*3+1] = 0.0f; g_ownNorms[i*3+2] = 0.0f;
    }
    for (int i = 0; i < GLS_SLEEVE_VERTS * 2; ++i) {
        const int right = (i >= GLS_SLEEVE_VERTS);
        g_ownSleeveVerts[i*3+0] = (right ? 7.5f : -7.5f) + (float)(i % 3) * 0.1f;
        g_ownSleeveVerts[i*3+1] = -5.0f + (float)(i % 4) * 0.2f;
        g_ownSleeveVerts[i*3+2] = -9.0f;
    }
    for (int i = 0; i < OWN_GUN_VERTS; ++i) {
        g_ownGunVerts[i*3+0] = 0.5f + (float)(i % 11) * 0.1f;
        g_ownGunVerts[i*3+1] = -1.0f + (float)(i % 9) * 0.1f;
        g_ownGunVerts[i*3+2] = -16.0f + (float)(i % 13) * 0.2f;
    }
}

// ---- rigid matrices, GL column-major --------------------------------------
static void OwnRigid(float m[16], const float R[9], float tx, float ty, float tz)
{
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            m[c * 4 + r] = R[r * 3 + c];
    m[3] = m[7] = m[11] = 0.0f;
    m[12] = tx; m[13] = ty; m[14] = tz; m[15] = 1.0f;
}
static void OwnRotX(float R[9], float d)      // pitch
{
    const float c = cosf(d * 0.01745329252f), s = sinf(d * 0.01745329252f);
    R[0]=1; R[1]=0; R[2]=0;  R[3]=0; R[4]=c; R[5]=-s;  R[6]=0; R[7]=s; R[8]=c;
}
static void OwnRotY(float R[9], float d)      // yaw
{
    const float c = cosf(d * 0.01745329252f), s = sinf(d * 0.01745329252f);
    R[0]=c; R[1]=0; R[2]=s;  R[3]=0; R[4]=1; R[5]=0;  R[6]=-s; R[7]=0; R[8]=c;
}
static void OwnRotZ(float R[9], float d)      // roll
{
    const float c = cosf(d * 0.01745329252f), s = sinf(d * 0.01745329252f);
    R[0]=c; R[1]=-s; R[2]=0;  R[3]=s; R[4]=c; R[5]=0;  R[6]=0; R[7]=0; R[8]=1;
}

static void OwnApply(const float m[16], const float v[3], float out[3])
{
    out[0] = m[0]*v[0] + m[4]*v[1] + m[8] *v[2] + m[12];
    out[1] = m[1]*v[0] + m[5]*v[1] + m[9] *v[2] + m[13];
    out[2] = m[2]*v[0] + m[6]*v[1] + m[10]*v[2] + m[14];
}

// THE THREE TRANSFORMS THE BRIEF SPECIFIES.
static void OwnGunM(float m[16])   { float R[9]; OwnRotY(R,  20.0f); OwnRigid(m, R, OwnCm(30.0f), 0, 0); }
static void OwnRightM(float m[16]) { float R[9]; OwnRotZ(R,  25.0f); OwnRigid(m, R, 0, OwnCm(15.0f), 0); }
static void OwnLeftM(float m[16])  { float R[9]; OwnRotX(R, -30.0f); OwnRigid(m, R, 0, OwnCm(-20.0f), 0); }

// ---------------------------------------------------------------------------
// One rendered scene, and what geometry each ownership group actually got.
// ---------------------------------------------------------------------------
enum { OWNG_GUN = 0, OWNG_RIGHT, OWNG_LEFT, OWNG_SLEEVE_R, OWNG_SLEEVE_L,
       OWNG_COUNT };
static const char* OwnGroupName(int g)
{
    static const char* n[OWNG_COUNT] =
        { "GUN", "RIGHT_HAND_ARM", "LEFT_HAND_ARM", "RIGHT_SLEEVE", "LEFT_SLEEVE" };
    return n[g];
}

struct OwnScene {
    int   seen[OWNG_COUNT];        // that group's geometry was issued
    float centroid[OWNG_COUNT][3]; // transformed centroid actually rendered
    int   draws;
};

// Push one recorded draw's referenced vertices through the matrix it was
// rendered under, and accumulate the centroid for its ownership group.
static void OwnAccum(OwnScene* sc, const GLRec* rec, const float* base,
                     int group, int lo, int hi)
{
    if (!rec) return;
    double acc[3] = { 0, 0, 0 };
    int n = 0;
    for (int i = 0; i < rec->count; ++i) {
        const unsigned vi = ((const unsigned*)rec->ptr)[i];
        if ((int)vi < lo || (int)vi >= hi) continue;
        float o[3];
        OwnApply(rec->m, base + (size_t)vi * 3, o);
        acc[0] += o[0]; acc[1] += o[1]; acc[2] += o[2];
        ++n;
    }
    if (!n) return;
    sc->seen[group] = 1;
    for (int k = 0; k < 3; ++k) sc->centroid[group][k] = (float)(acc[k] / n);
}

// Render the whole first-person scene through the PRODUCTION entry point and
// collect the geometry each ownership group ended up with.
//
// A draw the layer does NOT claim is still rendered -- by the engine, under the
// engine's own modelview -- and GLSurf_OnDrawElements returns 0 without going
// near the recorder. That case is the "untouched" answer, not a missing one, so
// it is accumulated against the engine matrix.
static void OwnAccumBatch(OwnScene* sc, const float* base, const unsigned* idx,
                          int count, int groupA, int loA, int hiA,
                          int groupB, int loB, int hiB, int claimed)
{
    if (claimed && CountKind(REC_DRAW) > 0) {
        const GLRec* a = NthKind(REC_DRAW, 0);
        const GLRec* b = NthKind(REC_DRAW, 1);
        OwnAccum(sc, a, base, groupA, loA, hiA);
        OwnAccum(sc, b ? b : a, base, groupB, loB, hiB);
        return;
    }
    // Unclaimed: the engine issues it verbatim under g_mockModelview.
    GLRec fake; memset(&fake, 0, sizeof(fake));
    fake.kind = REC_DRAW; fake.count = count; fake.ptr = idx;
    memcpy(fake.m, g_mockModelview, sizeof(fake.m));
    OwnAccum(sc, &fake, base, groupA, loA, hiA);
    OwnAccum(sc, &fake, base, groupB, loB, hiB);
}

static void OwnEnterPass(const float* vptr)
{
    GLSurf_ScenePhase(1);
    GLSurf_NoteDepthRange(0.0, 0.2);
    GLSurf_NoteEnable(GLS_GL_BLEND, 0);
    GLSurf_NoteViewport(0, 0, 1440, 1440);
    GLSurf_NoteClientState(GLS_GL_NORMAL_ARRAY, 1);
    GLSurf_NoteClientState(GLS_GL_VERTEX_ARRAY, 1);
    GLSurf_NoteNormalPointer(GLS_GL_FLOAT, 12, g_ownNorms);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, vptr);
}

static void OwnRenderScene(OwnScene* sc, const float* gun, const float* right,
                           const float* left, const float* sleeveR,
                           const float* sleeveL)
{
    memset(sc, 0, sizeof(*sc));

    // The layer is reset FIRST, then the deltas are installed -- installing
    // before the reset silently wipes them, which is how the first draft of
    // this case reported "0 draws".
    ResetLayerForCase();
    GLSurf_SetTransformEnabled(1);
    float id[16]; MockIdentity(id);
    GLSurf_SetClassDelta(GLSC_GUN,      gun      ? gun      : id);
    GLSurf_SetClassDelta(GLSC_HAND_R,   right    ? right    : id);
    GLSurf_SetClassDelta(GLSC_HAND_L,   left     ? left     : id);
    GLSurf_SetClassDelta(GLSC_SLEEVE_R, sleeveR  ? sleeveR  : id);
    GLSurf_SetClassDelta(GLSC_SLEEVE_L, sleeveL  ? sleeveL  : id);

    // ---- the merged hand batch -------------------------------------------
    OwnEnterPass(g_ownHandVerts);
    RecReset();
    const int cH = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                                         GLS_GL_UNSIGNED_INT, g_ownHandIdx);
    sc->draws += CountKind(REC_DRAW);
    {
        const int firstCls  = kRigOwnership[0].handMergedFirst;
        const int secondCls = kRigOwnership[0].handMergedSecond;
        OwnAccumBatch(sc, g_ownHandVerts, g_ownHandIdx, GLS_HAND_INDICES * 2,
                      firstCls  == GLSC_HAND_L ? OWNG_LEFT : OWNG_RIGHT,
                      0, GLS_HAND_VERTS,
                      secondCls == GLSC_HAND_L ? OWNG_LEFT : OWNG_RIGHT,
                      GLS_HAND_VERTS, GLS_HAND_VERTS * 2, cH);
    }

    // ---- the merged sleeve batch -----------------------------------------
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_ownSleeveVerts);
    RecReset();
    const int cS = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_SLEEVE_INDICES * 2,
                                         GLS_GL_UNSIGNED_INT, g_ownSleeveIdx);
    sc->draws += CountKind(REC_DRAW);
    {
        const int firstCls  = kRigOwnership[0].sleeveMergedFirst;
        const int secondCls = kRigOwnership[0].sleeveMergedSecond;
        OwnAccumBatch(sc, g_ownSleeveVerts, g_ownSleeveIdx,
                      GLS_SLEEVE_INDICES * 2,
                      firstCls  == GLSC_SLEEVE_L ? OWNG_SLEEVE_L : OWNG_SLEEVE_R,
                      0, GLS_SLEEVE_VERTS,
                      secondCls == GLSC_SLEEVE_L ? OWNG_SLEEVE_L : OWNG_SLEEVE_R,
                      GLS_SLEEVE_VERTS, GLS_SLEEVE_VERTS * 2, cS);
    }

    // ---- the weapon ------------------------------------------------------
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_ownGunVerts);
    RecReset();
    const int cG = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, OWN_GUN_VERTS,
                                         GLS_GL_UNSIGNED_INT, g_ownGunIdx);
    sc->draws += CountKind(REC_DRAW);
    OwnAccumBatch(sc, g_ownGunVerts, g_ownGunIdx, OWN_GUN_VERTS,
                  OWNG_GUN, 0, OWN_GUN_VERTS,
                  OWNG_GUN, 0, OWN_GUN_VERTS, cG);
}

// The UNTRANSFORMED scene, for byte-identity comparisons.
static OwnScene g_ownBaseline;

static int OwnSame(const float a[3], const float b[3], float tol)
{
    for (int k = 0; k < 3; ++k) if (fabsf(a[k] - b[k]) > tol) return 0;
    return 1;
}

// Which of the candidate transforms did this group's geometry actually
// receive? Compared against the BASELINE geometry pushed through each
// candidate, so the answer is about geometry, not about matrices.
static int OwnWhichTransform(const OwnScene* sc, int group,
                             const float* cand[], int nCand)
{
    if (!sc->seen[group]) return -2;
    for (int i = 0; i < nCand; ++i) {
        float want[3];
        OwnApply(cand[i], g_ownBaseline.centroid[group], want);
        if (OwnSame(sc->centroid[group], want, 1e-3f)) return i;
    }
    if (OwnSame(sc->centroid[group], g_ownBaseline.centroid[group], 1e-4f))
        return -1;                                  // untouched
    return -3;                                      // something else entirely
}

// Does the scene show TRUE three-object separation? Returns 1 only when each
// owner received its own transform and nothing else did.
// Used both for the positive gate and for the mutants, which must return 0.
static int OwnSeparationHolds(const float* gun, const float* right,
                              const float* left)
{
    float g[16], r[16], l[16];
    OwnGunM(g); OwnRightM(r); OwnLeftM(l);
    const float* cand[3] = { g, r, l };
    OwnScene sc; OwnRenderScene(&sc, gun, right, left, nullptr, nullptr);
    const int got[3] = {
        OwnWhichTransform(&sc, OWNG_GUN,   cand, 3),
        OwnWhichTransform(&sc, OWNG_RIGHT, cand, 3),
        OwnWhichTransform(&sc, OWNG_LEFT,  cand, 3),
    };
    return got[0] == 0 && got[1] == 1 && got[2] == 2;
}

// ===========================================================================
static void S_ThreeObjectRenderSeparation()
{
    printf("--- three-object render separation: GUN / RIGHT arm / LEFT arm ---\n");
    OwnBuildGeometry();

    float gunM[16], rightM[16], leftM[16], id[16];
    OwnGunM(gunM); OwnRightM(rightM); OwnLeftM(leftM); MockIdentity(id);

    // ---- 0: the classifier agrees with the SHIPPED ASSET -----------------
    //
    // The mapping the layer uses and the mapping derived from skin weights are
    // the same table, so this is a tautology by construction -- which is the
    // point. Before it was generated, the two lived in a comment that claimed
    // MergedHalves and BatchPrimaryClass agreed while they used opposite
    // orders (correctly, because the two rigs declare surfaces in opposite
    // orders -- but nothing was checking).
    {
        const CodvrRigOwnership& merged = kRigOwnership[0];   // hands_new4
        const CodvrRigOwnership& single = kRigOwnership[2];   // hands4
        CHECK(merged.handMergedFirst == GLSC_HAND_L
              && merged.handMergedSecond == GLSC_HAND_R,
              "own: merged rig declares surface 0 = LEFT, surface 1 = RIGHT");
        CHECK(merged.sleeveMergedFirst == GLSC_SLEEVE_R
              && merged.sleeveMergedSecond == GLSC_SLEEVE_L,
              "own: ...and its SLEEVES in the OPPOSITE order (2=R, 3=L)");
        CHECK(single.handSingleFirst == GLSC_HAND_R
              && single.handSingleSecond == GLSC_HAND_L,
              "own: the singles rig (hands4) declares hands R-then-L -- the two "
              "paths legitimately differ because the ASSETS differ");
        int impure = 0, seam = 0;
        for (int r = 0; r < 3; ++r)
            for (int i = 0; i < kRigOwnership[r].nSurf; ++i) {
                if (!kRigOwnership[r].surf[i].pure) ++impure;
                seam += kRigOwnership[r].surf[i].seamTris;
            }
        CHECK(impure == 0 && seam == 0,
              "own: every surface of every rig is STRUCTURALLY PURE -- %d impure, "
              "%d seam triangles (the tie policy is defined but unreachable)",
              impure, seam);
        CHECK(CODVR_TRIGGER_IS_ANATOMICAL_RIGHT == 1,
              "own: LATERALITY -- the rear-grip palm is anatomically RIGHT, "
              "unanimous across the sampled weapons");
    }

    // ---- baseline: nothing installed -------------------------------------
    OwnRenderScene(&g_ownBaseline, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK(g_ownBaseline.seen[OWNG_GUN] && g_ownBaseline.seen[OWNG_RIGHT]
          && g_ownBaseline.seen[OWNG_LEFT],
          "own: the production path issues gun, RIGHT and LEFT geometry (%d draws)",
          g_ownBaseline.draws);
    CHECK(g_ownBaseline.seen[OWNG_SLEEVE_R] && g_ownBaseline.seen[OWNG_SLEEVE_L],
          "own: ...and both sleeves, as separate owned objects");

    // ---- 1: ALL THREE AT ONCE -------------------------------------------
    //
    // GUN +30 cm X and yaw +20; RIGHT +15 cm Y and roll +25; LEFT -20 cm Y and
    // pitch -30, installed simultaneously. Each owner must receive ONLY its own.
    {
        OwnScene sc; OwnRenderScene(&sc, gunM, rightM, leftM, nullptr, nullptr);
        const float* cand[3] = { gunM, rightM, leftM };
        const int gg = OwnWhichTransform(&sc, OWNG_GUN,   cand, 3);
        const int gr = OwnWhichTransform(&sc, OWNG_RIGHT, cand, 3);
        const int gl = OwnWhichTransform(&sc, OWNG_LEFT,  cand, 3);
        CHECK(gg == 0, "own: GUN geometry received ONLY the gun transform (got %d)", gg);
        CHECK(gr == 1, "own: RIGHT hand/arm geometry received ONLY the RIGHT "
                       "transform (got %d)", gr);
        CHECK(gl == 2, "own: LEFT hand/arm geometry received ONLY the LEFT "
                       "transform (got %d)", gl);
        CHECK(gg != gr && gr != gl && gg != gl,
              "own: three DISTINCT transforms reached three distinct objects");
        CHECK(OwnSeparationHolds(gunM, rightM, leftM) == 1,
              "own: THREE-OBJECT SEPARATION HOLDS");
    }

    // ---- 2: GUN ONLY -- hands/arms byte-identical ------------------------
    {
        OwnScene sc; OwnRenderScene(&sc, gunM, nullptr, nullptr, nullptr, nullptr);
        const float* cand[1] = { gunM };
        CHECK(OwnWhichTransform(&sc, OWNG_GUN, cand, 1) == 0,
              "own: GUN ONLY -- the gun moves");
        CHECK(OwnSame(sc.centroid[OWNG_RIGHT], g_ownBaseline.centroid[OWNG_RIGHT], 1e-5f),
              "own: GUN ONLY -- RIGHT hand/arm geometry is byte-identical");
        CHECK(OwnSame(sc.centroid[OWNG_LEFT], g_ownBaseline.centroid[OWNG_LEFT], 1e-5f),
              "own: GUN ONLY -- LEFT hand/arm geometry is byte-identical");
        CHECK(OwnSame(sc.centroid[OWNG_SLEEVE_R], g_ownBaseline.centroid[OWNG_SLEEVE_R], 1e-5f)
              && OwnSame(sc.centroid[OWNG_SLEEVE_L], g_ownBaseline.centroid[OWNG_SLEEVE_L], 1e-5f),
              "own: GUN ONLY -- neither sleeve moves with the weapon "
              "(the arm cloth is NOT welded to the gun)");
    }

    // ---- 3: RIGHT ONLY -- gun and LEFT byte-identical --------------------
    {
        OwnScene sc; OwnRenderScene(&sc, nullptr, rightM, nullptr, nullptr, nullptr);
        const float* cand[1] = { rightM };
        CHECK(OwnWhichTransform(&sc, OWNG_RIGHT, cand, 1) == 0,
              "own: RIGHT ONLY -- the right hand/arm moves");
        CHECK(OwnSame(sc.centroid[OWNG_GUN], g_ownBaseline.centroid[OWNG_GUN], 1e-5f),
              "own: RIGHT ONLY -- the GUN is byte-identical "
              "(the hand does not carry the weapon)");
        CHECK(OwnSame(sc.centroid[OWNG_LEFT], g_ownBaseline.centroid[OWNG_LEFT], 1e-5f),
              "own: RIGHT ONLY -- the LEFT hand/arm is byte-identical");
    }

    // ---- 4: LEFT ONLY -- gun and RIGHT byte-identical --------------------
    {
        OwnScene sc; OwnRenderScene(&sc, nullptr, nullptr, leftM, nullptr, nullptr);
        const float* cand[1] = { leftM };
        CHECK(OwnWhichTransform(&sc, OWNG_LEFT, cand, 1) == 0,
              "own: LEFT ONLY -- the left hand/arm moves");
        CHECK(OwnSame(sc.centroid[OWNG_GUN], g_ownBaseline.centroid[OWNG_GUN], 1e-5f),
              "own: LEFT ONLY -- the GUN is byte-identical");
        CHECK(OwnSame(sc.centroid[OWNG_RIGHT], g_ownBaseline.centroid[OWNG_RIGHT], 1e-5f),
              "own: LEFT ONLY -- the RIGHT hand/arm is byte-identical");
    }

    // ---- 5: THE MUTANTS. Each MUST break separation. ---------------------
    //
    // A passing separation test is only worth what its mutants are worth: if
    // the check cannot tell a coupled build from an independent one, it is
    // decoration. Each mutant below is a specific way the architecture could
    // be wrong, wired into the REAL layer, and each must be caught.
    {
        // 1. one generic "viewmodel matrix" applied to everything
        CHECK(OwnSeparationHolds(gunM, gunM, gunM) == 0,
              "own: MUTANT 1 -- a single generic viewmodel matrix on all three "
              "is REJECTED");

        // 2. the gun's matrix applied to RIGHT
        CHECK(OwnSeparationHolds(gunM, gunM, leftM) == 0,
              "own: MUTANT 2 -- gun matrix on the RIGHT hand is REJECTED "
              "(the gun would be carrying the hand)");

        // 3. the gun's matrix applied to LEFT
        CHECK(OwnSeparationHolds(gunM, rightM, gunM) == 0,
              "own: MUTANT 3 -- gun matrix on the LEFT hand is REJECTED");

        // 4. RIGHT's matrix applied to both hands
        CHECK(OwnSeparationHolds(gunM, rightM, rightM) == 0,
              "own: MUTANT 4 -- one hand matrix on BOTH hands is REJECTED");

        // 5. swapped LEFT/RIGHT ownership
        CHECK(OwnSeparationHolds(gunM, leftM, rightM) == 0,
              "own: MUTANT 5 -- swapped LEFT/RIGHT ownership is REJECTED "
              "(this is the laterality regression, stated as geometry)");

        // and the honest control: the correct wiring still passes afterwards
        CHECK(OwnSeparationHolds(gunM, rightM, leftM) == 1,
              "own: after five mutants, the correct wiring still separates");
    }

    // ---- 6: the sleeves follow their OWN arm -----------------------------
    //
    // Structural ownership is only useful if it reaches the renderer. With
    // only the RIGHT hand moving, the RIGHT sleeve must move and the LEFT
    // sleeve must not.
    {
        OwnScene sc;
        OwnRenderScene(&sc, nullptr, rightM, nullptr, rightM, nullptr);
        const float* cand[1] = { rightM };
        CHECK(OwnWhichTransform(&sc, OWNG_SLEEVE_R, cand, 1) == 0,
              "own: the RIGHT sleeve takes the RIGHT arm's motion");
        CHECK(OwnSame(sc.centroid[OWNG_SLEEVE_L],
                      g_ownBaseline.centroid[OWNG_SLEEVE_L], 1e-5f),
              "own: ...and the LEFT sleeve does not move with it");
        GLSurf_ClearAllDeltas();
    }
}
