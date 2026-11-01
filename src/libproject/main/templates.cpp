#include "templates.h"
#include "project.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace project {

namespace {

const char* kBasicMakefile =
    "# mmemu project: {{NAME}}\n"
    "# make       - tokenise {{SRC}} into {{PRG}}\n"
    "# make run   - build, then start it ($(EMU) is \"true\" inside mmemu, which loads and runs it)\n"
    "MMEMU ?= {{MMEMU}}\n"
    "EMU   ?= x64sc\n"
    "\n"
    "all: {{PRG}}\n"
    "\n"
    "{{PRG}}: {{SRC}}\n"
    "\t$(MMEMU) --tokenize {{SRC}} {{PRG}} --dialect {{DIALECT}}\n"
    "\n"
    "run: all\n"
    "\t$(EMU) {{PRG}}\n"
    "\n"
    "clean:\n"
    "\trm -f {{PRG}}\n"
    "\n"
    ".PHONY: all run clean\n";

std::string fill(std::string s, const std::vector<std::pair<std::string, std::string>>& vars) {
    for (const auto& [k, v] : vars) {
        size_t pos;
        while ((pos = s.find(k)) != std::string::npos) s.replace(pos, k.size(), v);
    }
    return s;
}

Template basicTemplate(const std::string& id, const std::string& name, const std::string& machine,
                       const std::string& dialect, const std::string& program) {
    Template t;
    t.id = id;
    t.name = name;
    t.machine = machine;
    t.description = name + " program tokenised by mmemu-cli --tokenize";
    t.mainFile = "hello.bas";
    t.files = {
        {"Makefile", fill(kBasicMakefile, {{"{{NAME}}", name}, {"{{SRC}}", "hello.bas"},
                                           {"{{PRG}}", "hello.prg"}, {"{{DIALECT}}", dialect}})},
        {"hello.bas", program},
        {".mmsim/project.json", "{\n  \"machine\": \"" + machine + "\"\n}\n"},
    };
    return t;
}

}  // namespace

const std::vector<Template>& templates() {
    static const std::vector<Template> list = [] {
        std::vector<Template> v;
        v.push_back(basicTemplate("c64-basic", "C64 BASIC", "c64", "basic2",
            "10 print \"{clr}hello from mmemu!\"\n"
            "20 for i=0 to 15\n"
            "30 poke 53280,i\n"
            "40 for d=1 to 100:next d\n"
            "50 next i\n"
            "60 goto 20\n"));
        v.push_back(basicTemplate("mega65-basic", "MEGA65 BASIC 65", "mega65", "basic65",
            "10 print \"{clr}hello from mmemu!\"\n"
            "20 for i=0 to 15\n"
            "30 border i\n"
            "40 for d=1 to 500:next d\n"
            "50 next i\n"
            "60 goto 20\n"));

        Template a;
        a.id = "mega65-asm";
        a.name = "MEGA65 assembly (ca45)";
        a.machine = "mega65";
        a.description = "45GS02 assembly with a BASIC SYS stub, assembled by ca45 with a source map";
        a.mainFile = "main.s";
        a.files = {
            {"Makefile",
             "# mmemu project: MEGA65 assembly (ca45)\n"
             "# make       - assemble main.s into build/main.prg (+ source map for debugging)\n"
             "# make run   - build, then start it ($(EMU) is \"true\" inside mmemu, which loads and runs it)\n"
             "CA45 ?= ca45\n"
             "EMU  ?= xemu-xmega65 -prg\n"
             "\n"
             "all: build/main.prg\n"
             "\n"
             "build/main.prg: main.s\n"
             "\tmkdir -p build\n"
             "\t$(CA45) -o build/main.prg --source-map build/main.map main.s\n"
             "\n"
             "run: all\n"
             "\t$(EMU) build/main.prg\n"
             "\n"
             "clean:\n"
             "\trm -rf build\n"
             "\n"
             ".PHONY: all run clean\n"},
            {"main.s",
             "; mmemu project: MEGA65 assembly (ca45)\n"
             "        * = $2001\n"
             "\n"
             "; BASIC stub: 10 SYS 8205\n"
             "        .byte $0b, $20, $0a, $00, $9e, $38, $32, $30, $35, $00, $00, $00\n"
             "\n"
             "start:  inc $d020          ; cycle the border colour\n"
             "        jmp start\n"},
            {".mmsim/project.json",
             "{\n"
             "  \"machine\": \"mega65\",\n"
             "  \"debug\": { \"sourceMaps\": [ { \"map\": \"build/main.map\", \"source\": \"main.s\" } ] }\n"
             "}\n"},
        };
        v.push_back(a);

        Template c;
        c.id = "mega65-c";
        c.name = "MEGA65 C (cc45)";
        c.machine = "mega65";
        c.description = "C compiled by cc45 and assembled by ca45; starts at $2000";
        c.mainFile = "main.c";
        c.files = {
            {"Makefile",
             "# mmemu project: MEGA65 C (cc45)\n"
             "# make       - compile main.c to build/main.s and assemble build/main.prg\n"
             "# make run   - build, then start it ($(EMU) is \"true\" inside mmemu, which loads and runs it)\n"
             "CC45 ?= cc45\n"
             "CA45 ?= ca45\n"
             "EMU  ?= xemu-xmega65 -prg\n"
             "\n"
             "all: build/main.prg\n"
             "\n"
             "build/main.s: main.c\n"
             "\tmkdir -p build\n"
             "\t$(CC45) main.c -o build/main.s\n"
             "\n"
             "build/main.prg: build/main.s\n"
             "\t$(CA45) -o build/main.prg --source-map build/main.map build/main.s\n"
             "\n"
             "run: all\n"
             "\t$(EMU) build/main.prg\n"
             "\n"
             "clean:\n"
             "\trm -rf build\n"
             "\n"
             ".PHONY: all run clean\n"},
            {"main.c",
             "/* mmemu project: MEGA65 C (cc45) */\n"
             "void main(void) {\n"
             "    unsigned char *border = (unsigned char *)0xd020;\n"
             "    while (1) {\n"
             "        (*border)++;             /* cycle the border colour */\n"
             "    }\n"
             "}\n"},
            {".mmsim/project.json",
             "{\n"
             "  \"machine\": \"mega65\",\n"
             "  \"debug\": { \"sourceMaps\": [ { \"map\": \"build/main.map\", \"source\": \"build/main.s\" } ] }\n"
             "}\n"},
        };
        v.push_back(c);
        return v;
    }();
    return list;
}

std::string findMmemuCli() {
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        fs::path cli = self.parent_path() / "mmemu-cli";
        if (fs::exists(cli, ec)) return cli.string();
    }
    return "mmemu-cli";
}

const Template* findTemplate(const std::string& id) {
    for (const auto& t : templates()) if (t.id == id) return &t;
    return nullptr;
}

bool createFromTemplate(const std::string& dir, const std::string& templateId, const std::string& mmemuCli,
                        std::string& error, std::string* mainFileOut) {
    const Template* t = findTemplate(templateId);
    if (!t) { error = "Unknown template: " + templateId; return false; }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { error = "Cannot create " + dir + ": " + ec.message(); return false; }
    if (hasMakefile(dir)) { error = dir + " already has a Makefile"; return false; }
    for (const auto& [rel, content] : t->files) {
        fs::path p = fs::path(dir) / rel;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p);
        f << fill(content, {{"{{MMEMU}}", mmemuCli.empty() ? std::string("mmemu-cli") : mmemuCli}});
        if (!f) { error = "Cannot write " + p.string(); return false; }
    }
    if (mainFileOut) *mainFileOut = fs::weakly_canonical(fs::path(dir) / t->mainFile, ec).string();
    return true;
}

}  // namespace project
