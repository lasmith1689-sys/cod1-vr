// GL surface layer -- implementation. See vr_glsurf.h for why this exists.

#include "vr_glsurf.h"
#include "vr_handpose.h"
#include "vr_vbo.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <windows.h>

// Defined with the client-array probe further down; the state notes
// above it have to be able to invalidate its cache.
static void GLSurf_ForgetProbeCache();

// Defined with the client-array probe; IndexRange (which sits with the
// capture code above them) resolves every index array through this before
// reading a single element.
static const void* IndexArrayBase(unsigned type, const void* idx, int count);

static int      g_drawDepth;
static unsigned g_reentered;

struct DrawDepthScope {
    DrawDepthScope()  { ++g_drawDepth; }
    ~DrawDepthScope() { --g_drawDepth; }
};

unsigned GLSurf_ReentryCount() { return g_reentered; }


// ---------------------------------------------------------------- injection

static GLSurfDriver g_drv;
static bool         g_haveDriver;

// Static storage is ZERO, and a zero 4x4 is not the identity -- it collapses
// every vertex it touches to the origin. The first flat capture proved that
// the hard way: 1443 gun draws were "transformed" by an all-zero matrix
// before any probe had been configured. Nothing may read a delta before this
// has run. Defined below, once its dependencies exist.
static void EnsureInit();

void GLSurf_SetDriver(const GLSurfDriver* drv)
{
    EnsureInit();
    if (drv) { g_drv = *drv; g_haveDriver = true; }
    else     { memset(&g_drv, 0, sizeof(g_drv)); g_haveDriver = false; }
}

// ------------------------------------------------------------- matrix maths
//
// Column-major throughout, exactly as GL stores them: m[col*4 + row]. Getting
// this backwards produces a transform that looks *almost* right -- translation
// lands in the wrong row and the object shears instead of moving -- so every
// helper here is written in that one convention and never transposed silently.

static void MatIdentity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static bool MatIsIdentity(const float m[16])
{
    static const float kEps = 1e-6f;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            const float want = (c == r) ? 1.0f : 0.0f;
            if (fabsf(m[c * 4 + r] - want) > kEps)
                return false;
        }
    return true;
}

// out = a * b  (apply b first, then a)
static void MatMul(const float a[16], const float b[16], float out[16])
{
    float t[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = s;
        }
    memcpy(out, t, sizeof(t));
}

// Rotate a direction (no translation) by the upper 3x3.
static void MatRotVec3(const float m[16], const float v[3], float out[3])
{
    const float x = v[0], y = v[1], z = v[2];
    out[0] = m[0] * x + m[4] * y + m[8]  * z;
    out[1] = m[1] * x + m[5] * y + m[9]  * z;
    out[2] = m[2] * x + m[6] * y + m[10] * z;
}

// ---------------------------------------------------------------- GL state
//
// Mirrored, never queried on the hot path. A glGetFloatv per draw would stall
// the pipeline on ~10k draws a frame for information we can simply remember.

struct ArrayPtr {
    const void* p;
    int         size;
    unsigned    type;
    int         stride;
    int         enabled;
    // The GL_ARRAY_BUFFER binding at the moment the gl*Pointer call was made
    // -- GL latches it then, not at draw time. Nonzero means `p` is a byte
    // OFFSET into that buffer object, not an address, and every read must go
    // through the vr_vbo shadow.
    unsigned    buffer;
};

enum { GLS_MAX_TEXUNITS = 8 };

struct GLState {
    ArrayPtr vertex, normal, texcoord, color;
    unsigned boundTex[GLS_MAX_TEXUNITS];
    unsigned activeTex;
    double   depthNear, depthFar;
    unsigned depthFunc;
    unsigned char depthMask;
    int      depthTest, blend, alphaTest, cullEnable;
    unsigned cullFace, frontFace, blendSrc, blendDst, alphaFunc;
    float    alphaRef;
    int      viewport[4], scissor[4];
    unsigned matrixMode;
    int      lockFirst, lockCount;
};

static GLState g_st;

static void ResetGLState()
{
    memset(&g_st, 0, sizeof(g_st));
    g_st.depthNear  = 0.0;
    g_st.depthFar   = 1.0;
    g_st.depthFunc  = 0x0201;             // GL_LESS
    g_st.depthMask  = 1;
    g_st.cullFace   = 0x0405;             // GL_BACK
    g_st.frontFace  = 0x0901;             // GL_CCW
    g_st.blendSrc   = 1;                  // GL_ONE
    g_st.blendDst   = 0;                  // GL_ZERO
    g_st.alphaFunc  = 0x0207;             // GL_ALWAYS
    g_st.matrixMode = GLS_GL_MODELVIEW;
    g_st.lockCount  = 0;
    g_st.lockFirst  = -1;
}

// ------------------------------------------------------------ frame / phase

static int      g_inScene;
static int      g_curEye;
static unsigned g_frameIndex;
static unsigned g_sceneFrameIndex;
static unsigned g_drawSeq;               // draw ordinal within the frame

// The depth-hacked run: id Tech 3 narrows glDepthRange for RF_DEPTHHACK
// entities, which is the engine saying "this is the first-person overlay".
// Everything the viewmodel draws arrives inside such a run.
static int      g_vmRunActive;
static unsigned g_vmRunIndex;            // ordinal of this draw within the run
static unsigned g_handSeen;              // hand-shaped draws so far this frame
static unsigned g_sleeveSeen;
static unsigned g_gunSeen;

static void ResetFrameCounters()
{
    g_drawSeq     = 0;
    g_vmRunActive = 0;
    g_vmRunIndex  = 0;
    g_handSeen    = 0;
    g_sleeveSeen  = 0;
    g_gunSeen     = 0;
}

// ----------------------------------------------------------------- capture

enum { GLS_CAP_MAX = 24000, GLS_TEXCACHE = 512 };

struct DrawRecord {
    unsigned frame, sceneFrame, seq;
    int      eye;
    int      inScene;
    unsigned mode;
    int      count;
    unsigned type;
    unsigned idxPtr;
    int      minIndex, maxIndex, vertSpan;
    unsigned vertPtr, normPtr, texPtr, colPtr;
    int      vertSize, vertStride, normStride, texStride;
    unsigned vertType;
    int      vertEnabled, normEnabled, texEnabled, colEnabled;
    unsigned tex0, tex1;
    int      tex0W, tex0H;
    float    modelview[16];
    float    projection[16];
    double   depthNear, depthFar;
    int      depthTest, depthMask, blend, alphaTest, cullEnable;
    unsigned depthFunc, cullFace, frontFace, blendSrc, blendDst, alphaFunc;
    float    alphaRef;
    int      viewport[4], scissor[4];
    int      lockFirst, lockCount;
    int      cls;
    // The isolation evidence. `modelview` is what the ENGINE set; `mvUsed` is
    // what the draw actually rendered under. For an untouched draw the two are
    // byte-identical, and that -- not a screenshot -- is what proves the hands
    // did not move when the gun did. A screenshot taken half a second later
    // also contains idle sway; these two matrices contain nothing else.
    float    mvUsed[16];
    int      transformed;
};

static DrawRecord g_cap[GLS_CAP_MAX];
static int        g_capCount;
static int        g_capArmed;
static int        g_capDone;
static int        g_capSkip;             // scene frames still to skip
static int        g_capFrames;           // scene frames still to record
static int        g_capRecording;
static char       g_capTag[64];

struct TexInfo { unsigned id; int w, h; };
static TexInfo g_texCache[GLS_TEXCACHE];
static int     g_texCacheCount;

static void TexDims(unsigned tex, int* w, int* h)
{
    *w = 0; *h = 0;
    if (!tex) return;
    for (int i = 0; i < g_texCacheCount; ++i)
        if (g_texCache[i].id == tex) { *w = g_texCache[i].w; *h = g_texCache[i].h; return; }
    if (!g_drv.getTexLevelParameteriv) return;
    int tw = 0, th = 0;
    g_drv.getTexLevelParameteriv(GLS_GL_TEXTURE_2D, 0, GLS_GL_TEXTURE_WIDTH,  &tw);
    g_drv.getTexLevelParameteriv(GLS_GL_TEXTURE_2D, 0, GLS_GL_TEXTURE_HEIGHT, &th);
    if (g_texCacheCount < GLS_TEXCACHE) {
        g_texCache[g_texCacheCount].id = tex;
        g_texCache[g_texCacheCount].w  = tw;
        g_texCache[g_texCacheCount].h  = th;
        ++g_texCacheCount;
    }
    *w = tw; *h = th;
}

// Scan an index array for its range. Only ever done while capturing (and once
// per transformed draw when the scratch route is active), never on the ~10k
// untouched draws of an ordinary frame.
static bool IndexRange(unsigned type, const void* idx, int count, int* lo, int* hi)
{
    // Resolve before reading: with an element buffer bound `idx` is a byte
    // offset (the vertex-array lesson of 2026-08-26, applied to the OTHER
    // array), and even a client pointer is only read once the OS has vouched
    // for the whole span. A refusal means "no range", never a fault.
    idx = IndexArrayBase(type, idx, count);
    if (!idx || count <= 0) return false;
    int mn = 0x7FFFFFFF, mx = -1;
    if (type == GLS_GL_UNSIGNED_INT) {
        const unsigned* p = (const unsigned*)idx;
        for (int i = 0; i < count; ++i) { int v = (int)p[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
    } else if (type == GLS_GL_UNSIGNED_SHORT) {
        const unsigned short* p = (const unsigned short*)idx;
        for (int i = 0; i < count; ++i) { int v = p[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
    } else if (type == GLS_GL_UNSIGNED_BYTE) {
        const unsigned char* p = (const unsigned char*)idx;
        for (int i = 0; i < count; ++i) { int v = p[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
    } else {
        return false;
    }
    if (mx < 0) return false;
    *lo = mn; *hi = mx;
    return true;
}

// ------------------------------------------------------------- classifier
//
// FAIL CLOSED. Two independent conditions must both hold before a draw can be
// called viewmodel at all: the GL stream must be in the 3D scene pass, and the
// engine must have narrowed the depth range for it. Only inside that set does
// the asset fingerprint get consulted. A world weapon, an NPC's weapon, a HUD
// element and a muzzle flash all fail at least one of those, by construction
// rather than by threshold.

static int g_classifierEnabled = 1;

// Which hand rig the fingerprints matched, so the surface ORDER (and hence
// which ordinal is left) is taken from the right file layout:
//   viewmodel_hands_new4 / _sten4 : 0=L hand, 1=R hand, 2=R sleeve, 3=L sleeve
//   viewmodel_hands4              : 0=R sleeve, 1=L sleeve, 2=R hand, 3=L hand
enum { RIG_NONE = 0, RIG_NEW4 = 1, RIG_HANDS4 = 2 };
static int g_rigThisFrame;

// The asset-derived ownership table. Generated by
// tools/derive_render_ownership.py from skin weights and bone ANCESTRY, so
// "which half is which hand" is read out of the shipped rig rather than
// written by hand. Every laterality error this project has made came from a
// human reading a screenshot; nothing below consults a screen position or a
// bone name.
#include "vr_render_ownership.inc"

// kRigOwnership rows: 0 = hands_new4, 1 = hands_sten4 (identical), 2 = hands4.
static const CodvrRigOwnership* RigOwnership()
{
    const int row = (g_rigThisFrame == RIG_HANDS4) ? 2 : 0;
    return &kRigOwnership[row];
}

// What KIND of batch a viewmodel draw is. The engine does not hand us one
// draw per surface: it concatenates a model's surfaces into one batch, so the
// two hand shells arrive together and the two sleeves arrive together. That is
// why a batch has a kind and a class does not always map one-to-one onto it.
enum {
    BATCH_NONE = 0,
    BATCH_GUN,            // weapon geometry: one class, one draw
    BATCH_HANDS_MERGED,   // surfaces 0..1 concatenated: LEFT then RIGHT
    BATCH_SLEEVES_MERGED, // surfaces 2..3 concatenated: RIGHT then LEFT
    BATCH_HAND_SINGLE,    // one hand shell on its own (side by ordinal)
    BATCH_SLEEVE_SINGLE,
    BATCH_VM_OTHER
};

// MEASURED against the installed game, 2026-08-24 (tools/match_draw_signatures.py):
//
//   draw of 7824 indices / 1674 vertex span
//       == viewmodel_hands_new4 surfaces 0..1   (3912+3912 idx, 837+837 verts)
//   draw of  948 indices /  240 vertex span
//       == viewmodel_hands_new4 surfaces 2..3   ( 474+474 idx,  120+120 verts)
//
// Both equalities hold on the index count AND on the vertex count, which is
// two independent coincidences a wrong reading would have to survive. The same
// arithmetic matches viewmodel_hands_sten4, which is the point: the two rigs
// are interchangeable here because they declare identical surface sizes in
// identical order.
enum {
    GLS_HANDS_MERGED_INDICES   = GLS_HAND_INDICES   * 2,   // 7824
    GLS_HANDS_MERGED_VERTS     = GLS_HAND_VERTS     * 2,   // 1674
    GLS_SLEEVES_MERGED_INDICES = GLS_SLEEVE_INDICES * 2,   //  948
    GLS_SLEEVES_MERGED_VERTS   = GLS_SLEEVE_VERTS   * 2,   //  240
};

static int ClassifyBatch(unsigned mode, int count)
{
    if (!g_classifierEnabled)      return BATCH_NONE;
    if (!g_inScene)                return BATCH_NONE;  // HUD / menu: never
    if (!g_vmRunActive)            return BATCH_NONE;  // world, NPCs, sky
    if (mode != GLS_GL_TRIANGLES && mode != GLS_GL_TRIANGLE_STRIP &&
        mode != GLS_GL_TRIANGLE_FAN)
        return BATCH_VM_OTHER;

    // Hand rigs first: shipped-asset index counts, exact.
    if (count == GLS_HANDS_MERGED_INDICES)   { g_rigThisFrame = RIG_NEW4; return BATCH_HANDS_MERGED; }
    if (count == GLS_SLEEVES_MERGED_INDICES) { g_rigThisFrame = RIG_NEW4; return BATCH_SLEEVES_MERGED; }
    if (count == GLS_HAND_INDICES)   { g_rigThisFrame = RIG_NEW4;   return BATCH_HAND_SINGLE; }
    if (count == GLS_SLEEVE_INDICES) { g_rigThisFrame = RIG_NEW4;   return BATCH_SLEEVE_SINGLE; }
    if (count == GLS_HAND4_INDICES)  { g_rigThisFrame = RIG_HANDS4; return BATCH_HAND_SINGLE; }
    if (count == GLS_SLEEVE4_INDICES){ g_rigThisFrame = RIG_HANDS4; return BATCH_SLEEVE_SINGLE; }

    // Depth-hacked, opaque, not a hand: the weapon's own geometry. Blended
    // draws inside the run are effects (muzzle flash, shell ejection glow)
    // and are deliberately NOT claimed as the gun -- they must keep their
    // engine-authored placement unless something proves otherwise.
    if (!g_st.blend) {
        ++g_gunSeen;
        return BATCH_GUN;
    }
    return BATCH_VM_OTHER;
}

// The single class a whole-batch draw belongs to. Merged batches have no
// single class -- they are split instead -- so they report the LEFT/RIGHT pair
// only for the capture's benefit.
static int BatchPrimaryClass(int batch)
{
    switch (batch) {
        case BATCH_GUN:            return GLSC_GUN;
        case BATCH_HANDS_MERGED:   return GLSC_HAND_L;
        case BATCH_SLEEVES_MERGED: return GLSC_SLEEVE_R;
        case BATCH_HAND_SINGLE: {
            // Draw ordinal -> class, straight out of the generated table.
            // The single-draw rig (hands4) declares its HAND surfaces in the
            // opposite order to the merged rigs -- hands4 is
            // [sleeveR, sleeveL, handR, handL] while hands_new4 is
            // [handL, handR, sleeveR, sleeveL] -- so the two paths legitimately
            // disagree about "which comes first". They are not two guesses at
            // one convention; they are two different declared orders, and the
            // table carries both.
            const CodvrRigOwnership* ro = RigOwnership();
            const unsigned n = g_handSeen++;
            return (n == 0) ? ro->handSingleFirst
                 : (n == 1) ? ro->handSingleSecond : GLSC_VM_OTHER;
        }
        case BATCH_SLEEVE_SINGLE: {
            const CodvrRigOwnership* ro = RigOwnership();
            const unsigned n = g_sleeveSeen++;
            return (n == 0) ? ro->sleeveSingleFirst
                 : (n == 1) ? ro->sleeveSingleSecond : GLSC_VM_OTHER;
        }
        case BATCH_VM_OTHER:       return GLSC_VM_OTHER;
        default:                   return GLSC_NONE;
    }
}

// Which class each HALF of a merged batch is, in draw order.
//
// RE-CORRECTED 2026-08-28, back to the asset surface order: the first half
// [0,837) is surface 0 = `bip01 l hand` = the player's LEFT/support hand.
//
// The 2026-08-24 A/B sweep's pixel-disjointness stands (the halves share
// zero pixels), but its anatomical naming of the footprints was an eyeball
// error. Three measurements force the corrected mapping:
//
//   1. The hand batch's live modelview (runs/vbogame-20260827-155348 CSV)
//      is the pure Quake-view -> GL-eye permutation, det +1: submitted +x
//      is eye-FORWARD. In that space the solved surface-1 palm sits at
//      (1.6 fwd, 4.1 right, 5.1 down) -- near, right, low: the TRIGGER
//      hand -- and surface 0 at (16.6 fwd, ~centre, 7.6 down): the
//      extended FORE hand.
//   2. The fore palm is 15 inches FORWARD of the rear palm, matching the
//      authored inter-hand distance (0.30-0.42 m across six weapons).
//      Under the old mapping the "trigger" hand would float 15 inches in
//      front of the "support" hand -- physically unholdable.
//   3. Label-free authored physics (tools/check_grip_hand.py): across six
//      weapons `bip01 r hand` is the palm FARTHER from the muzzle -- the
//      rear hand -- and the profile rear grip was measured from exactly
//      that bone.
//
// hands4 never merges (per-surface singles), so one mapping serves.
static void MergedHalves(int batch, int* firstCls, int* secondCls)
{
    const CodvrRigOwnership* ro = RigOwnership();
    if (batch == BATCH_HANDS_MERGED) {
        *firstCls  = ro->handMergedFirst;    // hands_new4: surface 0 = bip01 l
        *secondCls = ro->handMergedSecond;   //             surface 1 = bip01 r
    } else {
        // The sleeve surfaces are declared in the OPPOSITE order to the hands
        // (surface 2 is the RIGHT arm's, surface 3 the LEFT's) -- read, not
        // assumed. See GLSurf_SleeveSplitIsAnatomical().
        *firstCls  = ro->sleeveMergedFirst;
        *secondCls = ro->sleeveMergedSecond;
    }
}

// The sleeve split IS anatomical -- one sleeve per arm.
//
// CORRECTED 2026-08-29, from the asset structure rather than from pixels.
// tools/derive_render_ownership.py walks every sleeve vertex's skin weights up
// the bone parent chain to an arm root and finds the two 120-vertex surfaces
// perfectly separated:
//
//   surface 2  verts [1674,1794)  158 tris  120/120 reach `bip01 r upperarm`
//   surface 3  verts [1794,1914)  158 tris  120/120 reach `bip01 l upperarm`
//   cross-arm-weighted vertices: 0     seam triangles: 0
//
// A single arm's cloth cannot be skinned 100% to two different arm chains, so
// "two pieces of one arm" is refuted outright.
//
// WHAT THE 2026-08-24 SWEEP ACTUALLY MEASURED. That pass moved each half and
// compared disturbed-pixel masks, reporting 71% overlap (IoU 0.38) and
// concluding one arm. It is the same eyeball-on-a-screenshot method that also
// produced the laterality inversion this project had to correct twice: screen
// footprints of two forearms at the bottom of a viewmodel frame overlap
// heavily whichever arms they belong to, so the metric could not have
// separated the hypotheses. Skin weights can, and do.
//
// Callers may therefore give each sleeve its own arm's transform. They must
// NOT leave them on the vanilla weapon while the hands move.
int GLSurf_SleeveSplitIsAnatomical() { return 1; }

const char* GLSurf_ClassName(int cls)
{
    switch (cls) {
        case GLSC_NONE:     return "none";
        case GLSC_GUN:      return "gun";
        case GLSC_HAND_L:   return "hand_L";
        case GLSC_HAND_R:   return "hand_R";
        case GLSC_SLEEVE_L: return "sleeve_L";
        case GLSC_SLEEVE_R: return "sleeve_R";
        case GLSC_VM_OTHER: return "vm_other";
        default:            return "?";
    }
}

// ---------------------------------------------------------------- transform

static float    g_delta[GLSC_COUNT][16];
static int      g_hidden[GLSC_COUNT];
static int      g_transformEnabled = 1;
static unsigned g_latchSerial;
static unsigned g_latchCount;
static unsigned g_eyeConsumed[2] = { 0u, 0u };
static int      g_eyeConsumedValid[2];
static unsigned g_pairMismatch;
static int      g_probeClass = GLSC_NONE;

static GLSurfStats g_stats;

static bool g_initDone;

static void EnsureInit()
{
    if (g_initDone) return;
    g_initDone = true;
    ResetGLState();
    for (int i = 0; i < GLSC_COUNT; ++i)
        MatIdentity(g_delta[i]);
}

// Route B scratch. Positions and normals are copied out, rigidly transformed
// and drawn from CODVR-owned memory: engine vertex buffers are NEVER written.
enum { GLS_SCRATCH_VERTS = 65536 };
static float g_scratchPos[GLS_SCRATCH_VERTS * 3];
static float g_scratchNrm[GLS_SCRATCH_VERTS * 3];

// Which mechanism moves geometry. Route A (modelview delta) is correct when
// the engine still places the surface with a matrix; Route B (scratch client
// arrays) is the fallback for pre-transformed geometry. The flat probe decides
// which one ships; both are built so the decision is measured, not assumed.
enum { GLS_ROUTE_MODELVIEW = 0, GLS_ROUTE_SCRATCH = 1 };
static int g_route = GLS_ROUTE_MODELVIEW;

void GLSurf_SetRoute(int route) { g_route = (route == GLS_ROUTE_SCRATCH) ? GLS_ROUTE_SCRATCH
                                                                        : GLS_ROUTE_MODELVIEW; }
int  GLSurf_Route()             { return g_route; }

static float g_lastLoaded[16], g_lastRestored[16];
static int   g_lastTransformValid;
static int   g_lastDrawClass;

void GLSurf_SetClassDelta(int cls, const float m[16])
{
    EnsureInit();
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT || !m) return;
    memcpy(g_delta[cls], m, 16 * sizeof(float));
}

void GLSurf_GetClassDelta(int cls, float m[16])
{
    if (!m) return;
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT) { MatIdentity(m); return; }
    memcpy(m, g_delta[cls], 16 * sizeof(float));
}

void GLSurf_ClearAllDeltas()
{
    for (int i = 0; i < GLSC_COUNT; ++i)
        MatIdentity(g_delta[i]);
}

void GLSurf_SetTransformEnabled(int on) { g_transformEnabled = on ? 1 : 0; }
int  GLSurf_TransformEnabled()          { return g_transformEnabled; }

void GLSurf_SetClassHidden(int cls, int hidden)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT) return;
    g_hidden[cls] = hidden ? 1 : 0;
}
int GLSurf_ClassHidden(int cls)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT) return 0;
    return g_hidden[cls];
}

void GLSurf_BeginLatch(unsigned serial)
{
    g_latchSerial = serial;
    ++g_latchCount;
}
unsigned GLSurf_LatchSerial() { return g_latchSerial; }
unsigned GLSurf_LatchCount()  { return g_latchCount; }
unsigned GLSurf_EyeConsumedSerial(int eye)
{
    if (eye < 0 || eye > 1 || !g_eyeConsumedValid[eye]) return 0u;
    return g_eyeConsumed[eye];
}
unsigned GLSurf_PairMismatchCount() { return g_pairMismatch; }

static int g_probePivotKnown;
int GLSurf_ProbePivotWasKnown() { return g_probePivotKnown; }

// Defined with the geometry sampler further down; declared here because the
// probe is built before a draw has ever run.
static void NoteClassPivot(int cls, unsigned type, const void* idx, int count);

void GLSurf_SetProbe(int cls, float dx, float dy, float dz, float yawDeg)
{
    EnsureInit();
    GLSurf_ClearAllDeltas();
    g_probeClass = GLSC_NONE;
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT) return;
    // Rotate the surface about ITSELF, not about the camera. The first sweep
    // built a bare rotation about eye-space (0,0,0), which for a viewmodel
    // 10 units in front of the eye is a swing around the player's head rather
    // than a turn of the weapon. If no pivot has been observed for this class
    // yet (nothing of it has been drawn), the old camera-pivot form is all
    // that is available and GLSurf_ProbePivotWasKnown() reports 0 so the log
    // can say which of the two was used.
    float p[3];
    g_probePivotKnown = GLSurf_ClassPivot(cls, p);
    if (!g_probePivotKnown) { p[0] = p[1] = p[2] = 0.0f; }
    float m[16];
    GLSurf_BuildDeltaAboutPivot(p, yawDeg, dx, dy, dz, m);
    memcpy(g_delta[cls], m, sizeof(m));
    g_probeClass = cls;
}
int GLSurf_ProbeClass() { return g_probeClass; }

// ------------------------------------------------------------- observation

void GLSurf_NoteVertexPointer(int size, unsigned type, int stride, const void* p)
{ g_st.vertex.size = size; g_st.vertex.type = type; g_st.vertex.stride = stride; g_st.vertex.p = p;
  g_st.vertex.buffer = VBO_BoundArrayBuffer(); }   // GL latches the binding HERE

void GLSurf_NoteNormalPointer(unsigned type, int stride, const void* p)
{ g_st.normal.type = type; g_st.normal.stride = stride; g_st.normal.p = p; g_st.normal.size = 3;
  g_st.normal.buffer = VBO_BoundArrayBuffer(); }

void GLSurf_NoteTexCoordPointer(int size, unsigned type, int stride, const void* p)
{ g_st.texcoord.size = size; g_st.texcoord.type = type; g_st.texcoord.stride = stride; g_st.texcoord.p = p; }

void GLSurf_NoteColorPointer(int size, unsigned type, int stride, const void* p)
{ g_st.color.size = size; g_st.color.type = type; g_st.color.stride = stride; g_st.color.p = p; }

void GLSurf_NoteClientState(unsigned cap, int on)
{
    switch (cap) {
        case GLS_GL_VERTEX_ARRAY:        g_st.vertex.enabled   = on; break;
        case GLS_GL_NORMAL_ARRAY:        g_st.normal.enabled   = on; break;
        case GLS_GL_TEXTURE_COORD_ARRAY: g_st.texcoord.enabled = on; break;
        case GLS_GL_COLOR_ARRAY:         g_st.color.enabled    = on; break;
        default: break;
    }
}

void GLSurf_NoteBindTexture(unsigned target, unsigned tex)
{
    if (target != GLS_GL_TEXTURE_2D) return;
    const unsigned u = (g_st.activeTex < GLS_MAX_TEXUNITS) ? g_st.activeTex : 0;
    g_st.boundTex[u] = tex;
}

void GLSurf_NoteActiveTexture(unsigned unit)
{
    // GL_TEXTURE0_ARB is 0x84C0; accept both the enum and a bare index.
    unsigned u = (unit >= 0x84C0) ? (unit - 0x84C0) : unit;
    if (u >= GLS_MAX_TEXUNITS) u = 0;
    g_st.activeTex = u;
}

void GLSurf_NoteDepthRange(double zn, double zf) { g_st.depthNear = zn; g_st.depthFar = zf; }
void GLSurf_NoteDepthMask(unsigned char f)       { g_st.depthMask = f; }
void GLSurf_NoteDepthFunc(unsigned f)            { g_st.depthFunc = f; }
void GLSurf_NoteCullFace(unsigned m)             { g_st.cullFace = m; }
void GLSurf_NoteFrontFace(unsigned m)            { g_st.frontFace = m; }
void GLSurf_NoteBlendFunc(unsigned s, unsigned d){ g_st.blendSrc = s; g_st.blendDst = d; }
void GLSurf_NoteAlphaFunc(unsigned f, float r)   { g_st.alphaFunc = f; g_st.alphaRef = r; }
void GLSurf_NoteMatrixMode(unsigned m)           { g_st.matrixMode = m; }
void GLSurf_NoteLockArrays(int f, int c)         { g_st.lockFirst = f; g_st.lockCount = c; }
void GLSurf_NoteUnlockArrays()                   { g_st.lockFirst = -1; g_st.lockCount = 0; }

void GLSurf_NoteEnable(unsigned cap, int on)
{
    switch (cap) {
        case GLS_GL_DEPTH_TEST: g_st.depthTest  = on; break;
        case GLS_GL_BLEND:      g_st.blend      = on; break;
        case GLS_GL_ALPHA_TEST: g_st.alphaTest  = on; break;
        case GLS_GL_CULL_FACE:  g_st.cullEnable = on; break;
        default: break;
    }
}

void GLSurf_NoteViewport(int x, int y, int w, int h)
{ g_st.viewport[0]=x; g_st.viewport[1]=y; g_st.viewport[2]=w; g_st.viewport[3]=h; }

void GLSurf_NoteScissor(int x, int y, int w, int h)
{ g_st.scissor[0]=x; g_st.scissor[1]=y; g_st.scissor[2]=w; g_st.scissor[3]=h; }

// ------------------------------------------------------------ frame / phase

void GLSurf_BeginFrame()
{
    ++g_frameIndex;
    ResetFrameCounters();
    g_inScene = 0;
    g_rigThisFrame = RIG_NONE;

    // Client-array probe results live at most ONE frame. Forgetting here
    // rather than on every glVertexPointer call lets one frame's vertex and
    // index ranges share the ring (a pointer-change wipe made every draw
    // re-query and the cache never carried the index array at all). The
    // staleness window is safe: an array we probe at draw time is an array
    // the DRIVER is about to read for that same draw, so it is necessarily
    // live memory at that moment -- the cache only ever re-vouches, within
    // the frame, for regions the OS validated this frame.
    GLSurf_ForgetProbeCache();

    if (g_capArmed && !g_capDone && g_capRecording) {
        // One recorded scene frame just ended.
        if (--g_capFrames <= 0) {
            g_capRecording = 0;
            g_capDone = 1;
        }
    }
}

void GLSurf_SceneQueued(int eye)
{
    g_curEye = eye;
    ++g_sceneFrameIndex;
    if (g_capArmed && !g_capDone && !g_capRecording) {
        if (g_capSkip > 0) --g_capSkip;
        else               g_capRecording = 1;
    }
    // Which latch serial this eye's draws are about to consume. A pair whose
    // two eyes report different serials is exactly the nausea failure the
    // pair-latch invariant exists to make impossible.
    if (eye == 0 || eye == 1) {
        if (g_eyeConsumedValid[eye] && g_eyeConsumed[eye] != g_latchSerial) {
            // not itself an error: eye 0 legitimately moves to a new serial
        }
        g_eyeConsumed[eye]      = g_latchSerial;
        g_eyeConsumedValid[eye] = 1;
        if (eye == 1 && g_eyeConsumedValid[0] && g_eyeConsumed[0] != g_eyeConsumed[1])
            ++g_pairMismatch;
    }
}

void GLSurf_ScenePhase(int inScene) { g_inScene = inScene ? 1 : 0; }

void GLSurf_TestForceScenePhase(int inScene) { GLSurf_ScenePhase(inScene); }
void GLSurf_TestSetDepthRange(double zn, double zf) { GLSurf_NoteDepthRange(zn, zf); }

// The depth-hack test. id Tech 3 narrows the depth range around the
// first-person overlay so it cannot poke through walls.
//
// MEASURED, 2026-08-24, chateau, three consecutive scene frames:
//
//     [0.0, 1.0]   129 scene draws   the world and its entities
//     [0.0, 0.2]    21 scene draws   the view weapon and hands   <-- ours
//     [1.0, 1.0]    15 scene draws   the SKY, pinned to the far plane
//
// The first version of this test asked only "is the range non-default", which
// swept the sky into the viewmodel set -- five GL_QUADS batches at a modelview
// translation of (-397, 423, -7259). So the test is specifically NEAR: the far
// end must be pulled in, and the near end must not be pushed out.
static void UpdateViewmodelRun()
{
    const bool hacked = (g_st.depthFar < 0.999) && (g_st.depthNear < 0.5);
    if (hacked && !g_vmRunActive) { g_vmRunActive = 1; g_vmRunIndex = 0; }
    else if (!hacked)             { g_vmRunActive = 0; }
}

// ------------------------------------------------------------- the hot path

// Returns the record's index so the transform can fill in what it actually
// did, or -1 when nothing was recorded.
static int RecordDraw(unsigned mode, int count, unsigned type, const void* idx, int cls)
{
    if (g_capCount >= GLS_CAP_MAX) return -1;
    const int recIdx = g_capCount;
    DrawRecord& r = g_cap[g_capCount++];
    memset(&r, 0, sizeof(r));
    r.frame = g_frameIndex; r.sceneFrame = g_sceneFrameIndex; r.seq = g_drawSeq;
    r.eye = g_curEye; r.inScene = g_inScene;
    r.mode = mode; r.count = count; r.type = type;
    r.idxPtr = (unsigned)(uintptr_t)idx;
    r.minIndex = -1; r.maxIndex = -1;
    if (IndexRange(type, idx, count, &r.minIndex, &r.maxIndex))
        r.vertSpan = r.maxIndex - r.minIndex + 1;
    r.vertPtr = (unsigned)(uintptr_t)g_st.vertex.p;
    r.normPtr = (unsigned)(uintptr_t)g_st.normal.p;
    r.texPtr  = (unsigned)(uintptr_t)g_st.texcoord.p;
    r.colPtr  = (unsigned)(uintptr_t)g_st.color.p;
    r.vertSize = g_st.vertex.size; r.vertStride = g_st.vertex.stride;
    r.vertType = g_st.vertex.type;
    r.normStride = g_st.normal.stride; r.texStride = g_st.texcoord.stride;
    r.vertEnabled = g_st.vertex.enabled; r.normEnabled = g_st.normal.enabled;
    r.texEnabled = g_st.texcoord.enabled; r.colEnabled = g_st.color.enabled;
    r.tex0 = g_st.boundTex[0]; r.tex1 = g_st.boundTex[1];
    TexDims(r.tex0, &r.tex0W, &r.tex0H);
    if (g_drv.getFloatv) {
        g_drv.getFloatv(GLS_GL_MODELVIEW_MATRIX,  r.modelview);
        g_drv.getFloatv(GLS_GL_PROJECTION_MATRIX, r.projection);
    }
    r.depthNear = g_st.depthNear; r.depthFar = g_st.depthFar;
    r.depthTest = g_st.depthTest; r.depthMask = g_st.depthMask;
    r.blend = g_st.blend; r.alphaTest = g_st.alphaTest; r.cullEnable = g_st.cullEnable;
    r.depthFunc = g_st.depthFunc; r.cullFace = g_st.cullFace; r.frontFace = g_st.frontFace;
    r.blendSrc = g_st.blendSrc; r.blendDst = g_st.blendDst;
    r.alphaFunc = g_st.alphaFunc; r.alphaRef = g_st.alphaRef;
    memcpy(r.viewport, g_st.viewport, sizeof(r.viewport));
    memcpy(r.scissor,  g_st.scissor,  sizeof(r.scissor));
    r.lockFirst = g_st.lockFirst; r.lockCount = g_st.lockCount;
    r.cls = cls;
    memcpy(r.mvUsed, r.modelview, sizeof(r.mvUsed));   // untouched until proven otherwise
    r.transformed = 0;
    return recIdx;
}

// Fill in what a draw actually rendered under, once the transform has run.
static void NoteRecordUsed(int recIdx, const float used[16], int transformed)
{
    if (recIdx < 0 || recIdx >= g_capCount) return;
    memcpy(g_cap[recIdx].mvUsed, used, 16 * sizeof(float));
    g_cap[recIdx].transformed = transformed;
}

// How far, in SCREEN PIXELS, the transform actually moved this surface.
//
// A screenshot pair can be defeated by one frame of idle animation; this
// cannot. But it has to be measured on the SURFACE, not on the batch's matrix
// origin, and the difference is not academic.
//
// The original version projected model-space (0,0,0) through both matrices.
// That is fine for the three gun batches the engine places with a real
// model->eye matrix. It is meaningless for the other four viewmodel draws --
// the hands, the sleeves and two gun sub-surfaces -- because those arrive
// already skinned into CAMERA-RELATIVE coordinates, so their modelview is a
// pure axis swap whose translation is exactly zero. Their "origin" is the eye
// itself: clip w = -z_eye = 0, and a 10 cm transform reported 146004688 px.
// The near-zero-w guard did not catch it either, because the residual
// translation was ~2e-5 rather than 0, which cleared a 1e-6 threshold.
//
// So: sample REAL VERTICES the draw references, project each through the
// engine's matrix and through ours, and report the distribution. A sample is
// only allowed to testify if it is actually in front of the near plane in
// BOTH projections -- geometry the rasteriser would clip cannot tell us
// anything about what moved on screen.
enum { GLS_SHIFT_SAMPLES = 24 };

struct ShiftAccum {
    float median, p95, maxPx;
    float lastX, lastY;
    unsigned valid, rejected, draws;
};
static ShiftAccum g_shift;

// Project a model-space point. Returns 0 if the sample must not testify.
static int ProjectPoint(const float mv[16], const float pj[16], const int vp[4],
                        const float v[3], float zNear, float out[2])
{
    float m[16];
    MatMul(pj, mv, m);
    const float x = m[0]*v[0] + m[4]*v[1] + m[8] *v[2] + m[12];
    const float y = m[1]*v[0] + m[5]*v[1] + m[9] *v[2] + m[13];
    const float w = m[3]*v[0] + m[7]*v[1] + m[11]*v[2] + m[15];
    if (!(w > zNear)) return 0;            // behind, on, or inside the near plane
    if (!(w == w) || !(x == x) || !(y == y)) return 0;      // NaN
    out[0] = vp[0] + (x / w * 0.5f + 0.5f) * (float)vp[2];
    out[1] = vp[1] + (y / w * 0.5f + 0.5f) * (float)vp[3];
    return 1;
}

// Near plane out of a GL perspective matrix: m14 = -2fn/(f-n), m10 = -(f+n)/(f-n),
// so n = m14 / (m10 - 1). Degenerate matrices fall back to a small positive.
static float NearFromProjection(const float pj[16])
{
    const float d = pj[10] - 1.0f;
    if (d > -1e-6f) return 0.05f;
    const float n = pj[14] / d;
    return (n > 1e-4f && n < 1e6f) ? n : 0.05f;
}

static float MedianOf(float* v, int n)
{
    for (int i = 1; i < n; ++i) {          // insertion sort; n <= 24
        const float k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; --j; }
        v[j + 1] = k;
    }
    return (n & 1) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

static float PercentileSorted(const float* v, int n, float q)
{
    if (n <= 0) return 0.0f;
    int i = (int)(q * (float)(n - 1) + 0.5f);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return v[i];
}

// ---------------------------------------------------------------------------
// IS THIS "POINTER" ACTUALLY A POINTER?
//
// THE 2026-08-26 CRASH-TO-DESKTOP
//
// The layer used to dereference g_st.vertex.p directly. It faulted on the
// first stereo scene of the first headset run that reached gameplay:
//
//     Faulting module   opengl32.DLL     Exception  0xc0000005
//     Fault offset      0x00021c3a   ->  SampledVertex, "mov eax,[edx]"
//
// The engine does not hand us a CPU pointer. Every one of the 125 draws in the
// archived capture records a vertex "pointer" under 64 KB -- 0x14, 0x1E0,
// 0x82C8, 0x12C0. Those are BYTE OFFSETS into a bound vertex buffer object.
// With a VBO bound, glVertexPointer's last argument stops meaning an address,
// and the first 64 KB of a Win32 address space is never mapped.
//
// We never saw the binds: the proxy intercepts glVertexPointer but not
// glBindBufferARB, and the engine resolves buffer-object entry points through
// wglGetProcAddress. So the layer cannot know, from the calls it sees, whether
// what it stored is an address or an offset.
//
// WHAT CAN BE KNOWN
//
// GL will not tell us how long a client array is, nor whether a buffer is
// bound. But the operating system will tell us whether an address is readable,
// and that is the question that actually matters. A real client array lives in
// committed, readable pages; a buffer offset does not.
//
// So: probe before reading, and refuse rather than guess. A refusal costs a
// pivot or a hand transform for one draw. A wrong guess costs the process.

// The last few ranges proved readable. Client arrays are re-read many times
// per frame from the same allocations, so this turns the common case into a
// handful of compares and keeps VirtualQuery off the hot path. It is a small
// RING rather than one slot because a draw now probes BOTH its vertex array
// and its index array -- two ranges, alternating, which a single slot would
// thrash on every draw.
enum { GLS_PROBE_CACHE = 4 };
static uintptr_t g_probeLo[GLS_PROBE_CACHE], g_probeHi[GLS_PROBE_CACHE];
static int       g_probeNext;

// No viewmodel batch has an index anywhere near this. An index beyond it is a
// misparse, not geometry, and chasing it would compute an address from noise.
enum { GLS_MAX_SANE_INDEX = 1 << 20 };

static void GLSurf_ForgetProbeCache()
{
    memset(g_probeLo, 0, sizeof(g_probeLo));
    memset(g_probeHi, 0, sizeof(g_probeHi));
    g_probeNext = 0;
}

// Can [base, base+bytes) be read on the CPU right now?
static int ClientRangeReadable(const void* base, size_t bytes)
{
    if (!base || bytes == 0)
        return 0;
    const uintptr_t lo = (uintptr_t)base;
    const uintptr_t hi = lo + bytes;
    if (hi < lo) {                       // address arithmetic wrapped
        ++g_stats.rejectedWildIndex;
        return 0;
    }
    // The null page, and everything the loader reserves below it. This is the
    // VBO-offset case, caught without asking the kernel anything.
    if (lo < 0x10000) {
        ++g_stats.rejectedNullPage;
        return 0;
    }
    for (int i = 0; i < GLS_PROBE_CACHE; ++i)
        if (lo >= g_probeLo[i] && hi <= g_probeHi[i] && g_probeHi[i]) {
            ++g_stats.probeCacheHits;
            return 1;
        }

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t cur = lo;
    uintptr_t regionLo = 0, regionHi = 0;
    for (int guard = 0; guard < 16 && cur < hi; ++guard) {
        ++g_stats.probeQueries;
        if (VirtualQuery((LPCVOID)cur, &mbi, sizeof(mbi)) != sizeof(mbi)) {
            ++g_stats.rejectedUnmapped;
            return 0;
        }
        const DWORD prot = mbi.Protect & 0xFF;
        const int readable =
            mbi.State == MEM_COMMIT
            && prot != PAGE_NOACCESS
            && !(mbi.Protect & PAGE_GUARD);
        if (!readable) {
            ++g_stats.rejectedUnmapped;
            return 0;
        }
        const uintptr_t rlo = (uintptr_t)mbi.BaseAddress;
        const uintptr_t rhi = rlo + mbi.RegionSize;
        if (regionHi == 0) { regionLo = rlo; regionHi = rhi; }
        else if (rlo == regionHi) { regionHi = rhi; }   // contiguous, extend
        else { regionLo = rlo; regionHi = rhi; }
        cur = rhi;
    }
    if (cur < hi) {
        ++g_stats.rejectedUnmapped;
        return 0;
    }
    // Cache only what was actually walked and proved.
    if (regionLo <= lo && regionHi >= hi) {
        g_probeLo[g_probeNext] = regionLo;
        g_probeHi[g_probeNext] = regionHi;
        g_probeNext = (g_probeNext + 1) % GLS_PROBE_CACHE;
    }
    return 1;
}

static int IndexElemSize(unsigned type)
{
    switch (type) {
        case GLS_GL_UNSIGNED_INT:   return 4;
        case GLS_GL_UNSIGNED_SHORT: return 2;
        case GLS_GL_UNSIGNED_BYTE:  return 1;
        default:                    return 0;
    }
}

// The index array as something the CPU may read. With an element buffer bound
// the "pointer" is a byte offset and the read goes to the CODVR shadow; with
// none it is the engine's own pointer, proved readable first. The DRAW is
// always issued with the ORIGINAL pointer/offset -- this resolution decides
// only what WE read, never what the driver receives.
static const void* IndexArrayBase(unsigned type, const void* idx, int count)
{
    if (count <= 0) return nullptr;
    const int esz = IndexElemSize(type);
    if (!esz) return nullptr;
    const size_t bytes = (size_t)count * (size_t)esz;
    const unsigned ebo = VBO_BoundElementBuffer();
    if (ebo) {
        // Null is a legal offset (0). The shadow's bounds check is the safety.
        const void* s = VBO_ResolveRange(ebo, (size_t)(uintptr_t)idx, bytes);
        if (!s) { ++g_stats.vboIdxMisses; return nullptr; }
        ++g_stats.vboIdxResolves;
        return s;
    }
    if (!idx || !ClientRangeReadable(idx, bytes)) return nullptr;
    return idx;
}

// The whole vertex-array question in one call: the current vertex array as
// something the CPU may READ for indices up to `maxIndex`, or null.
//
// Two sourcing modes, decided by the binding glVertexPointer latched:
//   client array  -> the engine's own pointer, proved readable by the OS
//   buffer object -> the recorded byte offset resolved into the CODVR shadow
//                    (vr_vbo). The null-page rule does NOT apply there: an
//                    offset under 64 KB is the NORMAL case, and the shadow's
//                    bounds check is what stands between us and nonsense.
static const unsigned char* VertexArrayBase(int maxIndex, int* strideOut)
{
    if (g_st.vertex.type != GLS_GL_FLOAT || g_st.vertex.size < 3)
        return nullptr;
    if (!g_st.vertex.p && !g_st.vertex.buffer)    // offset 0 into a VBO is legal
        return nullptr;
    if (maxIndex < 0 || maxIndex > GLS_MAX_SANE_INDEX) {
        ++g_stats.rejectedWildIndex;
        return nullptr;
    }
    const int stride = g_st.vertex.stride ? g_st.vertex.stride
                                          : (int)(g_st.vertex.size * sizeof(float));
    if (stride <= 0 || stride > (1 << 16)) {
        ++g_stats.rejectedAmbiguous;
        return nullptr;
    }
    const size_t bytes = (size_t)maxIndex * (size_t)stride + 3 * sizeof(float);
    if (g_st.vertex.buffer) {
        const void* s = VBO_ResolveRange(g_st.vertex.buffer,
                                         (size_t)(uintptr_t)g_st.vertex.p, bytes);
        if (!s) { ++g_stats.vboVertMisses; return nullptr; }
        ++g_stats.vboVertResolves;
        if (strideOut) *strideOut = stride;
        return (const unsigned char*)s;
    }
    if (!ClientRangeReadable(g_st.vertex.p, bytes))
        return nullptr;
    if (strideOut) *strideOut = stride;
    return (const unsigned char*)g_st.vertex.p;
}

// The palm and gun solves touch only a few landmark vertices of a large span.
// For a buffer-object array the span is resolved LOOSELY -- allocated, in
// range, not stale -- and every landmark is then checked for definedness
// through GLSurfCanReadVertex before it is read (vr_handpose's read gate).
// This is the "refuse before the solver" contract: no undefined byte reaches
// Horn's method, and no zero-filled substitute is ever read as geometry.
// Client arrays were proved readable by the OS and need no per-vertex gate.
struct GLSurfReadCtx {
    unsigned id;          // 0 = client array
    size_t   baseOff;     // byte offset of vertex 0 inside the buffer
    int      stride;
};

static const unsigned char* VertexArrayBaseForSolve(int maxIndex, int* strideOut,
                                                    GLSurfReadCtx* rc,
                                                    VBOResolveInfo* info)
{
    if (rc) { rc->id = 0; rc->baseOff = 0; rc->stride = 0; }
    if (info) memset(info, 0, sizeof(*info));
    if (g_st.vertex.type != GLS_GL_FLOAT || g_st.vertex.size < 3)
        return nullptr;
    if (!g_st.vertex.p && !g_st.vertex.buffer)    // offset 0 into a VBO is legal
        return nullptr;
    if (maxIndex < 0 || maxIndex > GLS_MAX_SANE_INDEX) {
        ++g_stats.rejectedWildIndex;
        return nullptr;
    }
    const int stride = g_st.vertex.stride ? g_st.vertex.stride
                                          : (int)(g_st.vertex.size * sizeof(float));
    if (stride <= 0 || stride > (1 << 16)) {
        ++g_stats.rejectedAmbiguous;
        return nullptr;
    }
    const size_t bytes = (size_t)maxIndex * (size_t)stride + 3 * sizeof(float);
    if (g_st.vertex.buffer) {
        const size_t off = (size_t)(uintptr_t)g_st.vertex.p;
        const void* s = VBO_ResolveAllocated(g_st.vertex.buffer, off, bytes, info);
        if (!s) { ++g_stats.vboVertMisses; return nullptr; }
        ++g_stats.vboVertResolves;
        if (strideOut) *strideOut = stride;
        if (rc) { rc->id = g_st.vertex.buffer; rc->baseOff = off; rc->stride = stride; }
        return (const unsigned char*)s;
    }
    if (!ClientRangeReadable(g_st.vertex.p, bytes))
        return nullptr;
    if (strideOut) *strideOut = stride;
    return (const unsigned char*)g_st.vertex.p;
}

// The read gate the solvers call per landmark (VRHandPoseCanRead).
static int GLSurfCanReadVertex(void* ctx, int vi)
{
    const GLSurfReadCtx* rc = (const GLSurfReadCtx*)ctx;
    if (!rc || rc->id == 0) return 1;             // client array: OS-proved
    if (vi < 0) return 0;
    return VBO_RangeDefined(rc->id,
                            rc->baseOff + (size_t)vi * (size_t)rc->stride,
                            3 * sizeof(float));
}

// Read one referenced vertex. Returns 0 when the array is not something we
// can read (non-float, fewer than 3 components, absent, or unresolvable).
//
// CONTRACT: `idx` must already be CPU-readable -- the caller resolves it
// through IndexArrayBase() ONCE per draw, not per sample. The vertex side is
// re-resolved here per index because `vi` comes out of the engine's data and
// is bounded by nothing we can see.
static int SampledVertex(unsigned type, const void* idx, int count, int k, float out[3])
{
    int vi;
    if (idx) {
        if (type == GLS_GL_UNSIGNED_INT)        vi = (int)((const unsigned*)idx)[k];
        else if (type == GLS_GL_UNSIGNED_SHORT) vi = (int)((const unsigned short*)idx)[k];
        else if (type == GLS_GL_UNSIGNED_BYTE)  vi = (int)((const unsigned char*)idx)[k];
        else return 0;
    } else {
        vi = k;                                 // DrawArrays: k is already the vertex
    }
    (void)count;
    // PROBE, THEN READ -- this is the read that took the process down on
    // 2026-08-26, when a buffer-object offset was walked as an address.
    int stride = 0;
    const unsigned char* base = VertexArrayBase(vi, &stride);
    if (!base)
        return 0;
    const float* s = (const float*)(base + (size_t)vi * stride);
    out[0] = s[0]; out[1] = s[1]; out[2] = s[2];
    return 1;
}

static void NoteTransformGeometry(const float engineMv[16], const float wantedMv[16],
                                  unsigned type, const void* idx, int count)
{
    if (!g_drv.getFloatv || count <= 0) return;
    float pj[16];
    g_drv.getFloatv(GLS_GL_PROJECTION_MATRIX, pj);
    const int vp[4] = { g_st.viewport[0], g_st.viewport[1],
                        g_st.viewport[2], g_st.viewport[3] };
    if (vp[2] < 2 || vp[3] < 2) return;
    const float zNear = NearFromProjection(pj);

    // Resolve the index array once for the whole sample loop; SampledVertex's
    // contract is that its idx is already CPU-readable. A null pointer with
    // an element buffer bound is offset 0, not "no indices".
    if (idx || VBO_BoundElementBuffer()) {
        idx = IndexArrayBase(type, idx, count);
        if (!idx) return;                                     // unreadable indices
    }

    float d[GLS_SHIFT_SAMPLES];
    float sumX = 0.0f, sumY = 0.0f;
    int n = 0;
    const int step = (count > GLS_SHIFT_SAMPLES) ? (count / GLS_SHIFT_SAMPLES) : 1;
    for (int k = 0; k < count && n < GLS_SHIFT_SAMPLES; k += step) {
        float v[3], a[2], b[2];
        if (!SampledVertex(type, idx, count, k, v)) return;   // unreadable array
        if (!ProjectPoint(engineMv, pj, vp, v, zNear, a) ||
            !ProjectPoint(wantedMv, pj, vp, v, zNear, b)) {
            ++g_shift.rejected;
            continue;
        }
        const float dx = b[0] - a[0], dy = b[1] - a[1];
        sumX += dx; sumY += dy;
        d[n++] = sqrtf(dx * dx + dy * dy);
    }
    if (n == 0) return;

    g_shift.lastX = sumX / (float)n;
    g_shift.lastY = sumY / (float)n;
    g_shift.median = MedianOf(d, n);                 // sorts d in place
    g_shift.p95 = PercentileSorted(d, n, 0.95f);
    if (d[n - 1] > g_shift.maxPx) g_shift.maxPx = d[n - 1];
    g_shift.valid += (unsigned)n;
    ++g_shift.draws;
}

// ---------------------------------------------------------- class pivots
//
// Where a surface actually IS, in eye space, so a rotation can be applied
// about the surface instead of about the camera.
//
// This cannot be read off the modelview. Three of the five gun batches carry a
// real model->eye matrix whose translation column is the gun's origin, but the
// hands, the sleeves and the other two gun batches arrive already skinned into
// camera-relative coordinates: their modelview is a pure axis swap and its
// translation is ~1e-5. Their authored position lives in the VERTICES.
//
// So the pivot is the eye-space centroid of a sample of the vertices the draw
// references -- which is defined for both families, needs no assumption about
// which one a batch belongs to, and is what the first gun_yaw probe should
// have rotated about. That probe rotated about eye-space (0,0,0) instead, and
// swung the weapon around the player's head on a 10-unit arm.
enum { GLS_PIVOT_SAMPLES = 16 };

static float g_classPivot[GLSC_COUNT][3];
static int   g_classPivotValid[GLSC_COUNT];

static void NoteClassPivot(int cls, unsigned type, const void* idx, int count)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT || count <= 0) return;
    if (!g_drv.getFloatv) return;
    // The per-sample probe in SampledVertex is the real defence; this is the
    // cheap early-out so an unusable array costs nothing per draw. A null
    // pointer with a buffer bound is offset 0, which is legal.
    if ((!g_st.vertex.p && !g_st.vertex.buffer) ||
        g_st.vertex.type != GLS_GL_FLOAT || g_st.vertex.size < 3)
        return;
    // Resolve the index array once for the whole sample loop (SampledVertex's
    // contract); an unreadable index array refuses the pivot, never faults.
    // Null with an element buffer bound is offset 0.
    if (idx || VBO_BoundElementBuffer()) {
        idx = IndexArrayBase(type, idx, count);
        if (!idx) return;
    }
    float mv[16];
    g_drv.getFloatv(GLS_GL_MODELVIEW_MATRIX, mv);

    float sx = 0.0f, sy = 0.0f, sz = 0.0f;
    int n = 0;
    const int step = (count > GLS_PIVOT_SAMPLES) ? (count / GLS_PIVOT_SAMPLES) : 1;
    for (int k = 0; k < count && n < GLS_PIVOT_SAMPLES; k += step) {
        float v[3];
        if (!SampledVertex(type, idx, count, k, v)) return;
        sx += mv[0]*v[0] + mv[4]*v[1] + mv[8] *v[2] + mv[12];
        sy += mv[1]*v[0] + mv[5]*v[1] + mv[9] *v[2] + mv[13];
        sz += mv[2]*v[0] + mv[6]*v[1] + mv[10]*v[2] + mv[14];
        ++n;
    }
    if (n == 0) return;
    g_classPivot[cls][0] = sx / (float)n;
    g_classPivot[cls][1] = sy / (float)n;
    g_classPivot[cls][2] = sz / (float)n;
    g_classPivotValid[cls] = 1;
}

int GLSurf_ClassPivot(int cls, float outEye[3])
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT || !g_classPivotValid[cls]) return 0;
    if (outEye) {
        outEye[0] = g_classPivot[cls][0];
        outEye[1] = g_classPivot[cls][1];
        outEye[2] = g_classPivot[cls][2];
    }
    return 1;
}

void GLSurf_BuildDeltaAboutPivot(const float pivotEye[3], float yawDeg,
                                 float dx, float dy, float dz, float out[16])
{
    const float rad = yawDeg * 3.14159265358979f / 180.0f;
    const float c = cosf(rad), s = sinf(rad);
    float r[16];
    MatIdentity(r);
    r[0] =  c; r[8]  = s;                 // yaw about eye-space Y (up)
    r[2] = -s; r[10] = c;

    // out = T(d) * T(p) * R * T(-p):  spin the surface where it stands, THEN
    // translate it in eye space.
    //
    // The other order -- rotating after translating -- would swing the offset
    // itself around the pivot, so asking for "10 cm right and 15 degrees of
    // yaw" would not actually put the surface 10 cm right. With no pivot this
    // reduces to T(d)*R, whose translation column is exactly d, which is the
    // property the probe's own test pins.
    const float px = pivotEye ? pivotEye[0] : 0.0f;
    const float py = pivotEye ? pivotEye[1] : 0.0f;
    const float pz = pivotEye ? pivotEye[2] : 0.0f;
    float tNeg[16], tPos[16], tOff[16], a[16], b[16];
    MatIdentity(tNeg); tNeg[12] = -px; tNeg[13] = -py; tNeg[14] = -pz;
    MatIdentity(tPos); tPos[12] =  px; tPos[13] =  py; tPos[14] =  pz;
    MatIdentity(tOff); tOff[12] =  dx; tOff[13] =  dy; tOff[14] =  dz;
    MatMul(r,    tNeg, a);        // R * T(-p)
    MatMul(tPos, a,    b);        // T(p) * R * T(-p)   -- rotate in place
    MatMul(tOff, b,    out);      // T(d) * that        -- then translate
}

void GLSurf_GetShiftMetrics(GLSurfShift* out)
{
    if (!out) return;
    out->medianPx = g_shift.median;
    out->p95Px = g_shift.p95;
    out->maxPx = g_shift.maxPx;
    out->lastX = g_shift.lastX;
    out->lastY = g_shift.lastY;
    out->validSamples = g_shift.valid;
    out->rejectedNearW = g_shift.rejected;
    out->draws = g_shift.draws;
}

void GLSurf_GetShift(float* lastX, float* lastY, float* maxPx, unsigned* samples)
{
    if (lastX)   *lastX   = g_shift.lastX;
    if (lastY)   *lastY   = g_shift.lastY;
    if (maxPx)   *maxPx   = g_shift.maxPx;
    if (samples) *samples = g_shift.valid;
}

// The RETIRED origin-based diagnostic, kept only so the suite can demonstrate
// that it blows up on exactly the draws the real metric handles. Nothing in
// the shipping path calls this.
float GLSurf_TestOriginShiftX(const float mv[16], const float pj[16], const int vp[4])
{
    float m[16];
    MatMul(pj, mv, m);
    const float cw = m[15];
    if (cw <= 1e-6f) return 0.0f;              // the guard as it originally stood
    return vp[0] + (m[12] / cw * 0.5f + 0.5f) * (float)vp[2];
}

// Route A: left-multiply the delta onto whatever modelview the engine loaded,
// issue the draw, then put the engine's matrix back byte-for-byte.
static int TransformDrawModelview(unsigned mode, int count, unsigned type,
                                  const void* idx, const float delta[16])
{
    if (!g_drv.getFloatv || !g_drv.loadMatrixf || !g_drv.drawElements) {
        ++g_stats.rejectedNoDriver;
        return 0;
    }
    float engine[16], wanted[16];
    // The engine may have left GL_PROJECTION selected; the modelview is what
    // we replace, so select it explicitly and put the mode back after.
    const unsigned prevMode = g_st.matrixMode;
    if (prevMode != GLS_GL_MODELVIEW && g_drv.matrixMode)
        g_drv.matrixMode(GLS_GL_MODELVIEW);

    g_drv.getFloatv(GLS_GL_MODELVIEW_MATRIX, engine);
    MatMul(delta, engine, wanted);
    NoteTransformGeometry(engine, wanted, type, idx, count);
    g_drv.loadMatrixf(wanted);
    g_drv.drawElements(mode, count, type, idx);
    g_drv.loadMatrixf(engine);                 // restore, byte-for-byte
    ++g_stats.matrixRestores;

    if (prevMode != GLS_GL_MODELVIEW && g_drv.matrixMode)
        g_drv.matrixMode(prevMode);

    memcpy(g_lastLoaded, wanted, sizeof(wanted));
    memcpy(g_lastRestored, engine, sizeof(engine));
    g_lastTransformValid = 1;
    return 1;
}

// Route B: copy the referenced vertices into CODVR-owned scratch, transform
// positions AND normals rigidly, point GL at the scratch, draw, restore the
// engine's own pointers. Texture coordinates, colours and indices are never
// touched, and no engine-owned byte is written.
static int TransformDrawScratch(unsigned mode, int count, unsigned type,
                                const void* idx, const float delta[16])
{
    if (!g_drv.drawElements || !g_drv.vertexPointer) { ++g_stats.rejectedNoDriver; return 0; }

    // With a buffer object bound, glVertexPointer(scratch) would hand the
    // driver our scratch ADDRESS as a byte OFFSET -- garbage geometry, or a
    // driver-side out-of-range read. Issuing scratch arrays safely would mean
    // unbinding and rebinding the engine's buffers around the draw; until that
    // is built and tested, this route refuses and the draw falls through to
    // the engine untransformed. The modelview route handles VBO draws fine.
    if (g_st.vertex.buffer || g_st.normal.buffer) {
        ++g_stats.rejectedVboScratch;
        return 0;
    }

    int lo = 0, hi = 0;
    if (!IndexRange(type, idx, count, &lo, &hi)) { ++g_stats.rejectedAmbiguous; return 0; }
    if (hi >= GLS_SCRATCH_VERTS)                 { ++g_stats.rejectedAmbiguous; return 0; }

    // This route copies every referenced vertex out of the engine's array.
    // Same hazard, same probe: an unreadable base is a refusal, not a fault.
    int vStride = 0;
    const unsigned char* vsrc = VertexArrayBase(hi, &vStride);
    if (!vsrc) return 0;
    for (int i = lo; i <= hi; ++i) {
        const float* s = (const float*)(vsrc + (size_t)i * vStride);
        float* d = &g_scratchPos[i * 3];
        // Full 4x4 apply: rigid, so w stays 1 and the translation column lands.
        d[0] = delta[0]*s[0] + delta[4]*s[1] + delta[8] *s[2] + delta[12];
        d[1] = delta[1]*s[0] + delta[5]*s[1] + delta[9] *s[2] + delta[13];
        d[2] = delta[2]*s[0] + delta[6]*s[1] + delta[10]*s[2] + delta[14];
    }

    // The normal array is a second client array with the same problem, and it
    // is bound by the same buffer object. Probe it on its own terms.
    const int nStride = g_st.normal.stride ? g_st.normal.stride : (int)(3 * sizeof(float));
    const bool doNormals = (g_st.normal.enabled && g_st.normal.p &&
                            g_st.normal.type == GLS_GL_FLOAT && g_drv.normalPointer &&
                            nStride > 0 &&
                            ClientRangeReadable(g_st.normal.p,
                                                (size_t)hi * (size_t)nStride
                                                + 3 * sizeof(float)));
    if (doNormals) {
        const unsigned char* nsrc = (const unsigned char*)g_st.normal.p;
        for (int i = lo; i <= hi; ++i) {
            const float* s = (const float*)(nsrc + (size_t)i * nStride);
            MatRotVec3(delta, s, &g_scratchNrm[i * 3]);   // rotation only
        }
    }

    g_drv.vertexPointer(3, GLS_GL_FLOAT, 3 * (int)sizeof(float), g_scratchPos);
    if (doNormals)
        g_drv.normalPointer(GLS_GL_FLOAT, 3 * (int)sizeof(float), g_scratchNrm);

    g_drv.drawElements(mode, count, type, idx);

    // Put the engine's own arrays back exactly as they were.
    g_drv.vertexPointer(g_st.vertex.size, g_st.vertex.type, g_st.vertex.stride, g_st.vertex.p);
    if (doNormals)
        g_drv.normalPointer(g_st.normal.type, g_st.normal.stride, g_st.normal.p);
    ++g_stats.matrixRestores;
    g_lastTransformValid = 0;
    return 1;
}

// Does a merged batch really partition at its midpoint?
//
// The asset says it must: the two hand shells share no vertex and no triangle
// crosses between them, so the first half's indices must all lie below the
// vertex midpoint and the second half's at or above it. That is a claim about
// THIS frame's index buffer, not about the file, so it is checked here rather
// than assumed -- a batch that fails is left completely alone.
static int VerifyMidpointSplit(unsigned type, const void* idx, int count, int vertHalf)
{
    const int esz = IndexElemSize(type);
    if (!esz || count < 2 || (count & 1)) return 0;
    // A null pointer is a legal byte offset (0) when an element buffer is
    // bound; IndexRange resolves either form. Only a null with NO buffer is
    // an absent array.
    if (!idx && !VBO_BoundElementBuffer()) return 0;
    const int half = count / 2;
    int lo0, hi0, lo1, hi1;
    if (!IndexRange(type, idx, half, &lo0, &hi0)) return 0;
    if (!IndexRange(type, (const unsigned char*)idx + (size_t)half * esz, half, &lo1, &hi1))
        return 0;
    if (hi0 >= vertHalf) return 0;      // first half reached into the second's verts
    if (lo1 <  vertHalf) return 0;      // second half reached back into the first's
    return 1;
}

static unsigned g_splitVerified, g_splitRejected;

// ---------------------------------------------------------------------------
// POSE-DRIVEN HANDS
//
// The delta for a hand is not a constant. Where the hand currently is changes
// every frame with the animation, so a fixed matrix would drag the animation
// along with the controller instead of replacing it. What is constant over an
// XR pair is the ABSOLUTE pose the player's controller asks for, and that is
// what gets latched; each draw's delta is Desired_eye * inverse(Authored_eye)
// against the pose recovered from THIS draw's vertices, so the pivot is this
// eye's own palm and the animation advance between the two game frames is
// absorbed rather than rendered as disparity.

struct HandTarget {
    int   enabled;
    int   hold;            // diagnostic: solve, then identity delta
    float posMid[3];       // desired palm origin, head-midpoint eyeish frame
    float axis[9];         // desired basis, rows-as-axes, same frame
};
static HandTarget      g_handTarget[GLSC_COUNT];
static GLSurfHandSolve g_handSolve[GLSC_COUNT];
static int             g_solveWanted[GLSC_COUNT];

// PER-EYE authored baselines, and the pair-level ownership gate they feed.
//
// THE FAILURE THIS CLOSES. The palm solve runs on each eye's OWN draw, so eye
// 0 and eye 1 can disagree about whether a hand is recoverable at all -- and
// with a real refusal rate they will. A build that acts on each answer as it
// arrives renders one eye's hand on the controller and the other eye's on the
// weapon: a HALF-NATIVE PAIR, which is not a cosmetic glitch but two
// irreconcilable images of the same hand, the same class of stereo fault as
// the 2026-08-23 moving-camera nausea build.
//
// So native ownership is a property of the PAIR, never of a draw. The wiring
// decides it once per pair from BOTH eyes' evidence and installs it here;
// until it is set, a hand delta is refused no matter how well this eye's own
// solve went. Fail-closed: the default is 0.
static GLSurfHandSolve g_handSolveEye[GLSC_COUNT][2];
// Scene index of the last solve ATTEMPT by each eye -- success or refusal.
// This is what says an eye is PARTICIPATING. An eye that has not rendered this
// class at all poses no stereo hazard (a monoscopic context: the flat probe,
// or the game before VR engages); an eye that HAS rendered and produced no
// usable baseline is exactly the half-native hazard and must fail the pair.
// 0 = never attempted.
static unsigned        g_handEyeAttempt[GLSC_COUNT][2];
static int             g_pairEligible[GLSC_COUNT];
static unsigned        g_pairGateRefusals[GLSC_COUNT];

static int HandClassToVRHP(int cls)
{
    // GLSC_HAND_R / _L are the RENDERED sides; VRHP_TRIGGER / _SUPPORT mean
    // the same thing. Which BATCH HALF carries which side was re-corrected
    // 2026-08-28 -- see MergedHalves() above and the laterality note in
    // vr_handpose.h before changing either.
    if (cls == GLSC_HAND_R) return VRHP_TRIGGER;
    if (cls == GLSC_HAND_L) return VRHP_SUPPORT;
    return -1;
}

void GLSurf_SetHandTarget(int cls, int enabled, const float desiredPosMid[3],
                          const float desiredAxis[9])
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT || HandClassToVRHP(cls) < 0)
        return;
    HandTarget& t = g_handTarget[cls];
    t.enabled = enabled ? 1 : 0;
    t.hold = 0;
    if (desiredPosMid) memcpy(t.posMid, desiredPosMid, sizeof(t.posMid));
    else               memset(t.posMid, 0, sizeof(t.posMid));
    if (desiredAxis) memcpy(t.axis, desiredAxis, sizeof(t.axis));
    else {
        memset(t.axis, 0, sizeof(t.axis));
        t.axis[0] = t.axis[4] = t.axis[8] = 1.0f;
    }
}

void GLSurf_SetHandTargetHold(int cls, int on)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT || HandClassToVRHP(cls) < 0)
        return;
    HandTarget& t = g_handTarget[cls];
    t.enabled = on ? 1 : 0;
    t.hold    = on ? 1 : 0;
}

void GLSurf_ClearHandTargets()
{
    g_drawDepth = 0;
    g_reentered = 0;
    memset(g_handTarget, 0, sizeof(g_handTarget));
}

// ---- eye offsets, per-eye apply records, palm-attempt telemetry ----------

static float g_eyeOff[2][3];

void GLSurf_SetEyeOffsets(const float offEyeish[2][3])
{
    if (offEyeish) memcpy(g_eyeOff, offEyeish, sizeof(g_eyeOff));
    else           memset(g_eyeOff, 0, sizeof(g_eyeOff));
}

void GLSurf_GetEyeOffsets(float out[2][3])
{
    if (out) memcpy(out, g_eyeOff, sizeof(g_eyeOff));
}

static GLSurfEyeApply g_eyeApply[GLSC_COUNT][2];
static GLSurfPalmTry  g_palmTry[GLS_PALMTRY_MAX];
static int            g_palmTryHead, g_palmTryCount, g_palmTryTotal;
// The hand delta each hand class applied THIS scene: its sleeve takes the
// same rigid motion (see BuildSleeveDelta).
static struct { unsigned scene; int valid; float m[16]; } g_handSceneDelta[GLSC_COUNT];

static void Mat4ApplyPoint(const float m[16], const float p[3], float out[3]);

int GLSurf_GetEyeApply(int cls, int eye, GLSurfEyeApply* out)
{
    if (!(cls == GLSC_HAND_R || cls == GLSC_HAND_L || cls == GLSC_GUN)) return 0;
    if (eye != 0 && eye != 1) return 0;
    if (out) *out = g_eyeApply[cls][eye];
    return 1;
}

// A class was drawn by the current eye under the current latch. A record
// from an earlier scene of this eye is replaced: the verdict only ever asks
// about the serial being submitted.
static void NoteEyeDrew(int cls)
{
    if (g_curEye != 0 && g_curEye != 1) return;
    GLSurfEyeApply& a = g_eyeApply[cls][g_curEye];
    if (a.serial != g_latchSerial || a.scene != g_sceneFrameIndex) {
        memset(&a, 0, sizeof(a));
        a.serial = g_latchSerial;
        a.scene  = g_sceneFrameIndex;
    }
    a.drew = 1;
}

static void NoteEyeApplied(int cls, const float finalPos[3], const float finalAxis[9])
{
    if (g_curEye != 0 && g_curEye != 1) return;
    NoteEyeDrew(cls);
    GLSurfEyeApply& a = g_eyeApply[cls][g_curEye];
    a.applied = 1;
    memcpy(a.finalPos,  finalPos,  sizeof(a.finalPos));
    memcpy(a.finalAxis, finalAxis, sizeof(a.finalAxis));
}

// The delta was built but never reached GL (no driver entry point): the
// record must say vanilla, because vanilla is what was rendered.
static void NoteEyeApplyFailed(int cls)
{
    if (g_curEye != 0 && g_curEye != 1) return;
    g_eyeApply[cls][g_curEye].applied = 0;
}

static void PushPalmTry(const GLSurfPalmTry& t)
{
    if (g_palmTryTotal >= GLS_PALMTRY_MAX) return;   // the first N, ever
    ++g_palmTryTotal;
    g_palmTry[(g_palmTryHead + g_palmTryCount) % GLS_PALMTRY_MAX] = t;
    ++g_palmTryCount;
}

int GLSurf_PopPalmTry(GLSurfPalmTry* out)
{
    if (g_palmTryCount <= 0) return 0;
    if (out) *out = g_palmTry[g_palmTryHead];
    g_palmTryHead = (g_palmTryHead + 1) % GLS_PALMTRY_MAX;
    --g_palmTryCount;
    return 1;
}

void GLSurf_SetHandSolveWanted(int cls, int on)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT || HandClassToVRHP(cls) < 0)
        return;
    g_solveWanted[cls] = on ? 1 : 0;
}

int GLSurf_HandSolveWanted(int cls)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT) return 0;
    return g_solveWanted[cls];
}

// Recover this class's authored palm pose from the draw's own vertices and
// record the verdict in g_handSolve. The one solve everyone shares:
// BuildHandDelta uses it to move the hand, the solve-wanted path uses it to
// keep the BASELINE fresh while nothing is engaged yet, and the diagnostics
// read its counters. `outAuthored` may be null.
// Refusal-shape histogram (see vr_glsurf.h): which draw shapes refuse.
static GLSurfRefuseBucket g_refuseHisto[GLSC_COUNT][GLS_REFUSE_BUCKETS];
static unsigned           g_refuseOverflow[GLSC_COUNT];

static void NoteRefusalShape(int cls, int site, int count, int span,
                             float bestRmsU, int reason, unsigned unknownBytes,
                             unsigned gen, unsigned allocGen)
{
    GLSurfHandSolve& hs = g_handSolve[cls];
    hs.lastRefSite    = site;
    hs.lastRefCount   = count;
    hs.lastRefSpan    = span;
    hs.lastRefBestRmsM = bestRmsU;
    hs.lastRefReason  = reason;
    hs.lastRefUnknownBytes = unknownBytes;
    hs.lastRefGen     = gen;
    hs.lastRefAllocGen = allocGen;
    hs.lastRefBuffer  = g_st.vertex.buffer;
    hs.lastRefOffset  = (unsigned)(uintptr_t)g_st.vertex.p;
    {
        const int hand = HandClassToVRHP(cls);
        if (hand >= 0) {
            VRHandPoseSample smp;
            VRHandPose_GetLastSample(hand, &smp);
            hs.lastRefShape = smp.shape;
        }
    }
    for (int i = 0; i < GLS_REFUSE_BUCKETS; ++i) {
        GLSurfRefuseBucket& b = g_refuseHisto[cls][i];
        if (b.hits && b.count == count && b.span == span) { ++b.hits; return; }
        if (!b.hits) { b.count = count; b.span = span; b.hits = 1; return; }
    }
    ++g_refuseOverflow[cls];
}

int GLSurf_GetRefuseHisto(int cls, GLSurfRefuseBucket out[GLS_REFUSE_BUCKETS],
                          unsigned* overflow)
{
    if (overflow) *overflow = 0;
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT)
        return 0;
    int n = 0;
    for (int i = 0; i < GLS_REFUSE_BUCKETS; ++i)
        if (g_refuseHisto[cls][i].hits) {
            if (out) out[n] = g_refuseHisto[cls][i];
            ++n;
        }
    if (overflow) *overflow = g_refuseOverflow[cls];
    return n;
}

// ---------------------------------------------------------------------------
// SUBMITTED SPACE -> EYE SPACE
//
// The palm and gun solves read the RAW submitted vertex array, so they recover
// a pose in whatever space those vertices are in. The delta this layer builds
// is installed as `wanted = delta * engineModelview` (TransformDrawModelview),
// so it acts on EYE-space points -- and the controller-derived target that the
// delta has to reach is eye space as well (vr_native_hands' RoomPosToEyeish).
// Those have to be ONE space or the composition is meaningless.
//
// They were not. The live capture of 2026-08-27 records the hand batch's
// modelview as the Quake-view -> GL-eye permutation with ~0 translation
// (submitted +x maps to eye -z, det +1), so the recovered pose sat ~90 degrees
// away from the space it was being differenced against: the latched offset was
// `target(eye) - baseline(submitted)`, which is not a displacement in any
// space. Nothing caught it because EVERY offline case drove this path with an
// IDENTITY modelview, where the two spaces coincide exactly.
//
// So the solve is mapped into eye space HERE, at the single place it is
// produced, and everything downstream -- the pair-latched offset, the per-draw
// hand delta, the gun's rear-grip pivot -- is eye space throughout. The
// relative palm->weapon relation vr_native_hands composes is unaffected: it is
// expressed in the palm's own frame, so it never depended on which space the
// two absolute poses were reported in, only that they agreed.
static void PoseToEyeSpace(const float mv[16], VRHandPose* p)
{
    float pos[3];
    pos[0] = mv[0]*p->pos[0] + mv[4]*p->pos[1] + mv[8] *p->pos[2] + mv[12];
    pos[1] = mv[1]*p->pos[0] + mv[5]*p->pos[1] + mv[9] *p->pos[2] + mv[13];
    pos[2] = mv[2]*p->pos[0] + mv[6]*p->pos[1] + mv[10]*p->pos[2] + mv[14];
    memcpy(p->pos, pos, sizeof(pos));
    for (int r = 0; r < 3; ++r) {          // rows are DIRECTIONS: rotation only
        const float* v = p->axis + r * 3;
        const float o[3] = {
            mv[0]*v[0] + mv[4]*v[1] + mv[8] *v[2],
            mv[1]*v[0] + mv[5]*v[1] + mv[9] *v[2],
            mv[2]*v[0] + mv[6]*v[1] + mv[10]*v[2],
        };
        memcpy(p->axis + r * 3, o, sizeof(o));
    }
}

// The batch's own modelview, or 0 when it cannot be read -- in which case the
// solve must REFUSE rather than report a pose in the wrong space.
static int CurrentModelview(float mv[16])
{
    if (!g_drv.getFloatv) return 0;
    g_drv.getFloatv(GLS_GL_MODELVIEW_MATRIX, mv);
    return 1;
}

// Ownership / identity counters (vr_glsurf.h, GLSurfOwnerStats).
static GLSurfOwnerStats g_owner;
void GLSurf_GetOwnerStats(GLSurfOwnerStats* out) { if (out) *out = g_owner; }

// The surface order the merged batch being split was found in: a hint for
// the per-half solves so the common case costs one solve. -1 = unknown.
static int g_orderHint = -1;

static int SolveHandFromDraw(int cls, unsigned type, const void* idx, int count,
                             VRHandPose* outAuthored, int preferSwapped = -1)
{
    const int hand = HandClassToVRHP(cls);
    if (hand < 0) return 0;
    // This eye is participating in this class, whatever the verdict turns out
    // to be. Recorded BEFORE any refusal path, because a refusing eye is
    // precisely the one the pair gate has to know about.
    if (g_curEye == 0 || g_curEye == 1)
        g_handEyeAttempt[cls][g_curEye] = GLSurf_SceneFrameCount() + 1;
    NoteEyeDrew(cls);

    // The attempt record: sourcing first, verdict when known.
    GLSurfPalmTry pt;
    memset(&pt, 0, sizeof(pt));
    pt.serial = g_latchSerial;
    pt.eye = g_curEye;
    pt.cls = cls;
    pt.rig = -2;
    pt.vbo = g_st.vertex.buffer;
    pt.vertexOffset = (unsigned)(uintptr_t)g_st.vertex.p;
    pt.indexOffset  = (unsigned)(uintptr_t)idx;
    pt.rmsM = -1.0f;
    pt.gen      = pt.vbo ? VBO_Generation(pt.vbo)      : 0;
    pt.allocGen = pt.vbo ? VBO_AllocGeneration(pt.vbo) : 0;

    int lo = 0, hi = 0;
    if (!IndexRange(type, idx, count, &lo, &hi)) {
        g_handSolve[cls].valid = 0;
        ++g_handSolve[cls].refusals;
        ++g_handSolve[cls].refusedIdx;
        NoteRefusalShape(cls, 1, count, 0, -1.0f, VRHP_REASON_INDEX_RANGE, 0,
                         pt.gen, pt.allocGen);
        pt.site = 1; pt.reason = VRHP_REASON_INDEX_RANGE;
        PushPalmTry(pt);
        return 0;
    }
    pt.span = hi + 1;
    // The palm solve reads landmark vertices straight out of the vertex
    // array. Resolve the span -- to the engine's own pointer when it is a
    // proven-readable client array, to the CODVR shadow when it is a
    // buffer-object offset -- or refuse. For a shadow the span is resolved
    // LOOSELY here and every landmark is gated individually inside the
    // solver (GLSurfCanReadVertex): undefined bytes refuse BEFORE Horn's
    // method, with their own site and reason.
    int vStride = 0;
    GLSurfReadCtx rc;
    VBOResolveInfo vi;
    const unsigned char* vbase = VertexArrayBaseForSolve(hi, &vStride, &rc, &vi);
    pt.unknownBytes = (unsigned)vi.unknownBytes;
    pt.validRange = (vbase && vi.unknownBytes == 0) ? 1 : 0;
    if (!vbase) {
        g_handSolve[cls].valid = 0;
        ++g_handSolve[cls].refusals;
        ++g_handSolve[cls].refusedBase;
        NoteRefusalShape(cls, 2, count, hi + 1, -1.0f, VRHP_REASON_INDEX_RANGE,
                         pt.unknownBytes, pt.gen, pt.allocGen);
        pt.site = 2; pt.reason = VRHP_REASON_INDEX_RANGE;
        PushPalmTry(pt);
        return 0;
    }
    VRHandPose authored;
    int rig = -2;
    int swapped = 0;
    float bestRms = -1.0f;
    int reason = VRHP_REASON_INDEX_RANGE;
    // Engine-submitted vertices are in game units (1 unit = 1 inch, proven
    // live); the landmark tables are metres. Naming the units here is what
    // the 2026-08-27 run was missing -- without it every real hand draw
    // refused on the residual gate. SolveAny tries every shipped rig's
    // table (hands_new, hands_sten, hands4) in BOTH surface orders, so a
    // weapon that swaps the hand rig -- or an engine that packs the two
    // hand surfaces the other way round this frame (the 2026-09-03 flicker)
    // -- cannot silently lose its hands. Only landmarks inside THIS draw's
    // own index range [lo, hi] are read.
    if (!VRHandPose_SolveAnyEx(hand, (const float*)vbase, vStride, hi + 1,
                               CODVR_UNITS_PER_M, &authored, &rig, &bestRms,
                               GLSurfCanReadVertex, &rc, &reason,
                               &swapped, preferSwapped, lo)) {
        g_handSolve[cls].valid = 0;
        ++g_handSolve[cls].refusals;
        int site;
        if (reason == VRHP_REASON_BYTES_UNKNOWN) {
            ++g_handSolve[cls].refusedUnknownBytes;
            site = 4;
        } else {
            ++g_handSolve[cls].refusedSolve;
            site = 3;
        }
        NoteRefusalShape(cls, site, count, hi + 1, bestRms, reason,
                         pt.unknownBytes, pt.gen, pt.allocGen);
        pt.site = site; pt.reason = reason; pt.rmsM = bestRms;
        {
            VRHandPoseSample smp;
            VRHandPose_GetLastSample(hand, &smp);
            pt.shape = smp.shape;
        }
        PushPalmTry(pt);
        return 0;
    }
    // Into the space the delta is applied in (see PoseToEyeSpace). Without a
    // readable modelview the space is unknown, so refuse instead of reporting
    // a pose that would be differenced against an eye-space target.
    float mv[16];
    if (!CurrentModelview(mv)) {
        g_handSolve[cls].valid = 0;
        ++g_handSolve[cls].refusals;
        ++g_handSolve[cls].refusedBase;
        NoteRefusalShape(cls, 2, count, hi + 1, bestRms, VRHP_REASON_INDEX_RANGE,
                         pt.unknownBytes, pt.gen, pt.allocGen);
        pt.site = 2; pt.reason = VRHP_REASON_INDEX_RANGE; pt.rmsM = bestRms;
        PushPalmTry(pt);
        return 0;
    }
    // Keep the SUBMITTED-space pose before the eye mapping: it is the frame
    // in which the two eyes are directly comparable (see GLSurfHandSolve).
    memcpy(g_handSolve[cls].submPos,  authored.pos,  sizeof(authored.pos));
    memcpy(g_handSolve[cls].submAxis, authored.axis, sizeof(authored.axis));
    PoseToEyeSpace(mv, &authored);
    g_handSolve[cls].valid = 1;
    g_handSolve[cls].lastRig = rig;
    g_handSolve[cls].everSolved = 1;
    g_handSolve[cls].lastGoodScene = GLSurf_SceneFrameCount();
    g_handSolve[cls].lastOkBuffer = g_st.vertex.buffer;
    g_handSolve[cls].lastOkOffset = (unsigned)(uintptr_t)g_st.vertex.p;
    g_handSolve[cls].lastOkGen = pt.gen;
    g_handSolve[cls].lastSwapped = swapped;
    if (swapped) ++g_handSolve[cls].swappedSolves;
    pt.swapped = swapped;
    {
        VRHandPoseSample smp;
        VRHandPose_GetLastSample(hand, &smp);
        g_handSolve[cls].lastOkShape = smp.shape;
        pt.shape = smp.shape;
    }
    memcpy(g_handSolve[cls].pos,  authored.pos,  sizeof(authored.pos));
    memcpy(g_handSolve[cls].axis, authored.axis, sizeof(authored.axis));
    g_handSolve[cls].rmsM = authored.rmsM;
    ++g_handSolve[cls].solves;
    pt.ok = 1; pt.rig = rig; pt.reason = VRHP_REASON_OK;
    pt.rmsM = authored.rmsM / CODVR_UNITS_PER_M;
    PushPalmTry(pt);
    // The same recovery, filed under the eye that produced it. The pair gate
    // needs the two eyes SEPARATELY -- a single shared slot cannot answer
    // "did both eyes see the same physical hand".
    if (g_curEye == 0 || g_curEye == 1)
        g_handSolveEye[cls][g_curEye] = g_handSolve[cls];
    if (outAuthored) *outAuthored = authored;
    return 1;
}

// ---------------------------------------------------------------------------
// HAND IDENTITY FROM CONTENT, NEVER FROM DRAW ORDER
//
// The 2026-09-03 hardware log (docs/FLICKER-ROOT-CAUSE-2026-09-03.md) proved
// the engine packs the two hand surfaces in either order from frame to
// frame. Every ordinal or file-order assumption -- "first half is surface
// 0", "first single draw is the left hand" -- is therefore wrong on roughly
// half the frames. The only thing that identifies a hand is that its
// landmark table reproduces from the vertices; that is what is asked here,
// once per draw, quietly (no attempt is attributed until the hand is known).
static int IdentifyHandDraw(unsigned type, const void* idx, int count,
                            int* outCls, int* outSwapped)
{
    int lo = 0, hi = 0;
    if (!IndexRange(type, idx, count, &lo, &hi)) return 0;
    int vStride = 0;
    GLSurfReadCtx rc;
    VBOResolveInfo vi;
    const unsigned char* vbase = VertexArrayBaseForSolve(hi, &vStride, &rc, &vi);
    if (!vbase) return 0;
    int hand = -1, swapped = 0, rig = -2;
    if (!VRHandPose_Identify((const float*)vbase, vStride, lo, hi + 1,
                             CODVR_UNITS_PER_M, GLSurfCanReadVertex, &rc,
                             &hand, &swapped, &rig, nullptr))
        return 0;
    if (outCls) *outCls = (hand == VRHP_TRIGGER) ? GLSC_HAND_R : GLSC_HAND_L;
    if (outSwapped) *outSwapped = swapped;
    return 1;
}

// A sleeve is the forearm of a hand, and its CUFF vertices sit within an
// inch or two of that hand's palm origin while the other palm is a forearm's
// length away. So a sleeve draw (or one half of the merged sleeve batch) is
// attributed to the palm its nearest sampled vertex is closest to, using the
// palms THIS scene recovered. Compared in eye space through the sleeve's own
// modelview, the same space the palm solves are published in. Declared order
// is only the fallback when no palm solved this scene -- in which case the
// sleeve stays vanilla anyway (BuildSleeveDelta needs its hand's delta).
enum { GLS_SLEEVE_SAMPLES = 48 };
static int SleeveNearestPalm(unsigned type, const void* idx, int count,
                             float* outDR, float* outDL)
{
    if (count <= 0 || !g_drv.getFloatv) return 0;
    if ((!g_st.vertex.p && !g_st.vertex.buffer) ||
        g_st.vertex.type != GLS_GL_FLOAT || g_st.vertex.size < 3)
        return 0;
    const unsigned scene = GLSurf_SceneFrameCount();
    const int haveR = g_handSolve[GLSC_HAND_R].everSolved
                   && g_handSolve[GLSC_HAND_R].lastGoodScene == scene;
    const int haveL = g_handSolve[GLSC_HAND_L].everSolved
                   && g_handSolve[GLSC_HAND_L].lastGoodScene == scene;
    if (!haveR && !haveL) return 0;
    if (idx || VBO_BoundElementBuffer()) {
        idx = IndexArrayBase(type, idx, count);
        if (!idx) return 0;
    }
    float mv[16];
    g_drv.getFloatv(GLS_GL_MODELVIEW_MATRIX, mv);
    const float* pr = g_handSolve[GLSC_HAND_R].pos;
    const float* pl = g_handSolve[GLSC_HAND_L].pos;
    float bestR = 1e30f, bestL = 1e30f;
    int n = 0;
    const int step = (count > GLS_SLEEVE_SAMPLES) ? (count / GLS_SLEEVE_SAMPLES) : 1;
    for (int k = 0; k < count && n < GLS_SLEEVE_SAMPLES; k += step) {
        float v[3];
        if (!SampledVertex(type, idx, count, k, v)) return 0;
        const float ex = mv[0]*v[0] + mv[4]*v[1] + mv[8] *v[2] + mv[12];
        const float ey = mv[1]*v[0] + mv[5]*v[1] + mv[9] *v[2] + mv[13];
        const float ez = mv[2]*v[0] + mv[6]*v[1] + mv[10]*v[2] + mv[14];
        if (haveR) {
            const float d = sqrtf((ex-pr[0])*(ex-pr[0]) + (ey-pr[1])*(ey-pr[1]) + (ez-pr[2])*(ez-pr[2]));
            if (d < bestR) bestR = d;
        }
        if (haveL) {
            const float d = sqrtf((ex-pl[0])*(ex-pl[0]) + (ey-pl[1])*(ey-pl[1]) + (ez-pl[2])*(ez-pl[2]));
            if (d < bestL) bestL = d;
        }
        ++n;
    }
    if (n == 0) return 0;
    if (outDR) *outDR = haveR ? bestR : -1.0f;
    if (outDL) *outDL = haveL ? bestL : -1.0f;
    return (haveR ? 1 : 0) | (haveL ? 2 : 0);
}

// The nearest-palm rule with its margins: both palms known -> the nearer
// one, if it wins by a clear inch; one palm known -> that side if the cuff
// is within a hand's length of it, else the other side.
enum { GLS_SLEEVE_MARGIN_U = 1, GLS_SLEEVE_REACH_U = 8 };
static int IdentifySleeveDraw(unsigned type, const void* idx, int count,
                              int declaredCls)
{
    float dR = -1.0f, dL = -1.0f;
    const int have = SleeveNearestPalm(type, idx, count, &dR, &dL);
    int cls = GLSC_NONE;
    if (have == 3) {
        if (dR + (float)GLS_SLEEVE_MARGIN_U < dL)      cls = GLSC_SLEEVE_R;
        else if (dL + (float)GLS_SLEEVE_MARGIN_U < dR) cls = GLSC_SLEEVE_L;
    } else if (have == 1) {
        cls = (dR < (float)GLS_SLEEVE_REACH_U) ? GLSC_SLEEVE_R : GLSC_SLEEVE_L;
    } else if (have == 2) {
        cls = (dL < (float)GLS_SLEEVE_REACH_U) ? GLSC_SLEEVE_L : GLSC_SLEEVE_R;
    }
    if (cls != GLSC_NONE) { ++g_owner.sleeveByProximity; return cls; }
    ++g_owner.sleeveByOrdinal;
    return declaredCls;
}

// ONE VISUAL OWNER PER EYE-SCENE. The first hand draw of a class in an
// eye-scene decides whether that class is native or vanilla there; a later
// draw of the same class that would show the other owner is made coherent
// with the first (a would-be vanilla copy after a native draw is not drawn;
// a would-be native draw after a vanilla one is drawn vanilla and the eye's
// apply record says vanilla, so the pair verdict judges it). Counted, so a
// hardware log can say it never happened -- or exactly how often it did.
static struct { unsigned scene; int eye; int state; } g_eyeOwner[GLSC_COUNT];
enum { OWN_NONE = 0, OWN_NATIVE = 1, OWN_VANILLA = 2 };
// Whether the class of the draw being issued was established from its
// vertex CONTENT (a table solved) rather than from draw order. Only a
// content-identified draw may claim ownership or be kept coherent with an
// earlier owner: an ordinal guess must never delete or demote geometry.
static int g_identityByContent;
// The batch being issued is the MERGED HANDS batch: its index count says it
// carries both hand shells whatever order they are in, so a second such
// batch in one eye-scene is a duplicate of both hands even when its halves
// cannot be told apart. Suppression of a would-be second owner is allowed
// on that evidence, or on a content-identified single; never on an
// ordinal guess about a single.
static int g_suppressOk;

// Has this hand class already been attempted by the current eye in the
// current scene? (Any attempt, solved or refused.) Used for the
// unidentifiable-single fallback: if one hand has been drawn, the other
// single of this eye-scene is the other hand.
static int HandDrewThisEyeScene(int cls)
{
    if (g_curEye != 0 && g_curEye != 1) return 0;
    return g_handEyeAttempt[cls][g_curEye] == GLSurf_SceneFrameCount() + 1;
}

// Returns the owner state this draw must render under.
static int ClaimOwner(int cls, int wantNative)
{
    if (HandClassToVRHP(cls) < 0) return wantNative ? OWN_NATIVE : OWN_VANILLA;
    const unsigned scene = g_sceneFrameIndex;
    const int eye = g_curEye;
    const int want = wantNative ? OWN_NATIVE : OWN_VANILLA;
    if (g_eyeOwner[cls].scene == scene && g_eyeOwner[cls].eye == eye
        && g_eyeOwner[cls].state != OWN_NONE) {
        if (g_eyeOwner[cls].state != want) ++g_owner.duplicateOwnerPrevented;
        return g_eyeOwner[cls].state;
    }
    g_eyeOwner[cls].scene = scene;
    g_eyeOwner[cls].eye = eye;
    g_eyeOwner[cls].state = want;
    return want;
}

void GLSurf_SetHandPairEligible(int cls, int on)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT) return;
    g_pairEligible[cls] = on ? 1 : 0;
}

int GLSurf_HandPairEligible(int cls)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT) return 0;
    return g_pairEligible[cls];
}

unsigned GLSurf_HandPairGateRefusals(int cls)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT) return 0;
    return g_pairGateRefusals[cls];
}

int GLSurf_HandEyeParticipating(int cls, int eye)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT) return 0;
    if (eye != 0 && eye != 1) return 0;
    const unsigned a = g_handEyeAttempt[cls][eye];
    if (!a) return 0;                                    // never rendered
    const unsigned now = GLSurf_SceneFrameCount() + 1;
    return (now - a <= (unsigned)GLS_BASELINE_FRESH_SCENES) ? 1 : 0;
}

int GLSurf_GetHandBaselineEye(int cls, int eye, GLSurfHandSolve* out)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT) return 0;
    if (eye != 0 && eye != 1) return 0;
    const GLSurfHandSolve& hs = g_handSolveEye[cls][eye];
    if (!hs.everSolved) return 0;
    const unsigned now = GLSurf_SceneFrameCount();
    if (now - hs.lastGoodScene > (unsigned)GLS_BASELINE_FRESH_SCENES) return 0;
    if (out) *out = hs;
    return 1;
}

int GLSurf_GetHandSolve(int cls, GLSurfHandSolve* out)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT)
        return 0;
    if (out) *out = g_handSolve[cls];
    return g_handSolve[cls].valid;
}

int GLSurf_GetHandBaseline(int cls, GLSurfHandSolve* out)
{
    if (cls <= GLSC_NONE || cls >= GLSC_COUNT)
        return 0;
    const GLSurfHandSolve& hs = g_handSolve[cls];
    if (out) *out = hs;
    if (!hs.everSolved)
        return 0;
    const unsigned scene = GLSurf_SceneFrameCount();
    // Scene counters only move forward; a stale baseline is one whose last
    // good solve is more than the freshness window behind the CURRENT scene.
    if (scene - hs.lastGoodScene > (unsigned)GLS_BASELINE_FRESH_SCENES)
        return 0;
    return 1;
}

// Build this draw's hand delta, or report that there is none.
//
// Returns 0 whenever anything is not exactly right -- no target, no usable
// vertex array, a batch whose landmarks do not reproduce. A 0 leaves the hand
// where the engine drew it, which is the only safe way to be wrong.
// ---------------------------------------------------------------- sleeves
//
// The two sleeve surfaces are ONE PER ARM -- proven from skin weights, see
// GLSurf_SleeveSplitIsAnatomical(). So a sleeve is not a separate object to
// place: it is the forearm of a hand that is already being placed, and it
// takes that hand's rigid motion.
//
// THE STEREO RULE STILL APPLIES, and it is why this does not simply copy the
// hand's finished matrix. That matrix was built about the hand's palm pivot in
// ONE eye's coordinates; reusing it on a draw in the other eye would rotate
// the forearm about a point that is not where the palm is for that eye --
// false disparity, exactly what vr_objects.h forbids. What IS eye-invariant
// is the latched offset and rotation, so the sleeve rebuilds its own delta
// from those, about a pivot taken from THIS eye's own vertices:
//
//   * the arm's palm, when that eye already solved it THIS scene (the
//     forearm then stays attached to the hand under rotation), or
//   * the sleeve's own centroid otherwise (still eye-correct; the forearm
//     turns about its own centre instead of about the wrist).
//
// No arm IK, which this milestone does not require. What it does require is
// that the sleeve never stay welded to the vanilla weapon while its hand moves.
static int SleeveArmClass(int cls)
{
    if (cls == GLSC_SLEEVE_R) return GLSC_HAND_R;
    if (cls == GLSC_SLEEVE_L) return GLSC_HAND_L;
    return GLSC_NONE;
}

static unsigned g_sleeveFollowedPalm, g_sleeveFollowedCentroid;

// The forearm is rigid with its hand. It takes the SAME delta the hand
// received in THIS eye-scene -- built about this eye's own palm, from this
// eye's own solve -- so the whole arm moves as one body per eye and the pair
// verdict judges the hand for both. A hand that did not solve this scene
// leaves its sleeve vanilla: a forearm turning about its own centre next to
// a vanilla hand would be a third object nobody asked for.
static int g_sleeveAnatomical = 1;
static int g_sleeveVisible = 1;          // RIGHT sleeve only (see the header)
void GLSurf_SetSleeveVisible(int mode)
{ g_sleeveVisible = (mode < 0) ? 0 : (mode > 2) ? 2 : mode; }
int  GLSurf_SleeveVisible() { return g_sleeveVisible; }
static int g_sleeveHideNearHead = 1;
static GLSurfSleeveStats g_sleeveStats;
void GLSurf_SetSleeveAnatomical(int on)   { g_sleeveAnatomical = on ? 1 : 0; }
int  GLSurf_SleeveAnatomical()            { return g_sleeveAnatomical; }
void GLSurf_SetSleeveHideNearHead(int on) { g_sleeveHideNearHead = on ? 1 : 0; }
int  GLSurf_SleeveHideNearHead()          { return g_sleeveHideNearHead; }
void GLSurf_GetSleeveStats(GLSurfSleeveStats* out) { if (out) *out = g_sleeveStats; }

static void RowsApply(const float rows[9], const float v[3], float out[3])
{
    for (int k = 0; k < 3; ++k)
        out[k] = v[0] * rows[k] + v[1] * rows[3 + k] + v[2] * rows[6 + k];
}

// The sleeve's rigid motion for this draw. Returns 1 with the delta, 0 for
// "nothing to do" (no target / no hand delta this scene), -1 for "not
// drawn" (the visibility policy).
static int BuildSleeveDelta(int cls, unsigned type, const void* idx, int count,
                            float out[16])
{
    const int arm = SleeveArmClass(cls);
    if (arm == GLSC_NONE) return 0;
    const HandTarget& t = g_handTarget[arm];
    // Floating hands also applies during hand-baseline reacquisition.
    if (g_sleeveVisible == 0) { ++g_sleeveStats.hiddenByPolicy; return -1; }
    if (!t.enabled || t.hold) return 0;
    // The visibility policy comes first: a sleeve the operator has excluded
    // is not drawn at all, and its hand is untouched.
    if (g_sleeveVisible == 0
        || (g_sleeveVisible == 1 && cls == GLSC_SLEEVE_L)) {
        ++g_sleeveStats.hiddenByPolicy;
        return -1;
    }
    if (!g_handSceneDelta[arm].valid
        || g_handSceneDelta[arm].scene != g_sceneFrameIndex)
        return 0;
    const int hand = HandClassToVRHP(arm);
    const GLSurfHandSolve& hs = g_handSolve[arm];
    const unsigned scene = GLSurf_SceneFrameCount();
    const CodvrSleeveDef* def = nullptr;
    VRHandPose forearm;
    int solved = 0;
    if (g_sleeveAnatomical && hand >= 0 && hs.everSolved && hs.lastGoodScene == scene) {
        int lo = 0, hi = 0;
        if (IndexRange(type, idx, count, &lo, &hi)) {
            int vStride = 0;
            GLSurfReadCtx rc;
            VBOResolveInfo vi;
            const unsigned char* vbase = VertexArrayBaseForSolve(hi, &vStride, &rc, &vi);
            if (vbase) {
                int reason = VRHP_REASON_INDEX_RANGE;
                float rms = -1.0f;
                def = VRSleevePose_SolveAny(hand, (const float*)vbase, vStride, hi + 1,
                                            CODVR_UNITS_PER_M, &forearm, hs.lastRig,
                                            GLSurfCanReadVertex, &rc, &reason, lo, &rms);
                g_sleeveStats.lastRmsM = rms;
                if (def) {
                    float mv[16];
                    if (CurrentModelview(mv)) { PoseToEyeSpace(mv, &forearm); solved = 1; }
                    else def = nullptr;
                } else {
                    ++g_sleeveStats.solveRefusals;
                }
            }
        }
    }
    if (!solved) {
        // Fallback: rigid with the hand (the animation's wrist bend kept).
        memcpy(out, g_handSceneDelta[arm].m, 16 * sizeof(float));
        ++g_sleeveFollowedPalm;
        ++g_sleeveStats.followedPalm;
        return 1;
    }
    // Desired palm in THIS eye's frame; desired forearm = palm * rest.
    const int eye = (g_curEye == 1) ? 1 : 0;
    const float wantPos[3] = { t.posMid[0] - g_eyeOff[eye][0],
                               t.posMid[1] - g_eyeOff[eye][1],
                               t.posMid[2] - g_eyeOff[eye][2] };
    float wantAxis[9];
    for (int r = 0; r < 3; ++r)
        RowsApply(t.axis, def->restRows + r * 3, wantAxis + r * 3);
    const float lenU = def->forearmLenM * CODVR_UNITS_PER_M;
    float elbow[3];
    for (int k = 0; k < 3; ++k) elbow[k] = wantPos[k] - lenU * wantAxis[k];
    g_sleeveStats.lastArm = cls;
    memcpy(g_sleeveStats.lastElbowEye, elbow, sizeof(elbow));
    memcpy(g_sleeveStats.lastForearmAxis, wantAxis, sizeof(wantAxis));
    if (g_sleeveHideNearHead) {
        // The mesh continues past the elbow up the upper arm, rigidly; check
        // the elbow and a point 15 cm beyond it. Eye space: the eye is the
        // origin, -Z is forward.
        const float clear = (float)GLS_SLEEVE_HEAD_CLEAR_U;
        float beyond[3];
        for (int k = 0; k < 3; ++k) beyond[k] = elbow[k] - clear * wantAxis[k];
        const float de = sqrtf(elbow[0]*elbow[0] + elbow[1]*elbow[1] + elbow[2]*elbow[2]);
        const float df = sqrtf(beyond[0]*beyond[0] + beyond[1]*beyond[1] + beyond[2]*beyond[2]);
        if (elbow[2] > -clear * 0.5f || de < clear || df < clear) {
            ++g_sleeveStats.hiddenNearHead;
            return -1;
        }
    }
    // ONE rigid motion about the wrist: the authored wrist (the palm solve's
    // origin, exact) goes to the desired palm origin; the drawn forearm's
    // orientation goes to the desired one.
    VRHandPose body;
    memset(&body, 0, sizeof(body));
    memcpy(body.pos, hs.pos, sizeof(body.pos));
    memcpy(body.axis, forearm.axis, sizeof(body.axis));
    body.valid = 1;
    VRGunPose_Delta(&body, hs.pos, wantAxis, wantPos, out);
    ++g_sleeveStats.followedForearm;
    return 1;
}

void GLSurf_GetSleeveFollowCounts(unsigned* palm, unsigned* centroid)
{
    if (palm)     *palm = g_sleeveFollowedPalm;
    if (centroid) *centroid = g_sleeveFollowedCentroid;
}

static int BuildHandDelta(int cls, unsigned type, const void* idx, int count,
                          float out[16])
{
    const int hand = HandClassToVRHP(cls);
    if (hand < 0) return 0;
    const HandTarget& t = g_handTarget[cls];
    if (!t.enabled) return 0;
    // THE PAIR GATE. Both eyes agreed about this hand, or neither eye moves
    // it. Checked before the solve so the refusal is attributable and so a
    // successful per-eye solve can never sneak one eye into native ownership.
    if (!g_pairEligible[cls]) { ++g_pairGateRefusals[cls]; return 0; }

    VRHandPose authored;
    if (!SolveHandFromDraw(cls, type, idx, count, &authored, g_orderHint))
        return 0;

    if (t.hold) {
        // The flat diagnostic: solve, then leave the hand exactly where the
        // engine drew it. An identity delta is "not transformed" downstream.
        MatIdentity(out);
        return 1;
    }

    // Desired = the pair's ONE absolute pose, shifted into THIS eye's frame;
    // delta = Desired_eye * inverse(Authored_eye), pivoting on this eye's own
    // authored palm. Whatever the animation did between the two game frames,
    // both eyes land on the same physical object.
    const int eye = (g_curEye == 1) ? 1 : 0;
    const float desiredPos[3] = { t.posMid[0] - g_eyeOff[eye][0],
                                  t.posMid[1] - g_eyeOff[eye][1],
                                  t.posMid[2] - g_eyeOff[eye][2] };
    VRHandPose_Delta(&authored, desiredPos, t.axis, out);

    // What this eye actually did: the authored anchor pushed through the delta
    // it is about to receive. Reconstructed from the matrix, not assumed, so a
    // wrong delta is recorded as wrong.
    float finalPos[3], finalAxis[9];
    Mat4ApplyPoint(out, authored.pos, finalPos);
    for (int r = 0; r < 3; ++r)
        MatRotVec3(out, authored.axis + r * 3, finalAxis + r * 3);
    NoteEyeApplied(cls, finalPos, finalAxis);
    g_handSceneDelta[cls].scene = g_sceneFrameIndex;
    g_handSceneDelta[cls].valid = 1;
    memcpy(g_handSceneDelta[cls].m, out, sizeof(g_handSceneDelta[cls].m));
    return 1;
}

// ---------------------------------------------------------------------------
// THE GUN ON THE RIGHT CONTROLLER
//
// Same recovery principle as the hands, one structural difference: a weapon
// is drawn as SEVERAL per-surface batches sharing one rigid body, so the
// delta is solved once per scene -- from the batch whose vertex count keys a
// shipped gun model -- and every other gun batch of that scene reuses it.
// The pivot is the model's measured REAR GRIP (STRONG+ only), never the
// camera, never a centroid: rotating about anything else swings the stock
// through the player's shoulder (the first gun_yaw probe's real behaviour).

struct GunTarget {
    int   enabled;
    float gripPosU[3];   // desired rear-grip position, head-relative eye space
    float axis[9];       // desired weapon basis, rows-as-axes
};
static GunTarget      g_gunTarget;
static GLSurfGunSolve g_gunSolve;
// The model the authored baseline belongs to (null until something keyed).
static const CodvrGunModel* g_gunModel;
// PRE-KEY POLICY (brief 2026-09-04, P0). The engine draws a weapon's
// surfaces in shader-sorted order, so a part can reach GL before the body
// surface that keys the scene. 1 = carry the previous scene's authored body
// (the part is placed by the same controller pose; only the one-frame
// animation advance of the body separates it from the fresh delta, and that
// deviation is measured); 0 = suppress the batch. NEVER vanilla: a
// head-coupled part beside a controller-owned body is the two-weapon image
// the hardware showed.
static int g_gunPreKeyPolicy = 0;
// Test-only mutant: the N-th gun batch of every scene stays vanilla.
static int g_forceGunVanillaOrdinal = -1;

static struct {
    unsigned scene;      // GLSurf_SceneFrameCount value the cache belongs to
    int      state;      // 0 undecided, 1 fresh delta, 2 baseline only, -1 refused
    float    delta[16];  // the ONE fresh delta every gun batch of the scene takes
    int      haveStale;
    float    stale[16];  // the carried-baseline delta (pre-key batches only)
    int      ordinal;    // gun batches seen this scene
} g_gunScene;

// Per-eye-scene weapon draw accounting: the record being filled, and the
// latest completed-or-current record per eye for the pre-submit verdict.
static GLSurfGunScene  g_gunSceneRec[2];
static GLSurfGunScene* g_gunSceneCur;
static unsigned        g_gunDeltaIds;
enum { GLS_WEAPON_STATS_MAX = 8 };
static GLSurfWeaponStats g_weaponStats[GLS_WEAPON_STATS_MAX];
static int               g_weaponStatsCount;

void GLSurf_SetGunTarget(int enabled, const float desiredGripPosU[3],
                         const float desiredAxis[9])
{
    g_gunTarget.enabled = enabled ? 1 : 0;
    if (desiredGripPosU)
        memcpy(g_gunTarget.gripPosU, desiredGripPosU, sizeof(g_gunTarget.gripPosU));
    if (desiredAxis)
        memcpy(g_gunTarget.axis, desiredAxis, sizeof(g_gunTarget.axis));
    if (!g_gunTarget.enabled)
        g_gunSolve.valid = 0;
}

void GLSurf_GetGunSolve(GLSurfGunSolve* out) { if (out) *out = g_gunSolve; }
int  GLSurf_GunVertexCount() { return g_gunSolve.lastVertexCount; }
void GLSurf_SetGunPreKeyPolicy(int policy) { g_gunPreKeyPolicy = policy ? 1 : 0; }
int  GLSurf_GunPreKeyPolicy() { return g_gunPreKeyPolicy; }
void GLSurf_TestForceGunBatchVanilla(int ordinal) { g_forceGunVanillaOrdinal = ordinal; }

int GLSurf_GetGunScene(int eye, GLSurfGunScene* out)
{
    if (!out || (eye != 0 && eye != 1)) return 0;
    *out = g_gunSceneRec[eye];
    return g_gunSceneRec[eye].scene != 0 || g_gunSceneRec[eye].weaponDrawSeen != 0;
}

int GLSurf_WeaponStatsCount() { return g_weaponStatsCount; }
int GLSurf_GetWeaponStats(int i, GLSurfWeaponStats* out)
{
    if (!out || i < 0 || i >= g_weaponStatsCount) return 0;
    *out = g_weaponStats[i];
    return 1;
}

static GLSurfWeaponStats* WeaponStatsFor(int modelKey)
{
    for (int i = 0; i < g_weaponStatsCount; ++i)
        if (g_weaponStats[i].modelKey == modelKey) return &g_weaponStats[i];
    if (g_weaponStatsCount >= GLS_WEAPON_STATS_MAX) return nullptr;
    GLSurfWeaponStats* w = &g_weaponStats[g_weaponStatsCount++];
    memset(w, 0, sizeof(*w));
    w->modelKey = modelKey;
    return w;
}

static int g_gunSolveWanted;
void GLSurf_SetGunSolveWanted(int on) { g_gunSolveWanted = on ? 1 : 0; }

static void Mat4ApplyPoint(const float m[16], const float p[3], float out[3])
{
    out[0] = m[0] * p[0] + m[4] * p[1] + m[8]  * p[2] + m[12];
    out[1] = m[1] * p[0] + m[5] * p[1] + m[9]  * p[2] + m[13];
    out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

// Reset the per-scene gun cache when the scene advanced, closing the
// previous eye-scene's record.

static void GunSceneRollover()
{
    const unsigned scene = GLSurf_SceneFrameCount();
    if (g_gunScene.scene == scene && g_gunSceneCur)
        return;
    g_gunScene.scene = scene;
    g_gunScene.state = 0;
    g_gunScene.haveStale = 0;
    g_gunScene.ordinal = 0;
    g_gunSolve.valid = 0;
    const int eye = (g_curEye == 1) ? 1 : 0;
    g_gunSceneCur = &g_gunSceneRec[eye];
    memset(g_gunSceneCur, 0, sizeof(*g_gunSceneCur));
    g_gunSceneCur->scene    = scene;
    g_gunSceneCur->serial   = g_latchSerial;
    g_gunSceneCur->eye      = eye;
    g_gunSceneCur->modelKey = g_gunModel ? g_gunModel->vertexCount : 0;
    g_gunSceneCur->staleErrU = -1.0f;
    g_gunSceneCur->staleErrDeg = -1.0f;
}

static void NoteGunDisposition(int disp, int verts)
{
    GLSurfGunScene* c = g_gunSceneCur;
    if (!c) return;
    const int hadConflict = c->weaponOwnerConflict;
    const int firstDraw = (c->weaponDrawSeen == 0);
    ++c->weaponDrawSeen;
    switch (disp) {
        case GLS_GDISP_NATIVE:     ++c->weaponDrawNative;     break;
        case GLS_GDISP_STALE:      ++c->weaponDrawStale;      break;
        case GLS_GDISP_PARTKEY:    ++c->weaponDrawPartKeyed;  break;
        case GLS_GDISP_SUPPRESSED: ++c->weaponDrawSuppressed; break;
        case GLS_GDISP_VANILLA:    ++c->weaponDrawVanilla;    break;
        default: break;
    }
    if (c->nDisp < GLS_GUN_DISP_MAX) {
        c->dispVerts[c->nDisp] = verts;
        c->disp[c->nDisp] = (signed char)disp;
        ++c->nDisp;
    }
    // Native AND vanilla weapon geometry in ONE eye-scene: two weapons.
    c->weaponOwnerConflict = (c->weaponDrawVanilla > 0
        && (c->weaponDrawNative + c->weaponDrawStale + c->weaponDrawPartKeyed) > 0) ? 1 : 0;
    if (c->weaponOwnerConflict && !hadConflict) ++g_gunSolve.ownerConflicts;
    GLSurfWeaponStats* w = WeaponStatsFor(c->modelKey);
    if (w) {
        if (firstDraw) ++w->scenes;
        ++w->draws;
        switch (disp) {
            case GLS_GDISP_NATIVE:     ++w->native;     break;
            case GLS_GDISP_STALE:      ++w->stale;      break;
            case GLS_GDISP_PARTKEY:    ++w->partKeyed;  break;
            case GLS_GDISP_SUPPRESSED: ++w->suppressed; break;
            case GLS_GDISP_VANILLA:    ++w->vanilla;    break;
            default: break;
        }
        if (c->weaponOwnerConflict && !hadConflict) ++w->conflicts;
    }
}

// Angle (deg) between the rotation parts of two column-major 4x4s.
static float Mat4RotAngleDeg(const float a[16], const float b[16])
{
    // trace(Ra^T Rb) = sum over columns of dot(colA, colB)
    float tr = 0.0f;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            tr += a[c * 4 + r] * b[c * 4 + r];
    float cs = (tr - 1.0f) * 0.5f;
    if (cs > 1.0f) cs = 1.0f;
    if (cs < -1.0f) cs = -1.0f;
    return acosf(cs) * (180.0f / 3.14159265358979f);
}

// Attempt the body solve on THIS draw from ANY key-usable surface of the
// weapon (vr_gun_surfaces.inc), preferring the model the baseline already
// belongs to, and fill the authored baseline on success. Returns 1 (solved:
// body and model out), 0 (this batch cannot key the body -- a moving part,
// or a surface no shipped weapon has), or -1 (a key-usable surface that
// would not solve, or whose weapon has no usable rear grip -- the scene
// stays vanilla).
static int SolveGunKeyedBatch(unsigned type, const void* idx, int count,
                              VRHandPose* outBody, const CodvrGunModel** outModel)
{
    int lo = 0, hi = 0;
    if (!IndexRange(type, idx, count, &lo, &hi) || lo != 0)
        return 0;
    const int vertCount = hi + 1;
    if (!VRGunPose_AnyKeySurface(vertCount))
        return 0;                        // not a keyable surface
    int vStride = 0;
    GLSurfReadCtx rc;
    VBOResolveInfo vi;
    const unsigned char* vbase = VertexArrayBaseForSolve(hi, &vStride, &rc, &vi);
    if (!vbase) {
        g_gunSolve.lastReason = VRHP_REASON_INDEX_RANGE;
        ++g_gunSolve.keyMisses;
        return 0;                        // unreadable: cannot key, not a refusal
    }
    VRHandPose body;
    int reason = VRHP_REASON_INDEX_RANGE;
    const CodvrGunSurface* surf = nullptr;
    const CodvrGunModel* m = VRGunPose_SolveAnySurfaceEx(vertCount,
        (const float*)vbase, vStride, vertCount, CODVR_UNITS_PER_M, &body,
        g_gunModel, &surf, GLSurfCanReadVertex, &rc, &reason);
    g_gunSolve.lastReason = reason;
    if (!m) {
        // A key-usable COUNT that did not reproduce. Surface counts collide
        // across models (the 46-vertex ammo clip is a key for viewmodel_ammo
        // and a moving part on the Enfield, Kar98k and Nagant), so this is
        // usually another weapon's part: it cannot key this scene, which is
        // not a refusal of the scene. Counted; the pre-key policy places it.
        if (reason == VRHP_REASON_BYTES_UNKNOWN) ++g_gunSolve.refusedUnknownBytes;
        ++g_gunSolve.keyMisses;
        return 0;
    }
    g_gunSolve.lastVertexCount = m->vertexCount;     // the model's body key
    g_gunSolve.keySurface = surf ? surf->surface : -1;
    g_gunSolve.keyBatchVerts = vertCount;
    g_gunSolve.lastRmsU = body.rmsM;
    // The rear grip is derived in the BODY's own frame, so it has to be taken
    // before the body pose leaves submitted space; the pivot is then mapped
    // with it. Same reason as the hands (see PoseToEyeSpace).
    // THE PIVOT. A STRONG rear grip is the physical one. A weapon without
    // one (the grenades, the panzerfaust) is NOT refused any more: refusing
    // it left a head-coupled object beside a controller-owned hand, which
    // the 2026-09-04d hardware run saw as a grenade floating two feet above
    // the hand. The object is placed rigidly by the LIVE authored
    // palm->body relation instead, pivoting on the body's own origin. The
    // pivot choice cannot change a rigid placement -- position and
    // orientation both come from the same measured relation -- it only
    // decides which point the relation is expressed about, and the weapon
    // layer expresses it about whatever this publishes.
    float pivot[3];
    const int haveRearGrip = VRGunPose_RearGripPivot(m->vertexCount, &body,
                                                     CODVR_UNITS_PER_M, pivot);
    if (!haveRearGrip)
        memcpy(pivot, body.pos, sizeof(pivot));
    g_gunSolve.pivotIsBodyOrigin = haveRearGrip ? 0 : 1;
    float mv[16];
    if (!CurrentModelview(mv)) {
        g_gunSolve.haveBody = 0;
        return -1;
    }
    PoseToEyeSpace(mv, &body);
    {
        const float p[3] = { pivot[0], pivot[1], pivot[2] };
        pivot[0] = mv[0]*p[0] + mv[4]*p[1] + mv[8] *p[2] + mv[12];
        pivot[1] = mv[1]*p[0] + mv[5]*p[1] + mv[9] *p[2] + mv[13];
        pivot[2] = mv[2]*p[0] + mv[6]*p[1] + mv[10]*p[2] + mv[14];
    }
    memcpy(g_gunSolve.bodyPosU, body.pos, sizeof(g_gunSolve.bodyPosU));
    memcpy(g_gunSolve.bodyAxis, body.axis, sizeof(g_gunSolve.bodyAxis));
    memcpy(g_gunSolve.pivotU, pivot, sizeof(g_gunSolve.pivotU));
    g_gunSolve.haveBody = 1;
    g_gunSolve.bodyScene = GLSurf_SceneFrameCount();
    ++g_gunSolve.bodySerial;
    g_gunModel = m;
    if (g_gunSceneCur) g_gunSceneCur->modelKey = m->vertexCount;
    if (outBody)  *outBody = body;
    if (outModel) *outModel = m;
    return 1;
}

// Baseline-only solve while no target is installed (the deadlock-breaker).
static void GunBaselineTick(unsigned type, const void* idx, int count)
{
    if (!g_gunSolveWanted || g_gunTarget.enabled)
        return;
    GunSceneRollover();
    if (g_gunScene.state != 0)
        return;
    VRHandPose body;
    const CodvrGunModel* m;
    const int r = SolveGunKeyedBatch(type, idx, count, &body, &m);
    if (r == 1)  g_gunScene.state = 2;
    if (r == -1) { g_gunScene.state = -1; ++g_gunSolve.refusals; }
}

// The vertex span of a gun batch (minIdx 0), or -1.
static int GunBatchSpan(unsigned type, const void* idx, int count)
{
    int lo = 0, hi = 0;
    if (!IndexRange(type, idx, count, &lo, &hi) || lo != 0) return -1;
    return hi + 1;
}

// Build the delta for a rear-grip target from an authored body (eye space).
static void GunDeltaFromBody(const float bodyPos[3], const float bodyAxis[9],
                             const float pivot[3], float out[16], float outMoved[3],
                             float outFinalAxis[9])
{
    VRHandPose body;
    memset(&body, 0, sizeof(body));
    memcpy(body.pos, bodyPos, sizeof(body.pos));
    memcpy(body.axis, bodyAxis, sizeof(body.axis));
    body.valid = 1;
    const int eye = (g_curEye == 1) ? 1 : 0;
    const float gripEye[3] = { g_gunTarget.gripPosU[0] - g_eyeOff[eye][0],
                               g_gunTarget.gripPosU[1] - g_eyeOff[eye][1],
                               g_gunTarget.gripPosU[2] - g_eyeOff[eye][2] };
    VRGunPose_Delta(&body, pivot, g_gunTarget.axis, gripEye, out);
    Mat4ApplyPoint(out, pivot, outMoved);
    for (int r = 0; r < 3; ++r)
        MatRotVec3(out, bodyAxis + r * 3, outFinalAxis + r * 3);
}

// The one delta that moves EVERY gun batch of this scene. Returns 1 with the
// scene delta (fresh, or the carried baseline for a pre-key batch), 0 when
// this scene's weapon stays vanilla, -1 when THIS batch must be suppressed
// (a pre-key batch with nothing trustworthy to place it by).
static int BuildGunDelta(unsigned type, const void* idx, int count, int ordinal,
                         float out[16])
{
    if (!g_gunTarget.enabled)
        return 0;
    GunSceneRollover();
    const int span = GunBatchSpan(type, idx, count);
    if (ordinal == 0)
        NoteEyeDrew(GLSC_GUN);           // this eye draws the weapon this pair
    if (g_gunScene.state == 1) {
        memcpy(out, g_gunScene.delta, sizeof(g_gunScene.delta));
        NoteGunDisposition(GLS_GDISP_NATIVE, span);
        return 1;
    }
    if (g_gunScene.state == -1) {
        NoteGunDisposition(GLS_GDISP_VANILLA, span);
        return 0;
    }

    VRHandPose body;
    const CodvrGunModel* m;
    const int r = SolveGunKeyedBatch(type, idx, count, &body, &m);
    if (r == 1) {
        float moved[3], finalAxis[9];
        GunDeltaFromBody(g_gunSolve.bodyPosU, g_gunSolve.bodyAxis, g_gunSolve.pivotU,
                         g_gunScene.delta, moved, finalAxis);
        g_gunScene.state = 1;
        ++g_gunSolve.solves;
        g_gunSolve.valid = 1;
        if (g_gunSceneCur) g_gunSceneCur->deltaId = ++g_gunDeltaIds;
        const int eye = (g_curEye == 1) ? 1 : 0;
        const float gripEye[3] = { g_gunTarget.gripPosU[0] - g_eyeOff[eye][0],
                                   g_gunTarget.gripPosU[1] - g_eyeOff[eye][1],
                                   g_gunTarget.gripPosU[2] - g_eyeOff[eye][2] };
        const float dx = moved[0] - gripEye[0];
        const float dy = moved[1] - gripEye[1];
        const float dz = moved[2] - gripEye[2];
        g_gunSolve.rearGripErrU = sqrtf(dx * dx + dy * dy + dz * dz);
        NoteEyeApplied(GLSC_GUN, moved, finalAxis);
        // How far the carried baseline placed this scene's pre-key batches
        // from where the fresh delta would have: the one-frame animation
        // advance of the body, measured, not assumed.
        if (g_gunScene.haveStale) {
            float a[3], b[3];
            Mat4ApplyPoint(g_gunScene.delta, g_gunSolve.pivotU, a);
            Mat4ApplyPoint(g_gunScene.stale, g_gunSolve.pivotU, b);
            const float ex = a[0]-b[0], ey = a[1]-b[1], ez = a[2]-b[2];
            const float eu = sqrtf(ex*ex + ey*ey + ez*ez);
            const float ed = Mat4RotAngleDeg(g_gunScene.delta, g_gunScene.stale);
            if (g_gunSceneCur) { g_gunSceneCur->staleErrU = eu; g_gunSceneCur->staleErrDeg = ed; }
            if (eu > g_gunSolve.staleErrMaxU)   g_gunSolve.staleErrMaxU = eu;
            if (ed > g_gunSolve.staleErrMaxDeg) g_gunSolve.staleErrMaxDeg = ed;
        }
        memcpy(out, g_gunScene.delta, sizeof(g_gunScene.delta));
        NoteGunDisposition(GLS_GDISP_NATIVE, span);
        return 1;
    }
    if (r == -1) {
        g_gunScene.state = -1;
        ++g_gunSolve.refusals;
        NoteGunDisposition(GLS_GDISP_VANILLA, span);
        return 0;
    }
    // A PRE-KEY batch: the body has not keyed this scene yet. Never vanilla.
    const unsigned scene = GLSurf_SceneFrameCount();
    const int baselineFresh = g_gunSolve.haveBody && g_gunModel
                           && (g_gunSolve.bodyScene + 1 == scene);
    const CodvrGunSurface* part = (g_gunModel && span > 0)
                                ? VRGunPose_SurfaceOfModel(g_gunModel, span) : nullptr;

    // FIRST CHOICE: let the part key ITSELF. Its landmark table is expressed
    // in the body frame at rest, so a solve that passes the residual gate
    // both proves the part is at rest AND hands us this scene's body pose --
    // no carried delta, no animation drift. A refusal means the part is
    // mid-animation, and a moving part drawn on a body pose would be the
    // duplicate we are here to remove, so it is hidden instead.
    const CodvrGunSurface* uniquePart = (g_gunModel && span > 0)
                                      ? VRGunPose_UniqueSurfaceOfModel(g_gunModel, span) : nullptr;
    if (uniquePart && !uniquePart->bodyRigid && g_gunModel) {
        part = uniquePart;
        int lo2 = 0, hi2 = 0;
        if (IndexRange(type, idx, count, &lo2, &hi2) && lo2 == 0) {
            int vStride = 0;
            GLSurfReadCtx rc;
            VBOResolveInfo vi;
            const unsigned char* vbase = VertexArrayBaseForSolve(hi2, &vStride, &rc, &vi);
            if (vbase) {
                VRHandPose pbody;
                int reason = VRHP_REASON_INDEX_RANGE;
                const CodvrGunModel* pm = VRGunPose_SolveSurfaceEx(part,
                    (const float*)vbase, vStride, hi2 + 1, CODVR_UNITS_PER_M, &pbody,
                    GLSurfCanReadVertex, &rc, &reason);
                float mv[16];
                if (pm && CurrentModelview(mv)) {
                    float pivot[3];
                    const int haveGrip = VRGunPose_RearGripPivot(pm->vertexCount, &pbody,
                                                                 CODVR_UNITS_PER_M, pivot);
                    if (!haveGrip)                     // no STRONG grip: the body origin
                        memcpy(pivot, pbody.pos, sizeof(pivot));
                    PoseToEyeSpace(mv, &pbody);
                    {
                        const float p[3] = { pivot[0], pivot[1], pivot[2] };
                        pivot[0] = mv[0]*p[0] + mv[4]*p[1] + mv[8] *p[2] + mv[12];
                        pivot[1] = mv[1]*p[0] + mv[5]*p[1] + mv[9] *p[2] + mv[13];
                        pivot[2] = mv[2]*p[0] + mv[6]*p[1] + mv[10]*p[2] + mv[14];
                    }
                    float moved[3], finalAxis[9];
                    GunDeltaFromBody(pbody.pos, pbody.axis, pivot, out, moved, finalAxis);
                    ++g_gunSolve.batchesPartKeyed;
                    NoteGunDisposition(GLS_GDISP_PARTKEY, span);
                    return 1;
                }
                if (!pm) ++g_gunSolve.batchesPartRefused;
            }
        }
    }
    if (g_gunPreKeyPolicy == 1 && part && baselineFresh) {
        if (!g_gunScene.haveStale) {
            float moved[3], finalAxis[9];
            GunDeltaFromBody(g_gunSolve.bodyPosU, g_gunSolve.bodyAxis, g_gunSolve.pivotU,
                             g_gunScene.stale, moved, finalAxis);
            g_gunScene.haveStale = 1;
            ++g_gunSolve.staleScenes;
            NoteEyeApplied(GLSC_GUN, moved, finalAxis);
        }
        memcpy(out, g_gunScene.stale, sizeof(g_gunScene.stale));
        ++g_gunSolve.batchesStale;
        NoteGunDisposition(GLS_GDISP_STALE, span);
        return 1;
    }
    ++g_gunSolve.batchesSuppressed;
    NoteGunDisposition(GLS_GDISP_SUPPRESSED, span);
    return -1;
}

// Issue one half of a split batch under its own class delta.
static void DrawOneHalf(unsigned mode, int half, unsigned type, const void* idx,
                        int cls, int recIdx)
{
    NoteClassPivot(cls, type, idx, half);
    // Baseline freshness: with solve-wanted set but no target engaged yet,
    // still recover the authored pose so the NEXT pair has something to
    // engage against. (An enabled target solves inside BuildHandDelta.)
    if (g_solveWanted[cls] && !g_handTarget[cls].enabled)
        SolveHandFromDraw(cls, type, idx, half, nullptr, g_orderHint);
    if (cls > GLSC_NONE && cls < GLSC_COUNT && g_hidden[cls]) {
        ++g_stats.suppressed;
        NoteRecordUsed(recIdx, g_delta[cls], -1);      // -1 == suppressed
        return;                                        // drawn nowhere, on purpose
    }
    // A hand's delta is solved from this draw's own vertices; everything else
    // uses the matrix the weapon layer installed.
    float handDelta[16];
    const float* delta = g_delta[cls];
    const int isHand = HandClassToVRHP(cls) >= 0;
    int handNative = 0;
    if (g_transformEnabled && BuildHandDelta(cls, type, idx, half, handDelta)) {
        delta = handDelta; handNative = 1;
    } else if (g_transformEnabled && SleeveArmClass(cls) != GLSC_NONE) {
        const int r = BuildSleeveDelta(cls, type, idx, half, handDelta);
        if (r < 0) {                                   // visibility policy
            ++g_stats.suppressed;
            NoteRecordUsed(recIdx, g_delta[cls], -1);
            return;
        }
        if (r > 0) delta = handDelta;
    }

    // One visual owner per eye-scene for a hand class (see ClaimOwner).
    if (isHand && g_handTarget[cls].enabled && !g_handTarget[cls].hold) {
        const int owner = ClaimOwner(cls, handNative);
        if (owner == OWN_NATIVE && !handNative && (g_suppressOk || g_identityByContent)) {
            // A vanilla copy after a native draw: not drawn at all.
            NoteRecordUsed(recIdx, g_delta[cls], -1);
            return;
        }
        if (owner == OWN_VANILLA && handNative) {
            // A native draw after a vanilla one: rendered vanilla, recorded
            // vanilla, judged by the pair verdict.
            delta = g_delta[cls]; handNative = 0;
            NoteEyeApplyFailed(cls);
        }
        if (handNative) ++g_owner.primaryNativeDraw;
        else            ++g_owner.primaryVanillaFallbackDraw;
    }

    if (cls > GLSC_NONE && cls < GLSC_COUNT &&
        g_transformEnabled && !MatIsIdentity(delta)) {
        const int done = (g_route == GLS_ROUTE_SCRATCH)
                       ? TransformDrawScratch(mode, half, type, idx, delta)
                       : TransformDrawModelview(mode, half, type, idx, delta);
        if (done) {
            ++g_stats.transformed;
            if (isHand) ++g_handSolve[cls].deltasApplied;
            if (g_lastTransformValid) NoteRecordUsed(recIdx, g_lastLoaded, 1);
            return;
        }
        if (isHand) NoteEyeApplyFailed(cls);
    }
    if (g_drv.drawElements)
        g_drv.drawElements(mode, half, type, idx);
}

// ---------------------------------------------------------------------------
// RE-ENTRANCY
//
// The layer re-issues draws: a merged batch becomes two sub-draws, and the
// scratch route redraws with swapped arrays. Those go out through g_drv, which
// dllmain fills with the REAL driver entry points rather than our own i_gl*
// wrappers -- so today they cannot come back through here.
//
// That is a property of one assignment in another file. This counter makes it
// a property of THIS one: if a re-issued draw ever does re-enter, it is
// declined and counted instead of recursing until the stack runs out. One
// increment per draw is not a cost worth reasoning about; a stack overflow in
// a renderer is.
int GLSurf_OnDrawElements(unsigned mode, int count, unsigned type, const void* idx)
{
    if (g_drawDepth > 0) { ++g_reentered; return 0; }   // let the engine have it
    DrawDepthScope depth;
    EnsureInit();
    ++g_stats.drawsSeen;
    ++g_drawSeq;
    UpdateViewmodelRun();
    if (g_inScene) ++g_stats.drawsScene;
    if (g_vmRunActive) { ++g_stats.drawsDepthHacked; ++g_vmRunIndex; }

    const int batch = ClassifyBatch(mode, count);
    int cls = BatchPrimaryClass(batch);
    // A single-surface hand draw is whichever hand's landmarks reproduce
    // from its vertices; the ordinal is only the fail-closed fallback (the
    // solve then refuses and the draw stays vanilla).
    g_identityByContent = 0;
    g_suppressOk = 0;
    if (batch == BATCH_HAND_SINGLE
        && (g_solveWanted[GLSC_HAND_R] || g_solveWanted[GLSC_HAND_L]
            || g_handTarget[GLSC_HAND_R].enabled || g_handTarget[GLSC_HAND_L].enabled)) {
        int c = GLSC_NONE, sw = 0;
        if (IdentifyHandDraw(type, idx, count, &c, &sw)) {
            cls = c; ++g_owner.singleByContent;
            g_orderHint = sw;
            g_identityByContent = 1;
        } else {
            // Unrecoverable single: if the other hand has already been
            // drawn by this eye this scene, this is the remaining one; else
            // the declared ordinal. Either way it is a guess, so it may not
            // claim ownership (g_identityByContent stays 0) -- it refuses
            // and renders vanilla, and the pair verdict judges it.
            const int rDrew = HandDrewThisEyeScene(GLSC_HAND_R);
            const int lDrew = HandDrewThisEyeScene(GLSC_HAND_L);
            if (rDrew && !lDrew)      cls = GLSC_HAND_L;
            else if (lDrew && !rDrew) cls = GLSC_HAND_R;
            ++g_owner.singleByOrdinal;
            g_orderHint = -1;
        }
    } else if (batch == BATCH_SLEEVE_SINGLE) {
        cls = IdentifySleeveDraw(type, idx, count, cls);
    }
    g_lastDrawClass = cls;
    if (cls > GLSC_NONE && cls < GLSC_COUNT) ++g_stats.classified[cls];

    const bool merged = (batch == BATCH_HANDS_MERGED || batch == BATCH_SLEEVES_MERGED);
    int recIdx = -1;
    if (g_capRecording && g_capArmed && !g_capDone && !merged)
        recIdx = RecordDraw(mode, count, type, idx, cls);

    // ---- merged batches: split into the two sides ------------------------
    //
    // Left and right arrive in ONE draw sharing ONE modelview, so a single
    // matrix could never move them independently. Re-issuing the batch as two
    // index sub-ranges gives each side its own matrix -- and touches no vertex
    // data at all, which is why this is preferable to rewriting arrays.
    if (merged) {
        int firstCls = GLSC_NONE, secondCls = GLSC_NONE;
        MergedHalves(batch, &firstCls, &secondCls);
        // THE SURFACE ORDER OF THIS BATCH, from its content. The engine packs
        // the two hand surfaces either way round from frame to frame
        // (docs/FLICKER-ROOT-CAUSE-2026-09-03.md); the half classes follow
        // whichever order the landmark tables actually solve in. The
        // declared order is kept only when neither hand can be recovered
        // at all, and then both halves refuse and stay vanilla.
        g_orderHint = -1;
        g_identityByContent = 0;

        // A hand's transform lives in g_handTarget (BuildHandDelta solves it
        // fresh per draw), not in the fixed g_delta a class-delta caller
        // installs -- so the split must be requested by EITHER mechanism.
        // Missing this left GLSurf_SetHandTarget structurally unreachable
        // through the real merged draw: with no non-identity g_delta also
        // set, wantSplit was false, GLSurf_OnDrawElements returned 0 ("not
        // mine"), and BuildHandDelta -- which only runs inside DrawOneHalf,
        // reached only from the split path -- never ran at all.
        // A sleeve half follows its ARM's hand target (BuildSleeveDelta), so
        // the merged sleeve batch must split whenever either hand is driven
        // -- without this the sleeves were only ever split when hidden or
        // under a probe delta, and stayed vanilla in every native pair.
        const int armFirst  = SleeveArmClass(firstCls);
        const int armSecond = SleeveArmClass(secondCls);
        const bool wantSplit =
            g_hidden[firstCls] || g_hidden[secondCls] ||
            (g_transformEnabled && (!MatIsIdentity(g_delta[firstCls]) ||
                                    !MatIsIdentity(g_delta[secondCls]) ||
                                    g_handTarget[firstCls].enabled ||
                                    g_handTarget[secondCls].enabled ||
                                    (armFirst  != GLSC_NONE && g_handTarget[armFirst].enabled) ||
                                    (armSecond != GLSC_NONE && g_handTarget[armSecond].enabled)));
        if (!wantSplit) {
            // Even with nothing to transform, keep the authored baselines
            // fresh when asked to: the landmarks are ABSOLUTE vertex indices,
            // so both halves solve fine from the unsplit batch. This is how
            // the very first enabled pair seeds the baseline that lets the
            // second pair engage.
            if (g_solveWanted[firstCls])
                SolveHandFromDraw(firstCls,  type, idx, count, nullptr);
            if (g_solveWanted[secondCls])
                SolveHandFromDraw(secondCls, type, idx, count, nullptr);
            if (g_capRecording && g_capArmed && !g_capDone)
                RecordDraw(mode, count, type, idx, cls);
            return 0;                                   // nothing to do: engine draws it
        }

        const int vertHalf = (batch == BATCH_HANDS_MERGED)
                           ? ((g_rigThisFrame == RIG_HANDS4) ? GLS_HAND4_VERTS
                                                             : GLS_HAND_VERTS)
                           : ((g_rigThisFrame == RIG_HANDS4) ? GLS_SLEEVE4_VERTS
                                                             : GLS_SLEEVE_VERTS);
        if (!VerifyMidpointSplit(type, idx, count, vertHalf)) {
            ++g_splitRejected;
            ++g_stats.rejectedAmbiguous;
            if (g_capRecording && g_capArmed && !g_capDone)
                RecordDraw(mode, count, type, idx, cls);
            return 0;                                   // fail closed: leave it alone
        }
        ++g_splitVerified;
        // WHICH HALF IS WHICH, from the content of THIS batch (the split is
        // the only place it matters, so the unsplit path above never pays
        // for the probe). Hands: whichever order the landmark tables solve
        // in; sleeves: the palm the first half's cuff is nearest.
        const bool handsWanted = g_solveWanted[GLSC_HAND_R] || g_solveWanted[GLSC_HAND_L]
                              || g_handTarget[GLSC_HAND_R].enabled || g_handTarget[GLSC_HAND_L].enabled;
        if (batch == BATCH_HANDS_MERGED) g_suppressOk = 1;
        if (batch == BATCH_HANDS_MERGED && handsWanted) {
            int c = GLSC_NONE, sw = 0;
            if (IdentifyHandDraw(type, idx, count, &c, &sw)) {
                g_orderHint = sw;
                g_identityByContent = 1;
                if (sw) { firstCls = GLSC_HAND_R; secondCls = GLSC_HAND_L; ++g_owner.orderSwapped; }
                else    { firstCls = GLSC_HAND_L; secondCls = GLSC_HAND_R; ++g_owner.orderNormal; }
            } else {
                ++g_owner.orderUnknown;
            }
        } else if (batch == BATCH_SLEEVES_MERGED) {
            const int f = IdentifySleeveDraw(type, idx, count / 2, firstCls);
            firstCls  = f;
            secondCls = (f == GLSC_SLEEVE_R) ? GLSC_SLEEVE_L : GLSC_SLEEVE_R;
        }
        const int esz  = IndexElemSize(type);
        const int half = count / 2;
        const void* idx2 = (const unsigned char*)idx + (size_t)half * esz;

        // Record the two halves SEPARATELY, so the census carries one row per
        // side with its own class and its own rendered matrix. A single row for
        // the merged batch could not express "the left half did not move".
        int recA = -1, recB = -1;
        if (g_capRecording && g_capArmed && !g_capDone) {
            recA = RecordDraw(mode, half, type, idx,  firstCls);
            recB = RecordDraw(mode, half, type, idx2, secondCls);
        }
        DrawOneHalf(mode, half, type, idx,  firstCls,  recA);
        DrawOneHalf(mode, half, type, idx2, secondCls, recB);
        g_orderHint = -1;
        g_suppressOk = 0;
        return 1;
    }

    NoteClassPivot(cls, type, idx, count);

    if (cls <= GLSC_NONE || cls >= GLSC_COUNT)
        return 0;

    // A hand SINGLE (the hands4 rig draws one hand per batch): keep the
    // authored baseline fresh exactly as the merged path does, so the
    // engagement contract works on every rig.
    if (HandClassToVRHP(cls) >= 0 && g_solveWanted[cls]
        && !g_handTarget[cls].enabled)
        SolveHandFromDraw(cls, type, idx, count, nullptr);
    // Same deadlock-breaker for the weapon: baseline body/rear-grip solves
    // keep running while no gun target is installed.
    if (cls == GLSC_GUN)
        GunBaselineTick(type, idx, count);

    if (g_hidden[cls]) {
        ++g_stats.suppressed;
        NoteRecordUsed(recIdx, g_delta[cls], -1);
        return 1;                                       // drawn: nowhere
    }

    if (!g_transformEnabled)          return 0;

    // Per-draw solved deltas outrank the fixed class delta: a hand single
    // takes the same solved delta a merged half takes, and every gun batch
    // takes this scene's one rigid weapon delta.
    float solvedDelta[16];
    const float* delta = g_delta[cls];
    int handNative = 0;
    if (HandClassToVRHP(cls) >= 0
        && BuildHandDelta(cls, type, idx, count, solvedDelta)) {
        delta = solvedDelta; handNative = 1;
    } else if (SleeveArmClass(cls) != GLSC_NONE) {
        const int r = BuildSleeveDelta(cls, type, idx, count, solvedDelta);
        if (r < 0) {                                    // visibility policy
            ++g_stats.suppressed;
            NoteRecordUsed(recIdx, g_delta[cls], -1);
            return 1;
        }
        if (r > 0) delta = solvedDelta;      // the forearm follows its own arm
    } else if (cls == GLSC_GUN) {
        GunSceneRollover();
        const int ord = g_gunScene.ordinal++;
        if (g_forceGunVanillaOrdinal >= 0 && ord == g_forceGunVanillaOrdinal
            && g_gunTarget.enabled) {
            // MUTANT (test seam): this batch stays vanilla. The scene's
            // accounting must call it a conflict and the pair be refused.
            ++g_gunSolve.batchesVanilla;
            NoteGunDisposition(GLS_GDISP_VANILLA, GunBatchSpan(type, idx, count));
        } else {
            const int r = BuildGunDelta(type, idx, count, ord, solvedDelta);
            if (r == 1) {
                delta = solvedDelta;
                ++g_gunSolve.batchesTransformed;
            } else if (r == -1) {
                ++g_stats.suppressed;
                NoteRecordUsed(recIdx, g_delta[cls], -1);
                return 1;                               // drawn: nowhere
            } else if (g_gunTarget.enabled) {
                ++g_gunSolve.batchesVanilla;
            }
        }
    }

    if (HandClassToVRHP(cls) >= 0 && g_handTarget[cls].enabled
        && !g_handTarget[cls].hold) {
        const int owner = ClaimOwner(cls, handNative);
        if (owner == OWN_NATIVE && !handNative && g_identityByContent) {
            NoteRecordUsed(recIdx, g_delta[cls], -1);
            return 1;                                   // the vanilla copy is not drawn
        }
        if (owner == OWN_VANILLA && handNative) {
            delta = g_delta[cls]; handNative = 0;
            NoteEyeApplyFailed(cls);
        }
        if (handNative) ++g_owner.primaryNativeDraw;
        else            ++g_owner.primaryVanillaFallbackDraw;
    }

    if (MatIsIdentity(delta))  return 0;

    const int done = (g_route == GLS_ROUTE_SCRATCH)
                   ? TransformDrawScratch(mode, count, type, idx, delta)
                   : TransformDrawModelview(mode, count, type, idx, delta);
    if (done) {
        ++g_stats.transformed;
        if (HandClassToVRHP(cls) >= 0) ++g_handSolve[cls].deltasApplied;
        if (g_lastTransformValid) NoteRecordUsed(recIdx, g_lastLoaded, 1);
    } else if (HandClassToVRHP(cls) >= 0 || cls == GLSC_GUN) {
        NoteEyeApplyFailed(cls);
    }
    return done;
}

unsigned GLSurf_SplitVerifiedCount() { return g_splitVerified; }
unsigned GLSurf_SplitRejectedCount() { return g_splitRejected; }

int GLSurf_OnDrawArrays(unsigned mode, int first, int count)
{
    if (g_drawDepth > 0) { ++g_reentered; return 0; }
    DrawDepthScope depth;
    ++g_stats.drawsSeen;
    ++g_drawSeq;
    UpdateViewmodelRun();
    if (g_inScene) ++g_stats.drawsScene;
    if (g_vmRunActive) { ++g_stats.drawsDepthHacked; ++g_vmRunIndex; }

    // glDrawArrays carries no index array to fingerprint against and is not
    // how the viewmodel reaches GL in any captured frame. Recorded for the
    // census, never claimed.
    if (g_capRecording && g_capArmed && !g_capDone) {
        RecordDraw(mode, count, 0, nullptr, GLSC_NONE);
        if (g_capCount > 0) {
            g_cap[g_capCount - 1].minIndex = first;
            g_cap[g_capCount - 1].maxIndex = first + count - 1;
            g_cap[g_capCount - 1].vertSpan = count;
        }
    }
    g_lastDrawClass = GLSC_NONE;
    return 0;
}

// ------------------------------------------------------------------ capture

void GLSurf_ArmCapture(int skip, int frames, const char* tag)
{
    EnsureInit();
    g_capCount = 0;
    g_capArmed = 1;
    g_capDone = 0;
    g_capRecording = 0;
    g_capSkip = skip < 0 ? 0 : skip;
    g_capFrames = frames < 1 ? 1 : frames;
    g_texCacheCount = 0;
    memset(g_capTag, 0, sizeof(g_capTag));
    if (tag) strncpy(g_capTag, tag, sizeof(g_capTag) - 1);
}

int GLSurf_CaptureArmed()       { return g_capArmed; }
int GLSurf_CaptureDone()        { return g_capDone; }
int GLSurf_CaptureRecordCount() { return g_capCount; }
unsigned GLSurf_SceneFrameCount() { return g_sceneFrameIndex; }

int GLSurf_DumpCapture(const char* path)
{
    if (!path) return -1;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) return -1;

    fprintf(f, "# CODVR GL draw capture  tag=%s  records=%d\n", g_capTag, g_capCount);
    fprintf(f, "frame,sceneFrame,seq,eye,inScene,mode,count,type,idxPtr,minIdx,maxIdx,span,"
               "vertPtr,vertSize,vertStride,vertType,vertEn,normPtr,normStride,normEn,"
               "texPtr,texStride,texEn,colPtr,colEn,tex0,tex0W,tex0H,tex1,"
               "dNear,dFar,dTest,dMask,dFunc,blend,bSrc,bDst,aTest,aFunc,aRef,"
               "cull,cullFace,frontFace,vpX,vpY,vpW,vpH,scX,scY,scW,scH,"
               "lockFirst,lockCount,cls,"
               "mv0,mv1,mv2,mv3,mv4,mv5,mv6,mv7,mv8,mv9,mv10,mv11,mv12,mv13,mv14,mv15,"
               "us0,us1,us2,us3,us4,us5,us6,us7,us8,us9,us10,us11,us12,us13,us14,us15,"
               "xformed,"
               "pj0,pj5,pj8,pj9,pj10,pj11,pj12,pj13,pj14\n");
    for (int i = 0; i < g_capCount; ++i) {
        const DrawRecord& r = g_cap[i];
        fprintf(f, "%u,%u,%u,%d,%d,0x%04X,%d,0x%04X,0x%08X,%d,%d,%d,",
                r.frame, r.sceneFrame, r.seq, r.eye, r.inScene,
                r.mode, r.count, r.type, r.idxPtr, r.minIndex, r.maxIndex, r.vertSpan);
        fprintf(f, "0x%08X,%d,%d,0x%04X,%d,0x%08X,%d,%d,0x%08X,%d,%d,0x%08X,%d,",
                r.vertPtr, r.vertSize, r.vertStride, r.vertType, r.vertEnabled,
                r.normPtr, r.normStride, r.normEnabled,
                r.texPtr, r.texStride, r.texEnabled, r.colPtr, r.colEnabled);
        fprintf(f, "%u,%d,%d,%u,", r.tex0, r.tex0W, r.tex0H, r.tex1);
        fprintf(f, "%.6f,%.6f,%d,%d,0x%04X,%d,0x%04X,0x%04X,%d,0x%04X,%.3f,",
                r.depthNear, r.depthFar, r.depthTest, r.depthMask, r.depthFunc,
                r.blend, r.blendSrc, r.blendDst, r.alphaTest, r.alphaFunc, r.alphaRef);
        fprintf(f, "%d,0x%04X,0x%04X,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,",
                r.cullEnable, r.cullFace, r.frontFace,
                r.viewport[0], r.viewport[1], r.viewport[2], r.viewport[3],
                r.scissor[0], r.scissor[1], r.scissor[2], r.scissor[3],
                r.lockFirst, r.lockCount, GLSurf_ClassName(r.cls));
        for (int k = 0; k < 16; ++k) fprintf(f, "%.5f,", r.modelview[k]);
        for (int k = 0; k < 16; ++k) fprintf(f, "%.5f,", r.mvUsed[k]);
        fprintf(f, "%d,", r.transformed);
        fprintf(f, "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n",
                r.projection[0], r.projection[5], r.projection[8], r.projection[9],
                r.projection[10], r.projection[11], r.projection[12],
                r.projection[13], r.projection[14]);
    }
    fclose(f);
    return g_capCount;
}

// ------------------------------------------------------------------- stats

void GLSurf_GetStats(GLSurfStats* out) { if (out) *out = g_stats; }

int GLSurf_TestLastTransformed(float loaded[16], float restored[16])
{
    if (!g_lastTransformValid) return 0;
    if (loaded)   memcpy(loaded,   g_lastLoaded,   16 * sizeof(float));
    if (restored) memcpy(restored, g_lastRestored, 16 * sizeof(float));
    return 1;
}

int GLSurf_TestClassOfLastDraw() { return g_lastDrawClass; }

void GLSurf_Reset()
{
    g_initDone = true;
    ResetGLState();
    ResetFrameCounters();
    GLSurf_ClearAllDeltas();
    memset(g_hidden, 0, sizeof(g_hidden));
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_eyeConsumed, 0, sizeof(g_eyeConsumed));
    memset(g_eyeConsumedValid, 0, sizeof(g_eyeConsumedValid));
    g_pairMismatch = 0;
    g_splitVerified = 0; g_splitRejected = 0;
    memset(&g_shift, 0, sizeof(g_shift));
    memset(g_classPivot, 0, sizeof(g_classPivot));
    memset(g_classPivotValid, 0, sizeof(g_classPivotValid));
    g_probePivotKnown = 0;
    g_latchSerial = 0; g_latchCount = 0;
    g_transformEnabled = 1;
    g_route = GLS_ROUTE_MODELVIEW;
    memset(g_handTarget, 0, sizeof(g_handTarget));
    memset(g_handSolve, 0, sizeof(g_handSolve));
    memset(g_refuseHisto, 0, sizeof(g_refuseHisto));
    memset(g_refuseOverflow, 0, sizeof(g_refuseOverflow));
    memset(g_solveWanted, 0, sizeof(g_solveWanted));
    memset(g_handSolveEye, 0, sizeof(g_handSolveEye));
    memset(g_handEyeAttempt, 0, sizeof(g_handEyeAttempt));
    memset(g_pairEligible, 0, sizeof(g_pairEligible));
    memset(g_pairGateRefusals, 0, sizeof(g_pairGateRefusals));
    g_probeClass = GLSC_NONE;
    g_capCount = 0; g_capArmed = 0; g_capDone = 0; g_capRecording = 0;
    g_capSkip = 0; g_capFrames = 0;
    g_texCacheCount = 0;
    g_frameIndex = 0; g_sceneFrameIndex = 0; g_inScene = 0; g_curEye = 0;
    g_lastTransformValid = 0; g_lastDrawClass = GLSC_NONE;
    g_rigThisFrame = RIG_NONE;
    g_classifierEnabled = 1;
    memset(&g_gunTarget, 0, sizeof(g_gunTarget));
    memset(&g_gunSolve, 0, sizeof(g_gunSolve));
    memset(&g_gunScene, 0, sizeof(g_gunScene));
    g_gunModel = nullptr;
    memset(g_gunSceneRec, 0, sizeof(g_gunSceneRec));
    g_gunSceneCur = nullptr;
    g_gunDeltaIds = 0;
    memset(g_weaponStats, 0, sizeof(g_weaponStats));
    g_weaponStatsCount = 0;
    g_forceGunVanillaOrdinal = -1;
    g_gunSolveWanted = 0;
    memset(g_eyeOff, 0, sizeof(g_eyeOff));
    memset(g_eyeApply, 0, sizeof(g_eyeApply));
    memset(g_palmTry, 0, sizeof(g_palmTry));
    g_palmTryHead = g_palmTryCount = g_palmTryTotal = 0;
    memset(g_handSceneDelta, 0, sizeof(g_handSceneDelta));
    g_sleeveFollowedPalm = g_sleeveFollowedCentroid = 0;
    memset(&g_sleeveStats, 0, sizeof(g_sleeveStats));
    g_sleeveAnatomical = 1;
    g_sleeveHideNearHead = 1;
    g_sleeveVisible = 1;
    memset(&g_owner, 0, sizeof(g_owner));
    memset(g_eyeOwner, 0, sizeof(g_eyeOwner));
    g_orderHint = -1;
    g_identityByContent = 0;
    g_suppressOk = 0;
}

// Test-only: the sleeve attribution measurement for a draw.
int GLSurf_TestSleeveNearest(unsigned type, const void* idx, int count, float* dR, float* dL)
{
    return SleeveNearestPalm(type, idx, count, dR, dL);
}
