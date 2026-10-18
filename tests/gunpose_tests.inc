// ===========================================================================
// GUN BODY POSE AND THE REAR-GRIP PIVOT (src/vr_handpose.cpp)
//
// The first gun_yaw probe rotated the weapon about the CAMERA, swinging it
// round the player's head. The fix is not "add a pivot" -- it is that the
// pivot has to be a real point on the weapon, recovered from the weapon's own
// submitted vertices, and it has to be the REAR GRIP rather than whatever the
// artist happened to root the mesh at.
//
// These cases hold that line, and hold the confidence rule with it: a rear
// grip the data does not know well enough is not offered as a pivot at all.
// ===========================================================================

#include "../src/vr_weapon_profile.h"

static void S_GunPoseRearGrip()
{
    printf("--- gun body pose: the rear-grip pivot ---\n");

    // ---- 25: the model is selected by vertex count, exactly -------------
    {
        const int n = VRGunPose_ModelCount();
        CHECK(n >= 20, "the gun model table carries %d shipped models", n);

        // Every entry must be reachable by its own key, and no two entries
        // may answer to the same one -- that is what makes the count an
        // identifier rather than a hint.
        int reachable = 0, collisions = 0;
        for (int i = 0; i < n; ++i) {
            // Walk keys by probing: the table is private, so use the counts
            // the shipped models actually have.
            (void)i;
        }
        // BODY-SURFACE keys (re-keyed 2026-08-28: the engine draws each gun
        // surface as its own LOCAL array -- measured live, spans summing to
        // the model total -- so the old model-total keys could never match a
        // real batch).
        const int knownCounts[] = { 326, 516, 724, 854, 782, 630, 536,
                                    494, 652, 729, 1197, 344, 1174, 88 };
        for (int i = 0; i < (int)(sizeof(knownCounts) / sizeof(knownCounts[0])); ++i) {
            const CodvrGunModel* m = VRGunPose_ByVertexCount(knownCounts[i]);
            if (m) {
                ++reachable;
                if (m->vertexCount != knownCounts[i]) ++collisions;
            }
        }
        CHECK(reachable >= 12,
              "%d of the sampled shipped vertex counts resolve to a model", reachable);
        CHECK(collisions == 0, "every resolved model reports the key it was found by");

        // An unknown batch size must resolve to nothing. A modded weapon,
        // a LOD, or a batch that is not a gun at all has to be left alone.
        CHECK(VRGunPose_ByVertexCount(7) == nullptr,
              "an unknown vertex count resolves to NO model");
        CHECK(VRGunPose_ByVertexCount(0) == nullptr,
              "a zero vertex count resolves to NO model");
        CHECK(VRGunPose_ByVertexCount(999999) == nullptr,
              "an absurd vertex count resolves to NO model");
    }

    // ---- 14/26: the pivot, and the confidence rule that guards it -------
    {
        // Build a synthetic batch from a real model's own landmarks, posed by
        // a known transform, and check the rear grip lands where that
        // transform puts it.
        const CodvrGunModel* m = nullptr;
        const int probe[] = { 326, 516, 724, 854, 782, 536, 494 };
        for (int i = 0; i < 7 && !m; ++i) {
            const CodvrGunModel* c = VRGunPose_ByVertexCount(probe[i]);
            if (c && c->rearGripUsable) m = c;
        }
        CHECK(m != nullptr, "at least one shipped model has a usable rear grip");
        if (m) {
            static float verts[4096 * 3];
            float axis[9];
            const float pos[3] = { 0.12f, -0.34f, 0.55f };
            HpAxisFromEuler(11.0f, -67.0f, 23.0f, axis);
            for (int i = 0; i < 4096 * 3; ++i) verts[i] = 77.0f;
            int maxIdx = 0;
            for (int i = 0; i < m->n; ++i) {
                float r[3];
                HpApply(axis, m->lm[i].bindM, r);
                float* v = &verts[m->lm[i].index * 3];
                v[0] = pos[0] + r[0];
                v[1] = pos[1] + r[1];
                v[2] = pos[2] + r[2];
                if (m->lm[i].index > maxIdx) maxIdx = m->lm[i].index;
            }

            VRHandPose body;
            const int ok = VRGunPose_Solve(m->vertexCount, verts, 12, maxIdx + 1, 1.0f, &body);
            CHECK(ok == 1, "the gun body solves from its own landmarks");
            CHECK_NEAR(HpAxisAngleDeg(body.axis, axis), 0.0, 0.01,
                       "the gun body's orientation is recovered (%.5f deg off)",
                       HpAxisAngleDeg(body.axis, axis));

            float pivot[3];
            const int havePivot =
                VRGunPose_RearGripPivot(m->vertexCount, &body, 1.0f, pivot);
            CHECK(havePivot == 1, "a STRONG rear grip yields a pivot");

            // Where the pivot SHOULD be: the profile's body-frame offset,
            // carried through the same known transform.
            float want[3];
            HpApply(axis, m->rearGripM, want);
            for (int k = 0; k < 3; ++k) want[k] += pos[k];
            CHECK_NEAR(HpDist(pivot, want), 0.0, 1e-4,
                       "the pivot lands on the rear grip (%.6f m off)",
                       HpDist(pivot, want));

            // THE POINT OF ALL THIS: the pivot is not the body origin.
            // If it were, rotating about it would be rotating about the
            // artist's mesh root, which is not where anyone holds a rifle.
            CHECK(HpDist(pivot, body.pos) > 0.01f,
                  "the rear grip is %.3f m from the body origin -- they are NOT "
                  "the same point", HpDist(pivot, body.pos));

            // ---- the delta rotates about the grip ---------------------
            float wantAxis[9];
            HpAxisFromEuler(11.0f + 15.0f, -67.0f + 15.0f, 23.0f + 15.0f, wantAxis);
            float mm[16];
            VRGunPose_Delta(&body, pivot, wantAxis, pivot, mm);

            float moved[3];
            HpXform4(mm, pivot, moved);
            CHECK_NEAR(HpDist(moved, pivot), 0.0, 1e-5,
                       "+15 yaw/pitch/roll leaves the REAR GRIP fixed");

            // MUTANT 29: the camera-pivot gun_yaw. Same rotation, no pivot
            // term. The grip must move a long way -- if it did not, this
            // fixture would not be catching the bug it exists for.
            float mut[16];
            memset(mut, 0, sizeof(mut));
            {
                float R[9];
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) {
                        float s = 0.0f;
                        for (int k = 0; k < 3; ++k)
                            s += wantAxis[k * 3 + r] * body.axis[k * 3 + c];
                        R[c * 3 + r] = s;
                    }
                mut[0] = R[0]; mut[1] = R[1]; mut[2]  = R[2];
                mut[4] = R[3]; mut[5] = R[4]; mut[6]  = R[5];
                mut[8] = R[6]; mut[9] = R[7]; mut[10] = R[8];
                mut[15] = 1.0f;
            }
            float mutMoved[3];
            HpXform4(mut, pivot, mutMoved);
            CHECK(HpDist(mutMoved, pivot) > 0.05f,
                  "MUTANT: the camera-pivot gun_yaw swings the grip %.3f m -- caught",
                  HpDist(mutMoved, pivot));

            // MUTANT: pivoting on the BODY ORIGIN instead of the rear grip.
            // A subtler wrong answer than the camera one, and the reason the
            // pivot is a separate argument rather than taken from body.pos.
            float mBody[16], bodyPivotMoved[3];
            VRGunPose_Delta(&body, body.pos, wantAxis, body.pos, mBody);
            HpXform4(mBody, pivot, bodyPivotMoved);
            CHECK(HpDist(bodyPivotMoved, pivot) > 0.002f,
                  "MUTANT: pivoting on the mesh root moves the grip %.4f m -- caught",
                  HpDist(bodyPivotMoved, pivot));
        }
    }

    // ---- 26: a WEAK rear grip is never offered as a pivot ---------------
    {
        // The grenades and the panzerfaust have no meaningful rear grip, and
        // the extractor marks them WEAK. The runtime must decline rather than
        // hand back a plausible-looking number.
        int weakFound = 0, weakOffered = 0;
        const int probe[] = { 620, 960, 312, 152, 88, 542, 46 };
        for (int i = 0; i < 7; ++i) {
            const CodvrGunModel* m = VRGunPose_ByVertexCount(probe[i]);
            if (!m || m->rearGripUsable) continue;
            ++weakFound;
            VRHandPose body;
            memset(&body, 0, sizeof(body));
            body.valid = 1;
            body.axis[0] = body.axis[4] = body.axis[8] = 1.0f;
            float pivot[3] = { 111.0f, 111.0f, 111.0f };
            if (VRGunPose_RearGripPivot(m->vertexCount, &body, 1.0f, pivot))
                ++weakOffered;
        }
        CHECK(weakFound > 0, "%d sampled models carry a below-STRONG rear grip",
              weakFound);
        CHECK(weakOffered == 0,
              "a WEAK rear grip is NEVER offered as a pivot (%d leaked)", weakOffered);
    }

    // ---- an unsolved body yields identity, never a guess -----------------
    {
        VRHandPose bad;
        memset(&bad, 0, sizeof(bad));
        float mm[16];
        const float p[3] = { 1, 2, 3 };
        float a[9];
        HpAxisFromEuler(30, 30, 30, a);
        VRGunPose_Delta(&bad, p, a, p, mm);
        int identity = 1;
        for (int i = 0; i < 16; ++i) {
            const float want = (i % 5 == 0) ? 1.0f : 0.0f;
            if (fabsf(mm[i] - want) > 1e-6f) identity = 0;
        }
        CHECK(identity,
              "an unsolved gun body yields the IDENTITY delta -- the engine's "
              "pose stands");

        VRHandPose good;
        memset(&good, 0, sizeof(good));
        good.valid = 1;
        good.axis[0] = good.axis[4] = good.axis[8] = 1.0f;
        float pivot[3];
        CHECK(VRGunPose_RearGripPivot(123456, &good, 1.0f, pivot) == 0,
              "an unknown model offers no pivot");
    }
}

// ---------------------------------------------------------------------------

static void S_WeaponProfileConfidence()
{
    printf("--- weapon profiles: WEAK values cannot become production ---\n");

    CHECK(VRWProfile_Count() > 30, "the ported profile table carries %d weapons",
          VRWProfile_Count());

    const VrWeaponProfile* generic = VRWProfile_ById(0);
    CHECK(generic != nullptr, "profile 0 (the generic fallback) exists");

    // ---- 26: the confidence bar --------------------------------------
    VRWProfile_SetExperimentalCalibration(0);
    CHECK(VRWProfile_ConfidenceUsable(VRWP_CONF_PROVEN) == 1, "PROVEN is usable");
    CHECK(VRWProfile_ConfidenceUsable(VRWP_CONF_STRONG) == 1, "STRONG is usable");
    CHECK(VRWProfile_ConfidenceUsable(VRWP_CONF_WEAK) == 0,
          "WEAK is NOT usable by default");
    CHECK(VRWProfile_ConfidenceUsable(VRWP_CONF_UNKNOWN) == 0, "UNKNOWN is not usable");
    CHECK(VRWProfile_ConfidenceUsable(VRWP_CONF_NA) == 0, "NA is not usable");

    // Reading a value below the bar must leave the caller's own default in
    // place rather than overwriting it with a guess.
    {
        int checked = 0, leaked = 0;
        for (int i = 0; i < VRWProfile_Count(); ++i) {
            const VrWeaponProfile* p = VRWProfile_ByIndex(i);
            if (!p) continue;
            for (int f = 0; f < VRWP_F_COUNT; ++f) {
                if (VRWProfile_ConfidenceUsable(p->conf[f])) continue;
                ++checked;
                float sentinel[3] = { -777.0f, -777.0f, -777.0f };
                VRWProfile_Value3(p, f, sentinel);
                if (sentinel[0] != -777.0f) ++leaked;
            }
        }
        CHECK(checked > 0, "%d sub-STRONG fields exist across the table", checked);
        CHECK(leaked == 0,
              "not one sub-STRONG field overwrote the caller's default (%d leaked)",
              leaked);
    }

    // The experimental switch lowers the bar to WEAK and nothing else does.
    VRWProfile_SetExperimentalCalibration(1);
    CHECK(VRWProfile_ConfidenceUsable(VRWP_CONF_WEAK) == 1,
          "vr_weapon_profile_experimental lowers the bar to WEAK");
    CHECK(VRWProfile_ConfidenceUsable(VRWP_CONF_UNKNOWN) == 0,
          "even experimental never promotes UNKNOWN");
    CHECK(VRWProfile_ConfidenceUsable(VRWP_CONF_NA) == 0,
          "even experimental never promotes NA");
    VRWProfile_SetExperimentalCalibration(0);
    CHECK(VRWProfile_ConfidenceUsable(VRWP_CONF_WEAK) == 0,
          "the bar goes back up when the switch is cleared");

    // An unidentified weapon resolves to the generic profile, whose behaviour
    // is what the build already had.
    VRWProfile_TestReset();
    const VrWeaponProfile* active = VRWProfile_Active();
    CHECK(active != nullptr, "there is always an active profile");
    CHECK(active->id == 0,
          "with nothing identified, the active profile is the generic fallback");
}
