# mmemu — Multi Machine Emulator
# Top-level Makefile
.PHONY: all cli gui mcp libs test test-mcp test-gdb plugins clean man serve cppcheck coverage sdk sdk-cpp sdk-python test-runner

all: cli gui mcp test-runner plugins

VERSION_HDR = src/include/version.h
VERSION    := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
GIT_HASH   := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Wno-unused-parameter -O2 -fPIC -fvisibility=default -MMD -MP
INCLUDES  = -Isrc -Isrc/include -Isrc/cli/main -Isrc/gui/main -Isrc/libcore/main \
	-Isrc/libdebug/main -Isrc/libdevices/main -Isrc/libmem/main \
	-Isrc/libtoolchain/main -Isrc/mcp/main -Isrc/plugin_loader/main \
	-Isrc/plugins/6502/main -Isrc/plugins/devices/via6522/main \
	-Isrc/plugins/devices/vic6560/main -Isrc/plugins/machines/vic20/main \
	-Isrc/plugins/devices/kbd_vic20/main -Isrc/plugins/viceImporter/main \
	-Isrc/plugins/devices/c64_pla/main -Isrc/plugins/devices/cia6526/main \
	-Isrc/plugins/devices/vic2/main -Isrc/plugins/devices/sid6581/main \
	-Isrc/plugins/devices/sid_pair/main \
	-Isrc/plugins/machines/c64/main -Isrc/plugins/devices/pia6520/main \
	-Isrc/plugins/devices/crtc6545/main -Isrc/plugins/devices/pet_video/main \
	-Isrc/plugins/devices/pokey/main -Isrc/plugins/devices/datasette/main \
	-Isrc/plugins/devices/keyboard/main \
	-Isrc/plugins/45gs02/main \
	-Isrc/plugins/devices/hyper_serial/main \
	-Isrc/plugins/devices/virtual_iec/main \
	-Isrc/plugins/devices/vic3/main \
	-Isrc/plugins/devices/mega65_hypervisor/main \
	-Isrc/plugins/devices/sdcard/main \
	-Isrc/plugins/devices/mega65_rtc/main \
	-Isrc/plugins/devices/mega65_io/main \
	-Isrc/plugins/devices/f018b_dma/main \
	-Isrc/plugins/devices/audio_dma/main \
	-Isrc/plugins/devices/map_mmu/main \
	-Isrc/plugins/machines/mega65/main \
	-Isrc/plugins/machines/pet/main -Itests/src \
	-I/usr/include/lua5.4

BINDIR   = bin
LIBDIR   = lib
ILIBDIR  = lib/internal
MANDIR   = man

# wxWidgets configuration
WXCXXFLAGS = $(shell wx-config --cxxflags)
WXLIBS     = $(shell wx-config --libs aui,xrc,html,qa,core,xml,net,base)

# Plugins must NOT link spdlog/fmt themselves; they use host API function pointers.
# The host binary (BASE_LIBS) exports spdlog symbols; dlopen resolves them at load time.
PLUGIN_LIBS =

# Library Sources
LIBMEM_SRCS       = src/libmem/main/ibus.cpp src/libmem/main/memory_bus.cpp \
	src/libmem/main/sparse_memory_bus.cpp src/libmem/main/libmem.cpp
LIBCORE_SRCS      = src/libcore/main/icore.cpp src/libcore/main/rom_loader.cpp src/libcore/main/core_registry.cpp \
	src/libcore/main/machines/machine_registry.cpp src/libcore/main/libcore.cpp \
	src/libcore/main/image_loader.cpp src/libcore/main/json_machine_loader.cpp \
	src/libcore/main/path_util.cpp src/libcore/main/sim_config.cpp
LIBDEVICES_SRCS   = src/libdevices/main/libdevices.cpp src/libdevices/main/io_registry.cpp \
                    src/libdevices/main/ivideo_output.cpp \
                    src/libdevices/main/device_registry.cpp src/libdevices/main/joystick.cpp \
                    src/libdevices/main/ieee488.cpp
LIBTOOLCHAIN_SRCS = src/libtoolchain/main/symbol_table.cpp src/libtoolchain/main/variable_symbol.cpp \
	src/libtoolchain/main/source_map.cpp src/libtoolchain/main/toolchain_registry.cpp \
	src/libtoolchain/main/debug_metadata.cpp src/libtoolchain/main/libtoolchain.cpp
LIBDEBUG_SRCS     = src/libdebug/main/breakpoint_list.cpp src/libdebug/main/debug_context.cpp \
	src/libdebug/main/trace_buffer.cpp src/libdebug/main/libdebug.cpp \
	src/libdebug/main/expression_evaluator.cpp src/libdebug/main/debug_helpers.cpp \
	src/libdebug/main/source_location_formatter.cpp src/libdebug/main/frame_analyzer.cpp \
	src/libdebug/main/stack_trace.cpp \
	src/libdebug/main/observer_registry.cpp src/libdebug/main/o45_symbol_parser.cpp \
	src/libdebug/main/o45_object_loader.cpp src/libdebug/main/lua_event_registry.cpp \
	src/libdebug/main/test_persistence.cpp src/libdebug/main/test_runner_integration.cpp \
	src/libdebug/main/test_performance_tracker.cpp
LIBPLUGINS_SRCS   = src/plugin_loader/main/plugin_loader.cpp src/plugin_loader/main/logging.cpp

# Plugin Sources
PLUGIN_CBMHLE_SRCS = src/plugins/cbm-hle/main/kernal_hle.cpp \
	src/plugins/cbm-hle/main/plugin_init.cpp

PLUGIN_6502_SRCS = src/plugins/6502/main/cpu6502.cpp \
	src/plugins/6502/main/cpu6510.cpp \
	src/plugins/6502/main/disassembler_6502.cpp \
	src/plugins/6502/main/assembler_6502.cpp \
	src/plugins/6502/main/kickassembler.cpp \
	src/plugins/6502/main/plugin_init.cpp

PLUGIN_45GS02_SRCS = src/plugins/45gs02/main/cpu45gs02.cpp \
	src/plugins/45gs02/main/ca45_assembler.cpp \
	src/plugins/45gs02/main/plugin_init.cpp

PLUGIN_HYPERSERIAL_SRCS = src/plugins/devices/hyper_serial/main/hyper_serial.cpp \
	src/plugins/devices/hyper_serial/main/plugin_init.cpp

PLUGIN_VIA6522_SRCS = src/plugins/devices/via6522/main/via6522.cpp \
	src/plugins/devices/via6522/main/plugin_init.cpp

PLUGIN_VIC6560_SRCS = src/plugins/devices/vic6560/main/vic6560.cpp \
	src/plugins/devices/vic6560/main/plugin_init.cpp

PLUGIN_KBDVIC20_SRCS = src/plugins/devices/kbd_vic20/main/kbd_vic20.cpp \
	src/plugins/devices/kbd_vic20/main/plugin_init.cpp

# Machine plugins: Split into Core vs GUI parts
PLUGIN_VIC20_CORE_SRCS =
PLUGIN_VIC20_GUI_SRCS  = src/plugins/machines/vic20/main/plugin_init.cpp

PLUGIN_C64_CORE_SRCS = src/plugins/machines/c64/main/kbd_c64.cpp
PLUGIN_C64_GUI_SRCS  = src/plugins/machines/c64/main/plugin_init.cpp

PLUGIN_VICEIMPORTER_SRCS = src/plugins/viceImporter/main/plugin_main.cpp \
	src/plugins/viceImporter/main/rom_discovery.cpp \
	src/plugins/viceImporter/main/rom_importer.cpp \
	src/plugins/viceImporter/main/rom_import_pane.cpp

PLUGIN_MEGA65IMPORTER_SRCS = src/plugins/mega65Importer/main/plugin_main.cpp \
	src/plugins/mega65Importer/main/rom_discovery.cpp \
	src/plugins/mega65Importer/main/rom_importer.cpp \
	src/plugins/mega65Importer/main/rom_import_pane.cpp

PLUGIN_C64PLA_SRCS = src/plugins/devices/c64_pla/main/c64_pla.cpp \
	src/plugins/devices/c64_pla/main/plugin_init.cpp

PLUGIN_CIA6526_SRCS = src/plugins/devices/cia6526/main/cia6526.cpp \
	src/plugins/devices/cia6526/main/plugin_init.cpp

PLUGIN_VIC2_SRCS = src/plugins/devices/vic2/main/vic2.cpp \
	src/plugins/devices/vic2/main/plugin_init.cpp

PLUGIN_VIC3_SRCS = src/plugins/devices/vic3/main/vic3.cpp \
	src/plugins/devices/vic3/main/plugin_init.cpp

PLUGIN_VIC4_SRCS = src/plugins/devices/vic4/main/vic4.cpp \
	src/plugins/devices/vic4/main/plugin_init.cpp

PLUGIN_SID6581_SRCS = src/plugins/devices/sid6581/main/sid6581.cpp \
	src/plugins/devices/sid6581/main/combined_waveforms.cpp \
	src/plugins/devices/sid6581/main/filter_curve.cpp \
	src/plugins/devices/sid6581/main/plugin_init.cpp

PLUGIN_PIA6520_SRCS = src/plugins/devices/pia6520/main/pia6520.cpp \
	src/plugins/devices/pia6520/main/plugin_init.cpp

PLUGIN_CBMLOADER_SRCS = src/plugins/cbm-loader/main/prg_loader.cpp \
	src/plugins/cbm-loader/main/crt_parser.cpp \
	src/plugins/cbm-loader/main/cbm_cart_handler.cpp \
	src/plugins/cbm-loader/main/cbm_sector_disk.cpp \
	src/plugins/cbm-loader/main/d64_parser.cpp \
	src/plugins/cbm-loader/main/g64_parser.cpp \
	src/plugins/cbm-loader/main/t64_parser.cpp \
	src/plugins/cbm-loader/main/disk_loader.cpp \
	src/plugins/cbm-loader/main/plugin_init.cpp

PLUGIN_CRTC6545_SRCS = src/plugins/devices/crtc6545/main/crtc6545.cpp \
	src/plugins/devices/crtc6545/main/plugin_init.cpp

PLUGIN_PETVIDEO_SRCS = src/plugins/devices/pet_video/main/pet_video.cpp \
	src/plugins/devices/pet_video/main/plugin_init.cpp

PLUGIN_PET_SRCS = src/plugins/machines/pet/main/plugin_init.cpp \
	src/plugins/devices/keyboard/main/keyboard_matrix_pet.cpp

PLUGIN_ANTIC_SRCS = src/plugins/devices/antic/main/antic.cpp \
	src/plugins/devices/antic/main/plugin_init.cpp

PLUGIN_GTIA_SRCS = src/plugins/devices/gtia/main/gtia.cpp \
	src/plugins/devices/gtia/main/plugin_init.cpp

PLUGIN_DATASETTE_SRCS = src/plugins/devices/datasette/main/datasette.cpp \
        src/plugins/cbm-loader/main/tap_parser.cpp \
        src/plugins/devices/datasette/main/plugin_init.cpp

PLUGIN_POKEY_SRCS = src/plugins/devices/pokey/main/pokey.cpp \
	src/plugins/devices/pokey/main/plugin_init.cpp

PLUGIN_VIRTUALIEC_SRCS = src/plugins/devices/virtual_iec/main/virtual_iec.cpp \
	src/plugins/devices/virtual_iec/main/plugin_init.cpp \
	src/plugins/cbm-loader/main/cbm_sector_disk.cpp \
	src/plugins/cbm-loader/main/d64_parser.cpp \
	src/plugins/cbm-loader/main/t64_parser.cpp \
	src/plugins/cbm-loader/main/g64_parser.cpp

PLUGIN_F018B_DMA_SRCS = src/plugins/devices/f018b_dma/main/f018b_dma.cpp \
	src/plugins/devices/f018b_dma/main/plugin_init.cpp

PLUGIN_AUDIODMA_SRCS = src/plugins/devices/audio_dma/main/audio_dma.cpp \
	src/plugins/devices/audio_dma/main/plugin_init.cpp

PLUGIN_MAP_MMU_SRCS = src/plugins/devices/map_mmu/main/map_mmu.cpp \
	src/plugins/devices/map_mmu/main/key_register.cpp \
	src/plugins/devices/map_mmu/main/c64_bank_controller.cpp \
	src/plugins/devices/map_mmu/main/plugin_init.cpp

PLUGIN_SID_PAIR_SRCS = src/plugins/devices/sid_pair/main/sid_pair.cpp \
	src/plugins/devices/sid_pair/main/plugin_init.cpp

PLUGIN_MEGA65_SRCS = src/plugins/machines/mega65/main/machine_mega65.cpp \
	src/plugins/devices/keyboard/main/keyboard_matrix_mega65.cpp \
	src/plugins/devices/cia6526/main/cia6526.cpp \
	src/plugins/devices/vic4/main/vic4.cpp \
	src/plugins/devices/vic3/main/vic3.cpp \
	src/plugins/devices/vic2/main/vic2.cpp \
	src/plugins/devices/map_mmu/main/map_mmu.cpp \
	src/plugins/devices/map_mmu/main/key_register.cpp \
	src/plugins/devices/map_mmu/main/c64_bank_controller.cpp \
	src/plugins/devices/f018b_dma/main/f018b_dma.cpp \
	src/plugins/devices/audio_dma/main/audio_dma.cpp \
	src/plugins/devices/mega65_math/main/mega65_math.cpp \
	src/plugins/devices/mega65_hypervisor/main/hypervisor_regs.cpp \
	src/plugins/devices/mega65_hypervisor/main/hdos_handler.cpp \
	src/plugins/devices/sdcard/main/sdcard.cpp \
	src/plugins/devices/mega65_rtc/main/mega65_rtc.cpp \
	src/plugins/devices/mega65_io/main/mega65_io_stub.cpp \
	src/plugins/machines/mega65/main/plugin_init.cpp

PLUGIN_EXIT_TRAP_SRCS = src/plugins/devices/exit_trap/main/exit_trap.cpp \
	src/plugins/devices/exit_trap/main/plugin_init.cpp

PLUGIN_MEGA65_MATH_SRCS = src/plugins/devices/mega65_math/main/mega65_math.cpp \
	src/plugins/devices/mega65_math/main/plugin_init.cpp

GUI_SRCS = src/gui/main/main.cpp \
	src/gui/main/machine_selector.cpp \
	src/gui/main/register_pane.cpp \
	src/gui/main/memory_pane.cpp \
	src/gui/main/disasm_pane.cpp \
	src/gui/main/console_pane.cpp \
	src/gui/main/cartridge_pane.cpp \
	src/gui/main/breakpoint_pane.cpp \
	src/gui/main/symbol_pane.cpp \
	src/gui/main/stack_pane.cpp \
	src/gui/main/machine_inspector_pane.cpp \
	src/gui/main/device_info_pane.cpp \
	src/gui/main/register_watch_pane.cpp \
	src/gui/main/trace_pane.cpp \
	src/gui/main/screen_pane.cpp \
        src/gui/main/tape_pane.cpp \
        src/gui/main/drive_status_pane.cpp \
	src/gui/main/mega65_status_pane.cpp \
	src/gui/main/dialogs/assemble_dialog.cpp \
	src/gui/main/dialogs/calculator_dialog.cpp \
	src/gui/main/dialogs/tool_runner_dialog.cpp \
	src/cli/main/cli_interpreter.cpp \
	src/gui/main/plugin_pane_manager.cpp \
	src/cli/main/plugin_command_registry.cpp \
	src/gui/main/gui_utils.cpp \
	src/gui/main/audio_output.cpp

CLI_SRCS = src/cli/main/main.cpp \
	src/cli/main/cli_interpreter.cpp \
	src/cli/main/gdb_server.cpp \
	src/cli/main/serial_monitor_server.cpp \
	src/cli/main/vice_monitor_protocol.cpp \
	src/cli/main/vice_monitor_server.cpp \
	src/cli/main/vice_snapshot.cpp \
	src/cli/main/lua_engine.cpp \
	src/cli/main/plugin_command_registry.cpp \
	src/cli/main/hardware_test_bridge.cpp \
	src/cli/main/cross_validation_runner.cpp \
	src/cli/main/unified_test_runner.cpp

MCP_SRCS = src/mcp/main/main.cpp src/plugins/devices/datasette/main/datasette.cpp src/plugins/cbm-loader/main/tap_parser.cpp \
	src/mcp/main/plugin_tool_registry.cpp

# Test Sources
TEST_SRCS = tests/src/test_main.cpp \
	tests/src/test_c64_tape.cpp \
	tests/src/test_tape_roundtrip.cpp \
	src/cli/test/test_cli.cpp \
	src/gui/test/test_gui_logic.cpp \
	src/libmem/test/test_flatmembus.cpp \
	src/libmem/test/test_sparse_memory_bus.cpp \
	src/libcore/test/test_libcore.cpp \
	src/libcore/test/test_registry.cpp \
	src/libcore/test/test_json_machine_loader.cpp \
	src/libcore/test/test_machine_boot.cpp \
	src/libdevices/test/test_devices.cpp \
	src/libdevices/test/test_ieee488.cpp \
	src/libdebug/test/test_debug.cpp \
	src/libdebug/test/test_trace_buffer.cpp \
	src/libdebug/test/test_expression_evaluator.cpp \
	src/libdebug/test/test_persistence.cpp \
	src/libtoolchain/test/test_toolchain.cpp \
	src/libtoolchain/test/test_symbol_table_enhanced.cpp \
	src/libtoolchain/test/test_debug_metadata.cpp \
	src/libcore/test/test_machine_symbols.cpp \
	src/plugins/6502/test/test_cpu6502.cpp \
	src/plugins/6502/test/test_disasm6502.cpp \
	src/plugins/6502/test/test_assembler6502.cpp \
	src/plugins/6502/test/test_cpu6510_io.cpp \
	src/plugins/machines/vic20/test/test_vic20_integration.cpp \
	src/plugins/machines/c64/test/test_c64_integration.cpp \
	src/plugins/machines/pet/test/test_pet_integration.cpp \
	src/plugins/cbm-loader/test/test_cbm_loader.cpp \
	src/plugins/cbm-loader/test/test_disk_formats.cpp \
	src/plugins/devices/pet_video/test/test_pet_video.cpp \
	src/plugins/devices/pia6520/test/test_pia6520.cpp \
	src/plugins/devices/via6522/test/test_via6522.cpp \
	src/plugins/devices/antic/test/test_antic.cpp \
	src/plugins/devices/gtia/test/test_gtia.cpp \
	src/plugins/devices/pokey/test/test_pokey.cpp \
	src/plugins/devices/cia6526/test/test_cia6526.cpp \
	src/plugins/devices/virtual_iec/test/test_virtual_iec.cpp \
	src/plugins/devices/virtual_iec/test/test_iec_d64.cpp \
	src/plugins/cbm-hle/test/test_kernal_hle.cpp \
	src/plugins/devices/f018b_dma/test/test_f018b_dma.cpp \
	src/plugins/devices/audio_dma/test/test_audio_dma.cpp \
	src/plugins/devices/map_mmu/test/test_map_mmu.cpp \
	src/plugins/devices/map_mmu/test/test_key_register.cpp \
	src/plugins/devices/map_mmu/test/test_c64_bank_controller.cpp \
	src/plugins/devices/vic4/test/test_vic4.cpp \
	src/plugins/machines/mega65/test/test_mega65_map.cpp \
	src/plugins/machines/mega65/test/test_mega65_integration.cpp \
	src/plugins/machines/mega65/test/test_mega65_keyboard.cpp \
	tests/src/test_cbm_disk_images.cpp \
	tests/src/test_d81_directory_listing.cpp \
	tests/src/test_plugin_validation.cpp \
	tests/src/test_ffd0_bug.cpp \
	tests/src/test_mega65_chips.cpp \
	src/plugins/devices/sid_pair/test/test_sid_pair.cpp \
	src/plugins/devices/sid6581/test/test_combined_waveforms.cpp \
	src/plugins/devices/sid6581/test/test_sid_filter.cpp \
	src/plugins/devices/sid6581/test/test_soft_clip_distortion.cpp \
	tests/src/test_sid_filter_integration.cpp \
	src/libdebug/test/test_stack_trace.cpp \
	src/libdebug/test/test_breakpoint_list.cpp \
	src/libdebug/test/test_frame_analyzer.cpp \
	src/libdebug/test/test_lua_event_registry.cpp \
	tests/src/test_mcp_integration.cpp \
	src/libtoolchain/test/test_source_map.cpp \
	src/plugins/cbm-loader/test/test_tap_parser.cpp \
	src/plugins/devices/mega65_math/test/test_mega65_math.cpp \
	src/plugins/devices/mega65_rtc/test/test_mega65_rtc.cpp \
	src/plugins/devices/mega65_hypervisor/test/test_hypervisor.cpp \
	src/plugins/devices/datasette/test/test_datasette.cpp \
	src/plugins/devices/crtc6545/test/test_crtc6545.cpp \
	src/libcore/test/test_sim_config.cpp \
	src/plugins/45gs02/test/test_cpu45gs02.cpp \
	src/plugins/45gs02/test/test_neg_prefix_flags.cpp \
	src/plugins/45gs02/test/test_neg_prefix_modes.cpp \
	src/plugins/devices/vic2/test/test_vic2_unit.cpp \
	src/plugins/devices/vic3/test/test_vic3.cpp \
	src/plugins/devices/virtual_iec/test/test_virtual_iec_unit.cpp \
	tests/src/test_cross_validation.cpp \
	tests/src/test_sid_filter_xemu_validation.cpp \
	src/cli/test/test_sid_program_generation.cpp

LIBDEBUG_TEST_SRCS = src/libdebug/test/test_breakpoints.cpp \
	src/libdebug/test/test_o45_symbol_parser.cpp
LIBCORE_TEST_SRCS = src/libcore/test/test_c_compatibility.c
PLUGINLOADER_TEST_SRCS = src/plugin_loader/test/test_plugin_extension.cpp
PLUGIN_VICEIMPORTER_TEST_SRCS = src/plugins/viceImporter/test/test_vice_importer.cpp
PLUGIN_MEGA65IMPORTER_TEST_SRCS = src/plugins/mega65Importer/test/test_mega65_importer.cpp
PLUGIN_VIC2_TEST_SRCS = src/plugins/devices/vic2/test/test_c64_video.cpp \
	src/plugins/devices/vic2/test/test_c64_boot_screen.cpp
PLUGIN_ANTIC_TEST_SRCS = src/plugins/devices/antic/test/test_atari_boot.cpp \
	src/plugins/devices/antic/test/test_atari_debug.cpp



# Test-related objects (excluding plugin entry points to avoid multiple mmemuPluginInit definitions)
ALL_PLUGIN_OBJS = src/plugins/6502/main/cpu6502.o \
	src/plugins/6502/main/cpu6510.o \
	src/plugins/6502/main/disassembler_6502.o \
	src/plugins/6502/main/assembler_6502.o \
	src/plugins/6502/main/kickassembler.o \
	src/plugins/45gs02/main/cpu45gs02.o \
	src/plugins/devices/via6522/main/via6522.o \
	src/plugins/devices/vic6560/main/vic6560.o \
	src/plugins/devices/kbd_vic20/main/kbd_vic20.o \
	src/plugins/viceImporter/main/rom_discovery.o \
	src/plugins/viceImporter/main/rom_importer.o \
	src/plugins/mega65Importer/main/rom_discovery.o \
	src/plugins/mega65Importer/main/rom_importer.o \
	src/plugins/devices/c64_pla/main/c64_pla.o \
	src/plugins/devices/cia6526/main/cia6526.o \
	src/plugins/devices/vic2/main/vic2.o \
	src/plugins/devices/sid6581/main/sid6581.o \
	src/plugins/devices/sid6581/main/combined_waveforms.o \
	src/plugins/devices/sid6581/main/filter_curve.o \
	src/plugins/devices/sid_pair/main/sid_pair.o \
	src/plugins/devices/datasette/main/datasette.o \
	src/plugins/cbm-loader/main/tap_parser.o \
	src/plugins/machines/c64/main/kbd_c64.o \
	src/plugins/devices/pia6520/main/pia6520.o \
	src/plugins/cbm-loader/main/prg_loader.o \
	src/plugins/cbm-loader/main/crt_parser.o \
	src/plugins/cbm-loader/main/cbm_cart_handler.o \
	src/plugins/cbm-loader/main/cbm_sector_disk.o \
	src/plugins/cbm-loader/main/d64_parser.o \
	src/plugins/cbm-loader/main/g64_parser.o \
	src/plugins/cbm-loader/main/t64_parser.o \
	src/plugins/cbm-loader/main/disk_loader.o \
	src/plugins/devices/crtc6545/main/crtc6545.o \
	src/plugins/devices/pet_video/main/pet_video.o \
	src/plugins/devices/keyboard/main/keyboard_matrix_pet.o \
	src/plugins/devices/keyboard/main/keyboard_matrix_mega65.o \
	src/plugins/devices/antic/main/antic.o \
	src/plugins/devices/gtia/main/gtia.o \
	src/plugins/devices/pokey/main/pokey.o \
	src/plugins/devices/virtual_iec/main/virtual_iec.o \
	src/plugins/devices/f018b_dma/main/f018b_dma.o \
	src/plugins/devices/audio_dma/main/audio_dma.o \
	src/plugins/devices/map_mmu/main/map_mmu.o \
	src/plugins/devices/map_mmu/main/key_register.o \
	src/plugins/devices/map_mmu/main/c64_bank_controller.o \
	src/plugins/devices/vic3/main/vic3.o \
	src/plugins/devices/vic4/main/vic4.o \
	src/plugins/devices/mega65_math/main/mega65_math.o \
	src/plugins/devices/mega65_hypervisor/main/hypervisor_regs.o \
	src/plugins/devices/mega65_hypervisor/main/hdos_handler.o \
	src/plugins/devices/sdcard/main/sdcard.o \
	src/plugins/devices/mega65_rtc/main/mega65_rtc.o \
	src/plugins/devices/mega65_io/main/mega65_io_stub.o \
	src/plugins/devices/hyper_serial/main/hyper_serial.o \
	src/plugins/devices/exit_trap/main/exit_trap.o \
	src/plugins/machines/mega65/main/machine_mega65.o \
	src/plugins/cbm-hle/main/kernal_hle.o
REGISTRY_OBJS = src/cli/main/cli_interpreter.o \
	src/cli/main/plugin_command_registry.o \
	src/mcp/main/plugin_tool_registry.o \
	src/mcp/main/main_test.o \
	src/gui/main/gui_utils.o \
	src/gui/main/plugin_pane_manager.o

TEST_OBJS = $(TEST_SRCS:.cpp=.o) \
            $(LIBDEBUG_TEST_SRCS:.cpp=.o) \
            $(LIBCORE_TEST_SRCS:.c=.o) \
            $(PLUGINLOADER_TEST_SRCS:.cpp=.o) \
            $(PLUGIN_VICEIMPORTER_TEST_SRCS:.cpp=.o) \
            $(PLUGIN_MEGA65IMPORTER_TEST_SRCS:.cpp=.o) \
            $(PLUGIN_VIC2_TEST_SRCS:.cpp=.o) \
	$(sort $(ALL_PLUGIN_OBJS)) \
	$(REGISTRY_OBJS)
TEST_BIN  = $(BINDIR)/mmemu-test

# Objects
LIBMEM_OBJS       = $(LIBMEM_SRCS:.cpp=.o)
LIBCORE_OBJS      = $(LIBCORE_SRCS:.cpp=.o)
LIBDEVICES_OBJS   = $(LIBDEVICES_SRCS:.cpp=.o)
LIBTOOLCHAIN_OBJS = $(LIBTOOLCHAIN_SRCS:.cpp=.o)
LIBDEBUG_OBJS     = $(LIBDEBUG_SRCS:.cpp=.o)
LIBPLUGINS_OBJS   = $(LIBPLUGINS_SRCS:.cpp=.o)

PLUGIN_6502_OBJS  = $(PLUGIN_6502_SRCS:.cpp=.o)
PLUGIN_45GS02_OBJS = $(PLUGIN_45GS02_SRCS:.cpp=.o)
PLUGIN_HYPERSERIAL_OBJS = $(PLUGIN_HYPERSERIAL_SRCS:.cpp=.o)
PLUGIN_VIA6522_OBJS = $(PLUGIN_VIA6522_SRCS:.cpp=.o)
PLUGIN_VIC6560_OBJS = $(PLUGIN_VIC6560_SRCS:.cpp=.o)
PLUGIN_KBDVIC20_OBJS = $(PLUGIN_KBDVIC20_SRCS:.cpp=.o)
PLUGIN_VIC20_CORE_OBJS = $(PLUGIN_VIC20_CORE_SRCS:.cpp=.o) \
	src/plugins/devices/via6522/main/via6522.o \
	src/plugins/devices/vic6560/main/vic6560.o
PLUGIN_VIC20_GUI_OBJS  = src/plugins/machines/vic20/main/plugin_init.o
PLUGIN_VICEIMPORTER_OBJS = $(PLUGIN_VICEIMPORTER_SRCS:.cpp=.o)
PLUGIN_MEGA65IMPORTER_OBJS = $(PLUGIN_MEGA65IMPORTER_SRCS:.cpp=.o)
PLUGIN_C64PLA_OBJS   = $(PLUGIN_C64PLA_SRCS:.cpp=.o)
PLUGIN_CIA6526_OBJS  = $(PLUGIN_CIA6526_SRCS:.cpp=.o)
PLUGIN_VIC2_OBJS   = $(PLUGIN_VIC2_SRCS:.cpp=.o)
PLUGIN_VIC3_OBJS   = $(PLUGIN_VIC3_SRCS:.cpp=.o)
PLUGIN_VIC4_OBJS   = $(PLUGIN_VIC4_SRCS:.cpp=.o)
PLUGIN_SID6581_OBJS = $(PLUGIN_SID6581_SRCS:.cpp=.o)

PLUGIN_C64_CORE_OBJS = $(PLUGIN_C64_CORE_SRCS:.cpp=.o) \
	src/plugins/devices/cia6526/main/cia6526.o \
	src/plugins/devices/vic2/main/vic2.o \
	src/plugins/devices/sid6581/main/sid6581.o \
	src/plugins/devices/c64_pla/main/c64_pla.o
PLUGIN_C64_GUI_OBJS  = src/plugins/machines/c64/main/plugin_init.o
PLUGIN_PIA6520_OBJS  = $(PLUGIN_PIA6520_SRCS:.cpp=.o)
PLUGIN_CBMLOADER_OBJS = $(PLUGIN_CBMLOADER_SRCS:.cpp=.o)
PLUGIN_CRTC6545_OBJS = $(PLUGIN_CRTC6545_SRCS:.cpp=.o)
PLUGIN_PETVIDEO_OBJS = $(PLUGIN_PETVIDEO_SRCS:.cpp=.o)
PLUGIN_PET_OBJS      = $(PLUGIN_PET_SRCS:.cpp=.o) \
	src/plugins/devices/pia6520/main/pia6520.o \
	src/plugins/devices/via6522/main/via6522.o \
	src/plugins/devices/crtc6545/main/crtc6545.o \
	src/plugins/devices/pet_video/main/pet_video.o

PLUGIN_ANTIC_OBJS = $(PLUGIN_ANTIC_SRCS:.cpp=.o)
PLUGIN_GTIA_OBJS = $(PLUGIN_GTIA_SRCS:.cpp=.o)
PLUGIN_DATASETTE_OBJS = $(PLUGIN_DATASETTE_SRCS:.cpp=.o)
PLUGIN_POKEY_OBJS = $(PLUGIN_POKEY_SRCS:.cpp=.o)
PLUGIN_VIRTUALIEC_OBJS = $(PLUGIN_VIRTUALIEC_SRCS:.cpp=.o)
PLUGIN_F018B_DMA_OBJS = $(PLUGIN_F018B_DMA_SRCS:.cpp=.o)
PLUGIN_AUDIODMA_OBJS = $(PLUGIN_AUDIODMA_SRCS:.cpp=.o)
PLUGIN_MAP_MMU_OBJS = $(PLUGIN_MAP_MMU_SRCS:.cpp=.o)
PLUGIN_SID_PAIR_OBJS = $(PLUGIN_SID_PAIR_SRCS:.cpp=.o)
PLUGIN_MEGA65_OBJS = $(PLUGIN_MEGA65_SRCS:.cpp=.o)
PLUGIN_EXIT_TRAP_OBJS = $(PLUGIN_EXIT_TRAP_SRCS:.cpp=.o)
PLUGIN_MEGA65_MATH_OBJS = $(PLUGIN_MEGA65_MATH_SRCS:.cpp=.o)
PLUGIN_CBMHLE_OBJS = $(PLUGIN_CBMHLE_SRCS:.cpp=.o)

GUI_OBJS = $(GUI_SRCS:.cpp=.o)

CLI_OBJS = $(CLI_SRCS:.cpp=.o)
CLI_OBJS_FOR_TEST = $(filter-out src/cli/main/main.o,$(CLI_OBJS))
MCP_OBJS = $(MCP_SRCS:.cpp=.o)

# Binaries
CLI_BIN = $(BINDIR)/mmemu-cli
GUI_BIN = $(BINDIR)/mmemu-gui
MCP_BIN = $(BINDIR)/mmemu-mcp
TEST_RUNNER_BIN = $(BINDIR)/mmemu-test-runner

PLUGINS = $(LIBDIR)/mmemu-plugin-6502.so \
	$(LIBDIR)/mmemu-plugin-45gs02.so \
	$(LIBDIR)/mmemu-plugin-hyper-serial.so \
	$(LIBDIR)/mmemu-plugin-via6522.so \
	$(LIBDIR)/mmemu-plugin-vic6560.so \
	$(LIBDIR)/mmemu-plugin-vic20.so \
	$(LIBDIR)/mmemu-plugin-kbd-vic20.so \
	$(LIBDIR)/mmemu-plugin-vice-importer.so \
	$(LIBDIR)/mmemu-plugin-mega65-importer.so \
	$(LIBDIR)/mmemu-plugin-c64-pla.so \
	$(LIBDIR)/mmemu-plugin-cia6526.so \
	$(LIBDIR)/mmemu-plugin-vic2.so \
	$(LIBDIR)/mmemu-plugin-vic3.so \
	$(LIBDIR)/mmemu-plugin-vic4.so \
	$(LIBDIR)/mmemu-plugin-sid6581.so \
	$(LIBDIR)/mmemu-plugin-c64.so \
	$(LIBDIR)/mmemu-plugin-pia6520.so \
	$(LIBDIR)/mmemu-plugin-cbm-loader.so \
	$(LIBDIR)/mmemu-plugin-crtc6545.so \
	$(LIBDIR)/mmemu-plugin-pet-video.so \
	$(LIBDIR)/mmemu-plugin-pet.so \
	$(LIBDIR)/mmemu-plugin-antic.so \
	$(LIBDIR)/mmemu-plugin-gtia.so \
	$(LIBDIR)/mmemu-plugin-pokey.so \
        $(LIBDIR)/mmemu-plugin-datasette.so \
	$(LIBDIR)/mmemu-plugin-virtual-iec.so \
	$(LIBDIR)/mmemu-plugin-f018b-dma.so \
	$(LIBDIR)/mmemu-plugin-audio-dma.so \
	$(LIBDIR)/mmemu-plugin-map-mmu.so \
	$(LIBDIR)/mmemu-plugin-mega65.so \
	$(LIBDIR)/mmemu-plugin-exit-trap.so \
	$(LIBDIR)/mmemu-plugin-mega65-math.so \
	$(LIBDIR)/mmemu-plugin-sid-pair.so \
	$(LIBDIR)/mmemu-plugin-cbm-hle.so

LIBS = $(ILIBDIR)/libmem.a $(ILIBDIR)/libcore.a $(ILIBDIR)/libdevices.a \
	$(ILIBDIR)/libtoolchain.a $(ILIBDIR)/libdebug.a $(ILIBDIR)/libplugins.a

BASE_LIBS = -Wl,--whole-archive -L$(ILIBDIR) -lplugins -ldebug -ltoolchain -ldevices -lcore -lmem -Wl,--no-whole-archive -ldl -lspdlog -lfmt /lib/x86_64-linux-gnu/liblua5.4.so

# ---------------------------------------------------------------------------
# Build rules
# ---------------------------------------------------------------------------

cli: $(CLI_BIN) plugins
gui: $(GUI_BIN) plugins
mcp: $(MCP_BIN) plugins
libs: $(LIBS)
plugins: $(PLUGINS)

$(BINDIR) $(LIBDIR) $(ILIBDIR):
	mkdir -p $@

# Internal Libraries
$(ILIBDIR)/libmem.a: $(LIBMEM_OBJS) | $(ILIBDIR)
	$(AR) rcs $@ $^

$(ILIBDIR)/libcore.a: $(LIBCORE_OBJS) | $(ILIBDIR)
	$(AR) rcs $@ $^

$(ILIBDIR)/libdevices.a: $(LIBDEVICES_OBJS) | $(ILIBDIR)
	$(AR) rcs $@ $^

$(ILIBDIR)/libtoolchain.a: $(LIBTOOLCHAIN_OBJS) | $(ILIBDIR)
	$(AR) rcs $@ $^

$(ILIBDIR)/libdebug.a: $(LIBDEBUG_OBJS) | $(ILIBDIR)
	$(AR) rcs $@ $^

$(ILIBDIR)/libplugins.a: $(LIBPLUGINS_OBJS) | $(ILIBDIR)
	$(AR) rcs $@ $^

# Plugin rules
$(LIBDIR)/mmemu-plugin-6502.so: $(PLUGIN_6502_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-45gs02.so: $(PLUGIN_45GS02_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-hyper-serial.so: $(PLUGIN_HYPERSERIAL_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-via6522.so: $(PLUGIN_VIA6522_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-vic6560.so: $(PLUGIN_VIC6560_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-vic20.so: $(PLUGIN_VIC20_CORE_OBJS) $(PLUGIN_VIC20_GUI_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-kbd-vic20.so: $(PLUGIN_KBDVIC20_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-vice-importer.so: $(PLUGIN_VICEIMPORTER_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-mega65-importer.so: $(PLUGIN_MEGA65IMPORTER_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-c64-pla.so: $(PLUGIN_C64PLA_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-cia6526.so: $(PLUGIN_CIA6526_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-vic2.so: $(PLUGIN_VIC2_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-vic3.so: $(PLUGIN_VIC3_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-vic4.so: $(PLUGIN_VIC4_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-sid6581.so: $(PLUGIN_SID6581_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-c64.so: $(PLUGIN_C64_CORE_OBJS) $(PLUGIN_C64_GUI_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-pia6520.so: $(PLUGIN_PIA6520_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-cbm-loader.so: $(PLUGIN_CBMLOADER_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-crtc6545.so: $(PLUGIN_CRTC6545_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-pet-video.so: $(PLUGIN_PETVIDEO_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-pet.so: $(PLUGIN_PET_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-antic.so: $(PLUGIN_ANTIC_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-gtia.so: $(PLUGIN_GTIA_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-datasette.so: $(PLUGIN_DATASETTE_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-pokey.so: $(PLUGIN_POKEY_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-virtual-iec.so: $(PLUGIN_VIRTUALIEC_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-f018b-dma.so: $(PLUGIN_F018B_DMA_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-audio-dma.so: $(PLUGIN_AUDIODMA_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-map-mmu.so: $(PLUGIN_MAP_MMU_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-mega65.so: $(PLUGIN_MEGA65_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-exit-trap.so: $(PLUGIN_EXIT_TRAP_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-mega65-math.so: $(PLUGIN_MEGA65_MATH_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-sid-pair.so: $(PLUGIN_SID_PAIR_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-cbm-hle.so: $(PLUGIN_CBMHLE_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

# Version header (regenerated when git hash changes)
$(VERSION_HDR): VERSION .FORCE
	@NEW='#pragma once\n// Auto-generated by Makefile — do not edit\n#define MMSIM_VERSION "$(VERSION)"\n#define MMSIM_GIT_HASH "$(GIT_HASH)"\n#define MMSIM_VERSION_FULL MMSIM_VERSION "." MMSIM_GIT_HASH\n'; \
	if [ ! -f $@ ] || [ "$$(cat $@)" != "$$(printf '$$NEW')" ]; then \
		printf '%b\n' "$$NEW" > $@; \
	fi
.FORCE:

src/cli/main/main.o: $(VERSION_HDR)
src/mcp/main/main.o: $(VERSION_HDR)

# Binary rules
$(CLI_BIN): $(CLI_OBJS) $(LIBS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -rdynamic -o $@ $(CLI_OBJS) $(BASE_LIBS)

$(GUI_BIN): $(GUI_OBJS) $(LIBS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(WXCXXFLAGS) -rdynamic -o $@ $(GUI_OBJS) src/cli/main/lua_engine.o src/cli/main/vice_snapshot.o $(BASE_LIBS) $(WXLIBS) -lasound

$(MCP_BIN): $(MCP_OBJS) $(LIBS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -rdynamic -o $@ $(MCP_OBJS) src/cli/main/lua_engine.o $(BASE_LIBS)

# Test runner binary (unified multi-backend test framework)
TEST_RUNNER_OBJS = src/cli/main/test_runner_main.o \
	src/cli/main/unified_test_runner.o \
	src/cli/main/test_report_generator.o \
	src/cli/main/hardware_test_bridge.o \
	src/cli/main/cross_validation_runner.o \
	src/cli/main/lua_engine.o

$(TEST_RUNNER_BIN): $(TEST_RUNNER_OBJS) $(LIBS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -rdynamic -o $@ $(TEST_RUNNER_OBJS) $(BASE_LIBS)

test-runner: $(TEST_RUNNER_BIN) plugins

$(TEST_BIN): $(TEST_OBJS) $(LIBS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -DTEST_BUILD -Itests/src -rdynamic -o $@ $(TEST_OBJS) src/cli/main/lua_engine.o src/cli/main/vice_snapshot.o src/cli/main/hardware_test_bridge.o src/cli/main/cross_validation_runner.o $(BASE_LIBS) $(WXLIBS) -lasound

# Generic rules
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(WXCXXFLAGS) -c -o $@ $<

# Special rule for test objects to include TEST_BUILD
src/%/test/%.o: src/%/test/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(WXCXXFLAGS) -DTEST_BUILD -c -o $@ $<

src/mcp/main/main_test.o: src/mcp/main/main.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(WXCXXFLAGS) -DTEST_BUILD -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

test: $(TEST_BIN) plugins test-mega65 test-mcp test-gdb
	./$(TEST_BIN)

test-performance: $(TEST_BIN) plugins
	@echo "Running full test suite with performance profiling..."
	./$(TEST_BIN) -performance

test-mcp: $(MCP_BIN) plugins
	@echo "Running MCP integration tests..."
	python3 src/mcp/test/mcp_test.py

test-gdb: $(CLI_BIN) plugins
	@echo "Running GDB RSP integration tests..."
	python3 src/cli/test/test_gdb_server.py

test-mega65: $(CLI_BIN) plugins
	@echo "Running 45GS02 Validation Suite..."
	./tests/45gs02/validate.py tests/45gs02/arithmetic.s
	./tests/45gs02/validate.py tests/45gs02/transfers.s
	./tests/45gs02/validate.py tests/45gs02/advanced.s
	./tests/45gs02/validate.py tests/45gs02/quad.s

COVDIR = coverage

COV_CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -Wno-unused-parameter -O0 -g --coverage -fPIC -fvisibility=default -MMD -MP
COV_CFLAGS   = -O0 -g --coverage

coverage:
	@echo "=== Building with coverage instrumentation ==="
	$(MAKE) clean
	$(MAKE) $(TEST_BIN) $(CLI_BIN) plugins CXXFLAGS="$(COV_CXXFLAGS)" CFLAGS="$(COV_CFLAGS)"
	@echo "=== Running unit tests ==="
	-./$(TEST_BIN)
	@echo "=== Running 45GS02 validation ==="
	-$(MAKE) test-mega65
	@echo ""
	@echo "=== Collecting coverage data ==="
	@mkdir -p $(COVDIR)
	@if command -v lcov >/dev/null 2>&1; then \
		lcov --capture --directory src --output-file $(COVDIR)/coverage.info --ignore-errors mismatch; \
		lcov --remove $(COVDIR)/coverage.info '*/test/*' '/usr/*' --output-file $(COVDIR)/coverage.info; \
		genhtml $(COVDIR)/coverage.info --output-directory $(COVDIR)/html; \
		echo ""; \
		echo "=== HTML report: $(COVDIR)/html/index.html ==="; \
	else \
		echo "(lcov not found — generating text summary via gcov)"; \
		find src -name '*.gcno' -exec gcov -r {} + > $(COVDIR)/gcov.log 2>&1; \
		mv *.gcov $(COVDIR)/ 2>/dev/null || true; \
		echo "=== Coverage Summary ==="; \
		awk '/^File.*src\//{file=$$0} /^Lines executed:/{if(file){print file; print $$0; print ""; file=""}}' $(COVDIR)/gcov.log; \
		echo "=== Full details: $(COVDIR)/gcov.log ==="; \
	fi

cppcheck:
	@echo "Running cppcheck..."
	cppcheck --enable=all --suppress=missingIncludeSystem --inline-suppr --quiet --force $(INCLUDES) src

serve: $(MCP_BIN) plugins
	@echo "============================================"
	@echo " mmemu MCP server"
	@echo "============================================"
	@echo ""
	@echo "--- Claude (claude_desktop_config.json or .mcp.json) ---"
	@echo '{'
	@echo '  "mcpServers": {'
	@echo '    "mmemu": {'
	@echo '      "command": "$(CURDIR)/$(MCP_BIN)",'
	@echo '      "args": [],'
	@echo '      "cwd": "$(CURDIR)"'
	@echo '    }'
	@echo '  }'
	@echo '}'
	@echo ""
	@echo "--- Gemini (mcp_config.json) ---"
	@echo '{'
	@echo '  "mcpServers": {'
	@echo '    "mmemu": {'
	@echo '      "command": "$(CURDIR)/$(MCP_BIN)",'
	@echo '      "args": [],'
	@echo '      "cwd": "$(CURDIR)",'
	@echo '      "timeout": 30'
	@echo '    }'
	@echo '  }'
	@echo '}'
	@echo ""
	@echo "============================================"
	@echo "Starting server on stdio..."
	@echo "============================================"
	@exec $(CURDIR)/$(MCP_BIN)

sdk: sdk-python sdk-cpp
	@echo "✓ SDKs built"

sdk-python:
	@echo "============================================"
	@echo " Python SDK (ready to use)"
	@echo "============================================"
	@echo "SDK location: sdk/python/"
	@echo ""
	@echo "Quick start:"
	@echo "  python3 sdk/python/examples/memory_inspector.py --host localhost --port 2000"
	@echo "  python3 sdk/python/examples/breakpoint_manager.py list"
	@echo ""
	@echo "Documentation:"
	@echo "  sdk/python/README.md"
	@echo "  sdk/python/mmemu_serial_monitor.py"

sdk-cpp:
	@echo "============================================"
	@echo " C++ SDK"
	@echo "============================================"
	@mkdir -p sdk/cpp/build
	@cd sdk/cpp/build && cmake .. && $(MAKE) -j $(JOBS)
	@echo ""
	@echo "SDK location: sdk/cpp/"
	@echo ""
	@echo "Quick start:"
	@echo "  ./sdk/cpp/build/memory_inspector --host localhost --port 2000"
	@echo ""
	@echo "Documentation:"
	@echo "  sdk/cpp/README.md (see sdk/cpp/CMakeLists.txt for build config)"

man:
	mkdir -p $(MANDIR)
	@echo "Generating man pages from documentation..."
	@for f in doc/README-*.md; do \
		base=$$(basename $$f .md | sed 's/README-//' | tr '[:upper:]' '[:lower:]'); \
		pandoc -s -t man $$f -o $(MANDIR)/mmemu-$$base.7; \
	done
	@pandoc -s -t man doc/iec.md -o $(MANDIR)/mmemu-iec.7
	@pandoc -s -t man README.md -o $(MANDIR)/mmemu.1

clean:
	rm -rf $(BINDIR) $(LIBDIR) $(ILIBDIR) $(MANDIR) $(COVDIR) sdk/cpp/build
	find src tests -name "*.o" -o -name "*.d" -o -name "*.gcno" -o -name "*.gcda" | xargs rm -f 2>/dev/null; true

# Auto-generated header dependency files (produced by -MMD -MP).
# The leading dash suppresses errors when .d files don't exist yet.
-include $(shell find src -name '*.d' 2>/dev/null)
