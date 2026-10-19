# DMC5 Turbo Mode — PS4 (GoldHEN plugin)

A GoldHEN plugin that brings the PC **Turbo Mode Injector** speed modifier to
*Devil May Cry 5* on PS4: a live global game-speed multiplier with in-game
controller adjustment and on-screen feedback.

This is an unofficial PS4 port of the turbo mode from
[**Turbo Mode And Character Swapper**](https://www.nexusmods.com/devilmaycry5/mods/29)
by **Naxshe** on Nexus Mods. Huge thanks to the original author — this project
would not exist without their work. It is not affiliated with or endorsed by
them.

- **Live speed control:** 1.00x – 2.00x, applied within 10 ms, no restart
- **In-game controls:** hold **L3 + D-pad Right/Left** to step by 0.05x
- **On-screen feedback:** PS4 notification shows the value and a slider bar
- **Persistent:** the chosen speed is written back to `turbo.ini`
- **Runtime discovery:** no hardcoded addresses at runtime (region/update tolerant)
- **Fail-safe:** if the game code does not match, the plugin leaves it untouched

Verified on **DMC5 v1.00** (PS4).

## Install

1. Copy `DMC5_turbo.prx` to `/data/GoldHEN/plugins/`.
2. Add the plugin to `/data/GoldHEN/plugins.ini` (keep your existing entries):

```ini
[CUSA08216]
/data/GoldHEN/plugins/DMC5_turbo.prx
```

3. Launch DMC5.

`[CUSA08216]` is the game's title ID — keep it exactly as shown. Only the
`.prx` filename changed to `DMC5_turbo.prx`; the section is what tells
GoldHEN to load the plugin for this game.

Requirements: **GoldHEN 2.3 or newer** with plugin loading enabled.

### `turbo.ini` (optional)

The plugin works without it and defaults to `speed=1.2`. To set a starting
speed, create `/data/GoldHEN/plugins/turbo.ini`:

```ini
[settings]
speed=1.2
```

Values: `1.0` = normal, `1.2` = +20%, `2.0` = 2x. The file is re-read every
5 seconds while the game runs, so FTP edits apply live. Hotkey changes update
this file automatically.

## Controls

| Input | Action |
|---|---|
| Hold **L3** + **D-pad Right** | +0.05x (hold to repeat) |
| Hold **L3** + **D-pad Left** | −0.05x (hold to repeat) |

Each change pops a notification:

```
DMC5 Turbo  1.35x  [###-------]
```

The game still receives the D-pad presses (typically taunts) — unavoidable
when sharing the pad. The plugin only reads the controller passively through
the handle the game itself opened; it never opens the pad on its own.

## Compatibility

- **DMC5 v1.00** — verified.
- **Other regions / game updates** — the plugin scans the game's executable
  for the reflection getter shape and identifies the speed field from the
  Application constructor defaults. Builds that share the engine code shape
  (typically regional discs of the same version) work without changes.
- Add one section per title ID in `plugins.ini`, all pointing at the same file:

```ini
[CUSA08215]
/data/GoldHEN/plugins/DMC5_turbo.prx

[CUSA08217]
/data/GoldHEN/plugins/DMC5_turbo.prx
```

If the scan finds no compatible code, the plugin logs
`no compatible getter code found` and does nothing — it never writes through
an unverified pointer.

## Build

Requires the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
(LLVM 10+):

```sh
export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
make
```

Output: `DMC5_turbo.prx` (fake SELF, ready for `/data/GoldHEN/plugins/`).

The repo vendors the GoldHEN Plugin SDK headers/helpers and `crtprx.c`
(see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

## How it works

The PC mod continuously pins the engine's `Application` speed float. The PS4
build exposes the same field; this plugin finds it without hardcoded
addresses:

1. Scan the main module's executable segment for the reflection native-getter
   shape:
   ```
   mov rax,[rip+slot]          ; singleton slot
   vmovss xmm0,[rax+field]     ; float property
   vcvtss2sd xmm0,xmm0
   vmovsd [rdi+0x38],xmm0      ; native return convention
   ret
   ```
   (SSE encoding is also accepted.) Every hit yields a singleton slot.
2. Wait for a slot to point at an object containing the `Application`
   constructor defaults: `1.0` at +0, `0.001` at +0xC, `60.0` at +0x18,
   `6000.0` at +0x1C. The matching offset is the speed field
   (`0x280` on v1.00).
3. Pin that float to the configured speed every 10 ms.

On v1.00 the plugin scans 80 getter sites, resolves 11 singleton slots, and
identifies the field at `+0x280` at runtime.

If no getter is found, it falls back to the exact v1.00 profile only after
verifying the v1.00 signature; otherwise it stays idle.

Logs: `/data/GoldHEN/plugins/turbo.log` and the GoldHEN kernel log.

## Troubleshooting

| Symptom | Check |
|---|---|
| Nothing happens | GoldHEN ≥ 2.3, plugin loading enabled, correct `[TITLEID]` section |
| No notification/hotkeys | Look for `pad: adopted game handle` in `turbo.log` |
| `no compatible getter code found` | Game build/version not matched by the scan |
| Wrong game version | Add its title ID section; if the scan still fails, open an issue with the eboot version |

## Credits

- Original PC project: [**Turbo Mode And Character Swapper**](https://www.nexusmods.com/devilmaycry5/mods/29) by **Naxshe** (Nexus Mods) — the turbo mode injector this PS4 port is based on
- [GoldHEN Plugin SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK) — OSM, jocover, bucanero, SiSTRo, ctn123, nik, OpenOrbis Team
- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
- `crtprx.c` — Specter + SiSTRo
- Passive pad adoption approach from the DDMK PS4 port

## Disclaimer

Homebrew plugin, provided as-is with no warranty. Use at your own risk.
Using modified game code with PSN services may violate Sony's terms and can
lead to account or console restrictions. Intended for offline/single-player
use.
