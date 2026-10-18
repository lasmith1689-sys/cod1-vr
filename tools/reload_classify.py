"""Reload classification: what the motion is, and which hand performs it.

Split out from extract_reload.py so the decision rules can be read -- and
tested -- without the asset plumbing around them.

WHICH HAND MATTERS AS MUCH AS WHICH CLASS
-----------------------------------------
A magazine weapon is reloaded by the SUPPORT hand: it leaves the forestock,
fetches a magazine, and brings it to the weapon while the trigger hand keeps
hold of the grip. A bolt-action rifle is the other way round -- the support
hand cradles the forestock and barely moves, and the TRIGGER hand works the
bolt and presses the charger in.

The first version of this classifier tracked only the support hand, and so
read every bolt-action rifle as "the hand did not move" and fell through to a
guess. Both hands are measured now, and which one moves is itself the
evidence that separates a fetched magazine from a pressed-in clip.
"""

UNKNOWN, NA, WEAK, STRONG, PROVEN = "UNKNOWN", "NA", "WEAK", "STRONG", "PROVEN"

# HOW THE MOTION IS STRUCTURED. Deliberately coarse: these four shapes are
# what the gesture machine has to drive, and each is decided by an authored
# signal rather than by a measurement that might be noise.
RC_NONE        = "NONE"          # nothing to reload (grenade, turret, prop)
RC_MAGAZINE    = "MAGAZINE"      # magazine or clip swapped whole, one motion
RC_INCREMENTAL = "INCREMENTAL"   # one round at a time, start/loop/end
RC_SINGLE_SHOT = "SINGLE_SHOT"   # one projectile, whole weapon reloaded

# WHICH HAND PERFORMS IT. Measured, never assumed.
HAND_NONE    = "NONE"
HAND_SUPPORT = "SUPPORT"
HAND_TRIGGER = "TRIGGER"
HAND_BOTH    = "BOTH"

# WHAT THE PLAYER IS HOLDING. Orthogonal to the class: a Bren's detachable
# magazine enters from ABOVE, which makes it no less a detachable magazine.
MT_NONE       = "NONE"
MT_DETACHABLE = "DETACHABLE"     # box magazine fetched from a pouch
MT_STRIPPER   = "STRIPPER_CLIP"  # charger pressed into a fixed magazine
MT_ENBLOC     = "EN_BLOC"        # clip that stays in the weapon until spent
MT_LOOSE      = "LOOSE_ROUNDS"   # fed individually
MT_SINGLE     = "SINGLE"         # the weapon is the round

# A hand holding still still wobbles. Below this, an excursion is not evidence
# that the hand went anywhere.
NOISE_M   = 0.03
STRONG_M  = 0.03
PROVEN_M  = 0.08
# One hand must out-travel the other by this factor before it is called the
# loading hand; otherwise both are moving and neither is "the" one.
DOMINANCE = 1.5


def zone_conf(reach):
    """How much a measured excursion is worth."""
    if reach >= PROVEN_M:
        return PROVEN
    if reach >= STRONG_M:
        return STRONG
    return WEAK


def which_hand(track_present, lreach, rreach, anim_name):
    """(hand, confidence, evidence) from the two measured excursions."""
    if not track_present:
        return HAND_SUPPORT, UNKNOWN, "reload animation not poseable"
    if max(lreach, rreach) < NOISE_M:
        return (HAND_SUPPORT, UNKNOWN,
                "neither hand moves more than %d cm in %s"
                % (int(NOISE_M * 100), anim_name))
    if lreach >= rreach * DOMINANCE:
        return (HAND_SUPPORT, PROVEN if lreach >= PROVEN_M else STRONG,
                "support hand travels %.3f m vs the trigger hand's %.3f m"
                % (lreach, rreach))
    if rreach >= lreach * DOMINANCE:
        return (HAND_TRIGGER, PROVEN if rreach >= PROVEN_M else STRONG,
                "trigger hand travels %.3f m vs the support hand's %.3f m"
                % (rreach, lreach))
    return (HAND_BOTH, STRONG,
            "both hands travel comparably (%.3f m / %.3f m)" % (lreach, rreach))


def classify(fields, track_present, lreach, rreach, wclass, wtype, anim_name):
    """(rclass, rconf, revidence, hand, hconf, hevidence)."""
    reload_anim = fields.get("reloadAnim", "")
    start_anim  = fields.get("reloadStartAnim", "")
    end_anim    = fields.get("reloadEndAnim", "")
    clip        = fields.get("clipSize", "")

    if wclass in ("turret", "non-player") or wtype == "grenade":
        return (RC_NONE, NA, "class '%s' / type '%s' has no reload" % (wclass, wtype),
                HAND_NONE, NA, "no reload to perform")
    if not reload_anim:
        return (RC_NONE, PROVEN, "weapon file names no reloadAnim",
                HAND_NONE, NA, "no reload to perform")

    hand, hconf, hev = which_hand(track_present, lreach, rreach, anim_name)

    # A weapon whose reloadAnim IS an idle does not reload in view. The
    # panzerfaust (one rocket, then the tube is discarded) and the Stalingrad
    # ammo crate (a prop wearing a rifle's weapon file) both do this, and both
    # would otherwise be classified off a motionless clip.
    if reload_anim.lower().endswith("_idle"):
        if clip == "1" or wclass == "rocketlauncher":
            return (RC_SINGLE_SHOT, STRONG,
                    "clipSize %s, and the game's reloadAnim is the idle '%s': "
                    "nothing is reloaded in view" % (clip, reload_anim),
                    HAND_NONE, NA, "no loading motion is animated")
        return (RC_NONE, STRONG,
                "reloadAnim '%s' is an idle -- this entry does not reload in view"
                % reload_anim,
                HAND_NONE, NA, "no loading motion is animated")

    if start_anim and end_anim:
        try:
            timed = (float(fields.get("reloadStartTime") or 0) > 0.0
                     or float(fields.get("reloadEndTime") or 0) > 0.0)
        except ValueError:
            timed = False
        return (RC_INCREMENTAL, PROVEN if timed else STRONG,
                "reloadStartAnim+reloadEndAnim present%s -- the engine repeats "
                "one feeding motion per round"
                % (" and reloadStart/EndTime nonzero" if timed else ""),
                hand, hconf, hev)

    if clip == "1" or wclass == "rocketlauncher":
        return (RC_SINGLE_SHOT, STRONG,
                "clipSize %s / class '%s': the whole weapon is the round"
                % (clip, wclass),
                hand, hconf, hev)

    return (RC_MAGAZINE, PROVEN if max(lreach, rreach) >= PROVEN_M else STRONG,
            "one reload motion, no start/end loop -- a magazine or clip is "
            "swapped whole",
            hand, hconf, hev)


def magazine_type(fields, rclass, hand, reach):
    """(type, confidence, evidence) -- what physically enters the weapon.

    Separate from the CLASS on purpose. The class says how the motion is
    structured; this says what the player is holding. Conflating them is what
    made an earlier pass read a Bren -- whose detachable magazine goes in from
    above -- as a bolt-action rifle.
    """
    bolt = fields.get("boltAction", "")
    clip = fields.get("clipSize", "")
    if rclass == RC_NONE:
        return MT_NONE, NA, "nothing is loaded"
    if rclass == RC_SINGLE_SHOT:
        return MT_SINGLE, STRONG, "one projectile, carried by the weapon itself"
    if rclass == RC_INCREMENTAL:
        return MT_LOOSE, PROVEN, "the engine feeds one round per loop iteration"
    if bolt == "1":
        return (MT_STRIPPER, STRONG if hand == HAND_TRIGGER else WEAK,
                "bolt-action with clipSize %s, loaded by the %s hand"
                % (clip, hand.lower()))
    if hand == HAND_TRIGGER:
        # KNOWN TO MISFIRE, AND LEFT WEAK BECAUSE OF IT. The Bren's magazine
        # is detachable but sits on TOP, so its animation has the trigger hand
        # do the swap while the support hand holds the carry handle -- and
        # this rule calls it a clip. The reach separates the cases in the
        # shipped data (Bren 0.56 m out to a pouch, Garand 0.43 m, Kar98k
        # 0.21 m), but a threshold picked to reproduce an answer already
        # believed is not evidence, so none is applied. WEAK values cannot
        # become production constants, which is exactly the right outcome:
        # nothing downstream needs the magazine type, and everything that
        # does need something -- which hand, which zone, one motion or a loop
        # -- is measured.
        return (MT_ENBLOC, WEAK,
                "single motion, clipSize %s, performed by the TRIGGER hand while "
                "the support hand cradles the weapon -- consistent with a clip "
                "pressed in, but a top-mounted detachable magazine animates the "
                "same way (see the Bren)" % clip)
    if hand in (HAND_BOTH, HAND_NONE) or reach < NOISE_M:
        return (MT_DETACHABLE, WEAK,
                "single motion, but no hand clearly performs it (%s, %.3f m) -- "
                "inference only" % (hand.lower(), reach))
    return (MT_DETACHABLE, PROVEN if reach >= PROVEN_M else STRONG,
            "single motion performed by the support hand, which travels %.3f m "
            "away from the weapon to fetch it" % reach)
