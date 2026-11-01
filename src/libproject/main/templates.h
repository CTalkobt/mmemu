#pragma once

#include <string>
#include <utility>
#include <vector>

/**
 * New-project templates (#149): a Makefile with all / run / clean, a
 * .mmsim/project.json and a hello-world source per machine and toolchain.
 */
namespace project {

struct Template {
    std::string id;            // e.g. "c64-basic"
    std::string name;          // e.g. "C64 BASIC"
    std::string machine;       // machine id written to project.json
    std::string description;
    std::string mainFile;      // file to open after creating the project
    std::vector<std::pair<std::string, std::string>> files;   // path -> content ({{MMEMU}} replaced)
};

const std::vector<Template>& templates();

/** mmemu-cli next to the running executable (bin/), or "mmemu-cli" (PATH). */
std::string findMmemuCli();
const Template* findTemplate(const std::string& id);

/**
 * Create a project from a template in `dir` (created if missing; must not
 * contain a Makefile already). `mmemuCli` is the mmemu-cli used by BASIC
 * templates to tokenise (written into the Makefile as MMEMU ?= ...).
 * `mainFileOut` receives the absolute path of the main source file.
 */
bool createFromTemplate(const std::string& dir, const std::string& templateId, const std::string& mmemuCli,
                        std::string& error, std::string* mainFileOut = nullptr);

}  // namespace project
