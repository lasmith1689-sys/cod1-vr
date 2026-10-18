# CoD1 VR — controller map

**Updated:** 2026-08-23, after the hardware run that threw three grenades and
switched weapons three times without being asked.

The governing rule of this map: **one physical control has exactly one owner,
and every one-shot game action is deliberate.** Nothing that a hand does
incidentally — resting a finger, flicking a stick diagonally, re-acquiring
tracking — may commit an action the player did not ask for.

---

## Right hand (dominant; mirrored when `vr_dominant_hand left`)

| Control | Action | Notes |
|---|---|---|
| Trigger | **Fire** | Level-driven, so automatics hold and a dead hand releases by itself |
| Grip / squeeze | **Weapon hold** — an accuracy/ADS owner | Level, never an edge |
| Stick X | **Turn** (smooth or snap, `vr_turn_mode`) | |
| Stick Y | *nothing* | **Deliberately inert** — see below |
| A | Jump | |
| B | Tap = crouch, hold ≥ 0.4 s = prone | |
| Stick click | Melee | |

## Left hand (off-hand)

| Control | Action | Notes |
|---|---|---|
| Stick | **Locomotion** | Direction from HMD or off-hand, `vr_move_direction` |
| Grip / squeeze | **Grab / support** — two-hand brace, and the reload interaction hand | |
| Trigger | **Grenade**, on a deliberate hold | Must be held `vr_grenade_hold` (0.35 s) |
| X | **Weapon cycle** (weapnext) | One press, one switch |
| Y | Use / activate | Level |
| Stick click | Recenter | |

## Either hand

| Control | Action |
|---|---|
| Menu button | ESC / menu |

---

## What changed, and why

### The grenade was on a bare off-hand trigger edge

`UpdateButtons` did `if (edges && oh.triggerPressed) Tap(bindGrenade)`. The
off-hand trigger is the most reflexively squeezed control on a Touch
controller, and the CoD1 grenade bind is **weapon slot `4`** — so a reflex
squeeze did not throw a grenade so much as *switch the weapon to grenades*,
which is exactly what the player reported.

The injection ring from the run says it plainly — every `L.trigger=1` is
immediately followed by a `'4'` at the same update counter:

```
u9306 R.trigger=1 | u9316 L.trigger=1 | u10748 L.trigger=1 | u12290 L.trigger=1
u9306 BDOWN MOUSE1 | u9316 DOWN '4' | u9316 UP '4'
                   | u10748 DOWN '4' | u10748 UP '4'
                   | u12290 DOWN '4' | u12290 UP '4'
```

Three left-trigger presses, three grenades, in seventy seconds.

Now: the off-hand trigger is the only grenade source in the build, it must be
**held** for `vr_grenade_hold` seconds, it arms only after a clean release has
actually been *witnessed by a live, tracked hand*, and one physical press can
commit at most one grenade.

### The weapon cycle was on the turn stick

`UpdateWeaponCycle` fired on dominant `|stickY| > 0.7`. The dominant stick is
the **turn** stick; a diagonal flick while turning clears 0.7 on Y trivially.
And CoD's `weapnext` walks *through the grenade slot* — so this was the second,
independent way the run kept ending up holding grenades. The same ring shows
three `WHEEL wheel+` events, each right after a right-stick push:

```
u1680 R.stick +19%/+86%  ->  u1680 WHEEL wheel+
u1978 R.stick +47%/+86%  ->  u1978 WHEEL wheel+
u4808 R.stick +67%/+68%  ->  u4812 WHEEL wheel+
```

Dominant stick Y is now **completely inert**. The cycle moved to off-hand X, a
discrete button that nothing else reads. The old behaviour survives only behind
`vr_weapon_cycle_binding stick`, so the regression suite can still drive it.

### Reload moved off X

X was reload; it is now the weapon cycle. Reload is on the reload binding for
now (`bindReload`, default `R`) and is the intended home of the physical
left-hand reload gesture once there is a mechanism to drive the hands — see
`DOBJ-PROBE-RESULT-2026-08-23.md` for why that is currently blocked.

### Re-acquiring tracking can no longer manufacture a press

Two holes, both closed:

* **Analog** (`grenade`): a hand that is absent reads its trigger as `0`. That
  was being treated as a *release*, which re-armed the grenade — so a hand that
  dropped out with the trigger held came back armed and threw one. An absent
  hand now loses its arm outright, and only a hand that is present **and**
  stably tracked can witness the release that re-arms it.
* **Boolean** (`BoolEdge`, so X / Y / A / B / stick-click): the same decay to
  `0` produced a rising edge on the first live sync after a dropout. A hand
  must now be seen holding the button down for one live sync before an edge is
  believed. Costs one sync on a real press; kills the artefact.

Focus changes, tracking transitions and menu boundaries all disarm every
deliberate action and start a 0.25 s holdoff.

---

## Counters

The recurring input block now reports, so this is checkable on hardware
instead of by feel:

```
GRENADE (deliberate off-hand trigger hold, 0.35 s):
  grenadeRequests N   grenadeAccepted N   grenadeRejectedNoise N
  grenadeSource ...   lastReject ...   armed N
  weapon-cycle source: off-hand X button (...)
```

**If the player never deliberately held the grenade control,
`grenadeAccepted` must read 0.** That is the acceptance test.

---

## Config keys

| Key | Default | Meaning |
|---|---|---|
| `vr_grenade_hold` | `0.35` | Seconds the off-hand trigger must be held. `0` restores instant-on-press, which is the old bug. |
| `vr_weapon_cycle_binding` | `button` | `button` = off-hand X. `stick` = legacy dominant stick Y (not recommended). |
| `vr_dominant_hand` | `right` | Mirrors the whole map. |
