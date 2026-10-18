# Physical reload controls

The right controller continues to carry and aim the weapon. The free left
hand performs the reload gesture, including on rifles whose original flat
animation uses the right hand. Gun placement, palm calibration and sights
are unchanged.

`vr_manual_reload_mode 2` enables gestures for measured weapons. A short X
press remains a stock reload fallback for an unknown or unmeasured weapon.
Mode `1` keeps that short-X fallback for every weapon; mode `0` disables
physical reload gestures. Holding X still cycles the weapon.

In mode `2`, grabbing/removing a magazine or carrying ammunition suspends
trigger firing. After insertion or cancellation, release and press the
trigger again to fire; holding it throughout the gesture cannot produce an
accidental shot. Simply reaching with the free hand does not interrupt fire.
Mode `1` retains the original trigger behavior during gestures.

## Detachable magazine weapons

For the Thompson, BAR, Bren, Sten, FG42, MP40, MP44, PPSh, Colt and Luger:

1. Release the left support grip and move the left hand to the magazine.
2. Squeeze the left grip there, then pull at least 15 cm away while holding it.
3. Reach the ammo pouch: about 45 cm below and 15 cm in front of the head.
   Release the grip, then squeeze again inside the pouch to acquire ammo.
4. Bring the hand back to the weapon's insertion point while holding the grip.
5. Release at the insertion point. This requests exactly one stock reload.

The Bren's loading zone is above the receiver; Sten and FG42 use their side
magazines. Loading points come from the stock animation at its ammunition
transfer event, expressed relative to the weapon body. Earlier profiles
used the hand's farthest excursion, which frequently put the zone off the
weapon; those extrema are no longer the loading target.

## Clips and receiver loaders

For Garand, Kar98, Mosin rifles, Springfield and Enfield, start directly at
the pouch. Squeeze the left grip to acquire ammunition, hold while moving
to the receiver, then release there. No detachable-magazine removal is
invented for these weapons.

Short haptic pulses mark a magazine grab, pull, pouch acquisition, insertion
contact and completed gesture. They respect `vr_haptics` and
`vr_haptics_strength`. The left rendered hand follows its controller during
the gesture rather than remaining locked to the foregrip.

The loading zone accepts a 12 cm radius and the pouch a 22 cm radius.
An already-held squeeze cannot start a pickup, a grip press outside the
pouch cannot acquire ammo, and releasing carried ammo before insertion
requires another pickup. A weapon switch, tracking loss, menu entry or an
eight-second stall cancels without issuing a reload.

## Engine behavior retained

Completion starts the game's normal reload animation and normal ammunition
transfer timing. The engine continues to own reserves, clip capacity,
per-round loops, charging and bolt animations. This implementation does not
render a detached magazine, change ammunition in mid-gesture, simulate
chamber state, require a manual bolt cycle after every shot, or block firing
outside the concrete ammunition-handling stages in mode `2`. Automatic
reload remains owned by the stock engine; no supported switch was found in
the installed single-player cvar/config mechanism to disable it per weapon.

Grenades, mounted weapons, the disposable Panzerfaust and unmeasured models
have no physical reload. M1 Carbine keeps the stock fallback because its
original reload uses both hands and the existing profile is ambiguous.
Scoped and unscoped Kar98 share a receiver/body key and therefore use the
same measured receiver zone.
