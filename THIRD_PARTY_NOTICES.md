# Third-party notices

This project vendors code from the GoldHEN Plugin SDK so it can be built
standalone.

## GoldHEN Plugin SDK

- Upstream: https://github.com/GoldHEN/GoldHEN_Plugins_SDK
- License: MIT — Copyright (c) 2022 GoldHEN
- Vendored files:
  - `include/Common.h`, `include/Detour.h`, `include/GoldHEN.h`,
    `include/HDE64.h`, `include/Patcher.h`, `include/Syscall.h`,
    `include/Table64.h`, `include/Utilities.h`
  - `source/Detour.c`, `source/GoldHEN.c`, `source/HDE64.c`,
    `source/Patcher.c`, `source/Syscall.c`, `source/Utilities.c`
  - `crt/crtprx.c` (coded by Specter + SiSTRo)
- Credits: OSM, jocover, bucanero, SiSTRo, ctn123, nik, OpenOrbis Team.

The MIT license text is in `LICENSE`. The original copyright notice above
applies to the vendored files.

## OpenOrbis PS4 Toolchain

- Upstream: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain
- Required at build time, not vendored.
