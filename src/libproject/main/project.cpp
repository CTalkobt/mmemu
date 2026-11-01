#include "project.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;

namespace project {

std::string Config::resolve(const std::string& rel) const {
    fs::path p(rel);
    if (p.is_relative()) p = fs::path(dir) / p;
    std::error_code ec;
    auto norm = fs::weakly_canonical(p, ec);
    return ec ? p.string() : norm.string();
}

bool hasMakefile(const std::string& dir) {
    for (const char* name : {"GNUmakefile", "makefile", "Makefile"})
        if (fs::is_regular_file(fs::path(dir) / name)) return true;
    return false;
}

bool open(const std::string& dirIn, Config& out, std::string& error) {
    std::error_code ec;
    fs::path dir = fs::weakly_canonical(fs::absolute(dirIn, ec), ec);
    if (ec || !fs::is_directory(dir)) { error = "Not a directory: " + dirIn; return false; }
    if (!hasMakefile(dir.string())) { error = "No Makefile in " + dir.string(); return false; }

    Config cfg;
    cfg.dir = dir.string();
    cfg.name = dir.filename().string();

    fs::path json = dir / ".mmsim" / "project.json";
    if (fs::exists(json)) {
        try {
            std::ifstream f(json);
            nlohmann::json j = nlohmann::json::parse(f, nullptr, true, /*ignore_comments=*/true);
            cfg.machine = j.value("machine", cfg.machine);
            if (j.contains("build")) cfg.buildTarget = j["build"].value("target", cfg.buildTarget);
            if (j.contains("run")) {
                const auto& r = j["run"];
                cfg.runTarget = r.value("target", cfg.runTarget);
                cfg.load      = r.value("load", cfg.load);
                cfg.reset     = r.value("reset", cfg.reset);
            }
            if (j.contains("debug")) {
                const auto& d = j["debug"];
                if (d.contains("symbols"))
                    for (const auto& s : d["symbols"]) cfg.symbols.push_back(s.get<std::string>());
                if (d.contains("sourceMaps"))
                    for (const auto& m : d["sourceMaps"])
                        cfg.sourceMaps.push_back({m.value("map", ""), m.value("source", "")});
            }
            if (j.contains("env")) {
                cfg.env.clear();
                for (auto it = j["env"].begin(); it != j["env"].end(); ++it)
                    cfg.env.emplace_back(it.key(), it.value().is_string() ? it.value().get<std::string>()
                                                                          : it.value().dump());
            }
        } catch (const std::exception& e) {
            error = ".mmsim/project.json: " + std::string(e.what());
            return false;
        }
    }
    out = cfg;
    return true;
}

static bool skipDir(const fs::path& p) {
    std::string n = p.filename().string();
    return n == ".git" || n == ".mmsim";
}

FileTimes snapshotPrograms(const std::string& dir) {
    FileTimes out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_directory() && skipDir(it->path())) { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file()) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".prg") out[it->path().string()] = it->last_write_time();
    }
    return out;
}

std::string newestChangedProgram(const std::string& dir, const FileTimes& before) {
    std::string best;
    fs::file_time_type bestTime{};
    for (const auto& [path, time] : snapshotPrograms(dir)) {
        auto it = before.find(path);
        bool changed = it == before.end() || time != it->second;
        if (changed && (best.empty() || time > bestTime)) { best = path; bestTime = time; }
    }
    return best;
}

}  // namespace project
