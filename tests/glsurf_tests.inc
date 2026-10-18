// ===========================================================================
// Render-surface layer (src/vr_glsurf.cpp) -- included by vr_safetest.cpp.
//
// The whole point of injecting the GL driver rather than linking it is that
// every claim in this milestone about "the gun moved and the world did not"
// can be proved with no game, no driver and no headset. The recorder below IS
// the driver as far as the layer is concerned, so the assertions are about the
// exact GL call sequence the layer would have issued.
// ===========================================================================

#include "../src/vr_glsurf.h"
#include "../src/vr_vbo.h"

// ------------------------------------------------------------ GL recorder

enum { GLREC_MAX = 256 };
enum { REC_DRAW = 1, REC_LOADMV = 2, REC_MATMODE = 3, REC_VPTR = 4, REC_NPTR = 5 };

struct GLRec {
    int         kind;
    unsigned    mode;
    int         count;
    unsigned    type;
    const void* ptr;
    float       m[16];
    int         size, stride;
};

static GLRec g_rec[GLREC_MAX];
static int   g_recN;
static float g_mockModelview[16];
static float g_mockProjection[16];
static int   g_mockViewport[4] = { 0, 0, 1440, 1440 };

static void RecReset()
{
    g_recN = 0;
    memset(g_rec, 0, sizeof(g_rec));
}

static void RecPush(const GLRec& r)
{
    if (g_recN < GLREC_MAX) g_rec[g_recN++] = r;
}

static void WINAPI Mock_DrawElements(unsigned mode, int count, unsigned type, const void* idx)
{
    GLRec r; memset(&r, 0, sizeof(r));
    r.kind = REC_DRAW; r.mode = mode; r.count = count; r.type = type; r.ptr = idx;
    memcpy(r.m, g_mockModelview, sizeof(r.m));      // what the draw actually saw
    RecPush(r);
}
static void WINAPI Mock_DrawArrays(unsigned, int, int) {}
static void WINAPI Mock_LoadMatrixf(const float* m)
{
    GLRec r; memset(&r, 0, sizeof(r));
    r.kind = REC_LOADMV;
    memcpy(r.m, m, sizeof(r.m));
    RecPush(r);
    memcpy(g_mockModelview, m, sizeof(g_mockModelview));
}
static void WINAPI Mock_MatrixMode(unsigned mode)
{
    GLRec r; memset(&r, 0, sizeof(r));
    r.kind = REC_MATMODE; r.mode = mode;
    RecPush(r);
}
static void WINAPI Mock_GetFloatv(unsigned pname, float* out)
{
    if (pname == GLS_GL_MODELVIEW_MATRIX)  memcpy(out, g_mockModelview, 16 * sizeof(float));
    else if (pname == GLS_GL_PROJECTION_MATRIX) memcpy(out, g_mockProjection, 16 * sizeof(float));
    else memset(out, 0, 16 * sizeof(float));
}
static void WINAPI Mock_GetIntegerv(unsigned, int* out) { if (out) *out = 0; }
static void WINAPI Mock_GetDoublev(unsigned, double* out) { if (out) *out = 0; }
static unsigned char WINAPI Mock_IsEnabled(unsigned) { return 0; }
static void WINAPI Mock_GetTexLevel(unsigned, int, unsigned, int* out) { if (out) *out = 64; }
static void WINAPI Mock_VertexPointer(int size, unsigned type, int stride, const void* p)
{
    GLRec r; memset(&r, 0, sizeof(r));
    r.kind = REC_VPTR; r.size = size; r.type = type; r.stride = stride; r.ptr = p;
    RecPush(r);
}
static void WINAPI Mock_NormalPointer(unsigned type, int stride, const void* p)
{
    GLRec r; memset(&r, 0, sizeof(r));
    r.kind = REC_NPTR; r.type = type; r.stride = stride; r.ptr = p;
    RecPush(r);
}

static void InstallMockDriver()
{
    GLSurfDriver d;
    memset(&d, 0, sizeof(d));
    d.drawElements = Mock_DrawElements;
    d.drawArrays   = Mock_DrawArrays;
    d.loadMatrixf  = Mock_LoadMatrixf;
    d.matrixMode   = Mock_MatrixMode;
    d.getFloatv    = Mock_GetFloatv;
    d.getIntegerv  = Mock_GetIntegerv;
    d.getDoublev   = Mock_GetDoublev;
    d.isEnabled    = Mock_IsEnabled;
    d.getTexLevelParameteriv = Mock_GetTexLevel;
    d.vertexPointer = Mock_VertexPointer;
    d.normalPointer = Mock_NormalPointer;
    GLSurf_SetDriver(&d);
}

// ------------------------------------------------------- synthetic geometry

// The merged hand batch as the engine really issues it: surfaces 0 and 1
// concatenated, first half referencing verts [0,837), second [837,1674).
static unsigned g_handIdx[GLS_HAND_INDICES * 2];
static unsigned g_sleeveIdx[GLS_SLEEVE_INDICES * 2];
static float    g_verts[4096 * 3];
static float    g_norms[4096 * 3];

static void BuildSyntheticGeometry(bool crossing = false)
{
    for (int i = 0; i < GLS_HAND_INDICES; ++i)
        g_handIdx[i] = (unsigned)(i % GLS_HAND_VERTS);
    for (int i = 0; i < GLS_HAND_INDICES; ++i)
        g_handIdx[GLS_HAND_INDICES + i] =
            (unsigned)(GLS_HAND_VERTS + (i % GLS_HAND_VERTS));
    if (crossing)                                 // one triangle reaching across
        g_handIdx[3] = (unsigned)(GLS_HAND_VERTS + 5);

    for (int i = 0; i < GLS_SLEEVE_INDICES; ++i)
        g_sleeveIdx[i] = (unsigned)(i % GLS_SLEEVE_VERTS);
    for (int i = 0; i < GLS_SLEEVE_INDICES; ++i)
        g_sleeveIdx[GLS_SLEEVE_INDICES + i] =
            (unsigned)(GLS_SLEEVE_VERTS + (i % GLS_SLEEVE_VERTS));

    for (int i = 0; i < 4096; ++i) {
        g_verts[i*3+0] = (float)(i % 17) - 8.0f;
        g_verts[i*3+1] = (float)(i % 13) - 6.0f;
        g_verts[i*3+2] = (float)(i % 11) - 5.0f;
        g_norms[i*3+0] = 1.0f; g_norms[i*3+1] = 0.0f; g_norms[i*3+2] = 0.0f;
    }
}

static void MockIdentity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void MockTranslate(float m[16], float x, float y, float z)
{
    MockIdentity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

// Put the layer into "a 3D scene is running and the viewmodel is being drawn".
static void EnterViewmodelPass()
{
    GLSurf_ScenePhase(1);
    GLSurf_NoteDepthRange(0.0, 0.2);              // the measured RF_DEPTHHACK
    GLSurf_NoteEnable(GLS_GL_BLEND, 0);
    GLSurf_NoteViewport(0, 0, 1440, 1440);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_verts);
    GLSurf_NoteNormalPointer(GLS_GL_FLOAT, 12, g_norms);
    GLSurf_NoteClientState(GLS_GL_NORMAL_ARRAY, 1);
    GLSurf_NoteClientState(GLS_GL_VERTEX_ARRAY, 1);
}

static void ResetLayerForCase()
{
    GLSurf_Reset();
    VBO_Reset();           // buffer bindings/shadows must not leak across cases
    InstallMockDriver();
    MockIdentity(g_mockModelview);
    MockIdentity(g_mockProjection);
    g_mockProjection[0] = 2.0f; g_mockProjection[5] = 2.0f;
    g_mockProjection[11] = -1.0f; g_mockProjection[15] = 0.0f;
    RecReset();
    GLSurf_BeginFrame();
    GLSurf_SceneQueued(0);
}

static int CountKind(int kind)
{
    int n = 0;
    for (int i = 0; i < g_recN; ++i) if (g_rec[i].kind == kind) ++n;
    return n;
}

static const GLRec* NthKind(int kind, int n)
{
    for (int i = 0; i < g_recN; ++i)
        if (g_rec[i].kind == kind && n-- == 0) return &g_rec[i];
    return nullptr;
}

// ===========================================================================
// 1. CLASSIFICATION
// ===========================================================================
static void S_GLSurfClassification()
{
    printf("--- render surface: classification ---\n");
    BuildSyntheticGeometry();

    // ---- the merged hand batch is recognised and split -------------------
    ResetLayerForCase();
    EnterViewmodelPass();
    float d[16]; MockTranslate(d, 4.0f, 0, 0);
    GLSurf_SetClassDelta(GLSC_HAND_R, d);
    int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                                      GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 1, "merged hand batch (7824 idx) is claimed by the surface layer");
    Check(CountKind(REC_DRAW) == 2,
          "merged hand batch is re-issued as TWO sub-draws (got %d)", CountKind(REC_DRAW));
    const GLRec* a = NthKind(REC_DRAW, 0);
    const GLRec* b = NthKind(REC_DRAW, 1);
    Check(a && a->count == GLS_HAND_INDICES && b && b->count == GLS_HAND_INDICES,
          "each sub-draw carries exactly one surface's 3912 indices");
    Check(b && b->ptr == (const void*)(g_handIdx + GLS_HAND_INDICES),
          "the second sub-draw starts at index 3912 of the same buffer");
    Check(GLSurf_SplitVerifiedCount() == 1 && GLSurf_SplitRejectedCount() == 0,
          "the midpoint split was verified against this frame's index buffer");

    // ---- a batch whose halves cross is REFUSED ---------------------------
    BuildSyntheticGeometry(true);
    ResetLayerForCase();
    EnterViewmodelPass();
    GLSurf_SetClassDelta(GLSC_HAND_R, d);
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                                  GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 0, "a hand batch whose halves cross is NOT claimed (fails closed)");
    Check(GLSurf_SplitRejectedCount() == 1, "the crossing batch is counted as rejected");
    Check(CountKind(REC_DRAW) == 0, "a rejected batch issues no draw of our own");
    BuildSyntheticGeometry();

    // ---- sleeves -------------------------------------------------------
    ResetLayerForCase();
    EnterViewmodelPass();
    GLSurf_SetClassDelta(GLSC_SLEEVE_L, d);
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_SLEEVE_INDICES * 2,
                                  GLS_GL_UNSIGNED_INT, g_sleeveIdx);
    Check(owned == 1, "merged sleeve batch (948 idx) is claimed and split");
    Check(CountKind(REC_DRAW) == 2, "sleeve batch becomes two sub-draws");

    // ---- the gun --------------------------------------------------------
    ResetLayerForCase();
    EnterViewmodelPass();
    GLSurf_SetClassDelta(GLSC_GUN, d);
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 1, "depth-hacked opaque non-hand geometry is claimed as the gun");
    Check(GLSurf_TestClassOfLastDraw() == GLSC_GUN, "and classified GLSC_GUN");

    // ---- everything that must NOT be claimed ----------------------------
    struct Neg { const char* what; double zn, zf; int inScene; int blend; int count; };
    static const Neg kNeg[] = {
        { "world geometry (depth range 0..1)",        0.0, 1.0, 1, 0, 1428 },
        { "the SKY (depth range 1..1)",               1.0, 1.0, 1, 0, 1428 },
        { "a HUD draw (not in the 3D pass)",          0.0, 0.2, 0, 0, 1428 },
        { "a world weapon at world depth",            0.0, 1.0, 1, 0, GLS_HAND_INDICES*2 },
        { "an NPC's geometry at world depth",         0.0, 1.0, 1, 0, 7824 },
    };
    for (size_t i = 0; i < sizeof(kNeg)/sizeof(kNeg[0]); ++i) {
        ResetLayerForCase();
        GLSurf_ScenePhase(kNeg[i].inScene);
        GLSurf_NoteDepthRange(kNeg[i].zn, kNeg[i].zf);
        GLSurf_NoteEnable(GLS_GL_BLEND, kNeg[i].blend);
        GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_verts);
        for (int c = 1; c < GLSC_COUNT; ++c) GLSurf_SetClassDelta(c, d);
        owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, kNeg[i].count,
                                      GLS_GL_UNSIGNED_INT, g_handIdx);
        Check(owned == 0 && CountKind(REC_DRAW) == 0,
              "REJECTED: %s", kNeg[i].what);
    }

    // ---- a blended draw inside the viewmodel run is not the gun ---------
    ResetLayerForCase();
    EnterViewmodelPass();
    GLSurf_NoteEnable(GLS_GL_BLEND, 1);
    GLSurf_SetClassDelta(GLSC_GUN, d);
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 0 && GLSurf_TestClassOfLastDraw() == GLSC_VM_OTHER,
          "a BLENDED depth-hacked draw (muzzle flash) is not claimed as the gun");
}

// ===========================================================================
// 2. THE TRANSFORM ITSELF
// ===========================================================================
static void S_GLSurfTransform()
{
    printf("--- render surface: transform and restore ---\n");
    BuildSyntheticGeometry();

    // ---- identity delta costs nothing ------------------------------------
    ResetLayerForCase();
    EnterViewmodelPass();
    int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 0 && g_recN == 0,
          "an identity delta issues NO GL call at all -- the engine's own draw stands");

    // ---- the delta is left-multiplied onto the engine's modelview --------
    ResetLayerForCase();
    EnterViewmodelPass();
    // z is NEGATIVE: eye space puts what the camera can see in front of it,
    // and the shift measurement refuses to project anything behind the eye.
    float engine[16]; MockTranslate(engine, 1.0f, 2.0f, -30.0f);
    memcpy(g_mockModelview, engine, sizeof(engine));
    float delta[16];  MockTranslate(delta, 10.0f, 0.0f, 0.0f);
    GLSurf_SetClassDelta(GLSC_GUN, delta);
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 1, "a non-identity delta makes the layer own the draw");

    float loaded[16], restored[16];
    Check(GLSurf_TestLastTransformed(loaded, restored) == 1,
          "the transform recorded what it loaded and what it restored");
    Check(fabsf(loaded[12] - 11.0f) < 1e-4f && fabsf(loaded[13] - 2.0f) < 1e-4f &&
          fabsf(loaded[14] + 30.0f) < 1e-4f,
          "delta*engine composes correctly: (1,2,-30) + 10 right -> (%.2f, %.2f, %.2f)",
          loaded[12], loaded[13], loaded[14]);
    Check(memcmp(restored, engine, sizeof(engine)) == 0,
          "the engine's modelview is restored BYTE-FOR-BYTE after the draw");

    // The recorded call order must be load(ours), draw, load(engine).
    Check(CountKind(REC_LOADMV) == 2 && CountKind(REC_DRAW) == 1,
          "exactly two matrix loads bracket exactly one draw");
    const GLRec* l0 = NthKind(REC_LOADMV, 0);
    const GLRec* dr = NthKind(REC_DRAW, 0);
    const GLRec* l1 = NthKind(REC_LOADMV, 1);
    Check(l0 && dr && l1 && memcmp(dr->m, l0->m, sizeof(l0->m)) == 0,
          "the draw executed under OUR matrix, not the engine's");
    Check(l1 && memcmp(l1->m, engine, sizeof(engine)) == 0,
          "the last matrix load puts the engine's own matrix back");
    Check(memcmp(g_mockModelview, engine, sizeof(engine)) == 0,
          "GL is left holding the engine's matrix when the layer returns");

    // ---- the measured pixel shift is non-zero and finite ------------------
    float sx = 0, sy = 0, mx = 0; unsigned ns = 0;
    GLSurf_GetShift(&sx, &sy, &mx, &ns);
    Check(ns >= 1 && (mx == mx && mx < 1e30f) && mx > 0.0f,
          "the transform reports a measured screen shift (%.2f px over %u samples)", mx, ns);

    // ---- scratch route: normals rotated, texcoords untouched -------------
    ResetLayerForCase();
    EnterViewmodelPass();
    GLSurf_SetRoute(GLS_ROUTE_SCRATCH_ID);
    float yaw90[16]; MockIdentity(yaw90);
    yaw90[0] = 0.0f; yaw90[8] = 1.0f; yaw90[2] = -1.0f; yaw90[10] = 0.0f;  // +90 about Y
    GLSurf_SetClassDelta(GLSC_GUN, yaw90);
    RecReset();
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 1, "the scratch route also owns the draw");
    Check(CountKind(REC_VPTR) == 2,
          "scratch route repoints the vertex array and then restores it");
    Check(CountKind(REC_NPTR) == 2,
          "scratch route repoints the normal array and then restores it");
    const GLRec* v0 = NthKind(REC_VPTR, 0);
    const GLRec* v1 = NthKind(REC_VPTR, 1);
    Check(v0 && v0->ptr != (const void*)g_verts,
          "the draw reads CODVR-owned scratch, never the engine's vertex array");
    Check(v1 && v1->ptr == (const void*)g_verts && v1->stride == 12 && v1->size == 3,
          "the engine's own vertex pointer/stride/size are restored exactly");
    const GLRec* n1 = NthKind(REC_NPTR, 1);
    Check(n1 && n1->ptr == (const void*)g_norms,
          "the engine's own normal pointer is restored exactly");
    // The engine's arrays must be unmodified: this is the "never write engine
    // memory" rule, and it is checked on the actual bytes.
    Check(fabsf(g_norms[0] - 1.0f) < 1e-6f && fabsf(g_norms[2]) < 1e-6f,
          "the engine's NORMAL data is untouched (transform went to scratch)");
    Check(fabsf(g_verts[0] - (-8.0f)) < 1e-6f,
          "the engine's VERTEX data is untouched (transform went to scratch)");
    GLSurf_SetRoute(GLS_ROUTE_MODELVIEW_ID);
}

// ===========================================================================
// 3. LEFT / RIGHT INDEPENDENCE
// ===========================================================================
static void S_GLSurfHandIndependence()
{
    printf("--- render surface: hands move independently ---\n");
    BuildSyntheticGeometry();

    // Move ONLY the right hand; the left half must render under the engine's
    // own matrix, unchanged.
    ResetLayerForCase();
    EnterViewmodelPass();
    float engine[16]; MockTranslate(engine, 0, 0, -10.0f);
    memcpy(g_mockModelview, engine, sizeof(engine));
    float right[16]; MockTranslate(right, 7.874f, 0, 0);       // +20 cm
    GLSurf_SetClassDelta(GLSC_HAND_R, right);
    RecReset();
    int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                                      GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 1, "the merged batch is split so one side can move alone");
    // RE-CORRECTED order (2026-08-28): half 0 is the LEFT (support) hand,
    // half 1 the RIGHT (trigger) hand -- the asset surface order, which the
    // live modelview + muzzle physics proved truthful. See MergedHalves().
    const GLRec* left   = NthKind(REC_DRAW, 0);
    const GLRec* rightD = NthKind(REC_DRAW, 1);
    Check(left && memcmp(left->m, engine, sizeof(engine)) == 0,
          "the LEFT hand still renders under the engine's own matrix");
    Check(rightD && fabsf(rightD->m[12] - 7.874f) < 1e-3f,
          "the RIGHT hand renders 20 cm to the right (%.3f units)",
          rightD ? rightD->m[12] : 0.0f);
    Check(memcmp(g_mockModelview, engine, sizeof(engine)) == 0,
          "the engine's matrix is restored after the split pair");

    // Now the mirror case: only the left moves.
    ResetLayerForCase();
    EnterViewmodelPass();
    memcpy(g_mockModelview, engine, sizeof(engine));
    float leftD[16]; MockTranslate(leftD, -7.874f, 0, 0);
    GLSurf_SetClassDelta(GLSC_HAND_L, leftD);
    RecReset();
    GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                          GLS_GL_UNSIGNED_INT, g_handIdx);
    const GLRec* l = NthKind(REC_DRAW, 0);
    const GLRec* r = NthKind(REC_DRAW, 1);
    Check(l && fabsf(l->m[12] + 7.874f) < 1e-3f,
          "the LEFT hand moves 20 cm left when only its own delta is set");
    Check(r && memcmp(r->m, engine, sizeof(engine)) == 0,
          "the RIGHT hand is unaffected by the left hand's delta");

    // Exact 20 cm on each axis, both hands, both directions.
    static const float kAxes[3][3] = { {7.874f,0,0}, {0,7.874f,0}, {0,0,7.874f} };
    for (int axis = 0; axis < 3; ++axis) {
        for (int side = 0; side < 2; ++side) {
            ResetLayerForCase();
            EnterViewmodelPass();
            MockIdentity(g_mockModelview);
            float m[16]; MockTranslate(m, kAxes[axis][0], kAxes[axis][1], kAxes[axis][2]);
            GLSurf_SetClassDelta(side ? GLSC_HAND_R : GLSC_HAND_L, m);
            RecReset();
            GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                                  GLS_GL_UNSIGNED_INT, g_handIdx);
            const GLRec* moved = NthKind(REC_DRAW, side ? 1 : 0);   // R = half 1
            const float got = moved ? moved->m[12 + axis] : 0.0f;
            Check(moved && fabsf(got - 7.874f) < 1e-3f,
                  "%s hand +20 cm on axis %d lands at %.3f units",
                  side ? "right" : "left", axis, got);
        }
    }

    // Suppression: hiding a side removes it and only it.
    ResetLayerForCase();
    EnterViewmodelPass();
    GLSurf_SetClassHidden(GLSC_SLEEVE_L, 1);
    GLSurf_SetClassHidden(GLSC_SLEEVE_R, 1);
    RecReset();
    int o = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_SLEEVE_INDICES * 2,
                                  GLS_GL_UNSIGNED_INT, g_sleeveIdx);
    Check(o == 1 && CountKind(REC_DRAW) == 0,
          "hiding both sleeves suppresses the whole sleeve batch");
    ResetLayerForCase();
    EnterViewmodelPass();
    GLSurf_SetClassHidden(GLSC_SLEEVE_R, 1);
    RecReset();
    GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_SLEEVE_INDICES * 2,
                          GLS_GL_UNSIGNED_INT, g_sleeveIdx);
    Check(CountKind(REC_DRAW) == 1,
          "hiding ONE sleeve still draws the other (got %d draws)", CountKind(REC_DRAW));
}

// ===========================================================================
// 4. PAIR LATCHING
// ===========================================================================
static void S_GLSurfPairLatch()
{
    printf("--- render surface: one object pose per XR pair ---\n");
    BuildSyntheticGeometry();

    ResetLayerForCase();
    GLSurf_BeginLatch(7);
    GLSurf_BeginFrame(); GLSurf_SceneQueued(0);
    GLSurf_BeginFrame(); GLSurf_SceneQueued(1);
    Check(GLSurf_EyeConsumedSerial(0) == 7 && GLSurf_EyeConsumedSerial(1) == 7,
          "both eyes of a pair consume the same latch serial");
    Check(GLSurf_PairMismatchCount() == 0, "a correct pair records no mismatch");

    // The mutant the invariant exists to catch: something re-latches between
    // the two eye renders, so eye 1 would see a different object pose.
    ResetLayerForCase();
    GLSurf_BeginLatch(11);
    GLSurf_BeginFrame(); GLSurf_SceneQueued(0);
    GLSurf_BeginLatch(12);                       // <-- forbidden mid-pair
    GLSurf_BeginFrame(); GLSurf_SceneQueued(1);
    Check(GLSurf_PairMismatchCount() == 1,
          "MUTANT DETECTED: re-latching between eye 0 and eye 1 is counted");
    Check(GLSurf_EyeConsumedSerial(0) != GLSurf_EyeConsumedSerial(1),
          "and the two eyes are recorded as having consumed different serials");

    // Deltas are pure state: nothing in this module samples a clock or a
    // controller, so eye 1 renders exactly what eye 0 did unless someone
    // pushes a new delta.
    ResetLayerForCase();
    GLSurf_BeginLatch(3);
    float m[16]; MockTranslate(m, 5.0f, 0, 0);
    GLSurf_SetClassDelta(GLSC_GUN, m);

    float mv[16]; MockTranslate(mv, 0, 0, -10.0f);
    float eye0Result[16], eye1Result[16];
    for (int eye = 0; eye < 2; ++eye) {
        GLSurf_BeginFrame();
        GLSurf_SceneQueued(eye);
        EnterViewmodelPass();
        memcpy(g_mockModelview, mv, sizeof(mv));
        RecReset();
        GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_handIdx);
        const GLRec* dr = NthKind(REC_DRAW, 0);
        memcpy(eye == 0 ? eye0Result : eye1Result, dr ? dr->m : mv, sizeof(mv));
    }
    Check(memcmp(eye0Result, eye1Result, sizeof(mv)) == 0,
          "eye 0 and eye 1 render the gun at an IDENTICAL world pose");
    Check(GLSurf_PairMismatchCount() == 0, "no mismatch across the rendered pair");
}

// ===========================================================================
// 5. THE CAMERA IS NEVER TOUCHED
// ===========================================================================
static void S_GLSurfCameraUntouched()
{
    printf("--- render surface: the camera is not part of this ---\n");
    BuildSyntheticGeometry();

    ResetLayerForCase();
    EnterViewmodelPass();
    float proj[16];
    memcpy(proj, g_mockProjection, sizeof(proj));
    float m[16]; MockTranslate(m, 9.0f, 3.0f, -2.0f);
    for (int c = 1; c < GLSC_COUNT; ++c) GLSurf_SetClassDelta(c, m);

    GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_handIdx);
    GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                          GLS_GL_UNSIGNED_INT, g_handIdx);
    GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_SLEEVE_INDICES * 2,
                          GLS_GL_UNSIGNED_INT, g_sleeveIdx);

    Check(memcmp(proj, g_mockProjection, sizeof(proj)) == 0,
          "the PROJECTION matrix is byte-identical after every viewmodel transform");
    Check(CountKind(REC_MATMODE) == 0,
          "the layer never changes the matrix mode while GL_MODELVIEW is current");

    // Every matrix the layer loaded must be a modelview it derived from the
    // engine's own -- it can never load a projection.
    bool projTouched = false;
    for (int i = 0; i < g_recN; ++i)
        if (g_rec[i].kind == REC_LOADMV && memcmp(g_rec[i].m, proj, sizeof(proj)) == 0)
            projTouched = true;
    Check(!projTouched, "no matrix load ever carries the projection matrix");

    // A world draw between two viewmodel draws must see untouched state.
    ResetLayerForCase();
    EnterViewmodelPass();
    GLSurf_SetClassDelta(GLSC_GUN, m);
    GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_handIdx);
    float afterVm[16];
    memcpy(afterVm, g_mockModelview, sizeof(afterVm));
    GLSurf_NoteDepthRange(0.0, 1.0);                       // back to the world
    RecReset();
    int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 0 && g_recN == 0,
          "the very next WORLD draw is untouched and issues no call of ours");
    Check(memcmp(afterVm, g_mockModelview, sizeof(afterVm)) == 0,
          "world draws see exactly the matrix the engine left set");
}

// ===========================================================================
// 6. STATE MIRROR AND FAIL-CLOSED BEHAVIOUR
// ===========================================================================
static void S_GLSurfFailClosed()
{
    printf("--- render surface: fails closed ---\n");
    BuildSyntheticGeometry();

    // No driver at all: the layer must never claim a draw it cannot issue.
    ResetLayerForCase();
    GLSurf_SetDriver(nullptr);
    EnterViewmodelPass();
    float m[16]; MockTranslate(m, 5.0f, 0, 0);
    GLSurf_SetClassDelta(GLSC_GUN, m);
    int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 0, "with no driver installed the layer declines the draw");
    InstallMockDriver();

    // Transform globally disabled.
    ResetLayerForCase();
    EnterViewmodelPass();
    GLSurf_SetClassDelta(GLSC_GUN, m);
    GLSurf_SetTransformEnabled(0);
    RecReset();
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 0 && g_recN == 0, "with transforms disabled nothing is claimed");
    GLSurf_SetTransformEnabled(1);

    // An index type the layer cannot read cannot be split.
    ResetLayerForCase();
    EnterViewmodelPass();
    GLSurf_SetClassDelta(GLSC_HAND_R, m);
    RecReset();
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                                  0x1234 /* not an index type */, g_handIdx);
    Check(owned == 0, "an unreadable index type is refused rather than guessed at");

    // A null index pointer (an element-buffer offset) cannot be verified.
    ResetLayerForCase();
    EnterViewmodelPass();
    GLSurf_SetClassDelta(GLSC_HAND_R, m);
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                                  GLS_GL_UNSIGNED_INT, nullptr);
    Check(owned == 0, "a batch whose indices live in a buffer object is refused");

    // Deltas start as the IDENTITY, not as zero. A zero 4x4 collapses every
    // vertex to the origin, and the first flat capture did exactly that.
    GLSurf_Reset();
    for (int c = 1; c < GLSC_COUNT; ++c) {
        float d[16];
        GLSurf_GetClassDelta(c, d);
        bool ident = fabsf(d[0]-1) < 1e-6f && fabsf(d[5]-1) < 1e-6f &&
                     fabsf(d[10]-1) < 1e-6f && fabsf(d[15]-1) < 1e-6f;
        Check(ident, "class %s starts at the IDENTITY, never at zero",
              GLSurf_ClassName(c));
    }

    // The probe helper builds a rigid transform, not a degenerate one.
    GLSurf_SetProbe(GLSC_GUN, 3.937f, 0, 0, 15.0f);
    float p[16];
    GLSurf_GetClassDelta(GLSC_GUN, p);
    const float det3 =
        p[0]*(p[5]*p[10]-p[9]*p[6]) - p[4]*(p[1]*p[10]-p[9]*p[2]) + p[8]*(p[1]*p[6]-p[5]*p[2]);
    Check(fabsf(det3 - 1.0f) < 1e-4f,
          "the probe delta is a rigid rotation (det = %.6f)", det3);
    Check(fabsf(p[12] - 3.937f) < 1e-4f, "the probe delta carries its translation");
    float other[16];
    GLSurf_GetClassDelta(GLSC_HAND_L, other);
    Check(fabsf(other[12]) < 1e-6f,
          "arming a probe on one class leaves every other class at identity");
}

// ===========================================================================
// The screen-shift metric.
//
// The first real sweep reported "shift last (146004688.00, 0.00) px" for a
// transform whose matrices were exactly right. That number came from
// projecting the batch's MATRIX ORIGIN: four of the seven viewmodel draws
// arrive already skinned into camera-relative coordinates, so their modelview
// translation is ~2e-5 and the origin lands on the eye itself. Clip w
// collapses, the 1e-6 guard does not fire, and a valid 10 cm move reports
// 1.5e8 px.
//
// These cases pin the replacement to the analytic answer, and keep the
// retired diagnostic around as a mutant so its failure stays demonstrated
// rather than remembered.
// ===========================================================================

// The projection the game actually runs: 1440x1440, infinite far, near = 4.
static void MockGameProjection(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 0.89382f; m[5] = 0.89382f;
    m[10] = -1.0f; m[11] = -1.0f; m[14] = -8.0f;
}

// A vertex array the test owns, so predictions are exact.
static float    g_mVerts[256 * 3];
static unsigned g_mIdx[1428];

static void BuildMetricGeometry(float z0, float zSpread)
{
    for (int i = 0; i < 256; ++i) {
        g_mVerts[i*3+0] = (float)((i % 7) - 3);
        g_mVerts[i*3+1] = (float)((i % 5) - 2);
        g_mVerts[i*3+2] = z0 + zSpread * (float)((i % 9) - 4) / 4.0f;
    }
    for (int i = 0; i < 1428; ++i) g_mIdx[i] = (unsigned)(i % 256);
}

static void S_GLSurfShiftMetric()
{
    printf("--- render surface: screen-shift metric ---\n");
    BuildSyntheticGeometry();

    // ---- it measures GEOMETRY, and agrees with the analytic answer -------
    //
    // Eye-space translation d along x moves a point at eye depth -z by
    //     dx_px = W/2 * pj0 * d / (-z)
    // and by EXACTLY ZERO vertically, because the projection has no shear.
    ResetLayerForCase();
    MockGameProjection(g_mockProjection);
    EnterViewmodelPass();
    BuildMetricGeometry(0.0f, 5.0f);              // model z in [-5, +5]
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_mVerts);

    float mv[16]; MockTranslate(mv, 0.0f, 0.0f, -30.0f);
    memcpy(g_mockModelview, mv, sizeof(mv));
    float dx10[16]; MockTranslate(dx10, 10.0f, 0.0f, 0.0f);
    GLSurf_SetClassDelta(GLSC_GUN, dx10);
    int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_mIdx);
    Check(owned == 1, "metric case: the gun batch was transformed");

    GLSurfShift sh; GLSurf_GetShiftMetrics(&sh);
    // Depth runs 25..35, so the displacement runs 720*0.89382*10/35 .. /25.
    const float lo = 720.0f * 0.89382f * 10.0f / 35.0f;   // ~183.9
    const float hi = 720.0f * 0.89382f * 10.0f / 25.0f;   // ~257.4
    Check(sh.validSamples > 0, "metric: real vertices were sampled (%u)", sh.validSamples);
    Check(sh.medianPx > lo - 1.0f && sh.medianPx < hi + 1.0f,
          "metric: median %.1f px lies in the analytic band %.1f..%.1f",
          sh.medianPx, lo, hi);
    Check(sh.p95Px >= sh.medianPx - 1e-3f,
          "metric: p95 (%.1f) is not below the median (%.1f)", sh.p95Px, sh.medianPx);
    Check(fabsf(sh.lastY) < 0.51f,
          "metric: a pure eye-x translation moves nothing vertically (dy %.3f px)",
          sh.lastY);
    Check(sh.lastX > lo - 1.0f && sh.lastX < hi + 1.0f,
          "metric: the signed mean carries the direction (%+.1f px)", sh.lastX);

    // ---- the case that produced 146004688: origin ON the eye -------------
    //
    // This is the measured hand modelview: the CoD world->eye axis swap with a
    // translation of ~1e-5. Its origin projects to clip w = 2e-5.
    ResetLayerForCase();
    MockGameProjection(g_mockProjection);
    EnterViewmodelPass();
    // In this frame the vertices are CoD world coordinates relative to the
    // eye -- x forward, y left, z up -- so depth lives on model x, and the
    // viewmodel sits 8..28 units in front. Putting the spread on model z
    // instead leaves every sample behind the near plane, which is a real
    // property of the space and not a quirk of the fixture.
    for (int i = 0; i < 256; ++i) {
        g_mVerts[i*3+0] = 8.0f + 2.5f * (float)(i % 9);     // forward 8..28
        g_mVerts[i*3+1] = (float)((i % 7) - 3);             // left
        g_mVerts[i*3+2] = (float)((i % 5) - 2);             // up
    }
    for (int i = 0; i < 1428; ++i) g_mIdx[i] = (unsigned)(i % 256);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_mVerts);

    float camRel[16] = {
         0.00019f,  0.00365f, -0.99999f, 0.0f,
        -0.99994f,  0.01120f, -0.00015f, 0.0f,
         0.01120f,  0.99993f,  0.00365f, 0.0f,
         0.00001f,  0.00000f, -0.00002f, 1.0f,
    };
    memcpy(g_mockModelview, camRel, sizeof(camRel));
    float d3937[16]; MockTranslate(d3937, 3.937f, 0.0f, 0.0f);
    GLSurf_SetClassDelta(GLSC_GUN, d3937);
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_mIdx);
    Check(owned == 1, "camera-relative case: the batch was transformed");

    GLSurf_GetShiftMetrics(&sh);
    Check(sh.validSamples > 0,
          "camera-relative: geometry still testifies (%u valid samples)", sh.validSamples);
    // 3.937 units at 8..28 forward: 720*0.89382*3.937/28 .. /8 = 90..317 px.
    Check(sh.medianPx > 80.0f && sh.medianPx < 330.0f,
          "camera-relative: median %.1f px is the real answer, NOT 1.5e8",
          sh.medianPx);
    Check(sh.maxPx < 5000.0f,
          "camera-relative: even the worst sample stays sane (%.1f px)", sh.maxPx);

    // MUTANT: the retired origin diagnostic, on these very matrices.
    {
        float wanted[16];
        // wanted = delta * engine, the same composition the transform does.
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k) s += d3937[k*4+r] * camRel[c*4+k];
                wanted[c*4+r] = s;
            }
        const int vp[4] = { 0, 0, 1440, 1440 };
        const float a = GLSurf_TestOriginShiftX(camRel,  g_mockProjection, vp);
        const float b = GLSurf_TestOriginShiftX(wanted,  g_mockProjection, vp);
        const float bogus = fabsf(b - a);
        Check(bogus > 1.0e6f,
              "MUTANT: the retired origin metric still reports %.3g px here -- "
              "which is why it was retired", bogus);
        Check(bogus > sh.maxPx * 1000.0f,
              "MUTANT: origin metric is >1000x the geometry metric's worst sample");
    }

    // ---- geometry at/behind the near plane is rejected, not divided by ----
    ResetLayerForCase();
    MockGameProjection(g_mockProjection);
    EnterViewmodelPass();
    BuildMetricGeometry(0.0f, 0.0f);
    for (int i = 0; i < 256; ++i)                 // half in front, half behind
        g_mVerts[i*3+2] = (i & 1) ? -20.0f : +20.0f;
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_mVerts);
    MockTranslate(mv, 0.0f, 0.0f, 0.0f);
    memcpy(g_mockModelview, mv, sizeof(mv));
    GLSurf_SetClassDelta(GLSC_GUN, dx10);
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_mIdx);
    Check(owned == 1, "near-plane case: the batch was transformed");

    GLSurf_GetShiftMetrics(&sh);
    Check(sh.rejectedNearW > 0,
          "near plane: samples in front of the eye are rejected (%u)", sh.rejectedNearW);
    Check(sh.validSamples > 0,
          "near plane: the valid half still reports (%u samples)", sh.validSamples);
    const float want20 = 720.0f * 0.89382f * 10.0f / 20.0f;
    Check(fabsf(sh.medianPx - want20) < 1.0f,
          "near plane: the surviving samples give the right answer (%.1f, want %.1f)",
          sh.medianPx, want20);

    // ---- an identity delta is not a transform, so nothing is measured -----
    ResetLayerForCase();
    MockGameProjection(g_mockProjection);
    EnterViewmodelPass();
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_mVerts);
    GLSurf_GetShiftMetrics(&sh);
    Check(sh.validSamples == 0 && sh.draws == 0 && sh.maxPx == 0.0f,
          "a fresh layer reports no shift at all");
}

// ===========================================================================
// Laterality: which index half is which HAND.
//
// This is the one fact in the layer that a controller wiring cannot survive
// getting wrong, and it is the one fact the asset file gets wrong. The
// declared surface order of viewmodel_hands_new4 is 0=left, 1=right; the
// rendered result is the opposite way round. These cases pin the mapping to
// the sweep measurement so that a future tidy-up of the classifier cannot
// quietly restore the asset order.
// ===========================================================================

static void S_GLSurfLaterality()
{
    printf("--- render surface: measured hand laterality ---\n");
    BuildSyntheticGeometry();

    // The merged batch splits at vertex 837. Give each class a delta that is
    // unmistakable and check WHICH INDEX RANGE each one moved.
    ResetLayerForCase();
    EnterViewmodelPass();
    float toR[16]; MockTranslate(toR, 4.0f, 0, 0);
    GLSurf_SetClassDelta(GLSC_HAND_R, toR);
    RecReset();
    int owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                                      GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 1, "laterality: the merged hand batch was split and owned");
    Check(CountKind(REC_DRAW) == 2, "laterality: exactly two halves were drawn");

    // The FIRST half starts at g_handIdx (verts [0,837)); the SECOND at
    // g_handIdx + GLS_HAND_INDICES (verts [837,1674)).
    const GLRec* h0 = NthKind(REC_DRAW, 0);
    const GLRec* h1 = NthKind(REC_DRAW, 1);
    Check(h0 && h0->ptr == (const void*)g_handIdx,
          "laterality: the first draw is the first index half");
    Check(h1 && h1->ptr == (const void*)(g_handIdx + GLS_HAND_INDICES),
          "laterality: the second draw is the second index half");

    // RE-CORRECTED 2026-08-28: HAND_R moves the SECOND half -- surface 1,
    // `bip01 r hand`, the trigger hand. The asset surface order is truthful;
    // the earlier "first half = trigger" reading mistook submitted-space
    // coordinates for eye coordinates (see MergedHalves in vr_glsurf.cpp).
    Check(h1 && fabsf(h1->m[12] - 4.0f) < 1e-4f,
          "MEASURED: HAND_R is the SECOND index half (verts [837,1674)) -- the "
          "trigger hand, the asset's `bip01 r`");
    Check(h0 && fabsf(h0->m[12]) < 1e-4f,
          "laterality: the first half did not move with it");

    // ...and HAND_L is the other one, the support hand up on the fore-end.
    ResetLayerForCase();
    EnterViewmodelPass();
    float toL[16]; MockTranslate(toL, -4.0f, 0, 0);
    GLSurf_SetClassDelta(GLSC_HAND_L, toL);
    RecReset();
    owned = GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                                  GLS_GL_UNSIGNED_INT, g_handIdx);
    Check(owned == 1, "laterality: the batch splits for the other side too");
    h0 = NthKind(REC_DRAW, 0);
    h1 = NthKind(REC_DRAW, 1);
    Check(h0 && fabsf(h0->m[12] + 4.0f) < 1e-4f,
          "MEASURED: HAND_L is the FIRST index half (verts [0,837)) -- the "
          "support hand on the fore-end, the asset's `bip01 l`");
    Check(h1 && fabsf(h1->m[12]) < 1e-4f,
          "laterality: the second half did not move with it");

    // The sweep's central result, restated as an invariant: one hand delta
    // never reaches the other hand's geometry.
    ResetLayerForCase();
    EnterViewmodelPass();
    GLSurf_SetClassDelta(GLSC_HAND_R, toR);
    RecReset();
    GLSurf_OnDrawElements(GLS_GL_TRIANGLES, GLS_HAND_INDICES * 2,
                          GLS_GL_UNSIGNED_INT, g_handIdx);
    h0 = NthKind(REC_DRAW, 0);
    Check(h0 && memcmp(h0->m, g_mockModelview, sizeof(g_mockModelview)) == 0,
          "the untouched hand renders under the ENGINE's matrix, byte-identical");

    // The sleeves ARE a pair -- one per arm -- and the layer says so on
    // structural grounds. This assertion was inverted until 2026-08-29, when
    // the sleeve surfaces were walked through their skin weights: surface 2 is
    // 120/120 weighted under `bip01 r upperarm`, surface 3 is 120/120 under
    // `bip01 l upperarm`, zero cross-arm vertices, zero seam triangles. The
    // earlier "71% pixel-footprint overlap -> one arm" reading is superseded;
    // two forearms overlap on screen whichever arms they belong to, so that
    // metric could not have separated the hypotheses. (It is the same
    // screenshot-reading method that produced the laterality inversion twice.)
    Check(GLSurf_SleeveSplitIsAnatomical() == 1,
          "the sleeve split is ANATOMICAL -- one sleeve per arm, proven from "
          "skin weights, not from screen footprints");
}

// ===========================================================================
// Rotation pivot.
//
// The first sweep's gun_yaw stage applied a bare rotation about eye-space
// (0,0,0). For a weapon sitting ~10 units in front of the eye that is a swing
// around the player's HEAD, not a turn of the gun -- the recorded delta came
// back as a pure rotation with the object origin moving from z -10.61 to
// -10.45. A yaw probe has to rotate the surface about the surface.
//
// The pivot cannot come from the modelview: four of the seven viewmodel draws
// arrive camera-relative with a ~1e-5 translation. It comes from the vertices.
// ===========================================================================

static void S_GLSurfPivot()
{
    printf("--- render surface: rotation pivots ---\n");
    BuildSyntheticGeometry();

    // ---- the pivot is learned from GEOMETRY, not from the matrix ---------
    ResetLayerForCase();
    MockGameProjection(g_mockProjection);
    EnterViewmodelPass();
    // Camera-relative modelview (translation ~1e-5) with the surface sitting
    // 20 units in front: exactly the hands' situation.
    for (int i = 0; i < 256; ++i) {
        g_mVerts[i*3+0] = 20.0f;    // forward
        g_mVerts[i*3+1] =  0.0f;
        g_mVerts[i*3+2] =  0.0f;
    }
    for (int i = 0; i < 1428; ++i) g_mIdx[i] = (unsigned)(i % 256);
    GLSurf_NoteVertexPointer(3, GLS_GL_FLOAT, 12, g_mVerts);
    float camRel2[16] = {
         0.0f,  0.0f, -1.0f, 0.0f,
        -1.0f,  0.0f,  0.0f, 0.0f,
         0.0f,  1.0f,  0.0f, 0.0f,
         0.00001f, 0.0f, -0.00002f, 1.0f,
    };
    memcpy(g_mockModelview, camRel2, sizeof(camRel2));
    float ident[16]; MockIdentity(ident);
    GLSurf_SetClassDelta(GLSC_GUN, ident);
    GLSurf_OnDrawElements(GLS_GL_TRIANGLES, 1428, GLS_GL_UNSIGNED_INT, g_mIdx);

    float piv[3] = { 0, 0, 0 };
    Check(GLSurf_ClassPivot(GLSC_GUN, piv) == 1,
          "a pivot is learned even though the modelview carries no origin");
    Check(fabsf(piv[2] + 20.0f) < 0.01f,
          "the pivot is 20 units IN FRONT of the eye (z %.3f), which is where "
          "the vertices are -- the matrix origin says 0.00002", piv[2]);
    Check(fabsf(piv[0]) < 0.01f && fabsf(piv[1]) < 0.01f,
          "the pivot is centred laterally (%.3f, %.3f)", piv[0], piv[1]);

    // ---- rotating about that pivot keeps the surface where it is --------
    {
        float d[16];
        GLSurf_BuildDeltaAboutPivot(piv, 90.0f, 0, 0, 0, d);
        // The pivot must be a FIXED POINT of the delta.
        const float px = d[0]*piv[0] + d[4]*piv[1] + d[8] *piv[2] + d[12];
        const float py = d[1]*piv[0] + d[5]*piv[1] + d[9] *piv[2] + d[13];
        const float pz = d[2]*piv[0] + d[6]*piv[1] + d[10]*piv[2] + d[14];
        Check(fabsf(px - piv[0]) < 1e-3f && fabsf(py - piv[1]) < 1e-3f &&
              fabsf(pz - piv[2]) < 1e-3f,
              "the pivot is a fixed point of the rotation (%.4f, %.4f, %.4f)",
              px, py, pz);
    }

    // ---- and the OLD form moves it, which is the bug that shipped -------
    {
        float bad[16];
        GLSurf_BuildDeltaAboutPivot(NULL, 90.0f, 0, 0, 0, bad);   // camera pivot
        const float pz = bad[2]*piv[0] + bad[6]*piv[1] + bad[10]*piv[2] + bad[14];
        const float px = bad[0]*piv[0] + bad[4]*piv[1] + bad[8] *piv[2] + bad[12];
        const float moved = sqrtf((px - piv[0])*(px - piv[0]) +
                                  (pz - piv[2])*(pz - piv[2]));
        Check(moved > 20.0f,
              "MUTANT: rotating about the CAMERA throws the surface %.1f units "
              "across the view -- the gun_yaw stage's real behaviour", moved);
    }

    // ---- translation stays a true eye-space translation -----------------
    {
        float d[16];
        GLSurf_BuildDeltaAboutPivot(piv, 30.0f, 3.937f, 0, 0, d);
        // Apply to the pivot: it must land exactly 3.937 to the right of itself.
        const float px = d[0]*piv[0] + d[4]*piv[1] + d[8] *piv[2] + d[12];
        const float py = d[1]*piv[0] + d[5]*piv[1] + d[9] *piv[2] + d[13];
        const float pz = d[2]*piv[0] + d[6]*piv[1] + d[10]*piv[2] + d[14];
        Check(fabsf((px - piv[0]) - 3.937f) < 1e-3f &&
              fabsf(py - piv[1]) < 1e-3f && fabsf(pz - piv[2]) < 1e-3f,
              "yaw and offset compose without the yaw stealing the offset "
              "(%+.4f, %+.4f, %+.4f)", px - piv[0], py - piv[1], pz - piv[2]);
    }

    // ---- a rigid delta stays rigid --------------------------------------
    {
        float d[16];
        GLSurf_BuildDeltaAboutPivot(piv, 37.0f, 1.0f, -2.0f, 3.0f, d);
        const float det =
            d[0]*(d[5]*d[10]-d[9]*d[6]) - d[4]*(d[1]*d[10]-d[9]*d[2]) +
            d[8]*(d[1]*d[6]-d[5]*d[2]);
        Check(fabsf(det - 1.0f) < 1e-4f,
              "the pivot delta is still a rigid motion (det %.6f)", det);
    }

    // ---- no pivot yet: the probe says so rather than pretending ---------
    GLSurf_Reset();
    InstallMockDriver();
    GLSurf_SetProbe(GLSC_GUN, 3.937f, 0, 0, 15.0f);
    Check(GLSurf_ProbePivotWasKnown() == 0,
          "with nothing drawn yet the probe reports that it had no pivot");
    float p2[16];
    GLSurf_GetClassDelta(GLSC_GUN, p2);
    Check(fabsf(p2[12] - 3.937f) < 1e-4f,
          "the camera-pivot fallback still carries its translation exactly");
}
