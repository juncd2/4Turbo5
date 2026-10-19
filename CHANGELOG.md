# Changelog

## 0.08
- In-game controller control: hold L3 + D-pad Left/Right, 0.05 steps,
  1.00x–2.00x, with repeat when held.
- PS4 notification toast with the current multiplier and a slider bar.
- Hotkey changes are written back to `turbo.ini` and persist across sessions.
- Pad access is passive (adopts the handle the game opened; never calls
  `scePadOpen`), so it cannot collide with the game's own controller open.

## 0.07
- Runtime discovery: scan the executable for the reflection native-getter
  shape, then identify the speed field from the Application constructor
  defaults (`1.0`, `0.001`, `60.0`, `6000.0`).
- The exact v1.00 profile is kept as a signature-verified fallback.
- Region/update tolerant; fails safe when the code does not match.

## 0.06
- First working build for DMC5 v1.00.
- Pins `Application + 0x280` (the engine GlobalSpeed float) every 10 ms.
- Live `turbo.ini` reload, klog and file logging, unload restore.
