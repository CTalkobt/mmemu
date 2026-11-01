#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>
#include <filesystem>

/**
 * Makefile projects (#148): a directory with a Makefile, plus an optional
 * .mmsim/project.json overriding the defaults:
 *
 *   {
 *     "machine": "mega65",
 *     "run":   { "target": "run", "load": "auto", "reset": true },
 *     "build": { "target": "all" },
 *     "debug": { "symbols": ["build/game.sym"],
 *                "sourceMaps": [{ "map": "build/game.map", "source": "src/game.s" }] },
 *     "env":   { "EMU": "true" }
 *   }
 *
 * "load": "auto" picks the newest .prg written by the run target's build; a
 * path loads that file instead. EMU=true is set by default so a `run` recipe
 * like `$(EMU) game.prg` (which normally starts VICE) does nothing.
 */
namespace project {

struct SourceMapRef {
    std::string map;      // ca45 --source-map file
    std::string source;   // the source file it describes
};

struct Config {
    std::string dir;                  // absolute project directory
    std::string name;                 // directory name
    std::string machine;              // machine id to use ("" = keep the current one)
    std::string buildTarget = "all";
    std::string runTarget   = "run";
    std::string load        = "auto"; // "auto" or a path (relative to dir)
    bool        reset       = true;   // reset and boot the machine before loading
    std::vector<std::string>  symbols;      // relative to dir
    std::vector<SourceMapRef> sourceMaps;   // relative to dir
    std::vector<std::pair<std::string, std::string>> env = {{"EMU", "true"}};

    /** Absolute path for a project-relative one. */
    std::string resolve(const std::string& rel) const;
};

/**
 * Open the project in `dir`: it must contain a Makefile (Makefile, makefile
 * or GNUmakefile); .mmsim/project.json is read if present.
 */
bool open(const std::string& dir, Config& out, std::string& error);

/** True if `dir` holds a Makefile. */
bool hasMakefile(const std::string& dir);

// ---- New program detection ------------------------------------------------

using FileTimes = std::map<std::string, std::filesystem::file_time_type>;

/** Modification times of every .prg under dir (skips .git and .mmsim). */
FileTimes snapshotPrograms(const std::string& dir);

/**
 * The newest .prg under dir that is new or changed since `before`, or "" if
 * none was written.
 */
std::string newestChangedProgram(const std::string& dir, const FileTimes& before);

}  // namespace project
