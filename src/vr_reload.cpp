#include "vr_reload.h"

#include <math.h>
#include <string.h>

#include "vr_reload_table.inc"
#include "vr_reload_insert_zones.inc"

static VRReloadProfile g_profile;
static VRReloadTuning  g_tune = {
    0.12f,   // zoneRadiusM
    0.15f,   // pullThresholdM
    0.45f,   // pouchBelowHeadM
    0.15f,   // pouchForwardM
    0.22f,   // pouchRadiusM
    0.10f,   // leaveGripM
    8.0f,    // timeoutSec
};

static int    g_state;
static double g_stateSince;
static int    g_reloads;
static int    g_cancels;
static int    g_prevGrip;

const char* VRReload_StateName(int s)
{
    switch (s) {
        case VRRL_IDLE:     return "idle";
        case VRRL_REACH:    return "reach";
        case VRRL_GRIPPED:  return "gripped";
        case VRRL_PULLED:   return "pulled";
        case VRRL_POUCH:    return "pouch";
        case VRRL_ACQUIRED: return "acquired";
        case VRRL_INSERT:   return "insert";
        case VRRL_COMMIT:   return "commit";
        case VRRL_RECOVER:  return "recover";
        default:            return "?";
    }
}

const char* VRReload_ClassName(int c)
{
    switch (c) {
        case VRRL_CLASS_NONE:        return "none";
        case VRRL_CLASS_MAGAZINE:    return "magazine";
        case VRRL_CLASS_INCREMENTAL: return "incremental";
        case VRRL_CLASS_SINGLE_SHOT: return "single_shot";
        default:                     return "?";
    }
}

void VRReload_GetTuning(VRReloadTuning* out) { if (out) *out = g_tune; }

void VRReload_SetTuning(const VRReloadTuning* t)
{
    if (!t)
        return;
    // Each bound is a physical statement, not a taste. A zone smaller than a
    // couple of centimetres cannot be hit by a tracked hand; one bigger than
    // an arm's length is not a zone.
    if (t->zoneRadiusM     > 0.02f && t->zoneRadiusM     < 0.50f) g_tune.zoneRadiusM     = t->zoneRadiusM;
    if (t->pullThresholdM  > 0.03f && t->pullThresholdM  < 0.60f) g_tune.pullThresholdM  = t->pullThresholdM;
    if (t->pouchBelowHeadM > 0.10f && t->pouchBelowHeadM < 1.00f) g_tune.pouchBelowHeadM = t->pouchBelowHeadM;
    if (t->pouchForwardM   > -0.5f && t->pouchForwardM   < 0.80f) g_tune.pouchForwardM   = t->pouchForwardM;
    if (t->pouchRadiusM    > 0.05f && t->pouchRadiusM    < 0.60f) g_tune.pouchRadiusM    = t->pouchRadiusM;
    if (t->leaveGripM      > 0.02f && t->leaveGripM      < 0.50f) g_tune.leaveGripM      = t->leaveGripM;
    if (t->timeoutSec      > 1.0f  && t->timeoutSec      < 60.0f) g_tune.timeoutSec      = t->timeoutSec;
}

void VRReload_Reset()
{
    memset(&g_profile, 0, sizeof(g_profile));
    g_state      = VRRL_IDLE;
    g_stateSince = 0.0;
    g_reloads    = 0;
    g_cancels    = 0;
    g_prevGrip   = 0;
}

void VRReload_SetProfile(const VRReloadProfile* p)
{
    if (p) g_profile = *p;
    else   memset(&g_profile, 0, sizeof(g_profile));
    // A profile change means a different weapon. Anything in flight belonged
    // to the old one and must not complete against the new one's geometry.
    g_state = VRRL_IDLE;
}

void VRReload_Get(VRReloadProfile* out) { if (out) *out = g_profile; }

int VRReload_TableRows()
{
    return (int)(sizeof(kReloadRows) / sizeof(kReloadRows[0]));
}

int VRReload_SelectByVertexCount(int vertexCount)
{
    const int n = VRReload_TableRows();
    for (int i = 0; i < n; ++i) {
        if (kReloadRows[i].vertexCount != vertexCount)
            continue;
        VRReloadProfile p;
        memset(&p, 0, sizeof(p));
        p.cls  = kReloadRows[i].cls;
        p.hand = kReloadRows[i].hand;
        memcpy(p.firingGripM, kReloadRows[i].firingGripM, sizeof(p.firingGripM));
        memcpy(p.loadZoneM,   kReloadRows[i].loadZoneM,   sizeof(p.loadZoneM));
        p.valid = kReloadRows[i].valid;
        // Clip/receiver loaders have no detachable magazine to pull out.
        // Bren is the one trigger-hand animation with a removable top magazine.
        p.directLoad = p.cls == VRRL_CLASS_INCREMENTAL
                    || (p.hand == VRRL_HAND_TRIGGER && vertexCount != 1284);
        // The old table records peak hand excursion, which is often the
        // off-weapon ammo reach. The contact table instead measures the
        // authored hand at the weapon-file ammunition-transfer event.
        const int zones = (int)(sizeof(kReloadInsertZones) / sizeof(kReloadInsertZones[0]));
        int haveContact = 0;
        for (int j = 0; j < zones; ++j) {
            if (kReloadInsertZones[j].vertexCount != vertexCount) continue;
            memcpy(p.loadZoneM, kReloadInsertZones[j].pointM, sizeof(p.loadZoneM));
            p.directLoad = kReloadInsertZones[j].directLoad;
            haveContact = 1;
            break;
        }
        if (!haveContact) p.valid = 0;
        VRReload_SetProfile(&p);
        return p.valid;
    }
    // Unknown model: no gesture. Substituting a generic magwell here would
    // put a reload zone in mid-air on a weapon nobody has measured.
    VRReload_SetProfile(nullptr);
    return 0;
}

int VRReload_State()          { return g_state; }
int VRReload_ReloadsIssued()  { return g_reloads; }
int VRReload_Cancellations()  { return g_cancels; }
int VRReload_HandBusy()       { return g_state > VRRL_IDLE; }

static float Dist(const float a[3], const float b[3])
{
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// The ammo pouch, relative to the head. Head-relative rather than
// world-relative on purpose: a player who turns round still finds their own
// pouch, and a play space that is not centred does not move it.
//
// The frame convention matches vr_math's: +X forward, +Y left, +Z up.
static float PouchDist(const float handWorldM[3])
{
    const float pouch[3] = { g_tune.pouchForwardM, 0.0f, -g_tune.pouchBelowHeadM };
    return Dist(handWorldM, pouch);
}

static void Go(int state, double now)
{
    g_state = state;
    g_stateSince = now;
}

VRReloadOutput VRReload_Step(const VRReloadInput* in)
{
    VRReloadOutput out;
    out.state = g_state;
    out.issueReload = 0;
    out.cancelled = 0;
    if (!in)
        return out;

    const int wasBusy = (g_state > VRRL_IDLE);

    // ---- the cancellation paths -----------------------------------------
    //
    // All of them land in the same place, and none of them issues a reload.
    // Listed rather than folded together so that adding a new one is a
    // one-line change in an obvious spot.
    const int mustCancel =
        !in->valid                                  // tracking lost
        || in->menuOpen                             // the player is elsewhere
        || in->weaponChanged                        // a different gun is in hand
        || (wasBusy && g_state != VRRL_RECOVER
            && (in->now - g_stateSince) > g_tune.timeoutSec);   // stalled

    if (mustCancel) {
        if (wasBusy) {
            Go(VRRL_RECOVER, in->now);
            ++g_cancels;
            out.cancelled = 1;
        }
        // A cancelled gesture returns to idle immediately rather than waiting
        // for the hand: the hand may be nowhere near the weapon, and holding
        // RECOVER would block the next attempt.
        g_state = VRRL_IDLE;
        g_prevGrip = in->gripHeld;
        out.state = g_state;
        return out;
    }

    if (!g_profile.valid || g_profile.cls == VRRL_CLASS_NONE
        || g_profile.cls == VRRL_CLASS_SINGLE_SHOT) {
        // Nothing to reload by hand. SINGLE_SHOT is deliberately included:
        // the panzerfaust is discarded after firing, so there is no gesture
        // to make and inventing one would fire the engine's reload at a
        // weapon that has none.
        g_state = VRRL_IDLE;
        g_prevGrip = in->gripHeld;
        out.state = g_state;
        return out;
    }

    const float dGrip  = Dist(in->handM, g_profile.firingGripM);
    const float dZone  = Dist(in->handM, g_profile.loadZoneM);
    const float dPouch = PouchDist(in->handWorldM);
    const int   gripEdgeDown = (in->gripHeld && !g_prevGrip);

    switch (g_state) {
    case VRRL_IDLE:
        // A pistol's magazine sits close to its support holding pose. A fresh
        // squeeze at contact, after leaving half the grip radius, is an
        // explicit request even when a full 10 cm reach is impossible.
        if (!g_profile.directLoad && g_profile.cls != VRRL_CLASS_INCREMENTAL
            && gripEdgeDown && dZone <= g_tune.zoneRadiusM
            && dGrip > g_tune.leaveGripM * 0.5f) {
            Go(VRRL_GRIPPED, in->now);
            break;
        }
        // Leaving the foregrip alone is ordinary free-hand motion. A reach
        // starts only at the measured loading point or, for receiver loaders,
        // at the ammunition pouch.
        if (dGrip > g_tune.leaveGripM
            && (dZone <= g_tune.zoneRadiusM
                || ((g_profile.directLoad || g_profile.cls == VRRL_CLASS_INCREMENTAL)
                    && dPouch <= g_tune.pouchRadiusM))) {
            // A receiver pickup may arrive with its fresh squeeze on this
            // very frame; preserve that edge instead of losing it in REACH.
            if ((g_profile.directLoad || g_profile.cls == VRRL_CLASS_INCREMENTAL)
                && dPouch <= g_tune.pouchRadiusM && gripEdgeDown)
                Go(VRRL_ACQUIRED, in->now);
            else
                Go(VRRL_REACH, in->now);
        }
        break;

    case VRRL_REACH:
        if (dGrip <= g_tune.leaveGripM * 0.5f) {
            g_state = VRRL_IDLE;              // thought better of it
        } else if (!g_profile.directLoad && g_profile.cls != VRRL_CLASS_INCREMENTAL
                   && dZone <= g_tune.zoneRadiusM && gripEdgeDown) {
            Go(VRRL_GRIPPED, in->now);
        } else if ((g_profile.directLoad || g_profile.cls == VRRL_CLASS_INCREMENTAL)
                   && dPouch <= g_tune.pouchRadiusM) {
            // A per-round loader has nothing to take OUT. The hand goes
            // straight for the ammunition, so the magazine-removal half of
            // the sequence is skipped rather than faked.
            Go(gripEdgeDown ? VRRL_ACQUIRED : VRRL_POUCH, in->now);
        }
        break;

    case VRRL_GRIPPED:
        if (!in->gripHeld) {
            g_state = VRRL_REACH;             // let go without pulling
        } else if (dZone >= g_tune.pullThresholdM) {
            Go(VRRL_PULLED, in->now);         // the magazine is out
        }
        break;

    case VRRL_PULLED:
        if (dPouch <= g_tune.pouchRadiusM)
            Go(VRRL_POUCH, in->now);
        break;

    case VRRL_POUCH:
        // A grip press at the pouch is what takes a fresh magazine. An
        // EDGE, not a level: a player who reached the pouch still holding the
        // old magazine must not acquire the new one without letting go first.
        if (gripEdgeDown && dPouch <= g_tune.pouchRadiusM)
            Go(VRRL_ACQUIRED, in->now);
        break;

    case VRRL_ACQUIRED:
        if (!in->gripHeld) {
            // Releasing in mid-air drops the carried ammunition. A fresh
            // pouch pickup is required before insertion can complete.
            Go(VRRL_REACH, in->now);
        } else if (dZone <= g_tune.zoneRadiusM)
            Go(VRRL_INSERT, in->now);
        break;

    case VRRL_INSERT:
        if (dZone > g_tune.zoneRadiusM) {
            Go(VRRL_ACQUIRED, in->now);       // pulled back out again
        } else if (!in->gripHeld && g_prevGrip) {
            // Releasing at the loading zone IS the insertion. This is the one
            // transition that issues the engine command, and it happens once.
            Go(VRRL_COMMIT, in->now);
            out.issueReload = 1;
            ++g_reloads;
        }
        break;

    case VRRL_COMMIT:
        // Exactly one frame. Nothing can re-enter COMMIT without going all
        // the way round, which is what makes double-fire structurally
        // impossible rather than a thing to remember.
        Go(VRRL_RECOVER, in->now);
        break;

    case VRRL_RECOVER:
        if (dGrip <= g_tune.leaveGripM)
            g_state = VRRL_IDLE;
        break;

    default:
        g_state = VRRL_IDLE;
        break;
    }

    g_prevGrip = in->gripHeld;
    out.state = g_state;
    return out;
}
