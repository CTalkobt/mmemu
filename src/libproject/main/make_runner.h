#pragma once

#include "project.h"
#include <atomic>
#include <functional>
#include <string>

namespace project {

struct MakeResult {
    bool        started = false;   // false: make could not be run (see error)
    bool        cancelled = false;
    int         exitCode = -1;     // make's exit status (0 = success)
    std::string error;
    bool ok() const { return started && !cancelled && exitCode == 0; }
};

/**
 * Run `make -C <cfg.dir> <target>` with cfg.env added to the environment,
 * stdout and stderr merged. Each output line is passed to onLine (without the
 * newline) as it arrives. Blocks until make exits; setting *cancel kills the
 * make process group. Safe to call from a worker thread.
 */
MakeResult runMake(const Config& cfg, const std::string& target,
                   const std::function<void(const std::string&)>& onLine,
                   const std::atomic<bool>* cancel = nullptr);

}  // namespace project
