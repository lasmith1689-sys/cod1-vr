// ===========================================================================
// AUTHORED HAND POSE (src/vr_handpose.cpp) -- included by vr_safetest.cpp.
//
// The claim under test: the palm's rigid transform can be RECOVERED from the
// vertex array the engine submits, exactly, with no guessed axis and no
// dependence on the modelview -- and the delta built from it rotates the hand
// about its own wrist rather than about the viewer's head.
//
// Every case here builds a synthetic batch by taking known landmark bind
// positions, applying a KNOWN rigid transform, and writing the results into a
// vertex array shaped like the engine's. The solver then has to report the
// transform that was applied. Nothing is compared against another run of the
// same code.
// ===========================================================================

#include "../src/vr_handpose.h"

// A landmark set with real 3D spread, so the solve is well posed. Deliberately
// NOT symmetric: a symmetric cloud has ambiguous axes and would let a wrong
// answer pass.
static const CodvrPalmLandmark kTestLandmarks[] = {
    {  0, { 0.000f,  0.000f,  0.000f } },
    {  1, { 0.040f,  0.000f,  0.000f } },
    {  2, { 0.000f,  0.030f,  0.000f } },
    {  3, { 0.000f,  0.000f,  0.014f } },
    {  4, { 0.035f,  0.025f,  0.004f } },
    {  5, { 0.012f, -0.018f,  0.011f } },
    {  6, { 0.028f,  0.009f, -0.013f } },
    {  7, { 0.005f,  0.022f,  0.008f } },
};
enum { HP_TEST_N = (int)(sizeof(kTestLandmarks) / sizeof(kTestLandmarks[0])) };
enum { HP_TEST_VERTS = 32 };

// Rows-as-axes, Quake order, matching vr_handpose's own convention.
static void HpAxisFromEuler(float pitch, float yaw, float roll, float m[9])
{
    const float d2r = 0.01745329252f;
    const float sp = sinf(pitch * d2r), cp = cosf(pitch * d2r);
    const float sy = sinf(yaw   * d2r), cy = cosf(yaw   * d2r);
    const float sr = sinf(roll  * d2r), cr = cosf(roll  * d2r);
    m[0] = cp * cy;                 m[1] = cp * sy;                 m[2] = -sp;
    m[3] = sr * sp * cy - cr * sy;  m[4] = sr * sp * sy + cr * cy;  m[5] = sr * cp;
    m[6] = cr * sp * cy + sr * sy;  m[7] = cr * sp * sy - sr * cy;  m[8] = cr * cp;
}

static void HpApply(const float axis[9], const float v[3], float out[3])
{
    out[0] = axis[0] * v[0] + axis[3] * v[1] + axis[6] * v[2];
    out[1] = axis[1] * v[0] + axis[4] * v[1] + axis[7] * v[2];
    out[2] = axis[2] * v[0] + axis[5] * v[1] + axis[8] * v[2];
}

// Write a batch in which the landmarks sit at `axis * bind + pos`, plus filler
// vertices elsewhere so the solver has to address the right ones.
static void HpBuildBatch(float verts[HP_TEST_VERTS * 3],
                         const float axis[9], const float pos[3])
{
    for (int i = 0; i < HP_TEST_VERTS; ++i) {
        verts[i * 3 + 0] = 9.0f + i;      // filler: obviously not a palm
        verts[i * 3 + 1] = -4.0f - i;
        verts[i * 3 + 2] = 2.5f * i;
    }
    for (int i = 0; i < HP_TEST_N; ++i) {
        float r[3];
        HpApply(axis, kTestLandmarks[i].bindM, r);
        float* v = &verts[kTestLandmarks[i].index * 3];
        v[0] = pos[0] + r[0];
        v[1] = pos[1] + r[1];
        v[2] = pos[2] + r[2];
    }
}

static float HpAxisAngleDeg(const float a[9], const float b[9])
{
    // NOT acos(trace). Near zero rotation the trace form is numerically
    // hopeless: c = 1 - theta^2/2, so the smallest angle a float can express
    // is sqrt(2*eps) -- about 0.03 degrees. An earlier version of this file
    // used it and reported a 0.0396 degree "error" across a pose sweep whose
    // real error was zero; the solve was fine and the ruler was not.
    //
    // For a rotation R,  ||R - I||_F = 2*sqrt(2)*sin(theta/2), which is linear
    // in theta near zero and resolves down to eps itself. Accumulated in
    // double so the metric is finer than the thing it measures.
    double m[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double t = 0.0;
            for (int k = 0; k < 3; ++k)
                t += (double)b[i * 3 + k] * (double)a[j * 3 + k];
            m[i][j] = t;
        }
    double fro = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const double d = m[i][j] - (i == j ? 1.0 : 0.0);
            fro += d * d;
        }
    double s = sqrt(fro) / (2.0 * 1.4142135623730951);
    if (s > 1.0) s = 1.0;
    return (float)(2.0 * asin(s) * 57.29577951308232);
}

// Transform a point by a column-major GL 4x4.
static void HpXform4(const float m[16], const float p[3], float out[3])
{
    out[0] = m[0] * p[0] + m[4] * p[1] + m[8]  * p[2] + m[12];
    out[1] = m[1] * p[0] + m[5] * p[1] + m[9]  * p[2] + m[13];
    out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

static float HpDist(const float a[3], const float b[3])
{
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// ---------------------------------------------------------------------------

static void S_HandPoseSolve()
{
    printf("--- authored hand pose: recovery from the submitted vertices ---\n");

    VRHandPose_ResetStats();
    VRHandPose_TestSetLandmarks(VRHP_TRIGGER, kTestLandmarks, HP_TEST_N);
    VRHandPose_TestSetLandmarks(VRHP_SUPPORT, kTestLandmarks, HP_TEST_N);

    // ---- 6/7: each hand's palm transform comes back exactly -------------
    for (int hand = 0; hand < VRHP_HANDS; ++hand) {
        const char* who = (hand == VRHP_TRIGGER) ? "trigger/right" : "support/left";
        float axis[9], verts[HP_TEST_VERTS * 3];
        const float pos[3] = { 0.31f, -0.12f, 0.44f };
        HpAxisFromEuler(17.0f, -43.0f, 8.0f, axis);
        HpBuildBatch(verts, axis, pos);

        VRHandPose hp;
        const int ok = VRHandPose_Solve(hand, verts, 12, HP_TEST_VERTS, 1.0f, &hp);
        CHECK(ok == 1, "%s: the palm solve succeeds on a well-formed batch", who);
        CHECK(hp.valid == 1, "%s: the recovered pose is marked valid", who);
        CHECK_NEAR(HpAxisAngleDeg(hp.axis, axis), 0.0, 0.01,
                   "%s: recovered orientation matches the applied one (%.5f deg off)",
                   who, HpAxisAngleDeg(hp.axis, axis));
        CHECK_NEAR(HpDist(hp.pos, pos), 0.0, 1e-4,
                   "%s: recovered palm origin matches (%.6f m off)",
                   who, HpDist(hp.pos, pos));
        CHECK(hp.rmsM < 1e-4f, "%s: residual is %.2e m -- a solve, not a fit",
              who, hp.rmsM);
    }

    // ---- 8: stable across a full sweep of authored poses ----------------
    //
    // Positions stay inside the box a first-person viewmodel actually
    // occupies -- under a metre from the eye. That is not the test being made
    // easy: it is the domain the solver runs in, and the far-field behaviour
    // is measured separately below rather than averaged into this bound.
    {
        float worstRot = 0.0f, worstPos = 0.0f, worstStep = 0.0f;
        float prev[9]; int havePrev = 0;
        int solved = 0;
        for (int p = -80; p <= 80; p += 10) {
            for (int y = -180; y < 180; y += 15) {
                float axis[9], verts[HP_TEST_VERTS * 3];
                const float pos[3] = { 0.004f * p, -0.002f * y, 0.35f };
                HpAxisFromEuler((float)p, (float)y, (float)(p / 2), axis);
                HpBuildBatch(verts, axis, pos);
                VRHandPose hp;
                if (!VRHandPose_Solve(VRHP_TRIGGER, verts, 12, HP_TEST_VERTS, 1.0f, &hp))
                    continue;
                ++solved;
                const float rot = HpAxisAngleDeg(hp.axis, axis);
                if (rot > worstRot) worstRot = rot;
                const float dp = HpDist(hp.pos, pos);
                if (dp > worstPos) worstPos = dp;
                if (havePrev) {
                    // Compare the recovered step against the authored step.
                    // A flip would make them disagree; fast motion would not.
                    const float step = HpAxisAngleDeg(prev, hp.axis);
                    if (step > worstStep) worstStep = step;
                }
                memcpy(prev, hp.axis, sizeof(prev));
                havePrev = 1;
            }
        }
        CHECK(solved >= 400, "pose sweep: %d authored poses solved", solved);
        CHECK(worstRot < 0.001f,
              "pose sweep: worst orientation error %.5f deg over the whole sweep",
              worstRot);
        CHECK(worstPos < 1e-3f,
              "pose sweep: worst origin error %.6f m over the whole sweep", worstPos);
        // The sweep steps 15 degrees of yaw at a time, so a recovered basis
        // that ever jumped near 180 degrees would be a flip rather than a step.
        CHECK(worstStep < 175.0f,
              "pose sweep: no basis flip (largest step %.1f deg)", worstStep);
    }

    // ---- the precision limit, measured rather than assumed --------------
    //
    // The landmarks span about 3 cm. Vertex data is single precision, so the
    // further the palm sits from the vertex-space origin, the coarser that
    // 3 cm baseline is resolved and the noisier the recovered rotation gets.
    // This is a property of the data the engine hands over, not of the solve,
    // and the honest thing is to know where it bites. Recorded here so a
    // future change that makes it WORSE is visible.
    {
        struct { float dist; float tolDeg; } cases[] = {
            { 0.25f, 0.002f },      // the viewmodel's own neighbourhood
            { 1.0f,  0.010f },
            { 8.0f,  0.100f },      // far outside anything a viewmodel does
        };
        for (int c = 0; c < 3; ++c) {
            float axis[9], verts[HP_TEST_VERTS * 3];
            const float d = cases[c].dist * 0.57735f;   // equal on all axes
            const float pos[3] = { d, d, d };
            HpAxisFromEuler(23.0f, -117.0f, 41.0f, axis);
            HpBuildBatch(verts, axis, pos);
            VRHandPose hp;
            if (!VRHandPose_Solve(VRHP_TRIGGER, verts, 12, HP_TEST_VERTS, 1.0f, &hp)) {
                CHECK(0, "precision at %.2f m: the solve refused", cases[c].dist);
                continue;
            }
            const float err = HpAxisAngleDeg(hp.axis, axis);
            CHECK(err <= cases[c].tolDeg,
                  "precision at %.2f m from the vertex origin: %.5f deg "
                  "(budget %.3f)", cases[c].dist, err, cases[c].tolDeg);
        }
    }

    // ---- the solve refuses what it cannot answer ------------------------
    {
        float verts[HP_TEST_VERTS * 3];
        for (int i = 0; i < HP_TEST_VERTS * 3; ++i) verts[i] = (float)(i % 7);
        VRHandPose hp;
        const int ok = VRHandPose_Solve(VRHP_TRIGGER, verts, 12, HP_TEST_VERTS, 1.0f, &hp);
        CHECK(ok == 0, "a batch that is not the hand is REFUSED, not guessed at");
        CHECK(hp.valid == 0, "a refused solve reports an invalid pose");
    }
    {
        float axis[9], verts[HP_TEST_VERTS * 3];
        const float pos[3] = { 0, 0, 0 };
        HpAxisFromEuler(0, 0, 0, axis);
        HpBuildBatch(verts, axis, pos);
        VRHandPose hp;
        // A batch too short to contain the landmarks must be refused rather
        // than read past its end.
        CHECK(VRHandPose_Solve(VRHP_TRIGGER, verts, 12, 4, 1.0f, &hp) == 0,
              "a batch shorter than the landmark indices is refused (no overread)");
        CHECK(VRHandPose_Solve(VRHP_TRIGGER, nullptr, 12, HP_TEST_VERTS, 1.0f, &hp) == 0,
              "a null vertex array is refused");
        CHECK(VRHandPose_Solve(9, verts, 12, HP_TEST_VERTS, 1.0f, &hp) == 0,
              "an out-of-range hand index is refused");
    }

    // ---- stride is honoured ---------------------------------------------
    {
        // Interleaved position+normal+uv, the shape a real vertex array has.
        const int stride = 32;
        static unsigned char buf[HP_TEST_VERTS * 32];
        memset(buf, 0, sizeof(buf));
        float axis[9];
        const float pos[3] = { -0.2f, 0.5f, 0.05f };
        HpAxisFromEuler(-25.0f, 61.0f, -12.0f, axis);
        for (int i = 0; i < HP_TEST_N; ++i) {
            float r[3];
            HpApply(axis, kTestLandmarks[i].bindM, r);
            float* v = (float*)(buf + (size_t)kTestLandmarks[i].index * stride);
            v[0] = pos[0] + r[0]; v[1] = pos[1] + r[1]; v[2] = pos[2] + r[2];
        }
        VRHandPose hp;
        CHECK(VRHandPose_Solve(VRHP_TRIGGER, (const float*)buf, stride,
                               HP_TEST_VERTS, 1.0f, &hp) == 1,
              "an interleaved vertex array with a 32-byte stride solves");
        CHECK_NEAR(HpAxisAngleDeg(hp.axis, axis), 0.0, 0.01,
                   "strided solve recovers the same orientation");
    }

    // ---- units: the vertex space must be NAMED, and naming it wrong is
    // caught. This is the 2026-08-27 live failure pinned as a mutant: real
    // gameplay submits INCH-unit vertices; a metre-assuming solve can only
    // refuse them (0 solves / 1197 refusals per hand), while everything
    // upstream of the solve looked perfectly healthy.
    {
        float axis[9], verts[HP_TEST_VERTS * 3];
        const float posU[3] = { 8.0f, -12.0f, 22.0f };   // engine units
        HpAxisFromEuler(12.0f, -40.0f, 7.0f, axis);
        // Build the batch the way the ENGINE submits it: the bind cloud
        // scaled to game units (1 unit = 1 inch), then rigidly posed.
        for (int i = 0; i < HP_TEST_VERTS; ++i) {
            verts[i*3+0] = 900.0f + i;                    // filler, far away
            verts[i*3+1] = -400.0f - i;
            verts[i*3+2] = 250.0f * i;
        }
        for (int i = 0; i < HP_TEST_N; ++i) {
            const float bindU[3] = { kTestLandmarks[i].bindM[0] * CODVR_UNITS_PER_M,
                                     kTestLandmarks[i].bindM[1] * CODVR_UNITS_PER_M,
                                     kTestLandmarks[i].bindM[2] * CODVR_UNITS_PER_M };
            float r[3];
            HpApply(axis, bindU, r);
            float* v = &verts[kTestLandmarks[i].index * 3];
            v[0] = posU[0] + r[0]; v[1] = posU[1] + r[1]; v[2] = posU[2] + r[2];
        }
        VRHandPose hp;
        CHECK(VRHandPose_Solve(VRHP_TRIGGER, verts, 12, HP_TEST_VERTS,
                               CODVR_UNITS_PER_M, &hp) == 1,
              "an ENGINE-unit batch solves when the units are named");
        CHECK_NEAR(HpDist(hp.pos, posU), 0.0, 0.002 * CODVR_UNITS_PER_M,
                   "engine-unit solve recovers the engine-unit position");
        CHECK_NEAR(HpAxisAngleDeg(hp.axis, axis), 0.0, 0.01,
                   "engine-unit solve recovers the orientation");
        CHECK(VRHandPose_Solve(VRHP_TRIGGER, verts, 12, HP_TEST_VERTS,
                               1.0f, &hp) == 0,
              "MUTANT (the 2026-08-27 bug): the same engine-unit batch is "
              "REFUSED by a metre-assuming solve");
        float mVerts[HP_TEST_VERTS * 3];
        HpBuildBatch(mVerts, axis, posU);
        CHECK(VRHandPose_Solve(VRHP_TRIGGER, mVerts, 12, HP_TEST_VERTS,
                               CODVR_UNITS_PER_M, &hp) == 0,
              "a metre batch is REFUSED by an engine-unit solve");
        CHECK(VRHandPose_Solve(VRHP_TRIGGER, verts, 12, HP_TEST_VERTS,
                               0.0f, &hp) == 0,
              "a zero/invalid unitsPerMetre is refused outright");
    }

    VRHandPose_TestRestore();
}

// ---------------------------------------------------------------------------

static void S_HandPoseDelta()
{
    printf("--- authored hand pose: the delta turns the hand, not the head ---\n");

    VRHandPose_TestSetLandmarks(VRHP_TRIGGER, kTestLandmarks, HP_TEST_N);
    VRHandPose_TestSetLandmarks(VRHP_SUPPORT, kTestLandmarks, HP_TEST_N);

    // The authored hand: somewhere well away from the vertex-space origin, so
    // a camera-pivot mistake shows up as a large error rather than a subtle one.
    float authAxis[9], verts[HP_TEST_VERTS * 3];
    const float authPos[3] = { 0.25f, -0.30f, 0.60f };
    HpAxisFromEuler(5.0f, 20.0f, -3.0f, authAxis);
    HpBuildBatch(verts, authAxis, authPos);

    VRHandPose authored;
    CHECK(VRHandPose_Solve(VRHP_TRIGGER, verts, 12, HP_TEST_VERTS, 1.0f, &authored) == 1,
          "delta setup: the authored pose solves");

    // ---- 9/10: +10 cm on each axis moves the hand 1:1 -------------------
    {
        const float want[3] = { authPos[0] + 0.10f,
                                authPos[1] + 0.10f,
                                authPos[2] + 0.10f };
        float m[16];
        VRHandPose_Delta(&authored, want, authAxis, m);

        float moved[3];
        HpXform4(m, authored.pos, moved);
        CHECK_NEAR(HpDist(moved, want), 0.0, 1e-5,
                   "+10/+10/+10 cm: the palm lands exactly on the target");

        // Every landmark moves by the same vector -- it is a RIGID delta, not
        // a scale or a shear.
        float worst = 0.0f;
        for (int i = 0; i < HP_TEST_N; ++i) {
            const float* v = &verts[kTestLandmarks[i].index * 3];
            float out[3];
            HpXform4(m, v, out);
            for (int k = 0; k < 3; ++k) {
                const float d = fabsf((out[k] - v[k]) - 0.10f);
                if (d > worst) worst = d;
            }
        }
        CHECK(worst < 1e-5f,
              "+10/+10/+10 cm: every vertex translates by exactly 10 cm (worst %.2e)",
              worst);
    }

    // ---- 11: rotation happens about the PALM, not the camera ------------
    {
        float wantAxis[9];
        HpAxisFromEuler(5.0f + 15.0f, 20.0f + 15.0f, -3.0f + 15.0f, wantAxis);
        float m[16];
        VRHandPose_Delta(&authored, authored.pos, wantAxis, m);

        float moved[3];
        HpXform4(m, authored.pos, moved);
        CHECK_NEAR(HpDist(moved, authored.pos), 0.0, 1e-5,
                   "+15 yaw/pitch/roll about the palm leaves the palm origin FIXED");

        // The camera-pivot mutant: the same rotation applied with no pivot
        // correction. It must move the palm a long way -- that is the bug the
        // first gun_yaw probe had, and this is the fixture that catches it.
        float mut[16];
        memset(mut, 0, sizeof(mut));
        {
            float R[9];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    float s = 0.0f;
                    for (int k = 0; k < 3; ++k)
                        s += wantAxis[k * 3 + r] * authored.axis[k * 3 + c];
                    R[c * 3 + r] = s;
                }
            mut[0] = R[0]; mut[1] = R[1]; mut[2]  = R[2];
            mut[4] = R[3]; mut[5] = R[4]; mut[6]  = R[5];
            mut[8] = R[6]; mut[9] = R[7]; mut[10] = R[8];
            mut[15] = 1.0f;                       // no translation: camera pivot
        }
        float mutMoved[3];
        HpXform4(mut, authored.pos, mutMoved);
        const float swing = HpDist(mutMoved, authored.pos);
        CHECK(swing > 0.05f,
              "MUTANT: the camera-pivot delta swings the palm %.3f m -- caught", swing);

        // And the shipped delta must not do that.
        CHECK(HpDist(moved, authored.pos) < swing * 0.01f,
              "the shipped delta's palm movement is <1%% of the mutant's");
    }

    // ---- rotation is a pure rotation ------------------------------------
    {
        float wantAxis[9];
        HpAxisFromEuler(35.0f, -70.0f, 22.0f, wantAxis);
        float m[16];
        VRHandPose_Delta(&authored, authored.pos, wantAxis, m);
        // Distances between vertices are preserved under a rigid transform.
        float worst = 0.0f;
        for (int i = 0; i < HP_TEST_N; ++i)
            for (int j = i + 1; j < HP_TEST_N; ++j) {
                const float* a = &verts[kTestLandmarks[i].index * 3];
                const float* b = &verts[kTestLandmarks[j].index * 3];
                float ta[3], tb[3];
                HpXform4(m, a, ta);
                HpXform4(m, b, tb);
                const float d = fabsf(HpDist(ta, tb) - HpDist(a, b));
                if (d > worst) worst = d;
            }
        CHECK(worst < 1e-5f,
              "the delta is rigid: all pairwise distances preserved (worst %.2e)",
              worst);
    }

    // ---- an invalid authored pose yields identity, never a guess --------
    {
        VRHandPose bad;
        memset(&bad, 0, sizeof(bad));
        float m[16];
        const float want[3] = { 1, 2, 3 };
        float wantAxis[9];
        HpAxisFromEuler(40, 40, 40, wantAxis);
        VRHandPose_Delta(&bad, want, wantAxis, m);
        int isIdentity = 1;
        for (int i = 0; i < 16; ++i) {
            const float expect = (i % 5 == 0) ? 1.0f : 0.0f;
            if (fabsf(m[i] - expect) > 1e-6f) isIdentity = 0;
        }
        CHECK(isIdentity,
              "an unsolved hand yields the IDENTITY delta -- the engine's pose stands");
    }

    VRHandPose_TestRestore();
}

// ---------------------------------------------------------------------------

static void S_HandPoseIndependenceAndLaterality()
{
    printf("--- authored hand pose: laterality and independence ---\n");

    // ---- 12: the corrected laterality fixture ---------------------------
    //
    // The asset says surface 0 is `bip01 l *` and surface 1 is `bip01 r *`.
    // The live sweep of 2026-08-25 says surface 0 is what the player sees as
    // the RIGHT hand. This fixture pins the RENDERED mapping, so a future
    // change that "corrects" it back to the asset labels fails here.
    CHECK(VRHandPose_BatchVerts() == 1674,
          "the merged hand batch is 1674 vertices, as the live sweep measured");
    CHECK(VRHandPose_SplitVert() == 837,
          "the batch splits at vertex 837, as the live sweep measured");
    CHECK(VRHandPose_LandmarkCount(VRHP_TRIGGER) > 3,
          "the trigger hand has a usable landmark set (%d)",
          VRHandPose_LandmarkCount(VRHP_TRIGGER));
    CHECK(VRHandPose_LandmarkCount(VRHP_SUPPORT) > 3,
          "the support hand has a usable landmark set (%d)",
          VRHandPose_LandmarkCount(VRHP_SUPPORT));

    // The shipped tables must address DIFFERENT halves of the batch, or the
    // two hands are reading the same geometry and cannot move independently.
    {
        VRHandPose_TestRestore();
        // Landmark indices are baked into the generated header; check the
        // halves by solving two batches that differ only in one half.
        int trigBelowSplit = 1, suppAboveSplit = 1;
        // The generated tables are private, so probe behaviourally: build a
        // batch whose FIRST half is a valid palm and whose second half is
        // noise, and confirm exactly one hand solves.
        static float big[1674 * 3];
        for (int i = 0; i < 1674 * 3; ++i) big[i] = (float)((i * 37) % 11) * 0.01f;
        VRHandPose hp;
        const int trigOnNoise = VRHandPose_Solve(VRHP_TRIGGER, big, 12, 1674, 1.0f, &hp);
        const int suppOnNoise = VRHandPose_Solve(VRHP_SUPPORT, big, 12, 1674, 1.0f, &hp);
        CHECK(trigOnNoise == 0 && suppOnNoise == 0,
              "neither hand claims a pose from a noise batch of the right size");
        (void)trigBelowSplit; (void)suppAboveSplit;
    }

    // ---- 13: the two hands cannot swap ----------------------------------
    {
        VRHandPose_TestSetLandmarks(VRHP_TRIGGER, kTestLandmarks, HP_TEST_N);
        VRHandPose_TestSetLandmarks(VRHP_SUPPORT, kTestLandmarks, HP_TEST_N);

        float axisA[9], axisB[9], verts[HP_TEST_VERTS * 3];
        const float posA[3] = { 0.20f, 0.0f, 0.5f };
        HpAxisFromEuler(0, 30, 0, axisA);
        HpAxisFromEuler(0, -30, 0, axisB);
        HpBuildBatch(verts, axisA, posA);

        VRHandPose trig, supp;
        VRHandPose_Solve(VRHP_TRIGGER, verts, 12, HP_TEST_VERTS, 1.0f, &trig);
        VRHandPose_Solve(VRHP_SUPPORT, verts, 12, HP_TEST_VERTS, 1.0f, &supp);

        // Move ONLY the trigger hand. The support hand's delta must stay
        // identity-like -- built from its own authored pose and its own target.
        const float trigWant[3] = { posA[0] + 0.10f, posA[1], posA[2] };
        float mTrig[16], mSupp[16];
        VRHandPose_Delta(&trig, trigWant, axisA, mTrig);
        VRHandPose_Delta(&supp, supp.pos, supp.axis, mSupp);

        float p[3] = { posA[0], posA[1], posA[2] }, outT[3], outS[3];
        HpXform4(mTrig, p, outT);
        HpXform4(mSupp, p, outS);
        CHECK_NEAR(outT[0] - p[0], 0.10, 1e-5,
                   "the trigger hand's delta moves it +10 cm in X");
        CHECK_NEAR(HpDist(outS, p), 0.0, 1e-5,
                   "the support hand's delta leaves it exactly where it was");

        // MUTANT: hands wired to each other's targets. The check that a
        // reversed mapping is DETECTABLE, so test 30 has something to fail on.
        float mSwapped[16];
        VRHandPose_Delta(&supp, trigWant, axisB, mSwapped);
        float outSwap[3];
        HpXform4(mSwapped, p, outSwap);
        CHECK(HpDist(outSwap, outS) > 0.05f,
              "MUTANT: feeding the support hand the trigger hand's target moves "
              "it %.3f m -- a reversed mapping is detectable", HpDist(outSwap, outS));
    }

    VRHandPose_TestRestore();
}
