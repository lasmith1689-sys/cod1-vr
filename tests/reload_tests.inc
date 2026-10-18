// ===========================================================================
// PHYSICAL RELOAD (src/vr_reload.cpp)
//
// The two things that must be true, and are worth more than every other case
// here: the gesture issues EXACTLY ONE engine reload when it completes, and
// issues NONE when it is interrupted. A reload that fires twice costs the
// player a magazine; one that fires unasked costs them a firefight.
// ===========================================================================

#include "../src/vr_reload.h"

// A Thompson-shaped profile: support hand, magwell below and behind the
// firing grip. The numbers are the shape the measured table produces, not the
// exact row -- the machine is being tested, not the data.
static VRReloadProfile RlThompson()
{
    VRReloadProfile p;
    memset(&p, 0, sizeof(p));
    p.cls  = VRRL_CLASS_MAGAZINE;
    p.hand = VRRL_HAND_SUPPORT;
    p.firingGripM[0] =  0.20f; p.firingGripM[1] = 0.02f; p.firingGripM[2] = -0.03f;
    p.loadZoneM[0]   =  0.05f; p.loadZoneM[1]   = 0.02f; p.loadZoneM[2]   = -0.14f;
    p.valid = 1;
    return p;
}

static VRReloadInput RlIn(const float hand[3], const float world[3], int grip,
                          double now)
{
    VRReloadInput in;
    memset(&in, 0, sizeof(in));
    in.valid = 1;
    memcpy(in.handM, hand, sizeof(in.handM));
    memcpy(in.handWorldM, world, sizeof(in.handWorldM));
    in.gripHeld = grip;
    in.now = now;
    return in;
}

// Somewhere far from every zone, used when only the weapon-frame position
// matters.
static const float kRlAway[3] = { 2.0f, 2.0f, 2.0f };

// Drive one full DETACHABLE_MAG gesture. Returns how many reloads it issued.
static int RlFullGesture(double* clock, int* sawStates)
{
    VRReloadTuning t;
    VRReload_GetTuning(&t);
    const float pouch[3] = { t.pouchForwardM, 0.0f, -t.pouchBelowHeadM };

    VRReloadProfile p = RlThompson();
    int issued = 0;
    struct Step { const float* h; const float* w; int grip; };
    const float atGrip[3]  = { 0.20f, 0.02f, -0.03f };
    const float midway[3]  = { 0.10f, 0.02f, -0.10f };   // 0.122 m from the grip
    const float atZone[3]  = { 0.05f, 0.02f, -0.14f };
    const float pulled[3]  = { 0.05f, 0.02f, -0.34f };

    const Step steps[] = {
        { atGrip, kRlAway, 0 },   // idle
        { midway, kRlAway, 0 },   // -> reach
        { atZone, kRlAway, 1 },   // -> gripped
        { pulled, kRlAway, 1 },   // -> pulled
        { pulled, pouch,   1 },   // -> pouch
        { pulled, pouch,   0 },   // release at the pouch
        { pulled, pouch,   1 },   // grip EDGE -> acquired
        { atZone, pouch,   1 },   // -> insert
        { atZone, pouch,   0 },   // release -> COMMIT, one reload
        { atZone, kRlAway, 0 },   // -> recover
        { atGrip, kRlAway, 0 },   // -> idle
    };
    for (int i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); ++i) {
        *clock += 0.1;
        VRReloadInput in = RlIn(steps[i].h, steps[i].w, steps[i].grip, *clock);
        VRReloadOutput o = VRReload_Step(&in);
        if (o.issueReload) ++issued;
        if (sawStates) sawStates[o.state] = 1;
    }
    (void)p;
    return issued;
}

static void S_PhysicalReload()
{
    printf("--- physical reload: the gesture state machine ---\n");

    // ---- 23: the full gesture, once ------------------------------------
    {
        VRReload_Reset();
        VRReloadProfile p = RlThompson();
        VRReload_SetProfile(&p);

        double clock = 0.0;
        int seen[VRRL_STATES] = { 0 };
        const int issued = RlFullGesture(&clock, seen);

        CHECK(issued == 1,
              "a complete DETACHABLE_MAG gesture issues EXACTLY ONE reload (%d)",
              issued);
        CHECK(VRReload_ReloadsIssued() == 1, "the counter agrees");
        CHECK(seen[VRRL_REACH] && seen[VRRL_GRIPPED] && seen[VRRL_PULLED],
              "the gesture passed through reach -> gripped -> pulled");
        CHECK(seen[VRRL_POUCH] && seen[VRRL_ACQUIRED] && seen[VRRL_INSERT],
              "the gesture passed through pouch -> acquired -> insert");
        CHECK(seen[VRRL_COMMIT], "the gesture reached COMMIT");
        CHECK(VRReload_State() == VRRL_IDLE, "and ended back at idle");
        CHECK(VRReload_HandBusy() == 0, "the hand is free again");
    }

    // ---- two gestures issue two reloads, not three ----------------------
    {
        VRReload_Reset();
        VRReloadProfile p = RlThompson();
        VRReload_SetProfile(&p);
        double clock = 0.0;
        const int a = RlFullGesture(&clock, nullptr);
        const int b = RlFullGesture(&clock, nullptr);
        CHECK(a == 1 && b == 1, "two gestures issue one reload each (%d, %d)", a, b);
        CHECK(VRReload_ReloadsIssued() == 2, "two in total, not three");
    }

    // ---- COMMIT cannot be held down -------------------------------------
    {
        VRReload_Reset();
        VRReloadProfile p = RlThompson();
        VRReload_SetProfile(&p);
        double clock = 0.0;
        RlFullGesture(&clock, nullptr);
        const int after = VRReload_ReloadsIssued();
        // Sit still at the loading zone with the grip released for a long
        // while. The pre-edge-trigger version of this machine would fire once
        // per frame here.
        const float atZone[3] = { 0.05f, 0.02f, -0.14f };
        for (int i = 0; i < 200; ++i) {
            clock += 0.016;
            VRReloadInput in = RlIn(atZone, kRlAway, 0, clock);
            VRReload_Step(&in);
        }
        CHECK(VRReload_ReloadsIssued() == after,
              "holding still at the loading zone issues no further reloads "
              "(%d -> %d)", after, VRReload_ReloadsIssued());
    }

    // ---- 24: every cancellation path issues nothing ---------------------
    {
        struct Case { const char* name; int lostTracking, menu, switched, stall; };
        const Case cases[] = {
            { "tracking loss",  1, 0, 0, 0 },
            { "the menu",       0, 1, 0, 0 },
            { "a weapon switch",0, 0, 1, 0 },
            { "a stalled hand", 0, 0, 0, 1 },
        };
        for (int c = 0; c < 4; ++c) {
            VRReload_Reset();
            VRReloadProfile p = RlThompson();
            VRReload_SetProfile(&p);
            double clock = 0.0;

            // Get as far as PULLED -- a magazine is out and the gesture is
            // committed to nothing yet.
            const float midway[3] = { 0.10f, 0.02f, -0.10f };    // 0.122 m from the grip
            const float atZone[3] = { 0.05f, 0.02f, -0.14f };
            const float pulled[3] = { 0.05f, 0.02f, -0.34f };
            const struct { const float* h; int g; } lead[] = {
                { midway, 0 }, { atZone, 1 }, { pulled, 1 },
            };
            for (int i = 0; i < 3; ++i) {
                clock += 0.1;
                VRReloadInput in = RlIn(lead[i].h, kRlAway, lead[i].g, clock);
                VRReload_Step(&in);
            }
            CHECK(VRReload_State() == VRRL_PULLED,
                  "%s: the gesture reached PULLED before the interruption",
                  cases[c].name);

            clock += cases[c].stall ? 30.0 : 0.1;
            VRReloadInput in = RlIn(pulled, kRlAway, 1, clock);
            in.valid         = cases[c].lostTracking ? 0 : 1;
            in.menuOpen      = cases[c].menu;
            in.weaponChanged = cases[c].switched;
            VRReloadOutput o = VRReload_Step(&in);

            CHECK(o.issueReload == 0, "%s: issues no reload", cases[c].name);
            CHECK(o.cancelled == 1,   "%s: is reported as a cancellation", cases[c].name);
            CHECK(VRReload_ReloadsIssued() == 0,
                  "%s: the reload counter is still zero", cases[c].name);
            CHECK(VRReload_State() == VRRL_IDLE,
                  "%s: the machine is back at idle, not stuck", cases[c].name);
            CHECK(VRReload_Cancellations() == 1,
                  "%s: exactly one cancellation recorded", cases[c].name);
        }
    }

    // ---- an interrupted gesture can be started again --------------------
    {
        VRReload_Reset();
        VRReloadProfile p = RlThompson();
        VRReload_SetProfile(&p);
        double clock = 0.0;
        const float midway[3] = { 0.10f, 0.02f, -0.10f };    // 0.122 m from the grip
        clock += 0.1;
        VRReloadInput a = RlIn(midway, kRlAway, 0, clock);
        VRReload_Step(&a);
        clock += 0.1;
        VRReloadInput b = RlIn(midway, kRlAway, 0, clock);
        b.menuOpen = 1;
        VRReload_Step(&b);
        const int issued = RlFullGesture(&clock, nullptr);
        CHECK(issued == 1,
              "after a cancelled attempt, a fresh gesture still completes");
    }

    // ---- 25: the profile decides, and a switch clears the machine -------
    {
        VRReload_Reset();
        VRReloadProfile p = RlThompson();
        VRReload_SetProfile(&p);
        double clock = 0.0;
        const float midway[3] = { 0.10f, 0.02f, -0.10f };    // 0.122 m from the grip
        clock += 0.1;
        VRReloadInput in = RlIn(midway, kRlAway, 0, clock);
        VRReload_Step(&in);
        CHECK(VRReload_State() == VRRL_REACH, "a gesture is under way");

        VRReloadProfile q = RlThompson();
        q.loadZoneM[2] = -0.30f;             // a different weapon's magwell
        VRReload_SetProfile(&q);
        CHECK(VRReload_State() == VRRL_IDLE,
              "installing a new profile abandons the gesture in flight");
    }

    // ---- weapons with no physical reload never gesture ------------------
    {
        const int classes[] = { VRRL_CLASS_NONE, VRRL_CLASS_SINGLE_SHOT };
        for (int c = 0; c < 2; ++c) {
            VRReload_Reset();
            VRReloadProfile p = RlThompson();
            p.cls = classes[c];
            VRReload_SetProfile(&p);
            double clock = 0.0;
            const int issued = RlFullGesture(&clock, nullptr);
            CHECK(issued == 0,
                  "class '%s' issues no reload however the hand moves",
                  VRReload_ClassName(classes[c]));
            CHECK(VRReload_State() == VRRL_IDLE,
                  "class '%s' never leaves idle", VRReload_ClassName(classes[c]));
        }
    }
    {
        VRReload_Reset();
        VRReload_SetProfile(nullptr);
        double clock = 0.0;
        CHECK(RlFullGesture(&clock, nullptr) == 0,
              "with no profile at all, nothing reloads");
    }

    // ---- INCREMENTAL skips the magazine-removal half --------------------
    {
        VRReload_Reset();
        VRReloadProfile p = RlThompson();
        p.cls  = VRRL_CLASS_INCREMENTAL;
        p.hand = VRRL_HAND_TRIGGER;
        VRReload_SetProfile(&p);

        VRReloadTuning t;
        VRReload_GetTuning(&t);
        const float pouch[3]  = { t.pouchForwardM, 0.0f, -t.pouchBelowHeadM };
        const float midway[3] = { 0.10f, 0.02f, -0.10f };    // 0.122 m from the grip
        const float atZone[3] = { 0.05f, 0.02f, -0.14f };
        const float atGrip[3] = { 0.20f, 0.02f, -0.03f };

        double clock = 0.0;
        int issued = 0;
        const struct { const float* h; const float* w; int g; } steps[] = {
            { midway, kRlAway, 0 },   // -> reach
            { midway, pouch,   0 },   // -> pouch directly: nothing to remove
            { midway, pouch,   1 },   // grip edge -> acquired
            { atZone, pouch,   1 },   // -> insert
            { atZone, pouch,   0 },   // release -> commit
            { atZone, kRlAway, 0 },
            { atGrip, kRlAway, 0 },
        };
        for (int i = 0; i < 7; ++i) {
            clock += 0.1;
            VRReloadInput in = RlIn(steps[i].h, steps[i].w, steps[i].g, clock);
            VRReloadOutput o = VRReload_Step(&in);
            if (o.issueReload) ++issued;
        }
        CHECK(issued == 1,
              "a per-round loader reaches the ammunition directly and issues one "
              "reload (%d)", issued);
    }

    // Deliberate physical actions: ordinary movement and an already-held
    // support squeeze must not manufacture a magazine pickup.
    {
        VRReload_Reset();
        VRReloadProfile p = RlThompson(); VRReload_SetProfile(&p);
        VRReloadInput in = RlIn(kRlAway, kRlAway, 0, 0.1);
        CHECK(VRReload_Step(&in).state == VRRL_IDLE,
              "ordinary free-hand movement does not start a reload");
        in = RlIn(p.firingGripM, kRlAway, 1, 0.2); VRReload_Step(&in);
        in = RlIn(p.loadZoneM, kRlAway, 1, 0.3); VRReload_Step(&in);
        in.now = 0.4;
        CHECK(VRReload_Step(&in).state == VRRL_REACH,
              "squeeze held from the foregrip cannot grab a magazine");
        in.gripHeld = 0; in.now = 0.5; VRReload_Step(&in);
        in.gripHeld = 1; in.now = 0.6;
        CHECK(VRReload_Step(&in).state == VRRL_GRIPPED,
              "a fresh squeeze at the measured zone grabs the magazine");
    }
    // Receiver loaders must consume a fresh squeeze arriving at the pouch,
    // whether that frame first leaves IDLE or first leaves REACH. Losing the
    // edge on a transitional frame makes a natural pickup require a retry.
    {
        for (int fromReach = 0; fromReach < 2; ++fromReach) {
            VRReload_Reset();
            VRReloadProfile p = RlThompson(); p.directLoad = 1;
            VRReload_SetProfile(&p);
            const float pouch[3] = { 0.15f, 0.0f, -0.45f };
            VRReloadInput in = RlIn(kRlAway, kRlAway, 0, 0.1);
            VRReload_Step(&in);
            if (fromReach) {
                in = RlIn(p.loadZoneM, kRlAway, 0, 0.2);
                VRReload_Step(&in);
            }
            in = RlIn(kRlAway, pouch, 1, 0.3);
            CHECK(VRReload_Step(&in).state == VRRL_ACQUIRED,
                  "receiver fresh squeeze on pouch arrival acquires ammo from %s",
                  fromReach ? "REACH" : "IDLE");
        }
        VRReload_Reset();
        VRReloadProfile p = RlThompson(); p.directLoad = 1;
        VRReload_SetProfile(&p);
        const float pouch[3] = { 0.15f, 0.0f, -0.45f };
        VRReloadInput in = RlIn(kRlAway, kRlAway, 1, 0.1);
        VRReload_Step(&in);     // already-held squeeze before pouch arrival
        in = RlIn(kRlAway, pouch, 1, 0.2); VRReload_Step(&in);
        in.now = 0.3; VRReload_Step(&in);
        in.now = 0.4;
        CHECK(VRReload_Step(&in).state == VRRL_POUCH
              && VRReload_ReloadsIssued() == 0,
              "receiver pouch arrival with a stale held squeeze acquires no ammo");
        in.gripHeld = 0; in.now = 0.5; VRReload_Step(&in);
        in.gripHeld = 1; in.now = 0.6;
        CHECK(VRReload_Step(&in).state == VRRL_ACQUIRED,
              "receiver stale squeeze requires release and fresh pouch squeeze");
    }
    // The old POUCH latch accepted a later squeeze anywhere in space. A
    // pickup now requires the hand to still be inside the ammunition zone.
    {
        VRReload_Reset();
        VRReloadProfile p = RlThompson(); p.directLoad = 1; VRReload_SetProfile(&p);
        const float pouch[3] = { 0.15f, 0.0f, -0.45f };
        VRReloadInput in = RlIn(p.loadZoneM, pouch, 0, 0.1); VRReload_Step(&in);
        in.now = 0.2; VRReload_Step(&in);
        in = RlIn(p.loadZoneM, kRlAway, 1, 0.3);
        CHECK(VRReload_Step(&in).state == VRRL_POUCH,
              "a squeeze after leaving the pouch acquires no ammunition");
        in = RlIn(p.loadZoneM, pouch, 0, 0.4); VRReload_Step(&in);
        in.gripHeld = 1; in.now = 0.5;
        CHECK(VRReload_Step(&in).state == VRRL_ACQUIRED,
              "a clean squeeze at the pouch acquires receiver ammunition");
        in = RlIn(kRlAway, kRlAway, 0, 0.6); VRReload_Step(&in);
        in = RlIn(p.loadZoneM, kRlAway, 0, 0.7); VRReload_Step(&in);
        in.now = 0.8;
        CHECK(VRReload_Step(&in).issueReload == 0 && VRReload_ReloadsIssued() == 0,
              "dropping ammunition before reaching the receiver cannot reload");
    }
    // A receiver/clip loader has no old magazine to pull from the weapon.
    // Stock ammunition/chamber timing still starts from exactly one commit.
    {
        VRReload_Reset();
        VRReloadProfile p = RlThompson(); p.directLoad = 1; VRReload_SetProfile(&p);
        const float pouch[3] = { 0.15f, 0.0f, -0.45f };
        VRReloadInput in = RlIn(kRlAway, pouch, 0, 0.1); VRReload_Step(&in);
        in.now = 0.2; VRReload_Step(&in);
        in.gripHeld = 1; in.now = 0.3; VRReload_Step(&in);
        in = RlIn(p.loadZoneM, kRlAway, 1, 0.4);
        CHECK(VRReload_Step(&in).state == VRRL_INSERT,
              "clip/receiver gesture reaches insertion without fake magazine removal");
        in.gripHeld = 0; in.now = 0.5;
        CHECK(VRReload_Step(&in).issueReload == 1,
              "deliberate release in the receiver requests one stock reload");
        for (int i = 0; i < 120; ++i) {
            in.now += 0.011; VRReload_Step(&in);
        }
        CHECK(VRReload_ReloadsIssued() == 1,
              "receiver insertion cannot repeat while the hand stays there");
    }

    // ---- the tuning refuses nonsense ------------------------------------
    {
        VRReloadTuning before, after, bad;
        VRReload_GetTuning(&before);
        memset(&bad, 0, sizeof(bad));
        bad.zoneRadiusM = 99.0f;      // an "anywhere" zone
        bad.pullThresholdM = -1.0f;
        bad.timeoutSec = 1e9f;
        VRReload_SetTuning(&bad);
        VRReload_GetTuning(&after);
        CHECK(after.zoneRadiusM == before.zoneRadiusM,
              "an absurd zone radius is refused");
        CHECK(after.pullThresholdM == before.pullThresholdM,
              "a negative pull threshold is refused");
        CHECK(after.timeoutSec == before.timeoutSec,
              "an absurd timeout is refused");
    }

    VRReload_Reset();
}

// ---------------------------------------------------------------------------

static void S_ReloadTableSelection()
{
    printf("--- physical reload: the measured table, keyed by gun model ---\n");

    CHECK(VRReload_TableRows() >= 20,
          "the reload table carries %d gun models", VRReload_TableRows());

    // Pistol contact is close to the authored support pose. Requiring the
    // full reach distance prevented a grip at the actual Colt/Luger magazine
    // center from ever starting removal.
    {
        const int pistols[] = { 344, 1174 };
        for (int key : pistols) {
            VRReload_Reset(); VRReload_SelectByVertexCount(key);
            VRReloadProfile p; VRReload_Get(&p);
            VRReloadInput in = RlIn(p.loadZoneM, kRlAway, 0, 0.1);
            VRReload_Step(&in);
            in.gripHeld = 1; in.now = 0.2;
            CHECK(VRReload_Step(&in).state == VRRL_GRIPPED,
                  "pistol %d: fresh squeeze at the magazine contact starts removal", key);
        }
    }

    // ---- 25: a weapon switch selects a different profile ---------------
    struct Case { int verts; const char* what; int wantValid; int wantHand; };
    // BODY-SURFACE keys (re-keyed 2026-08-28, matching kGunModels).
    const Case cases[] = {
        {  326, "thompson (magazine, support hand, magwell below)", 1, VRRL_HAND_SUPPORT },
        { 1284, "bren (magazine, TRIGGER hand, magwell on TOP)",    1, VRRL_HAND_TRIGGER },
        {  536, "kar98k (clip load, trigger hand, from above)",     1, VRRL_HAND_TRIGGER },
        {  630, "sten (magazine, support hand, magwell on the SIDE)", 1, VRRL_HAND_SUPPORT },
        {   88, "panzerfaust (single shot: no gesture)",            0, 0 },
        {  960, "MK2 grenade (nothing to reload)",                  0, 0 },
    };
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        VRReload_Reset();
        const int live = VRReload_SelectByVertexCount(cases[i].verts);
        VRReloadProfile p;
        VRReload_Get(&p);
        CHECK(live == cases[i].wantValid,
              "%s -> %s gesture", cases[i].what, live ? "a live" : "NO");
        if (cases[i].wantValid)
            CHECK(p.hand == cases[i].wantHand,
                  "%s -> the %s hand loads it", cases[i].what,
                  p.hand == VRRL_HAND_TRIGGER ? "TRIGGER" : "support");
    }

    // ---- the zones really do differ by weapon ---------------------------
    //
    // This is the case that would fail if anything ever replaced the measured
    // zones with a "magazines go underneath" rule.
    {
        // Zones now live in each model's BODY-BONE frame, whose axes are
        // whatever the artist rooted (the sten's bore is its body +z, the
        // thompson's +x), so per-component sign checks are meaningless.
        // The measured-not-rule-based facts, stated frame-independently:
        // every magwell sits well off the bore line, and different weapons'
        // zones sit in materially DIFFERENT directions from their grips.
        VRReloadProfile thompson, bren, sten;
        VRReload_SelectByVertexCount(326);  VRReload_Get(&thompson);
        VRReload_SelectByVertexCount(1284); VRReload_Get(&bren);
        VRReload_SelectByVertexCount(630);  VRReload_Get(&sten);

        struct ZoneFacts {
            float perpFromBoreM;   // zone's distance from the bore line
            float dirFromGrip[3];  // unit, zone - firing grip
        };
        auto facts = [](int key, const VRReloadProfile& p) -> ZoneFacts {
            ZoneFacts f = { 0, { 0, 0, 0 } };
            const CodvrGunModel* m = VRGunPose_ByVertexCount(key);
            if (m) {
                float rel[3], along = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    rel[k] = p.loadZoneM[k] - m->rearGripM[k];
                    along += rel[k] * m->barrelFwd[k];
                }
                float perp = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const float c = rel[k] - along * m->barrelFwd[k];
                    perp += c * c;
                }
                f.perpFromBoreM = sqrtf(perp);
            }
            float d[3], n = 0.0f;
            for (int k = 0; k < 3; ++k) {
                d[k] = p.loadZoneM[k] - p.firingGripM[k];
                n += d[k] * d[k];
            }
            n = sqrtf(n);
            if (n > 1e-6f)
                for (int k = 0; k < 3; ++k) f.dirFromGrip[k] = d[k] / n;
            return f;
        };
        const ZoneFacts ft = facts(326, thompson);
        const ZoneFacts fb = facts(1284, bren);
        const ZoneFacts fs = facts(630, sten);
        CHECK(ft.perpFromBoreM > 0.20f,
              "the Thompson's magwell is %.3f m off the bore line",
              ft.perpFromBoreM);
        CHECK(fb.perpFromBoreM > 0.10f,
              "the Bren's magwell is %.3f m off the bore line", fb.perpFromBoreM);
        CHECK(fs.perpFromBoreM > 0.05f && fs.perpFromBoreM < 0.25f,
              "the Sten's insertion point is on its side magazine, %.3f m off the bore line",
              fs.perpFromBoreM);
        const float dot = ft.dirFromGrip[0]*fb.dirFromGrip[0]
                        + ft.dirFromGrip[1]*fb.dirFromGrip[1]
                        + ft.dirFromGrip[2]*fb.dirFromGrip[2];
        CHECK(dot < 0.3f,
              "the Thompson and Bren reach in materially different directions "
              "(dot %.2f) -- no 'magazines are underneath' rule survives", dot);
    }

    // ---- an unknown model gets no gesture -------------------------------
    {
        VRReload_Reset();
        CHECK(VRReload_SelectByVertexCount(4242) == 0,
              "an unrecognised gun model installs no gesture profile");
        VRReloadProfile p;
        VRReload_Get(&p);
        CHECK(p.valid == 0, "and the profile is explicitly invalid");
        double clock = 0.0;
        CHECK(RlFullGesture(&clock, nullptr) == 0,
              "so no hand motion can reload it");
    }

    VRReload_Reset();
}
