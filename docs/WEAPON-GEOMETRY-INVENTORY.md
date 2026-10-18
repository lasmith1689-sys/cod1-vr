# Weapon geometry inventory

Derived from the shipped CoD1 pk3s by `tools/extract_geometry.py`. Every number here is an **asset-derived prior**, not a hardware measurement: it says where the game's own artists put the hands, the muzzle and the sight line relative to each weapon, which is a far better starting point than AUTO but is still subject to the headset check.

Units are inches (CoD units; 1 unit = 1 inch). Vectors are in **tag_weapon space** unless stated: +x forward along the weapon, +y left, +z up, matching the engine's Quake axes.


## Confidence roll-up

| field | PROVEN | STRONG | WEAK | UNKNOWN |
|---|---|---|---|---|
| muzzleOrigin | 0 | 27 | 0 | 16 |
| barrelForward | 24 | 3 | 0 | 16 |
| rearGripOffset | 0 | 28 | 5 | 15 |
| rearGripRotation | 0 | 28 | 5 | 15 |
| supportAlongAxis | 0 | 25 | 2 | 16 |
| supportRadius | 0 | 25 | 2 | 16 |
| sightOffset | 0 | 29 | 0 | 19 |
| sightForward | 0 | 29 | 0 | 19 |

## Per-weapon semantic tags

| weapon | gun parts | tags found on the gun rig |
|---|---|---|
| 30cal | viewmodel_bar2 | tag_weapon, tag_brass, tag_flash |
| 30cal_prone | - | - |
| 30cal_stand | - | - |
| bar | viewmodel_bar2 | tag_weapon, tag_brass, tag_flash |
| bar_slow | viewmodel_bar2 | tag_weapon, tag_brass, tag_flash |
| bren | viewmodel_bren2 | tag_weapon, tag_brass, tag_flash |
| colt | viewmodel_colt452 | tag_origin, tag_brass, tag_flash |
| enfield | viewmodel_enfield2 | tag_weapon, tag_brass, tag_flash |
| fg42 | viewmodel_fg422 | tag_weapon, tag_brass, tag_flash |
| fg42_auto | viewmodel_fg422 | tag_weapon, tag_brass, tag_flash |
| flak88_turret | - | - |
| flakvierling_turret | - | - |
| fraggrenade | mk2fraggrenade_viewmodel2 | tag_weapon, tag_flash |
| kar98k | viewmodel_kar982 | tag_weapon, tag_brass, tag_flash |
| kar98k_pavlovsniper | viewmodel_kar982 | tag_weapon, tag_brass, tag_flash |
| kar98k_sniper | viewmodel_kar98scoped2 | tag_weapon, tag_brass, tag_flash |
| luger | viewmodel_luger2 | tag_weapon, tag_brass, tag_flash |
| m1carbine | viewmodel_m1carbine2 | tag_origin, tag_brass, tag_flash |
| m1carbine_medic | viewmodel_m1carbine2 | tag_origin, tag_brass, tag_flash |
| m1garand | viewmodel_m1garand2 | tag_weapon, tag_brass, tag_flash |
| mg42_bipod_duck | - | - |
| mg42_bipod_prone | - | - |
| mg42_bipod_stand | - | - |
| mg42_bipod_unmanned | - | - |
| mg42_panzeriv_tank | - | - |
| mg42_tiger_tank | - | - |
| mk1britishfrag | viewmodel_mk12 | tag_weapon, tag_flash |
| mosin_nagant | viewmodel_mosinnagant2 | tag_weapon, tag_brass, tag_flash |
| mosin_nagant_sniper | viewmodel_mosinnagantscoped2 | tag_origin, tag_brass, tag_flash |
| mp40 | viewmodel_mp402 | tag_origin, tag_flash, tag_brass |
| mp44 | viewmodel_mp442 | tag_weapon, tag_brass, tag_flash |
| mp44_semi | viewmodel_mp442 | tag_weapon, tag_brass, tag_flash |
| panzerfaust | viewmodel_panzerfaust2 | tag_origin, tag_brass, tag_flash |
| panzeriv_turret | - | - |
| ppsh | viewmodel_ppsh2 | tag_origin, tag_brass, tag_flash |
| ppsh_semi | viewmodel_ppsh2 | tag_origin, tag_brass, tag_flash |
| ptrs41_antitank_rifle | - | - |
| rgd-33russianfrag | viewmodel_rgd332 | tag_weapon, tag_flash |
| springfield | viewmodel_springfield2 | tag_origin, tag_brass, tag_flash |
| stalingrad_ammo | viewmodel_ammo2 | tag_weapon, tag_brass, tag_flash |
| sten | viewmodel_sten2 | tag_weapon, tag_flash |
| sten_engineer | viewmodel_sten2 | tag_weapon, tag_flash |
| stielhandgranate | viewmodel_nebelhandgrenate2 | tag_weapon |
| stuka_guns | - | - |
| t34_turret | - | - |
| thompson | viewmodel_thompson2 | tag_origin, tag_brass, tag_flash |
| thompson_semi | viewmodel_thompson2 | tag_origin, tag_brass, tag_flash |
| tiger_turret | - | - |

## Muzzle and barrel axis

| weapon | frame bone | muzzle tag | muzzle origin (in) | dist | barrel forward | worst pair-disagreement deg | conf |
|---|---|---|---|---|---|---|---|
| 30cal | tag_weapon | tag_flash | (38.962, 0.044, -0.610) | 38.967 | (1.000, 0.001, -0.016) | 1.06 (BodyAabb~Rotation) | PROVEN |
| 30cal_prone | - | - | AUTO | - | AUTO | - | UNKNOWN |
| 30cal_stand | - | - | AUTO | - | AUTO | - | UNKNOWN |
| bar | tag_weapon | tag_flash | (38.962, 0.044, -0.610) | 38.967 | (1.000, 0.001, -0.016) | 1.06 (BodyAabb~Rotation) | PROVEN |
| bar_slow | tag_weapon | tag_flash | (38.962, 0.044, -0.610) | 38.967 | (1.000, 0.001, -0.016) | 1.06 (BodyAabb~Rotation) | PROVEN |
| bren | tag_weapon | tag_flash | (37.403, 0.283, -1.690) | 37.442 | (0.999, 0.008, -0.045) | 2.92 (Position~Rotation) | PROVEN |
| colt | tag_weapon | tag_flash | (8.114, 0.212, -0.456) | 8.130 | (0.998, 0.026, -0.056) | 14.41 (BodyAabb~Rotation) | STRONG |
| enfield | tag_weapon | tag_flash | (38.278, 0.429, -0.186) | 38.281 | (1.000, 0.011, -0.005) | 1.41 (BodyAabb~Position) | PROVEN |
| fg42 | tag_weapon | tag_flash | (36.891, 0.572, -0.999) | 36.909 | (1.000, 0.016, -0.027) | 1.81 (Position~Rotation) | PROVEN |
| fg42_auto | tag_weapon | tag_flash | (36.891, 0.572, -0.999) | 36.909 | (1.000, 0.016, -0.027) | 1.81 (Position~Rotation) | PROVEN |
| flak88_turret | - | - | AUTO | - | AUTO | - | UNKNOWN |
| flakvierling_turret | - | - | AUTO | - | AUTO | - | UNKNOWN |
| fraggrenade | tag_weapon | - | AUTO | - | AUTO | - | N/A |
| kar98k | tag_weapon | tag_flash | (36.433, 0.041, -0.016) | 36.434 | (1.000, 0.001, -0.000) | 0.73 (Position~Rotation) | PROVEN |
| kar98k_pavlovsniper | tag_weapon | tag_flash | (36.433, 0.041, -0.016) | 36.434 | (1.000, 0.001, -0.000) | 0.73 (Position~Rotation) | PROVEN |
| kar98k_sniper | tag_weapon | tag_flash | (36.433, 0.041, -0.016) | 36.434 | (1.000, 0.001, -0.000) | 0.73 (Position~Rotation) | PROVEN |
| luger | tag_weapon | tag_flash | (7.924, 0.165, -1.033) | 7.992 | (0.991, 0.021, -0.129) | 5.75 (BodyAabb~Position) | PROVEN |
| m1carbine | m1carbine | tag_flash | (0.105, 0.220, 31.811) | 31.812 | (0.003, 0.007, 1.000) | 1.81 (Position~Rotation) | PROVEN |
| m1carbine_medic | m1carbine | tag_flash | (0.105, 0.220, 31.811) | 31.812 | (0.003, 0.007, 1.000) | 1.81 (Position~Rotation) | PROVEN |
| m1garand | tag_weapon | tag_flash | (32.407, 0.128, -0.624) | 32.413 | (1.000, 0.004, -0.019) | 1.24 (Position~Rotation) | PROVEN |
| mg42_bipod_duck | - | - | AUTO | - | AUTO | - | UNKNOWN |
| mg42_bipod_prone | - | - | AUTO | - | AUTO | - | UNKNOWN |
| mg42_bipod_stand | - | - | AUTO | - | AUTO | - | UNKNOWN |
| mg42_bipod_unmanned | - | - | AUTO | - | AUTO | - | UNKNOWN |
| mg42_panzeriv_tank | - | - | AUTO | - | AUTO | - | UNKNOWN |
| mg42_tiger_tank | - | - | AUTO | - | AUTO | - | UNKNOWN |
| mk1britishfrag | tag_weapon | - | AUTO | - | AUTO | - | N/A |
| mosin_nagant | tag_weapon | tag_flash | (42.685, -0.006, -0.394) | 42.687 | (1.000, -0.000, -0.009) | 1.03 (BodyAabb~Position) | PROVEN |
| mosin_nagant_sniper | tag_weapon | tag_flash | (42.759, -0.006, -0.394) | 42.760 | (1.000, -0.000, -0.009) | 1.03 (BodyAabb~Position) | PROVEN |
| mp40 | tag_weapon | tag_flash | (23.092, 0.045, -0.243) | 23.094 | (1.000, 0.002, -0.011) | 1.80 (BodyAabb~Rotation) | PROVEN |
| mp44 | tag_weapon | tag_flash | (28.681, 0.339, -1.047) | 28.702 | (0.999, 0.012, -0.036) | 2.32 (BodyAabb~Position) | PROVEN |
| mp44_semi | tag_weapon | tag_flash | (28.681, 0.339, -1.047) | 28.702 | (0.999, 0.012, -0.036) | 2.32 (BodyAabb~Position) | PROVEN |
| panzerfaust | panzerfaust | - | AUTO | - | AUTO | - | UNKNOWN |
| panzeriv_turret | - | - | AUTO | - | AUTO | - | UNKNOWN |
| ppsh | tag_weapon | tag_flash | (17.539, 0.313, 0.857) | 17.563 | (0.999, 0.018, 0.049) | 4.64 (Position~Rotation) | PROVEN |
| ppsh_semi | tag_weapon | tag_flash | (17.539, 0.313, 0.857) | 17.563 | (0.999, 0.018, 0.049) | 4.64 (Position~Rotation) | PROVEN |
| ptrs41_antitank_rifle | - | - | AUTO | - | AUTO | - | UNKNOWN |
| rgd-33russianfrag | tag_weapon | - | AUTO | - | AUTO | - | N/A |
| springfield | tag_weapon | tag_flash | (36.391, -0.002, -0.324) | 36.392 | (1.000, -0.000, -0.009) | 0.72 (BodyAabb~Position) | PROVEN |
| stalingrad_ammo | tag_weapon | - | AUTO | - | AUTO | - | N/A |
| sten | tag_weapon | tag_flash | (20.361, 0.045, -0.243) | 20.363 | (1.000, 0.002, -0.012) | 1.00 (BodyAabb~Position) | PROVEN |
| sten_engineer | tag_weapon | tag_flash | (20.361, 0.045, -0.243) | 20.363 | (1.000, 0.002, -0.012) | 1.00 (BodyAabb~Position) | PROVEN |
| stielhandgranate | tag_weapon | - | AUTO | - | AUTO | - | N/A |
| stuka_guns | - | - | AUTO | - | AUTO | - | UNKNOWN |
| t34_turret | - | - | AUTO | - | AUTO | - | UNKNOWN |
| thompson | tag_weapon | tag_flash | (16.083, -0.212, 1.649) | 16.169 | (0.995, -0.013, 0.102) | 7.80 (Position~Rotation) | STRONG |
| thompson_semi | tag_weapon | tag_flash | (16.083, -0.212, 1.649) | 16.169 | (0.995, -0.013, 0.102) | 7.80 (Position~Rotation) | STRONG |
| tiger_turret | - | - | AUTO | - | AUTO | - | UNKNOWN |

## Grips

| weapon | rear grip offset (in) | wander | conf | support offset (in) | along | radius | conf |
|---|---|---|---|---|---|---|---|
| 30cal | (-1.160, -1.591, -1.602) | - | STRONG | (13.862, 1.397, -4.144) | 13.926 | 4.162 | STRONG |
| 30cal_prone | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| 30cal_stand | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| bar | (-1.160, -1.591, -1.602) | - | STRONG | (13.862, 1.397, -4.144) | 13.926 | 4.162 | STRONG |
| bar_slow | (-1.160, -1.591, -1.602) | - | STRONG | (13.862, 1.397, -4.144) | 13.926 | 4.162 | STRONG |
| bren | (1.494, -1.388, -4.590) | - | STRONG | (16.263, 4.857, -4.660) | 16.492 | 6.146 | STRONG |
| colt | (-3.064, -0.426, -3.805) | - | STRONG | (-2.532, 1.672, -6.885) | -2.098 | 7.226 | WEAK |
| enfield | (-1.163, -1.590, -1.837) | - | STRONG | (14.276, 1.512, -3.456) | 14.309 | 3.646 | STRONG |
| fg42 | (4.107, -1.903, -5.208) | - | STRONG | (12.767, 2.296, -3.384) | 12.888 | 3.691 | STRONG |
| fg42_auto | (4.107, -1.903, -5.208) | - | STRONG | (12.767, 2.296, -3.384) | 12.888 | 3.691 | STRONG |
| flak88_turret | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| flakvierling_turret | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| fraggrenade | (0.618, -2.159, -3.646) | - | WEAK | (-25.943, 13.745, -3.773) | - | - | WEAK |
| kar98k | (-1.164, -1.591, -1.833) | - | STRONG | (13.862, 1.387, -3.086) | 13.864 | 3.372 | STRONG |
| kar98k_pavlovsniper | (-1.164, -1.591, -1.833) | - | STRONG | (13.862, 1.387, -3.086) | 13.864 | 3.372 | STRONG |
| kar98k_sniper | (-1.164, -1.590, -1.833) | - | STRONG | (13.862, 1.387, -3.086) | 13.864 | 3.372 | STRONG |
| luger | (-2.782, -1.335, -4.002) | - | STRONG | (-13.612, 6.711, -9.135) | -12.176 | 12.865 | WEAK |
| m1carbine | (-1.660, -4.056, 0.864) | - | STRONG | (1.819, -2.574, 15.092) | 15.080 | 3.210 | STRONG |
| m1carbine_medic | (-1.660, -4.056, 0.864) | - | STRONG | (1.819, -2.574, 15.092) | 15.080 | 3.210 | STRONG |
| m1garand | (-2.421, -1.766, -3.235) | - | STRONG | (13.862, 1.288, -3.087) | 13.924 | 3.077 | STRONG |
| mg42_bipod_duck | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| mg42_bipod_prone | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| mg42_bipod_stand | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| mg42_bipod_unmanned | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| mg42_panzeriv_tank | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| mg42_tiger_tank | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| mk1britishfrag | (0.618, -2.159, -3.646) | - | WEAK | (-15.221, 16.713, -5.795) | - | - | WEAK |
| mosin_nagant | (-1.266, -1.583, -2.328) | - | STRONG | (13.614, 0.791, -4.244) | 13.653 | 4.194 | STRONG |
| mosin_nagant_sniper | (-1.057, -1.588, -1.606) | - | STRONG | (13.861, 1.328, -3.387) | 13.892 | 3.520 | STRONG |
| mp40 | (-2.560, -1.727, -3.919) | - | STRONG | (9.083, 1.635, -4.374) | 9.132 | 4.573 | STRONG |
| mp44 | (-1.682, -1.527, -4.350) | - | STRONG | (11.578, 0.996, -3.132) | 11.695 | 2.840 | STRONG |
| mp44_semi | (-1.682, -1.527, -4.350) | - | STRONG | (11.578, 0.996, -3.132) | 11.695 | 2.840 | STRONG |
| panzerfaust | (1.598, 15.015, -15.179) | - | WEAK | AUTO | - | - | UNKNOWN |
| panzeriv_turret | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| ppsh | (-8.150, -1.750, -1.911) | - | STRONG | (2.402, 5.658, -3.741) | 2.317 | 6.812 | STRONG |
| ppsh_semi | (-8.150, -1.750, -1.911) | - | STRONG | (2.402, 5.658, -3.741) | 2.317 | 6.812 | STRONG |
| ptrs41_antitank_rifle | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| rgd-33russianfrag | (0.721, 0.913, -2.390) | - | WEAK | (-11.569, 19.756, -3.925) | - | - | WEAK |
| springfield | (-1.173, -1.595, -1.859) | - | STRONG | (13.862, 1.387, -3.086) | 13.889 | 3.271 | STRONG |
| stalingrad_ammo | (-4.141, 7.548, 6.641) | - | STRONG | (4.388, -14.819, 5.678) | - | - | WEAK |
| sten | (-2.588, -1.743, -3.921) | - | STRONG | (8.475, 4.771, 1.019) | 8.473 | 4.883 | STRONG |
| sten_engineer | (-2.588, -1.743, -3.921) | - | STRONG | (8.475, 4.771, 1.019) | 8.473 | 4.883 | STRONG |
| stielhandgranate | (1.523, -1.970, -3.519) | - | WEAK | (-5.894, 7.342, -1.528) | - | - | WEAK |
| stuka_guns | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| t34_turret | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |
| thompson | (-4.553, -1.657, -2.879) | - | STRONG | (7.160, 0.604, -2.162) | 6.894 | 2.963 | STRONG |
| thompson_semi | (-4.553, -1.657, -2.879) | - | STRONG | (7.160, 0.604, -2.162) | 6.894 | 2.963 | STRONG |
| tiger_turret | AUTO | - | UNKNOWN | AUTO | - | - | UNKNOWN |

## Sight line

| weapon | sight offset (eye rel. weapon, in) | sight forward | barrel deviation deg | PHYSICAL_SIGHT_LINE |
|---|---|---|---|---|
| 30cal | (-0.253, 0.020, 1.693) | (1.000, 0.005, -0.024) | 0.551 | PROVEN |
| 30cal_prone | AUTO | AUTO | - | UNKNOWN |
| 30cal_stand | AUTO | AUTO | - | UNKNOWN |
| bar | (-0.253, 0.020, 1.693) | (1.000, 0.005, -0.024) | 0.551 | PROVEN |
| bar_slow | (-0.253, 0.020, 1.693) | (1.000, 0.005, -0.024) | 0.551 | PROVEN |
| bren | (-4.458, 0.298, 1.689) | (0.999, 0.019, -0.034) | 0.929 | PROVEN |
| colt | (-9.083, 0.139, 0.819) | (0.999, 0.000, -0.035) | 1.926 | PROVEN |
| enfield | (-0.849, 0.168, 1.026) | (1.000, 0.000, -0.016) | 0.894 | PROVEN |
| fg42 | (2.779, -0.021, 2.472) | (1.000, 0.000, -0.006) | 1.480 | PROVEN |
| fg42_auto | (2.779, -0.021, 2.472) | (1.000, 0.000, -0.006) | 1.480 | PROVEN |
| flak88_turret | AUTO | AUTO | - | UNKNOWN |
| flakvierling_turret | AUTO | AUTO | - | UNKNOWN |
| fraggrenade | AUTO | AUTO | - | UNKNOWN |
| kar98k | (-2.010, 0.039, 0.967) | (1.000, 0.004, -0.004) | 0.254 | PROVEN |
| kar98k_pavlovsniper | (-2.010, 0.039, 0.967) | (1.000, 0.004, -0.004) | 0.254 | PROVEN |
| kar98k_sniper | (-2.010, 0.039, 0.967) | (1.000, 0.004, -0.004) | 0.254 | PROVEN |
| luger | (-13.885, 0.137, 0.805) | (0.999, 0.000, -0.052) | 4.581 | PROVEN |
| m1carbine | (0.130, 1.330, -1.500) | (0.000, -0.012, 1.000) | 1.114 | PROVEN |
| m1carbine_medic | (0.130, 1.330, -1.500) | (0.000, -0.012, 1.000) | 1.114 | PROVEN |
| m1garand | (-2.010, 0.039, 0.967) | (1.000, 0.004, -0.004) | 0.899 | PROVEN |
| mg42_bipod_duck | AUTO | AUTO | - | UNKNOWN |
| mg42_bipod_prone | AUTO | AUTO | - | UNKNOWN |
| mg42_bipod_stand | AUTO | AUTO | - | UNKNOWN |
| mg42_bipod_unmanned | AUTO | AUTO | - | UNKNOWN |
| mg42_panzeriv_tank | AUTO | AUTO | - | UNKNOWN |
| mg42_tiger_tank | AUTO | AUTO | - | UNKNOWN |
| mk1britishfrag | AUTO | AUTO | - | UNKNOWN |
| mosin_nagant | (-0.449, -0.139, 0.485) | (1.000, 0.001, -0.007) | 0.135 | PROVEN |
| mosin_nagant_sniper | (2.804, -0.053, 1.946) | (1.000, 0.001, -0.002) | 0.420 | PROVEN |
| mp40 | (-6.031, 0.028, 1.734) | (1.000, 0.002, -0.027) | 0.936 | PROVEN |
| mp44 | (0.494, -0.053, 1.664) | (1.000, 0.001, -0.002) | 2.063 | PROVEN |
| mp44_semi | (0.494, -0.053, 1.664) | (1.000, 0.001, -0.002) | 2.063 | PROVEN |
| panzerfaust | (4.246, 0.057, 6.932) | (-0.992, -0.019, -0.126) | - | STRONG |
| panzeriv_turret | AUTO | AUTO | - | UNKNOWN |
| ppsh | (-10.175, 0.012, 2.233) | (1.000, -0.009, 0.004) | 2.966 | PROVEN |
| ppsh_semi | (-10.175, 0.012, 2.233) | (1.000, -0.009, 0.004) | 2.966 | PROVEN |
| ptrs41_antitank_rifle | AUTO | AUTO | - | UNKNOWN |
| rgd-33russianfrag | AUTO | AUTO | - | UNKNOWN |
| springfield | (1.796, 0.029, 1.619) | (1.000, -0.000, -0.000) | 0.510 | PROVEN |
| stalingrad_ammo | (7.140, 3.517, 12.289) | (-0.982, 0.118, 0.149) | - | STRONG |
| sten | (-3.539, -0.132, 1.378) | (1.000, 0.014, -0.005) | 0.794 | PROVEN |
| sten_engineer | (-3.539, -0.132, 1.378) | (1.000, 0.014, -0.005) | 0.794 | PROVEN |
| stielhandgranate | AUTO | AUTO | - | UNKNOWN |
| stuka_guns | AUTO | AUTO | - | UNKNOWN |
| t34_turret | AUTO | AUTO | - | UNKNOWN |
| thompson | (-11.989, -0.125, 2.704) | (1.000, -0.000, -0.013) | 6.643 | STRONG |
| thompson_semi | (-11.989, -0.125, 2.704) | (1.000, -0.000, -0.013) | 6.643 | STRONG |
| tiger_turret | AUTO | AUTO | - | UNKNOWN |

## Evidence


### 30cal  (rifle)

- status: `ok`; gun parts `viewmodel_bar2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(38.962, 0.044, -0.610)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.001, -0.016)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.160, -1.591, -1.602)`  [STRONG] -- bip01 r hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 10 firing frames 0.091 in
- **rearGripRotation** = `(0.004, 0.210, 0.052, 0.976)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `13.926`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 10 firing frames 0.127 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `4.162`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 10 firing frames 0.127 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-0.253, 0.020, 1.693)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_bar_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.005, -0.024)`  [STRONG] -- view forward in tag_weapon space with viewmodel_bar_ADS_up applied; deviates from the derived barrel axis by 0.551 deg

### 30cal_prone  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### 30cal_stand  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### bar  (rifle)

- status: `ok`; gun parts `viewmodel_bar2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(38.962, 0.044, -0.610)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.001, -0.016)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.160, -1.591, -1.602)`  [STRONG] -- bip01 r hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 10 firing frames 0.091 in
- **rearGripRotation** = `(0.004, 0.210, 0.052, 0.976)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `13.926`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 10 firing frames 0.127 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `4.162`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 10 firing frames 0.127 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-0.253, 0.020, 1.693)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_bar_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.005, -0.024)`  [STRONG] -- view forward in tag_weapon space with viewmodel_bar_ADS_up applied; deviates from the derived barrel axis by 0.551 deg

### bar_slow  (rifle)

- status: `ok`; gun parts `viewmodel_bar2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(38.962, 0.044, -0.610)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.001, -0.016)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.160, -1.591, -1.602)`  [STRONG] -- bip01 r hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 17 firing frames 0.201 in
- **rearGripRotation** = `(0.004, 0.210, 0.052, 0.976)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `13.926`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 17 firing frames 0.208 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `4.162`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 17 firing frames 0.208 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-0.253, 0.020, 1.693)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_bar_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.005, -0.024)`  [STRONG] -- view forward in tag_weapon space with viewmodel_bar_ADS_up applied; deviates from the derived barrel axis by 0.551 deg

### bren  (rifle)

- status: `ok`; gun parts `viewmodel_bren2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(37.403, 0.283, -1.690)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(0.999, 0.008, -0.045)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(1.494, -1.388, -4.590)`  [STRONG] -- bip01 r hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.229 in
- **rearGripRotation** = `(-0.013, 0.076, 0.005, 0.997)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `16.492`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.153 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `6.146`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.153 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-4.458, 0.298, 1.689)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_bren_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(0.999, 0.019, -0.034)`  [STRONG] -- view forward in tag_weapon space with viewmodel_bren_ADS_up applied; deviates from the derived barrel axis by 0.929 deg

### colt  (pistol)

- status: `ok`; gun parts `viewmodel_colt452`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(8.114, 0.212, -0.456)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(0.998, 0.026, -0.056)`  [STRONG] -- two estimates agree; a third dissents, via tag_flash
- **rearGripOffset** = `(-3.064, -0.426, -3.805)`  [STRONG] -- bip01 r hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 22 firing frames 0.837 in
- **rearGripRotation** = `(-0.091, 0.022, -0.141, 0.986)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `-2.098`  [WEAK] -- bip01 l hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 22 firing frames 0.837 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; the weapon file does not mark this weapon twoHanded, so the off hand is presentation, not a grip; projected on the barrel axis
- **supportRadius** = `7.226`  [WEAK] -- bip01 l hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 22 firing frames 0.837 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; the weapon file does not mark this weapon twoHanded, so the off hand is presentation, not a grip; perpendicular distance from the bore line
- **sightOffset** = `(-9.083, 0.139, 0.819)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_colt45_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(0.999, 0.000, -0.035)`  [STRONG] -- view forward in tag_weapon space with viewmodel_colt45_ADS_up applied; deviates from the derived barrel axis by 1.926 deg

### enfield  (rifle)

- status: `ok`; gun parts `viewmodel_enfield2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(38.278, 0.429, -0.186)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.011, -0.005)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.163, -1.590, -1.837)`  [STRONG] -- bip01 r hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 22 firing frames 0.448 in
- **rearGripRotation** = `(0.006, 0.218, 0.051, 0.975)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `14.309`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 22 firing frames 0.676 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `3.646`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 22 firing frames 0.676 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-0.849, 0.168, 1.026)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_enfield_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.000, -0.016)`  [STRONG] -- view forward in tag_weapon space with viewmodel_enfield_ADS_up applied; deviates from the derived barrel axis by 0.894 deg

### fg42  (rifle)

- status: `ok`; gun parts `viewmodel_fg422`; hand parts `viewmodel_hands_sten4`
- **muzzleOrigin** = `(36.891, 0.572, -0.999)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.016, -0.027)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(4.107, -1.903, -5.208)`  [STRONG] -- bip01 r hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.850 in
- **rearGripRotation** = `(-0.051, -0.021, 0.071, 0.996)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `12.888`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.221 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `3.691`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.221 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(2.779, -0.021, 2.472)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_fg42_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.000, -0.006)`  [STRONG] -- view forward in tag_weapon space with viewmodel_fg42_ADS_up applied; deviates from the derived barrel axis by 1.480 deg

### fg42_auto  (rifle)

- status: `ok`; gun parts `viewmodel_fg422`; hand parts `viewmodel_hands_sten4`
- **muzzleOrigin** = `(36.891, 0.572, -0.999)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.016, -0.027)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(4.107, -1.903, -5.208)`  [STRONG] -- bip01 r hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.850 in
- **rearGripRotation** = `(-0.051, -0.021, 0.071, 0.996)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `12.888`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.221 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `3.691`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.221 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(2.779, -0.021, 2.472)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_fg42_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.000, -0.006)`  [STRONG] -- view forward in tag_weapon space with viewmodel_fg42_ADS_up applied; deviates from the derived barrel axis by 1.480 deg

### flak88_turret  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### flakvierling_turret  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### fraggrenade  (grenade)

- status: `ok`; gun parts `mk2fraggrenade_viewmodel2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `AUTO`  [N/A] -- vrClass grenade has no barrel; the rig's tag_flash is vestigial
- **barrelForward** = `AUTO`  [N/A] -- vrClass grenade has no barrel; the rig's tag_flash is vestigial
- **rearGripOffset** = `(0.618, -2.159, -3.646)`  [WEAK] -- bip01 r hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 15 firing frames 7.524 in
- **rearGripRotation** = `(0.327, -0.071, 0.620, 0.709)`  [WEAK] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `AUTO`  [N/A] -- vrClass grenade: the off hand is not placed on a barrel, so an along-axis/radius decomposition has no meaning
- **supportRadius** = `AUTO`  [N/A] -- vrClass grenade: the off hand is not placed on a barrel, so an along-axis/radius decomposition has no meaning
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### kar98k  (rifle)

- status: `ok`; gun parts `viewmodel_kar982`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(36.433, 0.041, -0.016)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.001, -0.000)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.164, -1.591, -1.833)`  [STRONG] -- bip01 r hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.404 in
- **rearGripRotation** = `(0.005, 0.218, 0.051, 0.975)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `13.864`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.520 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `3.372`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.520 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-2.010, 0.039, 0.967)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_kar98_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.004, -0.004)`  [STRONG] -- view forward in tag_weapon space with viewmodel_kar98_ADS_up applied; deviates from the derived barrel axis by 0.254 deg

### kar98k_pavlovsniper  (rifle)

- status: `ok`; gun parts `viewmodel_kar982`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(36.433, 0.041, -0.016)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.001, -0.000)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.164, -1.591, -1.833)`  [STRONG] -- bip01 r hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.404 in
- **rearGripRotation** = `(0.005, 0.218, 0.051, 0.975)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `13.864`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.520 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `3.372`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.520 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-2.010, 0.039, 0.967)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_kar98_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.004, -0.004)`  [STRONG] -- view forward in tag_weapon space with viewmodel_kar98_ADS_up applied; deviates from the derived barrel axis by 0.254 deg

### kar98k_sniper  (rifle)

- status: `ok`; gun parts `viewmodel_kar98scoped2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(36.433, 0.041, -0.016)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.001, -0.000)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.164, -1.590, -1.833)`  [STRONG] -- bip01 r hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.404 in
- **rearGripRotation** = `(0.005, 0.218, 0.051, 0.975)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `13.864`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.520 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `3.372`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.520 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-2.010, 0.039, 0.967)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_kar98_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.004, -0.004)`  [STRONG] -- view forward in tag_weapon space with viewmodel_kar98_ADS_up applied; deviates from the derived barrel axis by 0.254 deg

### luger  (pistol)

- status: `ok`; gun parts `viewmodel_luger2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(7.924, 0.165, -1.033)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(0.991, 0.021, -0.129)`  [PROVEN] -- 2 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-2.782, -1.335, -4.002)`  [STRONG] -- bip01 r hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 6 firing frames 0.326 in
- **rearGripRotation** = `(-0.056, -0.005, 0.025, 0.998)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `-12.176`  [WEAK] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.199 in); worst excursion over 6 firing frames 10.120 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; the weapon file does not mark this weapon twoHanded, so the off hand is presentation, not a grip; projected on the barrel axis
- **supportRadius** = `12.865`  [WEAK] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.199 in); worst excursion over 6 firing frames 10.120 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; the weapon file does not mark this weapon twoHanded, so the off hand is presentation, not a grip; perpendicular distance from the bore line
- **sightOffset** = `(-13.885, 0.137, 0.805)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_luger_ads_up applied at its last frame, in tag_weapon space
- **sightForward** = `(0.999, 0.000, -0.052)`  [STRONG] -- view forward in tag_weapon space with viewmodel_luger_ads_up applied; deviates from the derived barrel axis by 4.581 deg

### m1carbine  (rifle)

- status: `ok`; gun parts `viewmodel_m1carbine2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(0.105, 0.220, 31.811)`  [STRONG] -- tag_flash resolved through the idle pose, in m1carbine space
- **barrelForward** = `(0.003, 0.007, 1.000)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.660, -4.056, 0.864)`  [STRONG] -- bip01 r hand relative to m1carbine across 1 idle frames (spread 0.000 in); worst excursion over 16 firing frames 0.108 in
- **rearGripRotation** = `(-0.500, -0.465, -0.469, 0.559)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `15.080`  [STRONG] -- bip01 l hand relative to m1carbine across 1 idle frames (spread 0.000 in); worst excursion over 16 firing frames 0.121 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `3.210`  [STRONG] -- bip01 l hand relative to m1carbine across 1 idle frames (spread 0.000 in); worst excursion over 16 firing frames 0.121 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(0.130, 1.330, -1.500)`  [STRONG] -- eye position relative to m1carbine with viewmodel_m1carbine_ADS_up applied at its last frame, in m1carbine space
- **sightForward** = `(0.000, -0.012, 1.000)`  [STRONG] -- view forward in m1carbine space with viewmodel_m1carbine_ADS_up applied; deviates from the derived barrel axis by 1.114 deg

### m1carbine_medic  (rifle)

- status: `ok`; gun parts `viewmodel_m1carbine2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(0.105, 0.220, 31.811)`  [STRONG] -- tag_flash resolved through the idle pose, in m1carbine space
- **barrelForward** = `(0.003, 0.007, 1.000)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.660, -4.056, 0.864)`  [STRONG] -- bip01 r hand relative to m1carbine across 1 idle frames (spread 0.000 in); worst excursion over 16 firing frames 0.108 in
- **rearGripRotation** = `(-0.500, -0.465, -0.469, 0.559)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `15.080`  [STRONG] -- bip01 l hand relative to m1carbine across 1 idle frames (spread 0.000 in); worst excursion over 16 firing frames 0.121 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `3.210`  [STRONG] -- bip01 l hand relative to m1carbine across 1 idle frames (spread 0.000 in); worst excursion over 16 firing frames 0.121 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(0.130, 1.330, -1.500)`  [STRONG] -- eye position relative to m1carbine with viewmodel_m1carbine_ADS_up applied at its last frame, in m1carbine space
- **sightForward** = `(0.000, -0.012, 1.000)`  [STRONG] -- view forward in m1carbine space with viewmodel_m1carbine_ADS_up applied; deviates from the derived barrel axis by 1.114 deg

### m1garand  (rifle)

- status: `ok`; gun parts `viewmodel_m1garand2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(32.407, 0.128, -0.624)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.004, -0.019)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-2.421, -1.766, -3.235)`  [STRONG] -- bip01 r hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 21 firing frames 0.777 in
- **rearGripRotation** = `(0.209, 0.140, 0.091, 0.964)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `13.924`  [STRONG] -- bip01 l hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 21 firing frames 0.520 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `3.077`  [STRONG] -- bip01 l hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 21 firing frames 0.520 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-2.010, 0.039, 0.967)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_m1garandfov_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.004, -0.004)`  [STRONG] -- view forward in tag_weapon space with viewmodel_m1garandfov_ADS_up applied; deviates from the derived barrel axis by 0.899 deg

### mg42_bipod_duck  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### mg42_bipod_prone  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### mg42_bipod_stand  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### mg42_bipod_unmanned  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### mg42_panzeriv_tank  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### mg42_tiger_tank  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### mk1britishfrag  (grenade)

- status: `ok`; gun parts `viewmodel_mk12`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `AUTO`  [N/A] -- vrClass grenade has no barrel; the rig's tag_flash is vestigial
- **barrelForward** = `AUTO`  [N/A] -- vrClass grenade has no barrel; the rig's tag_flash is vestigial
- **rearGripOffset** = `(0.618, -2.159, -3.646)`  [WEAK] -- bip01 r hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 2 firing frames 8.850 in
- **rearGripRotation** = `(0.327, -0.071, 0.620, 0.709)`  [WEAK] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `AUTO`  [N/A] -- vrClass grenade: the off hand is not placed on a barrel, so an along-axis/radius decomposition has no meaning
- **supportRadius** = `AUTO`  [N/A] -- vrClass grenade: the off hand is not placed on a barrel, so an along-axis/radius decomposition has no meaning
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### mosin_nagant  (rifle)

- status: `ok`; gun parts `viewmodel_mosinnagant2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(42.685, -0.006, -0.394)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, -0.000, -0.009)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.266, -1.583, -2.328)`  [STRONG] -- bip01 r hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.237 in
- **rearGripRotation** = `(0.009, 0.158, 0.051, 0.986)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `13.653`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.169 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `4.194`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.169 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-0.449, -0.139, 0.485)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_mosinnagant_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.001, -0.007)`  [STRONG] -- view forward in tag_weapon space with viewmodel_mosinnagant_ADS_up applied; deviates from the derived barrel axis by 0.135 deg

### mosin_nagant_sniper  (rifle)

- status: `ok`; gun parts `viewmodel_mosinnagantscoped2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(42.759, -0.006, -0.394)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, -0.000, -0.009)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.057, -1.588, -1.606)`  [STRONG] -- bip01 r hand relative to tag_weapon across 2 idle frames (spread 0.001 in); worst excursion over 11 firing frames 0.486 in
- **rearGripRotation** = `(0.005, 0.218, 0.051, 0.975)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `13.892`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.524 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `3.520`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.524 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(2.804, -0.053, 1.946)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_mosinnagantscoped_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.001, -0.002)`  [STRONG] -- view forward in tag_weapon space with viewmodel_mosinnagantscoped_ADS_up applied; deviates from the derived barrel axis by 0.420 deg

### mp40  (smg)

- status: `ok`; gun parts `viewmodel_mp402`; hand parts `viewmodel_hands_sten4`
- **muzzleOrigin** = `(23.092, 0.045, -0.243)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.002, -0.011)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-2.560, -1.727, -3.919)`  [STRONG] -- bip01 r hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.602 in
- **rearGripRotation** = `(0.029, -0.008, 0.075, 0.997)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `9.132`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.337 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `4.573`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.337 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-6.031, 0.028, 1.734)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_mp40_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.002, -0.027)`  [STRONG] -- view forward in tag_weapon space with viewmodel_mp40_ADS_up applied; deviates from the derived barrel axis by 0.936 deg

### mp44  (rifle)

- status: `ok`; gun parts `viewmodel_mp442`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(28.681, 0.339, -1.047)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(0.999, 0.012, -0.036)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.682, -1.527, -4.350)`  [STRONG] -- bip01 r hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 10 firing frames 0.041 in
- **rearGripRotation** = `(0.040, 0.086, 0.038, 0.995)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `11.695`  [STRONG] -- bip01 l hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 10 firing frames 0.001 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `2.840`  [STRONG] -- bip01 l hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 10 firing frames 0.001 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(0.494, -0.053, 1.664)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_mp44_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.001, -0.002)`  [STRONG] -- view forward in tag_weapon space with viewmodel_mp44_ADS_up applied; deviates from the derived barrel axis by 2.063 deg

### mp44_semi  (rifle)

- status: `ok`; gun parts `viewmodel_mp442`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(28.681, 0.339, -1.047)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(0.999, 0.012, -0.036)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.682, -1.527, -4.350)`  [STRONG] -- bip01 r hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 10 firing frames 0.041 in
- **rearGripRotation** = `(0.040, 0.086, 0.038, 0.995)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `11.695`  [STRONG] -- bip01 l hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 10 firing frames 0.001 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `2.840`  [STRONG] -- bip01 l hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 10 firing frames 0.001 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(0.494, -0.053, 1.664)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_mp44_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.001, -0.002)`  [STRONG] -- view forward in tag_weapon space with viewmodel_mp44_ADS_up applied; deviates from the derived barrel axis by 2.063 deg

### panzerfaust  (launcher)

- status: `ok`; gun parts `viewmodel_panzerfaust2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- tag_flash resolves onto the weapon frame origin (0.000 in): degenerate, not a muzzle
- **barrelForward** = `AUTO`  [UNKNOWN] -- no usable muzzle offset
- **rearGripOffset** = `(1.598, 15.015, -15.179)`  [WEAK] -- bip01 r hand relative to panzerfaust across 2 idle frames (spread 0.021 in); worst excursion over 15 firing frames 55.081 in
- **rearGripRotation** = `(-0.402, 0.276, 0.771, 0.410)`  [WEAK] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `(4.246, 0.057, 6.932)`  [STRONG] -- eye position relative to panzerfaust with viewmodel_panzerfaust_ADS_up applied at its last frame, in panzerfaust space
- **sightForward** = `(-0.992, -0.019, -0.126)`  [STRONG] -- view forward in panzerfaust space with viewmodel_panzerfaust_ADS_up applied

### panzeriv_turret  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### ppsh  (smg)

- status: `ok`; gun parts `viewmodel_ppsh2`; hand parts `viewmodel_hands_sten4`
- **muzzleOrigin** = `(17.539, 0.313, 0.857)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(0.999, 0.018, 0.049)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-8.150, -1.750, -1.911)`  [STRONG] -- bip01 r hand relative to tag_weapon across 4 idle frames (spread 0.000 in); worst excursion over 11 firing frames 1.208 in
- **rearGripRotation** = `(-0.028, 0.116, 0.086, 0.989)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `2.317`  [STRONG] -- bip01 l hand relative to tag_weapon across 4 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.118 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `6.812`  [STRONG] -- bip01 l hand relative to tag_weapon across 4 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.118 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-10.175, 0.012, 2.233)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_ppsh_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, -0.009, 0.004)`  [STRONG] -- view forward in tag_weapon space with viewmodel_ppsh_ADS_up applied; deviates from the derived barrel axis by 2.966 deg

### ppsh_semi  (smg)

- status: `ok`; gun parts `viewmodel_ppsh2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(17.539, 0.313, 0.857)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(0.999, 0.018, 0.049)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-8.150, -1.750, -1.911)`  [STRONG] -- bip01 r hand relative to tag_weapon across 4 idle frames (spread 0.000 in); worst excursion over 11 firing frames 1.208 in
- **rearGripRotation** = `(-0.028, 0.116, 0.086, 0.989)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `2.317`  [STRONG] -- bip01 l hand relative to tag_weapon across 4 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.118 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `6.812`  [STRONG] -- bip01 l hand relative to tag_weapon across 4 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.118 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-10.175, 0.012, 2.233)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_ppsh_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, -0.009, 0.004)`  [STRONG] -- view forward in tag_weapon space with viewmodel_ppsh_ADS_up applied; deviates from the derived barrel axis by 2.966 deg

### ptrs41_antitank_rifle  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### rgd-33russianfrag  (grenade)

- status: `ok`; gun parts `viewmodel_rgd332`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `AUTO`  [N/A] -- vrClass grenade has no barrel; the rig's tag_flash is vestigial
- **barrelForward** = `AUTO`  [N/A] -- vrClass grenade has no barrel; the rig's tag_flash is vestigial
- **rearGripOffset** = `(0.721, 0.913, -2.390)`  [WEAK] -- bip01 r hand relative to tag_weapon across 4 idle frames (spread 3.753 in); worst excursion over 15 firing frames 4.407 in
- **rearGripRotation** = `(0.261, -0.315, 0.656, 0.634)`  [WEAK] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `AUTO`  [N/A] -- vrClass grenade: the off hand is not placed on a barrel, so an along-axis/radius decomposition has no meaning
- **supportRadius** = `AUTO`  [N/A] -- vrClass grenade: the off hand is not placed on a barrel, so an along-axis/radius decomposition has no meaning
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### springfield  (rifle)

- status: `ok`; gun parts `viewmodel_springfield2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(36.391, -0.002, -0.324)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, -0.000, -0.009)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-1.173, -1.595, -1.859)`  [STRONG] -- bip01 r hand relative to tag_weapon across 3 idle frames (spread 0.006 in); worst excursion over 11 firing frames 0.423 in
- **rearGripRotation** = `(0.002, 0.222, 0.052, 0.974)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `13.889`  [STRONG] -- bip01 l hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.444 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `3.271`  [STRONG] -- bip01 l hand relative to tag_weapon across 3 idle frames (spread 0.000 in); worst excursion over 11 firing frames 0.444 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(1.796, 0.029, 1.619)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_springfield_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, -0.000, -0.000)`  [STRONG] -- view forward in tag_weapon space with viewmodel_springfield_ADS_up applied; deviates from the derived barrel axis by 0.510 deg

### stalingrad_ammo  (prop)

- status: `ok`; gun parts `viewmodel_ammo2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `AUTO`  [N/A] -- vrClass prop has no barrel; the rig's tag_flash is vestigial
- **barrelForward** = `AUTO`  [N/A] -- vrClass prop has no barrel; the rig's tag_flash is vestigial
- **rearGripOffset** = `(-4.141, 7.548, 6.641)`  [STRONG] -- bip01 r hand relative to tag_weapon across 4 idle frames (spread 0.000 in); worst excursion over 0 firing frames 0.000 in
- **rearGripRotation** = `(-0.243, 0.560, -0.586, 0.533)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `AUTO`  [N/A] -- vrClass prop: the off hand is not placed on a barrel, so an along-axis/radius decomposition has no meaning
- **supportRadius** = `AUTO`  [N/A] -- vrClass prop: the off hand is not placed on a barrel, so an along-axis/radius decomposition has no meaning
- **sightOffset** = `(7.140, 3.517, 12.289)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_ammo_ads_up applied at its last frame, in tag_weapon space
- **sightForward** = `(-0.982, 0.118, 0.149)`  [STRONG] -- view forward in tag_weapon space with viewmodel_ammo_ads_up applied

### sten  (smg)

- status: `ok`; gun parts `viewmodel_sten2`; hand parts `viewmodel_hands_sten4`
- **muzzleOrigin** = `(20.361, 0.045, -0.243)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.002, -0.012)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-2.588, -1.743, -3.921)`  [STRONG] -- bip01 r hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 7 firing frames 2.372 in
- **rearGripRotation** = `(0.028, -0.008, 0.077, 0.997)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `8.473`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.001 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `4.883`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.001 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-3.539, -0.132, 1.378)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_sten_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.014, -0.005)`  [STRONG] -- view forward in tag_weapon space with viewmodel_sten_ADS_up applied; deviates from the derived barrel axis by 0.794 deg

### sten_engineer  (smg)

- status: `ok`; gun parts `viewmodel_sten2`; hand parts `viewmodel_hands_sten4`
- **muzzleOrigin** = `(20.361, 0.045, -0.243)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(1.000, 0.002, -0.012)`  [PROVEN] -- 3 of 3 independent estimates agree within 5 deg, via tag_flash
- **rearGripOffset** = `(-2.588, -1.743, -3.921)`  [STRONG] -- bip01 r hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 7 firing frames 2.372 in
- **rearGripRotation** = `(0.028, -0.008, 0.077, 0.997)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `8.473`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.001 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `4.883`  [STRONG] -- bip01 l hand relative to tag_weapon across 1 idle frames (spread 0.000 in); worst excursion over 7 firing frames 0.001 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-3.539, -0.132, 1.378)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_sten_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, 0.014, -0.005)`  [STRONG] -- view forward in tag_weapon space with viewmodel_sten_ADS_up applied; deviates from the derived barrel axis by 0.794 deg

### stielhandgranate  (grenade)

- status: `ok`; gun parts `viewmodel_nebelhandgrenate2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `AUTO`  [N/A] -- vrClass grenade has no barrel; the rig's tag_flash is vestigial
- **barrelForward** = `AUTO`  [N/A] -- vrClass grenade has no barrel; the rig's tag_flash is vestigial
- **rearGripOffset** = `(1.523, -1.970, -3.519)`  [WEAK] -- bip01 r hand relative to tag_weapon across 4 idle frames (spread 0.686 in); worst excursion over 14 firing frames 8.911 in
- **rearGripRotation** = `(0.261, -0.315, 0.656, 0.634)`  [WEAK] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `AUTO`  [N/A] -- vrClass grenade: the off hand is not placed on a barrel, so an along-axis/radius decomposition has no meaning
- **supportRadius** = `AUTO`  [N/A] -- vrClass grenade: the off hand is not placed on a barrel, so an along-axis/radius decomposition has no meaning
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### stuka_guns  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### t34_turret  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence

### thompson  (smg)

- status: `ok`; gun parts `viewmodel_thompson2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(16.083, -0.212, 1.649)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(0.995, -0.013, 0.102)`  [STRONG] -- two estimates agree; a third dissents, via tag_flash
- **rearGripOffset** = `(-4.553, -1.657, -2.879)`  [STRONG] -- bip01 r hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 8 firing frames 2.542 in
- **rearGripRotation** = `(-0.013, -0.094, 0.068, 0.993)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `6.894`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 8 firing frames 2.543 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `2.963`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 8 firing frames 2.543 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-11.989, -0.125, 2.704)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_thompson_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, -0.000, -0.013)`  [STRONG] -- view forward in tag_weapon space with viewmodel_thompson_ADS_up applied; deviates from the derived barrel axis by 6.642 deg

### thompson_semi  (smg)

- status: `ok`; gun parts `viewmodel_thompson2`; hand parts `viewmodel_hands_new4`
- **muzzleOrigin** = `(16.083, -0.212, 1.649)`  [STRONG] -- tag_flash resolved through the idle pose, in tag_weapon space
- **barrelForward** = `(0.995, -0.013, 0.102)`  [STRONG] -- two estimates agree; a third dissents, via tag_flash
- **rearGripOffset** = `(-4.553, -1.657, -2.879)`  [STRONG] -- bip01 r hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 8 firing frames 2.542 in
- **rearGripRotation** = `(-0.013, -0.094, 0.068, 0.993)`  [STRONG] -- bip01 r hand basis relative to tag_weapon basis in the idle pose
- **supportAlongAxis** = `6.894`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 8 firing frames 2.543 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; projected on the barrel axis
- **supportRadius** = `2.963`  [STRONG] -- bip01 l hand relative to tag_weapon across 2 idle frames (spread 0.000 in); worst excursion over 8 firing frames 2.543 in; reload, melee, rechamber, last-shot and raise/drop animations excluded by construction; perpendicular distance from the bore line
- **sightOffset** = `(-11.989, -0.125, 2.704)`  [STRONG] -- eye position relative to tag_weapon with viewmodel_thompson_ADS_up applied at its last frame, in tag_weapon space
- **sightForward** = `(1.000, -0.000, -0.013)`  [STRONG] -- view forward in tag_weapon space with viewmodel_thompson_ADS_up applied; deviates from the derived barrel axis by 6.642 deg

### tiger_turret  (mounted)

- status: `no gun model`; gun parts `None`; hand parts `None`
- **muzzleOrigin** = `AUTO`  [UNKNOWN] -- no evidence
- **barrelForward** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **rearGripRotation** = `AUTO`  [UNKNOWN] -- no evidence
- **supportAlongAxis** = `AUTO`  [UNKNOWN] -- no evidence
- **supportRadius** = `AUTO`  [UNKNOWN] -- no evidence
- **sightOffset** = `AUTO`  [UNKNOWN] -- no evidence
- **sightForward** = `AUTO`  [UNKNOWN] -- no evidence
