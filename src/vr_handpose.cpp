#include "vr_handpose.h"

#include <math.h>
#include <string.h>

// The generated landmark tables. See tools/extract_palm_landmarks.py.
#include "vr_weapon_profile.h"   // VRWP_CONF_* only
#include "vr_palm_landmarks.inc"
#include "vr_gun_landmarks.inc"
#include "vr_gun_surfaces.inc"
#include "vr_sleeve_landmarks.inc"

// ---------------------------------------------------------------------------
// active tables (the test seam swaps these)

struct LandmarkSet {
    const CodvrPalmLandmark* lm;
    int                      n;
};

static LandmarkSet g_set[VRHP_HANDS] = {
    { kPalmTRIG, (int)(sizeof(kPalmTRIG) / sizeof(kPalmTRIG[0])) },
    { kPalmSUPP, (int)(sizeof(kPalmSUPP) / sizeof(kPalmSUPP[0])) },
};

static VRHandPoseStats g_stats;

int VRHandPose_BatchVerts() { return CODVR_HAND_BATCH_VERTS; }
int VRHandPose_SplitVert()  { return CODVR_HAND_SPLIT_VERT; }

int VRHandPose_LandmarkCount(int hand)
{
    if (hand < 0 || hand >= VRHP_HANDS)
        return 0;
    return g_set[hand].n;
}

void VRHandPose_TestSetLandmarks(int hand, const CodvrPalmLandmark* lm, int n)
{
    if (hand < 0 || hand >= VRHP_HANDS)
        return;
    g_set[hand].lm = lm;
    g_set[hand].n  = n;
}

void VRHandPose_TestRestore()
{
    g_set[VRHP_TRIGGER].lm = kPalmTRIG;
    g_set[VRHP_TRIGGER].n  = (int)(sizeof(kPalmTRIG) / sizeof(kPalmTRIG[0]));
    g_set[VRHP_SUPPORT].lm = kPalmSUPP;
    g_set[VRHP_SUPPORT].n  = (int)(sizeof(kPalmSUPP) / sizeof(kPalmSUPP[0]));
}

void VRHandPose_GetStats(VRHandPoseStats* out) { if (out) *out = g_stats; }
void VRHandPose_ResetStats() { memset(&g_stats, 0, sizeof(g_stats)); }

// ---------------------------------------------------------------------------
// orthogonal Procrustes by Horn's quaternion method
//
// WHY NOT AN SVD, AND WHY NOT A POLAR ITERATION
//
// Both work, and both have a failure mode this one does not. An SVD needs the
// reflection case handled by hand -- a mirrored fit is a perfectly good
// least-squares answer and a completely wrong hand. A polar iteration diverges
// on a near-singular covariance instead of reporting that it cannot answer.
//
// Horn's method builds a symmetric 4x4 whose largest eigenvector IS the
// optimal rotation quaternion. A quaternion cannot represent a reflection, so
// the proper-rotation constraint is structural rather than patched on
// afterwards, and the eigen-solve below is a fixed number of Jacobi sweeps
// with no convergence question.

static void Jacobi4(double a[4][4], double v[4][4])
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            v[i][j] = (i == j) ? 1.0 : 0.0;

    // 32 sweeps is far past convergence for a 4x4; fixed count keeps the cost
    // and the result identical on every call, which a renderer wants.
    for (int sweep = 0; sweep < 32; ++sweep) {
        int p = 0, q = 1;
        double off = 0.0;
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j)
                if (fabs(a[i][j]) > off) { off = fabs(a[i][j]); p = i; q = j; }
        if (off < 1e-18)
            break;

        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t = (theta >= 0 ? 1.0 : -1.0)
                       / (fabs(theta) + sqrt(theta * theta + 1.0));
        const double c = 1.0 / sqrt(t * t + 1.0);
        const double s = t * c;

        for (int k = 0; k < 4; ++k) {
            const double akp = a[k][p], akq = a[k][q];
            a[k][p] = c * akp - s * akq;
            a[k][q] = s * akp + c * akq;
        }
        for (int k = 0; k < 4; ++k) {
            const double apk = a[p][k], aqk = a[q][k];
            a[p][k] = c * apk - s * aqk;
            a[q][k] = s * apk + c * aqk;
        }
        for (int k = 0; k < 4; ++k) {
            const double vkp = v[k][p], vkq = v[k][q];
            v[k][p] = c * vkp - s * vkq;
            v[k][q] = s * vkp + c * vkq;
        }
    }
}

// Rows of `axis` are the frame's axes: row k is the k-th basis vector, so
//     world = origin + sum_k local[k] * row_k
// which is the convention codasset and vr_math already use.
static void QuatToAxisRows(const double q[4], float axis[9])
{
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    axis[0] = (float)(1 - 2 * (y * y + z * z));
    axis[1] = (float)(2 * (x * y + w * z));
    axis[2] = (float)(2 * (x * z - w * y));
    axis[3] = (float)(2 * (x * y - w * z));
    axis[4] = (float)(1 - 2 * (x * x + z * z));
    axis[5] = (float)(2 * (y * z + w * x));
    axis[6] = (float)(2 * (x * z + w * y));
    axis[7] = (float)(2 * (y * z - w * x));
    axis[8] = (float)(1 - 2 * (x * x + y * y));
}

// local -> parent, using rows as axes.
static void AxisApply(const float axis[9], const float v[3], float out[3])
{
    out[0] = axis[0] * v[0] + axis[3] * v[1] + axis[6] * v[2];
    out[1] = axis[1] * v[0] + axis[4] * v[1] + axis[7] * v[2];
    out[2] = axis[2] * v[0] + axis[5] * v[1] + axis[8] * v[2];
}

// ---------------------------------------------------------------------------

// The solve itself, over any rigid landmark set. Hands and the gun body are
// the same problem, so they are the same code -- and the gun inherits the
// hands' proof rather than needing its own.
static int SolveRigid(const CodvrPalmLandmark* lm, int n, const float* verts,
                      int strideBytes, int count, float unitsPerMetre,
                      VRHandPose* out, float* rmsMetresOut = 0,
                      VRHandPoseCanRead canRead = 0, void* readCtx = 0,
                      int* reasonOut = 0, int idxShift = 0, int firstIndex = 0,
                      float maxRmsM = VRHP_MAX_RMS_M)
{
    if (reasonOut) *reasonOut = VRHP_REASON_INDEX_RANGE;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->axis[0] = out->axis[4] = out->axis[8] = 1.0f;
    }
    if (!lm || n < 3 || !verts || strideBytes < 12)
        return 0;
    // The bind cloud is stored in metres; the observed cloud is in whatever
    // space the engine submitted. A rotation-only Procrustes cannot absorb a
    // scale mismatch -- it can only refuse -- so the caller must NAME the
    // vertex units and the bind cloud is scaled into them here. This is the
    // line the 2026-08-27 attended run was missing: real gameplay vertices
    // (inches) against metre landmarks refused 1197/1197 hand draws.
    if (!(unitsPerMetre > 0.0f) || !(unitsPerMetre < 1.0e6f))
        return 0;

    // Refuse a batch that cannot contain the landmarks rather than reading
    // past its end. A pose is never worth a fault.
    // The draw references [firstIndex, count): a landmark outside it is
    // not this draw's geometry even when the buffer holds bytes there.
    for (int i = 0; i < n; ++i) {
        const int vi = lm[i].index + idxShift;
        if (vi < 0 || vi < firstIndex || vi >= count)
            return 0;
    }

    // THE READ GATE. Every landmark this table will touch must be DEFINED
    // before any of it is read. This runs before the centroid loop -- before
    // the first dereference -- so a mixed valid/unknown cloud cannot reach the
    // covariance, let alone Horn's method. It is the structural form of "an
    // orphaned store is not geometry".
    if (canRead) {
        for (int i = 0; i < n; ++i)
            if (!canRead(readCtx, lm[i].index + idxShift)) {
                if (reasonOut) *reasonOut = VRHP_REASON_BYTES_UNKNOWN;
                return 0;
            }
    }
    ++g_stats.solverRuns;

    LandmarkSet set;
    set.lm = lm;
    set.n  = n;

    const unsigned char* base = (const unsigned char*)verts;

    // Centroids, with the bind cloud scaled into the vertex units.
    const double s = (double)unitsPerMetre;
    double cb[3] = { 0, 0, 0 }, co[3] = { 0, 0, 0 };
    for (int i = 0; i < set.n; ++i) {
        const float* o = (const float*)(base + (size_t)(set.lm[i].index + idxShift) * strideBytes);
        for (int k = 0; k < 3; ++k) {
            cb[k] += set.lm[i].bindM[k] * s;
            co[k] += o[k];
        }
    }
    for (int k = 0; k < 3; ++k) { cb[k] /= set.n; co[k] /= set.n; }

    // Covariance of the centred clouds.
    double h[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
    for (int i = 0; i < set.n; ++i) {
        const float* o = (const float*)(base + (size_t)(set.lm[i].index + idxShift) * strideBytes);
        const double a[3] = { set.lm[i].bindM[0] * s - cb[0],
                              set.lm[i].bindM[1] * s - cb[1],
                              set.lm[i].bindM[2] * s - cb[2] };
        const double b[3] = { o[0] - co[0], o[1] - co[1], o[2] - co[2] };
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                h[r][c] += a[r] * b[c];
    }

    // Horn's symmetric 4x4. Its largest eigenvector is the optimal quaternion.
    double N[4][4];
    N[0][0] =  h[0][0] + h[1][1] + h[2][2];
    N[1][1] =  h[0][0] - h[1][1] - h[2][2];
    N[2][2] = -h[0][0] + h[1][1] - h[2][2];
    N[3][3] = -h[0][0] - h[1][1] + h[2][2];
    N[0][1] = N[1][0] = h[1][2] - h[2][1];
    N[0][2] = N[2][0] = h[2][0] - h[0][2];
    N[0][3] = N[3][0] = h[0][1] - h[1][0];
    N[1][2] = N[2][1] = h[0][1] + h[1][0];
    N[1][3] = N[3][1] = h[2][0] + h[0][2];
    N[2][3] = N[3][2] = h[1][2] + h[2][1];

    double v[4][4];
    Jacobi4(N, v);

    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (N[i][i] > N[best][best]) best = i;

    double q[4] = { v[0][best], v[1][best], v[2][best], v[3][best] };
    const double qn = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(qn > 1e-9)) {
        if (reasonOut) *reasonOut = VRHP_REASON_DEGENERATE;
        return 0;
    }
    for (int k = 0; k < 4; ++k) q[k] /= qn;

    float axis[9];
    QuatToAxisRows(q, axis);

    const float pos[3] = {
        (float)(co[0] - (axis[0] * cb[0] + axis[3] * cb[1] + axis[6] * cb[2])),
        (float)(co[1] - (axis[1] * cb[0] + axis[4] * cb[1] + axis[7] * cb[2])),
        (float)(co[2] - (axis[2] * cb[0] + axis[5] * cb[1] + axis[8] * cb[2])),
    };

    // The residual is the witness that this batch really is the hand. A wrong
    // batch of the right vertex count would still produce SOME rotation; it
    // would not reproduce these landmarks to a micrometre.
    double sum = 0.0;
    for (int i = 0; i < set.n; ++i) {
        const float* o = (const float*)(base + (size_t)(set.lm[i].index + idxShift) * strideBytes);
        const float bindU[3] = { set.lm[i].bindM[0] * unitsPerMetre,
                                 set.lm[i].bindM[1] * unitsPerMetre,
                                 set.lm[i].bindM[2] * unitsPerMetre };
        float pred[3];
        AxisApply(axis, bindU, pred);
        for (int k = 0; k < 3; ++k) {
            const double d = (double)(pos[k] + pred[k]) - o[k];
            sum += d * d;
        }
    }
    const float rms = (float)sqrt(sum / set.n);
    if (rmsMetresOut)
        *rmsMetresOut = rms / unitsPerMetre;   // physical distance, any space

    if (out) {
        memcpy(out->pos, pos, sizeof(pos));
        memcpy(out->axis, axis, sizeof(axis));
        out->rmsM  = rms;
    }
    // The gate means the same PHYSICAL distance whatever units the vertices
    // arrived in.
    if (!(rms <= maxRmsM * unitsPerMetre)) {
        if (reasonOut) *reasonOut = VRHP_REASON_RMS;
        return 0;
    }
    if (out)
        out->valid = 1;
    if (reasonOut) *reasonOut = VRHP_REASON_OK;
    return 1;
}

const char* VRHandPose_ReasonName(int reason)
{
    switch (reason) {
    case VRHP_REASON_OK:            return "OK";
    case VRHP_REASON_INDEX_RANGE:   return "INDEX_RANGE";
    case VRHP_REASON_BYTES_UNKNOWN: return "VBO_BYTES_UNKNOWN";
    case VRHP_REASON_DEGENERATE:    return "DEGENERATE";
    case VRHP_REASON_RMS:           return "LANDMARK_RMS";
    default:                        return "?";
    }
}

int VRHandPose_RigCount()
{
    return (int)(sizeof(kHandRigDefs) / sizeof(kHandRigDefs[0]));
}

const CodvrHandRigDef* VRHandPose_Rig(int i)
{
    if (i < 0 || i >= VRHandPose_RigCount())
        return nullptr;
    return &kHandRigDefs[i];
}

static VRHandPoseSample g_lastSample[VRHP_HANDS];

// Keep the attempt's RAW inputs so an attended run's log carries
// offline-verifiable evidence whatever the verdict was.
static void CaptureSample(int hand, const CodvrPalmLandmark* lm, int n,
                          const float* verts, int strideBytes, int count,
                          float unitsPerMetre, int ok, const VRHandPose* out,
                          VRHandPoseCanRead canRead = 0, void* readCtx = 0,
                          int idxShift = 0)
{
    if (hand < 0 || hand >= VRHP_HANDS)
        return;
    VRHandPoseSample& s = g_lastSample[hand];
    ++s.attempts;
    s.ok = ok;
    s.rms = out ? out->rmsM : 0.0f;
    s.unitsPerMetre = unitsPerMetre;
    s.n = 0;
    s.shape = 0.0f;
    s.shift = idxShift;
    if (!verts || strideBytes < 12 || !lm)
        return;
    const unsigned char* base = (const unsigned char*)verts;
    for (int i = 0; i < n && s.n < VRHP_MAX_LM; ++i) {
        const int vi = lm[i].index + idxShift;
        if (vi < 0 || vi >= count)
            continue;
        // The diagnostic copy obeys the read gate too: an undefined byte is
        // not evidence of anything, and a log line built from it would be
        // the very zero-cloud this contract forbids.
        if (canRead && !canRead(readCtx, vi))
            continue;
        const float* o = (const float*)(base + (size_t)vi * strideBytes);
        s.idx[s.n] = vi;
        s.obs[s.n * 3 + 0] = o[0];
        s.obs[s.n * 3 + 1] = o[1];
        s.obs[s.n * 3 + 2] = o[2];
        ++s.n;
    }
    if (s.n > 1) {
        double c[3] = { 0, 0, 0 };
        for (int i = 0; i < s.n; ++i)
            for (int k = 0; k < 3; ++k) c[k] += s.obs[i * 3 + k];
        for (int k = 0; k < 3; ++k) c[k] /= s.n;
        double acc = 0.0;
        for (int i = 0; i < s.n; ++i) {
            double d = 0.0;
            for (int k = 0; k < 3; ++k) {
                const double e = s.obs[i * 3 + k] - c[k];
                d += e * e;
            }
            acc += d;
        }
        s.shape = (float)sqrt(acc / s.n);
    }
}

int VRHandPose_SolveAny(int hand, const float* verts, int strideBytes,
                        int count, float unitsPerMetre, VRHandPose* out,
                        int* outRig, float* outBestRmsM)
{
    return VRHandPose_SolveAnyEx(hand, verts, strideBytes, count, unitsPerMetre,
                                 out, outRig, outBestRmsM, 0, 0, 0);
}

// The two orders a merged-rig table can be read in. Order 0 is the file
// order the table was extracted in; order 1 shifts the table into the other
// half of the batch. A rig with no split (per-surface single draws) has one.
static int g_swapAllowed = 1;
void VRHandPose_TestSetSwapAllowed(int on) { g_swapAllowed = on ? 1 : 0; }

static int OrderShift(int hand, int splitVert, int swapped)
{
    if (!swapped || splitVert <= 0) return 0;
    return (hand == VRHP_TRIGGER) ? -splitVert : splitVert;
}

// Table span check: does every landmark, shifted, lie inside [first, count)?
static int TableInRange(const CodvrPalmLandmark* lm, int n, int shift,
                        int first, int count)
{
    for (int i = 0; i < n; ++i) {
        const int vi = lm[i].index + shift;
        if (vi < 0 || vi < first || vi >= count) return 0;
    }
    return 1;
}

// Which half-order a hand last solved in: the common case then costs one
// solve whichever order the engine happens to be packing this frame.
static int g_lastSwapped[VRHP_HANDS] = { 0, 0 };

int VRHandPose_SolveAnyEx(int hand, const float* verts, int strideBytes,
                          int count, float unitsPerMetre, VRHandPose* out,
                          int* outRig, float* outBestRmsM,
                          VRHandPoseCanRead canRead, void* readCtx,
                          int* outReason, int* outSwapped, int preferSwapped,
                          int firstIndex)
{
    if (outRig) *outRig = -2;
    if (outBestRmsM) *outBestRmsM = -1.0f;
    if (outReason) *outReason = VRHP_REASON_INDEX_RANGE;
    if (outSwapped) *outSwapped = 0;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->axis[0] = out->axis[4] = out->axis[8] = 1.0f;
    }
    if (hand < 0 || hand >= VRHP_HANDS)
        return 0;
    int sawUnknown = 0, sawRms = 0, sawDegenerate = 0;
    // Order try-list: the hinted (or last successful) order first.
    int firstOrder = (preferSwapped == 0 || preferSwapped == 1)
                   ? preferSwapped : g_lastSwapped[hand];
    if (!g_swapAllowed) firstOrder = 0;
    const int orders[2] = { firstOrder, 1 - firstOrder };
    const int nOrders = g_swapAllowed ? 2 : 1;
    // The installed set first: the proven default (hands_new) in production,
    // the injected table under test. Then every shipped rig's table for this
    // hand, last success first, so a sten sequence costs one extra solve on
    // its first frame only. A wrong table cannot win: a different rig's bind
    // cloud misses the micrometre residual gate by centimetres, and a
    // MIRRORED hand cannot be reached by a proper rotation at all.
    static int lastHit[VRHP_HANDS] = { -1, -1 };
    const int nRigs = VRHandPose_RigCount();
    int lastShift = 0;
    for (int pass = -2; pass < nRigs; ++pass) {
        const CodvrPalmLandmark* lm;
        int n, ri, split;
        if (pass == -2) {
            lm = g_set[hand].lm; n = g_set[hand].n; ri = -1;
            split = CODVR_HAND_SPLIT_VERT;
        } else if (pass == -1) {
            ri = lastHit[hand];
            if (ri < 0) continue;
            const CodvrHandRigDef& rd = kHandRigDefs[ri];
            lm = (hand == VRHP_TRIGGER) ? rd.trig : rd.supp;
            n  = (hand == VRHP_TRIGGER) ? rd.nTrig : rd.nSupp;
            split = rd.splitVert;
            if (lm == g_set[hand].lm) continue;
        } else {
            ri = pass;
            if (ri == lastHit[hand]) continue;      // tried in pass -1
            const CodvrHandRigDef& rd = kHandRigDefs[ri];
            lm = (hand == VRHP_TRIGGER) ? rd.trig : rd.supp;
            n  = (hand == VRHP_TRIGGER) ? rd.nTrig : rd.nSupp;
            split = rd.splitVert;
            if (lm == g_set[hand].lm) continue;     // the default, refused already
            // A rig smaller than the draw cannot be the rig being drawn. Its
            // landmark indices still fall INSIDE the bigger batch, so without
            // this it runs anyway on data that is not its own: viewmodel_hands4
            // (302 verts) was being solved against every 1674-vertex merged
            // batch, contributing a meaningless residual to outBestRmsM and
            // making the refusal diagnostic report a number from the wrong
            // rig. It could also, in principle, produce a wrong-rig match.
            if (count > rd.batchVerts) continue;
        }
        for (int oi = 0; oi < nOrders; ++oi) {
            const int swapped = orders[oi];
            if (swapped && split <= 0) continue;    // single-surface rig: one order
            const int shift = OrderShift(hand, split, swapped);
            if (!TableInRange(lm, n, shift, firstIndex, count)) continue;
            lastShift = shift;
            float rmsM = -1.0f;
            int reason = VRHP_REASON_INDEX_RANGE;
            const int ok = SolveRigid(lm, n, verts, strideBytes, count,
                                      unitsPerMetre, out, &rmsM, canRead, readCtx,
                                      &reason, shift, firstIndex);
            if (outBestRmsM && rmsM >= 0.0f
                && (*outBestRmsM < 0.0f || rmsM < *outBestRmsM))
                *outBestRmsM = rmsM;
            if (ok) {
                if (ri >= 0) lastHit[hand] = ri;
                g_lastSwapped[hand] = swapped;
                if (outRig) *outRig = ri;
                if (outSwapped) *outSwapped = swapped;
                if (outReason) *outReason = VRHP_REASON_OK;
                CaptureSample(hand, lm, n, verts, strideBytes, count,
                              unitsPerMetre, 1, out, canRead, readCtx, shift);
                if (out) g_stats.lastRmsM[hand] = out->rmsM;
                ++g_stats.solves[hand];
                return 1;
            }
            if (reason == VRHP_REASON_BYTES_UNKNOWN) sawUnknown = 1;
            else if (reason == VRHP_REASON_RMS)      sawRms = 1;
            else if (reason == VRHP_REASON_DEGENERATE) sawDegenerate = 1;
        }
    }
    // Total refusal: sample the DEFAULT set's observed landmarks -- the most
    // useful diagnostic for "why did nothing match".
    CaptureSample(hand, g_set[hand].lm, g_set[hand].n, verts, strideBytes,
                  count, unitsPerMetre, 0, out, canRead, readCtx, lastShift);
    ++g_stats.refusals[hand];
    // Precedence: unknown bytes outrank every residual verdict. A residual
    // computed on a store with undefined bytes is not evidence about the
    // geometry, so the refusal is attributed to the sourcing, not the fit.
    if (sawUnknown) {
        ++g_stats.refusedUnknownBytes[hand];
        if (outReason) *outReason = VRHP_REASON_BYTES_UNKNOWN;
    } else if (sawRms) {
        if (outReason) *outReason = VRHP_REASON_RMS;
    } else if (sawDegenerate) {
        if (outReason) *outReason = VRHP_REASON_DEGENERATE;
    }
    return 0;
}

int VRHandPose_Identify(const float* verts, int strideBytes, int firstIndex,
                        int count, float unitsPerMetre,
                        VRHandPoseCanRead canRead, void* readCtx,
                        int* outHand, int* outSwapped, int* outRig,
                        VRHandPose* outPose)
{
    if (outHand) *outHand = -1;
    if (outSwapped) *outSwapped = 0;
    if (outRig) *outRig = -2;
    const int nRigs = VRHandPose_RigCount();
    const int nOrders = g_swapAllowed ? 2 : 1;
    // Hands, then orders (last successful first), then tables. A quiet
    // solve: g_stats and the sample record are the ATTRIBUTED attempt's,
    // which the caller makes once it knows which hand this is.
    for (int hand = 0; hand < VRHP_HANDS; ++hand) {
        const int orders[2] = { g_lastSwapped[hand], 1 - g_lastSwapped[hand] };
        for (int oi = 0; oi < nOrders; ++oi) {
            const int swapped = g_swapAllowed ? orders[oi] : 0;
            for (int pass = -1; pass < nRigs; ++pass) {
                const CodvrPalmLandmark* lm;
                int n, split;
                if (pass < 0) {
                    lm = g_set[hand].lm; n = g_set[hand].n;
                    split = CODVR_HAND_SPLIT_VERT;
                } else {
                    const CodvrHandRigDef& rd = kHandRigDefs[pass];
                    lm = (hand == VRHP_TRIGGER) ? rd.trig : rd.supp;
                    n  = (hand == VRHP_TRIGGER) ? rd.nTrig : rd.nSupp;
                    split = rd.splitVert;
                    if (lm == g_set[hand].lm) continue;
                    if (count > rd.batchVerts) continue;
                }
                if (swapped && split <= 0) continue;
                const int shift = OrderShift(hand, split, swapped);
                if (!TableInRange(lm, n, shift, firstIndex, count)) continue;
                VRHandPose pose;
                int reason = VRHP_REASON_INDEX_RANGE;
                if (SolveRigid(lm, n, verts, strideBytes, count, unitsPerMetre,
                               &pose, 0, canRead, readCtx, &reason, shift,
                               firstIndex)) {
                    if (outHand) *outHand = hand;
                    if (outSwapped) *outSwapped = swapped;
                    if (outRig) *outRig = pass;
                    if (outPose) *outPose = pose;
                    return 1;
                }
            }
        }
    }
    return 0;
}

void VRHandPose_GetLastSample(int hand, VRHandPoseSample* out)
{
    if (!out) return;
    if (hand < 0 || hand >= VRHP_HANDS) { memset(out, 0, sizeof(*out)); return; }
    *out = g_lastSample[hand];
}

int VRHandPose_Solve(int hand, const float* verts, int strideBytes, int count,
                     float unitsPerMetre, VRHandPose* out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        out->axis[0] = out->axis[4] = out->axis[8] = 1.0f;
    }
    if (hand < 0 || hand >= VRHP_HANDS)
        return 0;
    const LandmarkSet& set = g_set[hand];
    const int ok = SolveRigid(set.lm, set.n, verts, strideBytes, count,
                              unitsPerMetre, out);

    // Keep the attempt's RAW inputs so an attended run's log carries
    // offline-verifiable evidence, whatever the verdict was.
    {
        VRHandPoseSample& s = g_lastSample[hand];
        ++s.attempts;
        s.ok = ok;
        s.rms = out ? out->rmsM : 0.0f;
        s.unitsPerMetre = unitsPerMetre;
        s.n = 0;
        if (verts && strideBytes >= 12) {
            const unsigned char* base = (const unsigned char*)verts;
            for (int i = 0; i < set.n && s.n < VRHP_MAX_LM; ++i) {
                if (set.lm[i].index < 0 || set.lm[i].index >= count)
                    continue;
                const float* o = (const float*)(base
                    + (size_t)set.lm[i].index * strideBytes);
                s.idx[s.n] = set.lm[i].index;
                s.obs[s.n * 3 + 0] = o[0];
                s.obs[s.n * 3 + 1] = o[1];
                s.obs[s.n * 3 + 2] = o[2];
                ++s.n;
            }
        }
    }

    if (out) g_stats.lastRmsM[hand] = out->rmsM;
    if (ok) ++g_stats.solves[hand];
    else    ++g_stats.refusals[hand];
    return ok;
}

// ---------------------------------------------------------------------------
// the gun

int VRGunPose_ModelCount()
{
    return (int)(sizeof(kGunModels) / sizeof(kGunModels[0]));
}

const CodvrGunModel* VRGunPose_ByVertexCount(int vertexCount)
{
    const int n = VRGunPose_ModelCount();
    for (int i = 0; i < n; ++i)
        if (kGunModels[i].vertexCount == vertexCount)
            return &kGunModels[i];
    return nullptr;    // unknown model: leave it exactly as the engine drew it
}

int VRGunPose_Solve(int vertexCount, const float* verts, int strideBytes,
                    int count, float unitsPerMetre, VRHandPose* out)
{
    const CodvrGunModel* m = VRGunPose_ByVertexCount(vertexCount);
    if (!m)
        return 0;
    return SolveRigid(m->lm, m->n, verts, strideBytes, count, unitsPerMetre, out);
}

const CodvrGunModel* VRGunPose_SolveByCount(int vertexCount, const float* verts,
                                            int strideBytes, int count,
                                            float unitsPerMetre, VRHandPose* out)
{
    return VRGunPose_SolveByCountEx(vertexCount, verts, strideBytes, count,
                                    unitsPerMetre, out, 0, 0, 0);
}

const CodvrGunModel* VRGunPose_SolveByCountEx(int vertexCount, const float* verts,
                                              int strideBytes, int count,
                                              float unitsPerMetre, VRHandPose* out,
                                              VRHandPoseCanRead canRead,
                                              void* readCtx, int* outReason)
{
    if (outReason) *outReason = VRHP_REASON_INDEX_RANGE;
    int sawUnknown = 0, sawRms = 0;
    const int nM = VRGunPose_ModelCount();
    for (int i = 0; i < nM; ++i) {
        const CodvrGunModel& m = kGunModels[i];
        if (m.vertexCount != vertexCount)
            continue;
        int reason = VRHP_REASON_INDEX_RANGE;
        if (SolveRigid(m.lm, m.n, verts, strideBytes, count, unitsPerMetre, out,
                       0, canRead, readCtx, &reason)) {
            if (outReason) *outReason = VRHP_REASON_OK;
            return &m;
        }
        if (reason == VRHP_REASON_BYTES_UNKNOWN) sawUnknown = 1;
        else if (reason == VRHP_REASON_RMS || reason == VRHP_REASON_DEGENERATE) sawRms = 1;
    }
    if (outReason) {
        if (sawUnknown)   *outReason = VRHP_REASON_BYTES_UNKNOWN;
        else if (sawRms)  *outReason = VRHP_REASON_RMS;
    }
    return nullptr;
}

int VRSleevePose_DefCount()
{
    return (int)(sizeof(kSleeveDefs) / sizeof(kSleeveDefs[0]));
}

const CodvrSleeveDef* VRSleevePose_Def(int i)
{
    if (i < 0 || i >= VRSleevePose_DefCount()) return nullptr;
    return &kSleeveDefs[i];
}

const CodvrSleeveDef* VRSleevePose_DefFor(int rig, int hand)
{
    const CodvrHandRigDef* rd = (rig >= 0) ? VRHandPose_Rig(rig) : nullptr;
    const int n = VRSleevePose_DefCount();
    for (int i = 0; i < n; ++i) {
        const CodvrSleeveDef& d = kSleeveDefs[i];
        if (d.hand != hand) continue;
        if (!rd || strcmp(rd->name, d.rig) == 0) return &d;
    }
    return nullptr;
}

const CodvrSleeveDef* VRSleevePose_SolveAny(int hand, const float* verts, int strideBytes,
                                            int count, float unitsPerMetre, VRHandPose* out,
                                            int preferRig, VRHandPoseCanRead canRead,
                                            void* readCtx, int* outReason, int firstIndex,
                                            float* outRmsM)
{
    if (outReason) *outReason = VRHP_REASON_INDEX_RANGE;
    if (outRmsM) *outRmsM = -1.0f;
    const CodvrHandRigDef* rd = (preferRig >= 0) ? VRHandPose_Rig(preferRig) : nullptr;
    const int n = VRSleevePose_DefCount();
    int sawUnknown = 0, sawRms = 0;
    float bestRms = -1.0f;
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < n; ++i) {
            const CodvrSleeveDef& d = kSleeveDefs[i];
            if (d.hand != hand) continue;
            const int mine = rd && strcmp(rd->name, d.rig) == 0;
            if ((pass == 0) != (mine != 0)) continue;
            int reason = VRHP_REASON_INDEX_RANGE;
            float rms = -1.0f;
            if (SolveRigid(d.lm, d.n, verts, strideBytes, count, unitsPerMetre, out, &rms,
                           canRead, readCtx, &reason, firstIndex, firstIndex, d.rmsGateM)) {
                if (outReason) *outReason = VRHP_REASON_OK;
                if (outRmsM) *outRmsM = rms;
                return &d;
            }
            if (rms >= 0.0f && (bestRms < 0.0f || rms < bestRms)) bestRms = rms;
            if (reason == VRHP_REASON_BYTES_UNKNOWN) sawUnknown = 1;
            else if (reason == VRHP_REASON_RMS || reason == VRHP_REASON_DEGENERATE) sawRms = 1;
        }
    }
    if (outRmsM) *outRmsM = bestRms;
    if (outReason) {
        if (sawUnknown)  *outReason = VRHP_REASON_BYTES_UNKNOWN;
        else if (sawRms) *outReason = VRHP_REASON_RMS;
    }
    return nullptr;
}

int VRGunPose_SurfaceCount()
{
    return (int)(sizeof(kGunSurfaces) / sizeof(kGunSurfaces[0]));
}

const CodvrGunSurface* VRGunPose_Surface(int i)
{
    if (i < 0 || i >= VRGunPose_SurfaceCount()) return nullptr;
    return &kGunSurfaces[i];
}

const CodvrGunModel* VRGunPose_SolveSurfaceEx(const CodvrGunSurface* s, const float* verts,
                                              int strideBytes, int count, float unitsPerMetre,
                                              VRHandPose* out, VRHandPoseCanRead canRead,
                                              void* readCtx, int* outReason)
{
    if (outReason) *outReason = VRHP_REASON_INDEX_RANGE;
    if (!s || s->n < VRGP_PARTKEY_MIN_POINTS
        || s->minSpreadM < VRGP_PARTKEY_MIN_SPREAD_M || !s->restStable)
        return nullptr;
    int reason = VRHP_REASON_INDEX_RANGE;
    if (!SolveRigid(s->lm, s->n, verts, strideBytes, count, unitsPerMetre, out,
                    0, canRead, readCtx, &reason)) {
        if (outReason) *outReason = reason;
        return nullptr;
    }
    if (outReason) *outReason = VRHP_REASON_OK;
    return VRGunPose_ByVertexCount(s->modelKey);
}

const CodvrGunSurface* VRGunPose_SurfaceOfModel(const CodvrGunModel* m, int vertexCount)
{
    if (!m) return nullptr;
    const int n = VRGunPose_SurfaceCount();
    for (int i = 0; i < n; ++i)
        if (kGunSurfaces[i].modelKey == m->vertexCount
            && kGunSurfaces[i].vertexCount == vertexCount)
            return &kGunSurfaces[i];
    return nullptr;
}

const CodvrGunSurface* VRGunPose_UniqueSurfaceOfModel(const CodvrGunModel* m, int vertexCount)
{
    if (!m) return nullptr;
    const CodvrGunSurface* found = nullptr;
    const int n = VRGunPose_SurfaceCount();
    for (int i = 0; i < n; ++i) {
        if (kGunSurfaces[i].modelKey != m->vertexCount
            || kGunSurfaces[i].vertexCount != vertexCount)
            continue;
        if (found) return nullptr;               // ambiguous within this weapon
        found = &kGunSurfaces[i];
    }
    return found;
}

int VRGunPose_AnyKeySurface(int vertexCount)
{
    const int n = VRGunPose_SurfaceCount();
    for (int i = 0; i < n; ++i)
        if (kGunSurfaces[i].keyUsable && kGunSurfaces[i].vertexCount == vertexCount)
            return 1;
    return 0;
}

const CodvrGunModel* VRGunPose_SolveAnySurfaceEx(int vertexCount, const float* verts,
                                                 int strideBytes, int count,
                                                 float unitsPerMetre, VRHandPose* out,
                                                 const CodvrGunModel* prefer,
                                                 const CodvrGunSurface** outSurf,
                                                 VRHandPoseCanRead canRead,
                                                 void* readCtx, int* outReason)
{
    if (outReason) *outReason = VRHP_REASON_INDEX_RANGE;
    if (outSurf) *outSurf = nullptr;
    int sawUnknown = 0, sawRms = 0;
    const int nS = VRGunPose_SurfaceCount();
    // Pass 0: the preferred model's surfaces; pass 1: everyone else's.
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < nS; ++i) {
            const CodvrGunSurface& s = kGunSurfaces[i];
            if (!s.keyUsable || s.vertexCount != vertexCount)
                continue;
            const int mine = prefer && (s.modelKey == prefer->vertexCount);
            if ((pass == 0) != (mine != 0))
                continue;
            int reason = VRHP_REASON_INDEX_RANGE;
            if (SolveRigid(s.lm, s.n, verts, strideBytes, count, unitsPerMetre, out,
                           0, canRead, readCtx, &reason)) {
                if (outReason) *outReason = VRHP_REASON_OK;
                if (outSurf) *outSurf = &s;
                // The model: the preferred one when it owns this surface
                // (kar98k / kar98kscoped share body key 536), else by key.
                if (mine) return prefer;
                return VRGunPose_ByVertexCount(s.modelKey);
            }
            if (reason == VRHP_REASON_BYTES_UNKNOWN) sawUnknown = 1;
            else if (reason == VRHP_REASON_RMS || reason == VRHP_REASON_DEGENERATE) sawRms = 1;
        }
    }
    if (outReason) {
        if (sawUnknown)   *outReason = VRHP_REASON_BYTES_UNKNOWN;
        else if (sawRms)  *outReason = VRHP_REASON_RMS;
    }
    return nullptr;
}

int VRGunPose_RearGripPivot(int vertexCount, const VRHandPose* body,
                            float unitsPerMetre, float outPivot[3])
{
    if (!body || !body->valid || !outPivot)
        return 0;
    if (!(unitsPerMetre > 0.0f))
        return 0;
    const CodvrGunModel* m = VRGunPose_ByVertexCount(vertexCount);
    // A rear grip below STRONG is not a pivot. Returning it anyway would put a
    // plausible number where a measured one belongs, and it would be believed.
    if (!m || !m->rearGripUsable)
        return 0;
    // rearGripM is stored in metres (body-bone frame); the body pose lives in
    // the vertex units it was solved in.
    const float gripU[3] = { m->rearGripM[0] * unitsPerMetre,
                             m->rearGripM[1] * unitsPerMetre,
                             m->rearGripM[2] * unitsPerMetre };
    float r[3];
    AxisApply(body->axis, gripU, r);
    for (int k = 0; k < 3; ++k)
        outPivot[k] = body->pos[k] + r[k];
    return 1;
}

void VRGunPose_Delta(const VRHandPose* body, const float pivot[3],
                     const float desiredAxis[9], const float desiredPivotPos[3],
                     float outM4[16])
{
    memset(outM4, 0, 16 * sizeof(float));
    outM4[0] = outM4[5] = outM4[10] = outM4[15] = 1.0f;
    if (!body || !body->valid || !pivot || !desiredAxis || !desiredPivotPos)
        return;

    // Same construction as the hand's, with ONE difference that is the whole
    // point: the pivot is the REAR GRIP, not the body origin. The body bone's
    // origin is wherever the artist rooted the mesh -- often the receiver's
    // centre, sometimes the muzzle end -- and rotating about it would swing
    // the stock through the player's shoulder.
    float R[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k)
                s += desiredAxis[k * 3 + r] * body->axis[k * 3 + c];
            R[c * 3 + r] = s;
        }

    float rp[3];
    AxisApply(R, pivot, rp);

    outM4[0]  = R[0]; outM4[1]  = R[1]; outM4[2]  = R[2];
    outM4[4]  = R[3]; outM4[5]  = R[4]; outM4[6]  = R[5];
    outM4[8]  = R[6]; outM4[9]  = R[7]; outM4[10] = R[8];
    outM4[12] = desiredPivotPos[0] - rp[0];
    outM4[13] = desiredPivotPos[1] - rp[1];
    outM4[14] = desiredPivotPos[2] - rp[2];
    outM4[15] = 1.0f;
}

// ---------------------------------------------------------------------------

void VRHandPose_Delta(const VRHandPose* authored, const float desiredPos[3],
                      const float desiredAxis[9], float outM4[16])
{
    // Identity until proven otherwise: a delta this function declines to build
    // must leave the hand exactly where the engine drew it.
    memset(outM4, 0, 16 * sizeof(float));
    outM4[0] = outM4[5] = outM4[10] = outM4[15] = 1.0f;
    if (!authored || !authored->valid || !desiredPos || !desiredAxis)
        return;

    // R = desired * authored^-1, with authored^-1 = transpose (both are
    // orthonormal). In row-as-axis form, applying `authored` then its inverse
    // is the identity, so:
    //     R[r][c] = sum_k desiredAxis[k][r] * authoredAxis[k][c]
    // which is what falls out of composing the two row-major bases.
    float R[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k)
                s += desiredAxis[k * 3 + r] * authored->axis[k * 3 + c];
            R[c * 3 + r] = s;      // stored rows-as-axes, same as the inputs
        }

    // THE PIVOT IS THE AUTHORED PALM ORIGIN.
    //
    // t = desiredPos - R * authoredPos. Without this term the rotation happens
    // about the vertex-space origin, which for a camera-relative draw sits on
    // the eye -- so the hand swings around the viewer's head instead of
    // turning about its own wrist. That is exactly the failure the first
    // gun_yaw probe produced, and the reason a pivot is never assumed here.
    float rp[3];
    AxisApply(R, authored->pos, rp);

    // Column-major GL 4x4 from rows-as-axes: column j is the image of basis
    // vector j, which for this convention is row j.
    outM4[0]  = R[0]; outM4[1]  = R[1]; outM4[2]  = R[2];  outM4[3]  = 0.0f;
    outM4[4]  = R[3]; outM4[5]  = R[4]; outM4[6]  = R[5];  outM4[7]  = 0.0f;
    outM4[8]  = R[6]; outM4[9]  = R[7]; outM4[10] = R[8];  outM4[11] = 0.0f;
    outM4[12] = desiredPos[0] - rp[0];
    outM4[13] = desiredPos[1] - rp[1];
    outM4[14] = desiredPos[2] - rp[2];
    outM4[15] = 1.0f;
}

void VRHandPose_DesiredFromGrip(const float gripPos[3], const float gripAxis[9],
                                const float palmOffsetM[3], const float palmRotDeg[3],
                                float outPos[3], float outAxis[9])
{
    VRHandPose_DesiredFromGripCal(gripPos, gripAxis, 0, palmOffsetM, palmRotDeg,
                                  outPos, outAxis);
}

void VRHandPose_DesiredFromGripCal(const float gripPos[3], const float gripAxis[9],
                                   const float calRows[9],
                                   const float palmOffsetM[3], const float palmRotDeg[3],
                                   float outPos[3], float outAxis[9])
{
    // The calibrated base: bone axis r = sum_k calRows[r][k] * gripRow_k.
    float baseAxis[9];
    if (calRows) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                float acc = 0.0f;
                for (int k = 0; k < 3; ++k)
                    acc += calRows[r * 3 + k] * gripAxis[k * 3 + c];
                baseAxis[r * 3 + c] = acc;
            }
    } else {
        memcpy(baseAxis, gripAxis, sizeof(baseAxis));
    }
    // Position: the grip pose carries the calibrated controller->palm offset,
    // expressed in the grip's own frame.
    //
    // The offset is METRES (its name says so) and gripPos is in the vertex
    // space's units -- game units for every production caller. Adding them
    // raw applied the calibration at 1/39.37 of its intended size: a 2 cm
    // adjustment would have moved the palm half a millimetre. Latent today
    // because production ships zero calibration, which is exactly the kind of
    // bug that waits for the first person to set a value and then reads as
    // "calibration does nothing".
    float off[3] = { 0, 0, 0 };
    if (palmOffsetM) {
        const float offU[3] = { palmOffsetM[0] * CODVR_UNITS_PER_M,
                                palmOffsetM[1] * CODVR_UNITS_PER_M,
                                palmOffsetM[2] * CODVR_UNITS_PER_M };
        AxisApply(gripAxis, offU, off);
    }
    for (int k = 0; k < 3; ++k)
        outPos[k] = gripPos[k] + off[k];

    // Orientation: the calibrated base, then the operator's trim in Quake
    // order (pitch, yaw, roll) applied about the base's own axes.
    if (!palmRotDeg
        || (palmRotDeg[0] == 0.0f && palmRotDeg[1] == 0.0f && palmRotDeg[2] == 0.0f)) {
        memcpy(outAxis, baseAxis, 9 * sizeof(float));
        return;
    }
    const float* gripAxisTrim = baseAxis;
    const float d2r = 0.01745329252f;
    const float sp = sinf(palmRotDeg[0] * d2r), cp = cosf(palmRotDeg[0] * d2r);
    const float sy = sinf(palmRotDeg[1] * d2r), cy = cosf(palmRotDeg[1] * d2r);
    const float sr = sinf(palmRotDeg[2] * d2r), cr = cosf(palmRotDeg[2] * d2r);
    // Quake's yaw-pitch-roll, rows as axes.
    const float m[9] = {
        cp * cy,                      cp * sy,                     -sp,
        sr * sp * cy - cr * sy,       sr * sp * sy + cr * cy,       sr * cp,
        cr * sp * cy + sr * sy,       cr * sp * sy - sr * cy,       cr * cp,
    };
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k)
                s += m[r * 3 + k] * gripAxisTrim[k * 3 + c];
            outAxis[r * 3 + c] = s;
        }
}
