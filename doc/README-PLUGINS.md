# mmemu Plugin Index

This document provides a categorized index of all dynamically loadable plugins available in the mmemu ecosystem, along with documentation for the Plugin API.

---

## 1. Plugin Discovery

At startup, all **mmsim** binaries scan for plugins in the following standard locations (in order):

1. **Local Development**: `./lib/` (relative to current working directory).
2. **User Local**: `~/.local/lib/mmsim/plugins/`
3. **System Local**: `/usr/local/lib/mmsim/plugins/`
4. **System Standard**: `/usr/lib/mmsim/plugins/`

Plugins are shared library files (usually with a `.so` extension on Linux) that follow the naming convention `mmemu-plugin-*.so`.

---

## 2. Plugin Index

### 1.1 Processor Plugins (CPU)
Plugins that implement the `ICore` interface.

| Plugin File | Description | Documentation |
|-------------|-------------|---------------|
| `lib/mmemu-plugin-6502.so` | MOS 6502 (NMOS) with illegal opcode support; also provides MOS 6510 and WDC 65C02. | [README-6502.md](README-6502.md) · [README-6510.md](README-6510.md) |
| `lib/mmemu-plugin-z80.so` | Zilog Z80 with the undocumented instructions and flags (ZEXALL clean), its disassembler, and the `rawz80` machine. | [README-Z80.md](README-Z80.md) |
| `lib/mmemu-plugin-45gs02.so` | MEGA65 45GS02 CPU: 65CE02 extensions, 28-bit mapping (MAP), 32-bit Quad operations. | [README-45GS02.md](README-45GS02.md) |

### 1.2 Device Plugins (I/O & Video)
Plugins that implement the `IOHandler` interface for memory-mapped hardware.

| Plugin File | Description | Documentation |
|-------------|-------------|---------------|
| `lib/mmemu-plugin-via6522.so` | MOS 6522 Versatile Interface Adapter (VIA). | [README-6522.md](README-6522.md) |
| `lib/mmemu-plugin-vic6560.so` | MOS 6560 VIC-I Video/Sound generator. | [README-6560.md](README-6560.md) |
| `lib/mmemu-plugin-kbd-vic20.so` | Standard VIC-20 8x8 Keyboard Matrix. | [README-KBD-VIC20.md](README-KBD-VIC20.md) |
| `lib/mmemu-plugin-c64-pla.so` | C64 PLA banking controller (LORAM/HIRAM/CHAREN). | [README-C64PLA.md](README-C64PLA.md) |
| `lib/mmemu-plugin-cia6526.so` | MOS 6526 CIA: timers, TOD clock, I/O ports, IRQ. | [README-6526.md](README-6526.md) |
| `lib/mmemu-plugin-vic2.so` | MOS 6567/6569 VIC-II video chip with sprite engine. | [README-VIC2.md](README-VIC2.md) |
| `lib/mmemu-plugin-sid6581.so` | MOS 6581 SID sound chip: 3 voices + SVF filter. | [README-SID.md](README-SID.md) |
| `lib/mmemu-plugin-pia6520.so` | MOS 6520 PIA: dual 8-bit I/O ports, handshake lines. | [README-6520.md](README-6520.md) |
| `lib/mmemu-plugin-crtc6545.so` | MOS 6545/6845 CRTC video controller. | [README-6545.md](README-6545.md) |
| `lib/mmemu-plugin-pet-video.so` | PET Video Subsystem (Discrete and CRTC models). | [README-PET-VIDEO.md](README-PET-VIDEO.md) |
| `lib/mmemu-plugin-antic.so` | Atari ANTIC DMA engine and display controller. | [README-ANTIC.md](README-ANTIC.md) |
| `lib/mmemu-plugin-gtia.so` | Atari GTIA color and PMG chip. | [README-GTIA.md](README-GTIA.md) |
| `lib/mmemu-plugin-pokey.so` | Atari POKEY audio, timers, and keyboard/SIO. | [README-POKEY.md](README-POKEY.md) |
| `lib/mmemu-plugin-vera.so` | Commander X16 VERA: video (layers, sprites, FX), PSG and PCM audio, SPI. | [README-X16.md](README-X16.md) |
| `lib/mmemu-plugin-virtual-iec.so` | Commodore IEC Serial Bus (Level 2 virtual device) with .d64 support. | [README-IEC.md](README-IEC.md) |
| `lib/mmemu-plugin-hyper-serial.so` | HyperSerial debugging logger for MEGA65 / 45GS02 tests. | [README-HYPER-SERIAL.md](README-HYPER-SERIAL.md) |
| `lib/mmemu-plugin-exit-trap.so` | Minimal exit signaling device for automated test validation. | [README-EXIT-TRAP.md](README-EXIT-TRAP.md) |
| `lib/mmemu-plugin-cbm-hle.so` | Commodore KERNAL High-Level Emulation traps (Level 1). | [README-CBM-HLE.md](README-CBM-HLE.md) |

### 1.2.1 Integrated Devices (no separate .so)
These devices are compiled into the machine plugins but are documented separately:

| Device | Description | Documentation |
|--------|-------------|---------------|
| Datasette | Cassette tape drive model — playback, recording, `.tap` file I/O. | [README-DATASETTE.md](README-DATASETTE.md) |

### 1.3 Machine Plugins (Presets)
Plugins that implement `MachineDescriptor` factories.

| Plugin File | Description | Documentation |
|-------------|-------------|---------------|
| `lib/mmemu-plugin-vic20.so` | Commodore VIC-20 (stock and RAM expansions). | [README-VIC20.md](README-VIC20.md) |
| `lib/mmemu-plugin-c64.so` | Commodore 64 — full chip set and banking. | [README-C64.md](README-C64.md) |
| `lib/mmemu-plugin-c128.so` | Commodore 128 — 8502, MMU, VIC-IIe, SID, CIAs, VDC (C128 mode). | [README-C128.md](README-C128.md) |
| `lib/mmemu-plugin-pet.so` | Commodore PET Series (2001, 4032, 8032). | [README-PET.md](README-PET.md) |
| `lib/mmemu-plugin-x16.so` | Commander X16 (65C02, VERA, VIAs, I2C SMC/RTC, banked RAM/ROM). | [README-X16.md](README-X16.md) |

| Note | The `rawMega65` machine is defined via JSON in `machines/rawMega65.json` and uses the `45gs02` core and `hyper_serial` / `exit_trap` devices. | [README-RAW-MEGA65.md](README-RAW-MEGA65.md) |

### 1.4 Utility & Loader Plugins
Plugins providing support services like ROM importing or file format parsing.

| Plugin File | Description |
|-------------|-------------|
| `lib/mmemu-plugin-vice-importer.so` | Discovers and imports ROMs from a local VICE installation. |
| `lib/mmemu-plugin-cbm-loader.so` | Implements `.prg` program loading, `.crt` cartridge, and disk image parsing. |

---

## 2. Plugin API (C ABI)

mmemu uses a stable C-compatible ABI for plugins. Every plugin must export a single entry point:

```c
extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host);
```

### 2.1 The Host API (`SimPluginHostAPI`)
The host provides a set of function pointers for plugins to interact with the core emulator without linking against C++ classes.

- **Logging**: `log`, `getLogger`, `logNamed`. Named loggers are visible in the CLI/GUI "log" commands.
- **Registry Access**: `createCore`, `createDevice`, `createMachine`, `createDisassembler`, `createAssembler`.
- **Image Handling**: `findImageLoader`, `createCartridgeHandler`.
- **Extension Points**:
    - `registerPane`: Add a new tab/window to the GUI.
    - `registerCommand`: Add a new command to the CLI.
    - `registerMcpTool`: Add a new tool to the AI-facing MCP server.

### 2.2 The Manifest (`SimPluginManifest`)
The plugin describes its contents via this struct returned from `mmemuPluginInit`.

- **Metadata**: `pluginId`, `displayName`, `version`.
- **Dependencies**: `deps` (null-terminated list of required plugin IDs).
- **Filtering**: `supportedMachineIds` (null-terminated list of machines this plugin is relevant for).
- **Factories**: Arrays of `CorePluginInfo`, `DevicePluginInfo`, `MachinePluginInfo`, etc.

---

## 3. Distribution Strategy
The `lib/` directory contains the dynamic plugins (`.so` files). Standard distribution bundles include:
- **VIC-20 Support**: `6502`, `via6522`, `vic6560`, `kbd-vic20`, `vic20`, `cbm-loader`, `vice-importer`. (Datasette built-in.)
- **C64 Support**: `6502`, `c64-pla`, `cia6526`, `vic2`, `sid6581`, `c64`, `cbm-loader`, `vice-importer`, `virtual-iec` (optional). (Datasette built-in.)
- **PET Support**: `6502`, `pia6520`, `via6522`, `crtc6545`, `pet-video`, `pet`, `cbm-loader`, `vice-importer`. (Datasette built-in.)
- **MEGA65 Support (Dev)**: `45gs02`, `hyper-serial`, `exit-trap`, `cbm-loader`. (Machine defined via `rawMega65.json`.)
