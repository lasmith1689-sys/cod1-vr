// Pair-latched object poses -- implementation. See vr_objects.h for why.

#include "vr_objects.h"

#include <string.h>

static VRObjectSet g_set;
static unsigned    g_eyeSerial[2];
static int         g_eyeSeen[2];
static int         g_pairOpen;

// Which parts of the CURRENT pair have already been sampled. A second sample
// of the same thing inside one pair is refused, not applied: that is the whole
// point -- a controller that moved between eye 0 and eye 1 must not be able to
// reach the objects, no matter who calls what.
static int g_sampled[2 + VROBJ_HANDS];   // head, leftState, grip[0], grip[1]
enum { SAMP_HEAD = 0, SAMP_LEFTSTATE = 1, SAMP_GRIP0 = 2 };

static unsigned g_pairs, g_mismatches, g_lateSamples, g_eyeSplits;

static void PoseSet(VRObjPose* p, const float pos[3], const float axis[9], int valid)
{
    if (pos)  memcpy(p->pos,  pos,  3 * sizeof(float));
    if (axis) memcpy(p->axis, axis, 9 * sizeof(float));
    p->valid = valid;
}

void VRObj_Reset()
{
    memset(&g_set, 0, sizeof(g_set));
    memset(g_eyeSerial, 0, sizeof(g_eyeSerial));
    memset(g_eyeSeen, 0, sizeof(g_eyeSeen));
    memset(g_sampled, 0, sizeof(g_sampled));
    g_pairOpen = 0;
    g_pairs = g_mismatches = g_lateSamples = g_eyeSplits = 0;
}

void VRObj_BeginPair(unsigned pairSerial)
{
    // A new pair clears the "already sampled" flags -- and ONLY a new pair
    // does. Nothing else in this file reopens them.
    memset(g_sampled, 0, sizeof(g_sampled));
    memset(g_eyeSeen, 0, sizeof(g_eyeSeen));
    g_set.pairSerial = pairSerial;
    g_set.valid = 1;
    g_pairOpen = 1;
    ++g_pairs;
}

// Returns 1 if this slot may be written now, and marks it written.
static int ClaimSlot(int slot)
{
    if (!g_pairOpen)     { ++g_lateSamples; return 0; }
    if (g_sampled[slot]) { ++g_lateSamples; return 0; }
    g_sampled[slot] = 1;
    return 1;
}

void VRObj_LatchHead(const float pos[3], const float axis[9], int valid)
{
    if (!ClaimSlot(SAMP_HEAD)) return;
    PoseSet(&g_set.head, pos, axis, valid);
}

void VRObj_LatchGrip(int hand, const float pos[3], const float axis[9], int tracked)
{
    if (hand < 0 || hand >= VROBJ_HANDS) return;
    if (!ClaimSlot(SAMP_GRIP0 + hand)) return;
    PoseSet(&g_set.grip[hand], pos, axis, tracked);
    g_set.handTracked[hand] = tracked;
}

void VRObj_LatchLeftState(int state, int twoHand, float supportT, float residualCm)
{
    if (!ClaimSlot(SAMP_LEFTSTATE)) return;
    if (state < 0 || state >= VROBJ_LEFT_STATES) state = VROBJ_LEFT_FREE;
    g_set.leftState = state;
    g_set.twoHand = twoHand ? 1 : 0;
    g_set.supportT = supportT;
    g_set.supportResidualCm = residualCm;
}

// The solved poses are stamped with the pair serial as they are published. A
// solve that runs outside a pair leaves the previous serial in place, so
// VRObj_PairConsistent() reports the disagreement instead of the renderer
// inheriting a stale pose without noticing.
void VRObj_SetHandPose(int hand, const float pos[3], const float axis[9])
{
    if (hand < 0 || hand >= VROBJ_HANDS) return;
    PoseSet(&g_set.hand[hand], pos, axis, 1);
    if (g_pairOpen) g_set.handSerial[hand] = g_set.pairSerial;
}

void VRObj_SetGunPose(const float pos[3], const float axis[9])
{
    PoseSet(&g_set.gun, pos, axis, 1);
    if (g_pairOpen) g_set.gunSerial = g_set.pairSerial;
}

// A pair with nothing to publish must say so: leaving last pair's pose
// marked valid is exactly the stale-pose inheritance PairConsistent exists
// to catch (and the 10k production soak DID catch, 2026-08-28).
void VRObj_ClearHandPose(int hand)
{
    if (hand < 0 || hand >= VROBJ_HANDS) return;
    g_set.hand[hand].valid = 0;
    if (g_pairOpen) g_set.handSerial[hand] = g_set.pairSerial;
}

void VRObj_ClearGunPose()
{
    g_set.gun.valid = 0;
    if (g_pairOpen) g_set.gunSerial = g_set.pairSerial;
}

const VRObjectSet* VRObj_Set() { return &g_set; }

int VRObj_PairConsistent()
{
    if (!g_set.valid) return 0;
    if (g_set.gun.valid && g_set.gunSerial != g_set.pairSerial) return 0;
    for (int h = 0; h < VROBJ_HANDS; ++h)
        if (g_set.hand[h].valid && g_set.handSerial[h] != g_set.pairSerial)
            return 0;
    return 1;
}

void VRObj_NoteEyeConsumed(int eye)
{
    if (eye < 0 || eye > 1) return;

    // SEAL the pair. Refusing a second sample of the same slot is not enough
    // on its own: a slot nobody had filled yet would still be a way in, so a
    // controller that started tracking between the eyes could change the set
    // underneath eye 1. Once an eye has rendered from this set, every slot is
    // closed until the next VRObj_BeginPair.
    for (int i = 0; i < (int)(sizeof(g_sampled) / sizeof(g_sampled[0])); ++i)
        g_sampled[i] = 1;

    g_eyeSerial[eye] = g_set.pairSerial;
    g_eyeSeen[eye] = 1;
    if (!VRObj_PairConsistent()) ++g_mismatches;
    // Both eyes seen: they must have consumed the SAME latch. This is the
    // check that would have caught the camera bug in 2026-08-23 had the
    // camera carried a serial at the time.
    if (g_eyeSeen[0] && g_eyeSeen[1] && g_eyeSerial[0] != g_eyeSerial[1])
        ++g_eyeSplits;
}

unsigned VRObj_EyeConsumedSerial(int eye)
{
    return (eye >= 0 && eye <= 1) ? g_eyeSerial[eye] : 0u;
}

void VRObj_GetCounters(unsigned* pairs, unsigned* mismatches,
                       unsigned* lateSamples, unsigned* eyeSerialSplits)
{
    if (pairs)           *pairs           = g_pairs;
    if (mismatches)      *mismatches      = g_mismatches;
    if (lateSamples)     *lateSamples     = g_lateSamples;
    if (eyeSerialSplits) *eyeSerialSplits = g_eyeSplits;
}
