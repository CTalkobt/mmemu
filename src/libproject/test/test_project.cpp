#include "test_harness.h"
#include "libproject/main/project.h"
#include "libproject/main/make_runner.h"
#include "libproject/main/diagnostics.h"
#include "libproject/main/launcher.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/image_loader.h"
#include "libdebug/main/debug_context.h"
#include "libmem/main/memory_bus.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/cbm-loader/main/prg_loader.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

namespace {
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) : path(fs::temp_directory_path() / name) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    void write(const std::string& rel, const std::string& content) const {
        fs::create_directories((path / rel).parent_path());
        std::ofstream(path / rel) << content;
    }
};
}

TEST_CASE(project_open_requires_makefile_and_reads_config) {
    TempDir t("mmemu_proj_open");
    project::Config cfg;
    std::string err;
    ASSERT(!project::open(t.path.string(), cfg, err));
    ASSERT(err.find("No Makefile") != std::string::npos);

    t.write("Makefile", "all:\n\t@true\n");
    ASSERT(project::open(t.path.string(), cfg, err));
    ASSERT(cfg.buildTarget == "all" && cfg.runTarget == "run" && cfg.load == "auto" && cfg.reset);
    ASSERT(cfg.env.size() == 1 && cfg.env[0].first == "EMU" && cfg.env[0].second == "true");

    t.write(".mmsim/project.json", R"({
        // comments are allowed
        "machine": "mega65",
        "run":   { "target": "go", "load": "out/game.prg", "reset": false },
        "build": { "target": "prg" },
        "debug": { "symbols": ["out/game.sym"], "sourceMaps": [{ "map": "out/game.map", "source": "game.s" }] },
        "env":   { "EMU": "x64sc", "OPT": 2 }
    })");
    ASSERT(project::open(t.path.string(), cfg, err));
    ASSERT(cfg.machine == "mega65" && cfg.runTarget == "go" && cfg.buildTarget == "prg" && !cfg.reset);
    ASSERT(cfg.load == "out/game.prg");
    ASSERT(cfg.symbols.size() == 1 && cfg.sourceMaps.size() == 1 && cfg.sourceMaps[0].source == "game.s");
    ASSERT(cfg.env.size() == 2);
    ASSERT(cfg.resolve("out/game.prg") == (fs::weakly_canonical(t.path) / "out/game.prg").string());

    t.write(".mmsim/project.json", "{ broken");
    ASSERT(!project::open(t.path.string(), cfg, err));
    ASSERT(err.find("project.json") != std::string::npos);
}

TEST_CASE(project_run_make_streams_output_and_exit_code) {
    TempDir t("mmemu_proj_make");
    t.write("Makefile",
            "all:\n\t@echo line one\n\t@echo line two >&2\n"
            "emu:\n\t@echo EMU=$(EMU)\n"
            "fail:\n\t@echo bad.s:3:1: error: Unknown instruction\n\t@exit 2\n"
            "slow:\n\t@echo started\n\t@sleep 10\n");
    project::Config cfg;
    std::string err;
    ASSERT(project::open(t.path.string(), cfg, err));

    std::vector<std::string> lines;
    auto collect = [&](const std::string& l) { lines.push_back(l); };
    auto r = project::runMake(cfg, "all", collect);
    ASSERT(r.ok());
    ASSERT(std::find(lines.begin(), lines.end(), "line one") != lines.end());
    ASSERT(std::find(lines.begin(), lines.end(), "line two") != lines.end());   // stderr merged

    lines.clear();
    ASSERT(project::runMake(cfg, "emu", collect).ok());
    ASSERT(std::find(lines.begin(), lines.end(), "EMU=true") != lines.end());   // default env

    lines.clear();
    r = project::runMake(cfg, "fail", collect);
    ASSERT(r.started && !r.ok() && r.exitCode == 2);

    std::atomic<bool> cancel{false};
    std::thread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); cancel = true; });
    auto t0 = std::chrono::steady_clock::now();
    r = project::runMake(cfg, "slow", [](const std::string&) {}, &cancel);
    stopper.join();
    ASSERT(r.cancelled);
    ASSERT(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
}

TEST_CASE(project_diagnostic_formats) {
    project::DiagnosticParser p("/proj");
    auto d = p.feed("bad.s:3:1: error: Unknown instruction 'bogus'");             // ca45 / gcc
    ASSERT(d && d->file == "/proj/bad.s" && d->line == 3 && d->column == 1);
    ASSERT(d->severity == project::Diagnostic::Severity::Error && d->message == "Unknown instruction 'bogus'");
    d = p.feed("src/x.c:12: warning: unused variable");
    ASSERT(d && d->file == "/proj/src/x.c" && d->line == 12 && d->severity == project::Diagnostic::Severity::Warning);
    d = p.feed("Compilation error: :2:11: error: Expected expression.");             // cc45: no file
    ASSERT(d && d->file.empty() && d->line == 2 && d->column == 11);
    d = p.feed("main.s(42): Error: Unknown label");                                  // ca65
    ASSERT(d && d->file == "/proj/main.s" && d->line == 42);
    d = p.feed("Error - File main.a, line 7 (Zone <untitled>): Value not defined.");  // acme
    ASSERT(d && d->file == "/proj/main.a" && d->line == 7 && d->message == "Value not defined.");
    ASSERT(!p.feed("Error: Unknown symbol 'foo'"));                                  // KickAssembler, 2 lines
    d = p.feed("at line 3, column 9 in game.asm");
    ASSERT(d && d->file == "/proj/game.asm" && d->line == 3 && d->column == 9 && d->message == "Unknown symbol 'foo'");
    ASSERT(!p.feed("make: Entering directory '/proj/sub'"));
    d = p.feed("inner.s:1:1: error: x");
    ASSERT(d && d->file == "/proj/sub/inner.s");
    ASSERT(!p.feed("make: *** [Makefile:3: all] Error 2"));
    ASSERT(!p.feed("Assembled to out.prg (15 bytes)"));
}

TEST_CASE(project_newest_changed_program) {
    TempDir t("mmemu_proj_prg");
    t.write("old.prg", "x");
    t.write(".git/ignored.prg", "x");
    auto before = project::snapshotPrograms(t.path.string());
    ASSERT_EQ((int)before.size(), 1);
    ASSERT(project::newestChangedProgram(t.path.string(), before).empty());
    t.write("build/new.prg", "yy");
    ASSERT(fs::path(project::newestChangedProgram(t.path.string(), before)).filename() == "new.prg");
}

// Launch on a ROM-less 6502: a program not at a BASIC start runs from its load address
TEST_CASE(project_launch_machine_code_sets_pc) {
    if (!ImageLoaderRegistry::instance().findLoader("x.prg"))
        ImageLoaderRegistry::instance().registerLoader(std::make_unique<PrgLoader>());
    TempDir t("mmemu_proj_launch");
    {
        std::ofstream f(t.path / "game.prg", std::ios::binary);
        const unsigned char prg[] = { 0x00, 0xC0, 0xA9, 0x42, 0x4C, 0x02, 0xC0 };   // $C000: LDA #$42 / JMP $C002
        f.write((const char*)prg, sizeof(prg));
    }
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);
    MachineDescriptor m;
    m.machineId = "raw6502";
    m.buses.push_back({"system", &bus});
    m.cpus.push_back({"main", &cpu, &bus, &bus, nullptr, true, 1});
    auto r = project::launch(m, nullptr, (t.path / "game.prg").string(), false);
    ASSERT(r.ok && !r.typedRun);
    ASSERT_EQ((int)r.loadAddress, 0xC000);
    ASSERT_EQ((int)cpu.pc(), 0xC000);
    ASSERT_EQ((int)bus.peek8(0xC001), 0x42);
    m.buses.clear(); m.cpus.clear();   // not owned by the descriptor here
}

// Run when the build was up to date still finds the program
TEST_CASE(project_program_to_run_falls_back_to_newest) {
    TempDir t("mmemu_proj_torun");
    t.write("Makefile", "all:\n\t@true\n");
    project::Config cfg;
    std::string err;
    ASSERT(project::open(t.path.string(), cfg, err));
    t.write("build/game.prg", "x");
    auto before = project::snapshotPrograms(cfg.dir);           // nothing changes after this
    ASSERT(fs::path(project::programToRun(cfg, before)).filename() == "game.prg");
    cfg.load = "other.prg";
    ASSERT(fs::path(project::programToRun(cfg, before)).filename() == "other.prg");
}

#include "libproject/main/templates.h"

TEST_CASE(project_templates_create) {
    TempDir t("mmemu_proj_tpl");
    ASSERT(project::templates().size() >= 4);
    std::string err, mainFile;
    fs::path dir = t.path / "game";
    ASSERT(project::createFromTemplate(dir.string(), "c64-basic", "/opt/mmemu/mmemu-cli", err, &mainFile));
    ASSERT(fs::path(mainFile).filename() == "hello.bas");
    std::ifstream mk(dir / "Makefile");
    std::string makefile((std::istreambuf_iterator<char>(mk)), std::istreambuf_iterator<char>());
    ASSERT(makefile.find("MMEMU ?= /opt/mmemu/mmemu-cli") != std::string::npos);
    ASSERT(makefile.find("--dialect basic2") != std::string::npos);

    project::Config cfg;
    ASSERT(project::open(dir.string(), cfg, err));
    ASSERT(cfg.machine == "c64");

    ASSERT(!project::createFromTemplate(dir.string(), "c64-basic", "", err));     // already has a Makefile
    ASSERT(err.find("already has a Makefile") != std::string::npos);
    ASSERT(!project::createFromTemplate((t.path / "x").string(), "nope", "", err));
    ASSERT(err.find("Unknown template") != std::string::npos);

    ASSERT(project::createFromTemplate((t.path / "asm").string(), "mega65-asm", "", err));
    ASSERT(project::open((t.path / "asm").string(), cfg, err));
    ASSERT(cfg.machine == "mega65" && cfg.sourceMaps.size() == 1);
}
