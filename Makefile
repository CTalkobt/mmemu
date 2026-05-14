# mmemu — Multi Machine Emulator
# Top-level Makefile
.PHONY: all cli gui mcp libs test test-mcp plugins clean man serve

all: cli gui mcp plugins

VERSION_HDR = src/include/version.h
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
	-Isrc/plugins/devices/f018b_dma/main \
	-Isrc/plugins/devices/map_mmu/main \
	-Isrc/plugins/machines/mega65/main \
	-Isrc/plugins/machines/pet/main -Itests/src

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
LIBTOOLCHAIN_SRCS = src/libtoolchain/main/symbol_table.cpp src/libtoolchain/main/source_map.cpp \
	src/libtoolchain/main/toolchain_registry.cpp src/libtoolchain/main/libtoolchain.cpp
LIBDEBUG_SRCS     = src/libdebug/main/breakpoint_list.cpp src/libdebug/main/debug_context.cpp \
	src/libdebug/main/trace_buffer.cpp src/libdebug/main/libdebug.cpp \
	src/libdebug/main/expression_evaluator.cpp \
	src/libdebug/main/stack_trace.cpp \
	src/libdebug/main/observer_registry.cpp
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

PLUGIN_C64PLA_SRCS = src/plugins/devices/c64_pla/main/c64_pla.cpp \
	src/plugins/devices/c64_pla/main/plugin_init.cpp

PLUGIN_CIA6526_SRCS = src/plugins/devices/cia6526/main/cia6526.cpp \
	src/plugins/devices/cia6526/main/plugin_init.cpp

PLUGIN_VIC2_SRCS = src/plugins/devices/vic2/main/vic2.cpp \
	src/plugins/devices/vic2/main/plugin_init.cpp

PLUGIN_SID6581_SRCS = src/plugins/devices/sid6581/main/sid6581.cpp \
	src/plugins/devices/sid6581/main/plugin_init.cpp

PLUGIN_PIA6520_SRCS = src/plugins/devices/pia6520/main/pia6520.cpp \
	src/plugins/devices/pia6520/main/plugin_init.cpp

PLUGIN_CBMLOADER_SRCS = src/plugins/cbm-loader/main/prg_loader.cpp \
	src/plugins/cbm-loader/main/crt_parser.cpp \
	src/plugins/cbm-loader/main/cbm_cart_handler.cpp \
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
	src/plugins/cbm-loader/main/d64_parser.cpp \
	src/plugins/cbm-loader/main/t64_parser.cpp \
	src/plugins/cbm-loader/main/g64_parser.cpp

PLUGIN_F018B_DMA_SRCS = src/plugins/devices/f018b_dma/main/f018b_dma.cpp

PLUGIN_MAP_MMU_SRCS = src/plugins/devices/map_mmu/main/map_mmu.cpp \
	src/plugins/devices/map_mmu/main/key_register.cpp \
	src/plugins/devices/map_mmu/main/plugin_init.cpp

PLUGIN_SID_PAIR_SRCS = src/plugins/devices/sid_pair/main/sid_pair.cpp \
	src/plugins/devices/sid_pair/main/plugin_init.cpp

PLUGIN_MEGA65_SRCS = src/plugins/machines/mega65/main/machine_mega65.cpp \
	src/plugins/machines/mega65/main/plugin_init.cpp

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
	src/gui/main/screen_pane.cpp \
        src/gui/main/tape_pane.cpp \
        src/gui/main/drive_status_pane.cpp \
	src/gui/main/dialogs/memory_dialogs.cpp \
	src/gui/main/dialogs/assemble_dialog.cpp \
	src/gui/main/dialogs/image_dialogs.cpp \
	src/gui/main/dialogs/calculator_dialog.cpp \
	src/cli/main/cli_interpreter.cpp \
	src/gui/main/plugin_pane_manager.cpp \
	src/cli/main/plugin_command_registry.cpp \
	src/gui/main/gui_utils.cpp \
	src/gui/main/audio_output.cpp

CLI_SRCS = src/cli/main/main.cpp \
	src/cli/main/cli_interpreter.cpp \
	src/cli/main/plugin_command_registry.cpp

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
	src/libtoolchain/test/test_toolchain.cpp \
	src/libtoolchain/test/test_symbol_table_enhanced.cpp \
	src/libcore/test/test_machine_symbols.cpp \
	src/plugins/6502/test/test_cpu6502.cpp \
	src/plugins/6502/test/test_disasm6502.cpp \
	src/plugins/6502/test/test_assembler6502.cpp \
	src/plugins/6502/test/test_cpu6510_io.cpp \
	src/plugins/machines/vic20/test/test_vic20_integration.cpp \
	src/plugins/machines/c64/test/test_c64_integration.cpp \
	src/plugins/machines/pet/test/test_pet_integration.cpp \
	src/plugins/cbm-loader/test/test_cbm_loader.cpp \
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
	src/plugins/devices/map_mmu/test/test_map_mmu.cpp \
	src/plugins/devices/map_mmu/test/test_key_register.cpp \
	src/plugins/machines/mega65/test/test_mega65_map.cpp \
	tests/src/test_cbm_disk_images.cpp \
	tests/src/test_d81_directory_listing.cpp \
	tests/src/test_plugin_validation.cpp \
	tests/src/test_ffd0_bug.cpp \
	src/plugins/devices/sid_pair/test/test_sid_pair.cpp

LIBDEBUG_TEST_SRCS = src/libdebug/test/test_breakpoints.cpp
LIBCORE_TEST_SRCS = src/libcore/test/test_c_compatibility.c
PLUGINLOADER_TEST_SRCS = src/plugin_loader/test/test_plugin_extension.cpp
PLUGIN_VICEIMPORTER_TEST_SRCS = src/plugins/viceImporter/test/test_vice_importer.cpp
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
	src/plugins/devices/c64_pla/main/c64_pla.o \
	src/plugins/devices/cia6526/main/cia6526.o \
	src/plugins/devices/vic2/main/vic2.o \
	src/plugins/devices/sid6581/main/sid6581.o \
	src/plugins/devices/sid_pair/main/sid_pair.o \
	src/plugins/devices/datasette/main/datasette.o \
	src/plugins/cbm-loader/main/tap_parser.o \
	src/plugins/machines/c64/main/kbd_c64.o \
	src/plugins/devices/pia6520/main/pia6520.o \
	src/plugins/cbm-loader/main/prg_loader.o \
	src/plugins/cbm-loader/main/crt_parser.o \
	src/plugins/cbm-loader/main/cbm_cart_handler.o \
	src/plugins/cbm-loader/main/d64_parser.o \
	src/plugins/cbm-loader/main/g64_parser.o \
	src/plugins/cbm-loader/main/t64_parser.o \
	src/plugins/cbm-loader/main/disk_loader.o \
	src/plugins/devices/crtc6545/main/crtc6545.o \
	src/plugins/devices/pet_video/main/pet_video.o \
	src/plugins/devices/keyboard/main/keyboard_matrix_pet.o \
	src/plugins/devices/antic/main/antic.o \
	src/plugins/devices/gtia/main/gtia.o \
	src/plugins/devices/pokey/main/pokey.o \
	src/plugins/devices/virtual_iec/main/virtual_iec.o \
	src/plugins/devices/f018b_dma/main/f018b_dma.o \
	src/plugins/devices/map_mmu/main/map_mmu.o \
	src/plugins/devices/map_mmu/main/key_register.o \
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
PLUGIN_C64PLA_OBJS   = $(PLUGIN_C64PLA_SRCS:.cpp=.o)
PLUGIN_CIA6526_OBJS  = $(PLUGIN_CIA6526_SRCS:.cpp=.o)
PLUGIN_VIC2_OBJS     = $(PLUGIN_VIC2_SRCS:.cpp=.o)
PLUGIN_SID6581_OBJS  = $(PLUGIN_SID6581_SRCS:.cpp=.o)
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
PLUGIN_MAP_MMU_OBJS = $(PLUGIN_MAP_MMU_SRCS:.cpp=.o)
PLUGIN_SID_PAIR_OBJS = $(PLUGIN_SID_PAIR_SRCS:.cpp=.o)
PLUGIN_MEGA65_OBJS = $(PLUGIN_MEGA65_SRCS:.cpp=.o)
PLUGIN_EXIT_TRAP_OBJS = src/plugins/devices/exit_trap/main/exit_trap.o
PLUGIN_MEGA65_MATH_OBJS = src/plugins/devices/mega65_math/main/mega65_math.o
PLUGIN_CBMHLE_OBJS = $(PLUGIN_CBMHLE_SRCS:.cpp=.o)

GUI_OBJS = $(GUI_SRCS:.cpp=.o)

CLI_OBJS = $(CLI_SRCS:.cpp=.o)
MCP_OBJS = $(MCP_SRCS:.cpp=.o)

# Binaries
CLI_BIN = $(BINDIR)/mmemu-cli
GUI_BIN = $(BINDIR)/mmemu-gui
MCP_BIN = $(BINDIR)/mmemu-mcp

PLUGINS = $(LIBDIR)/mmemu-plugin-6502.so \
	$(LIBDIR)/mmemu-plugin-45gs02.so \
	$(LIBDIR)/mmemu-plugin-hyper-serial.so \
	$(LIBDIR)/mmemu-plugin-via6522.so \
	$(LIBDIR)/mmemu-plugin-vic6560.so \
	$(LIBDIR)/mmemu-plugin-vic20.so \
	$(LIBDIR)/mmemu-plugin-kbd-vic20.so \
	$(LIBDIR)/mmemu-plugin-vice-importer.so \
	$(LIBDIR)/mmemu-plugin-c64-pla.so \
	$(LIBDIR)/mmemu-plugin-cia6526.so \
	$(LIBDIR)/mmemu-plugin-vic2.so \
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
	$(LIBDIR)/mmemu-plugin-map-mmu.so \
	$(LIBDIR)/mmemu-plugin-mega65.so \
	$(LIBDIR)/mmemu-plugin-exit-trap.so \
	$(LIBDIR)/mmemu-plugin-mega65-math.so \
	$(LIBDIR)/mmemu-plugin-sid-pair.so \
	$(LIBDIR)/mmemu-plugin-cbm-hle.so

LIBS = $(ILIBDIR)/libmem.a $(ILIBDIR)/libcore.a $(ILIBDIR)/libdevices.a \
	$(ILIBDIR)/libtoolchain.a $(ILIBDIR)/libdebug.a $(ILIBDIR)/libplugins.a

BASE_LIBS = -Wl,--whole-archive -L$(ILIBDIR) -lplugins -ldebug -ltoolchain -ldevices -lcore -lmem -Wl,--no-whole-archive -ldl -lspdlog -lfmt

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

$(LIBDIR)/mmemu-plugin-c64-pla.so: $(PLUGIN_C64PLA_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-cia6526.so: $(PLUGIN_CIA6526_OBJS) | $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(WXLIBS) $(PLUGIN_LIBS)

$(LIBDIR)/mmemu-plugin-vic2.so: $(PLUGIN_VIC2_OBJS) | $(LIBDIR)
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
$(VERSION_HDR): .FORCE
	@NEW='#pragma once\n// Auto-generated by Makefile — do not edit\n#define MMSIM_VERSION "0.2.0"\n#define MMSIM_GIT_HASH "$(GIT_HASH)"\n#define MMSIM_VERSION_FULL MMSIM_VERSION "-" MMSIM_GIT_HASH\n'; \
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
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(WXCXXFLAGS) -rdynamic -o $@ $(GUI_OBJS) $(BASE_LIBS) $(WXLIBS) -lasound

$(MCP_BIN): $(MCP_OBJS) $(LIBS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -rdynamic -o $@ $(MCP_OBJS) $(BASE_LIBS)

$(TEST_BIN): $(TEST_OBJS) $(LIBS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -DTEST_BUILD -Itests/src -rdynamic -o $@ $(TEST_OBJS) $(BASE_LIBS) $(WXLIBS) -lasound

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

test: $(TEST_BIN) plugins test-mega65 test-mcp
	./$(TEST_BIN)

test-mcp: $(MCP_BIN) plugins
	@echo "Running MCP integration tests..."
	python3 src/mcp/test/mcp_test.py

test-mega65: $(CLI_BIN) plugins
	@echo "Running 45GS02 Validation Suite..."
	./tests/45gs02/validate.py tests/45gs02/arithmetic.s
	./tests/45gs02/validate.py tests/45gs02/transfers.s
	./tests/45gs02/validate.py tests/45gs02/advanced.s
	./tests/45gs02/validate.py tests/45gs02/quad.s

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
	rm -rf $(BINDIR) $(LIBDIR) $(ILIBDIR) $(MANDIR)
	find src -name "*.o" -delete
	find src -name "*.d" -delete

# Auto-generated header dependency files (produced by -MMD -MP).
# The leading dash suppresses errors when .d files don't exist yet.
-include $(shell find src -name '*.d' 2>/dev/null)
