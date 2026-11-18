#pragma once

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

class IOHandler;

/**
 * When a device option takes effect (IOHandler::setOptionWhen). Some
 * devices can't take a setting at once: the guest has to send its init
 * codes first, or the device isn't on the reset line and only a delay
 * tells when the machine has started.
 */
struct OptionTiming {
    enum class Kind { Now, Started, Delay };
    Kind kind = Kind::Now;
    double seconds = 0;     // Delay: guest seconds from when the option was set

    static OptionTiming now() { return {}; }
    static OptionTiming started() { return {Kind::Started, 0}; }
    static OptionTiming delay(double s) { return {Kind::Delay, s}; }

    /** "now", "started" (or "startup", "ready"), or seconds of delay ("2.5", "2.5s"); false if none. */
    static bool parse(const std::string& text, OptionTiming& out) {
        if (text.empty() || text == "now") { out = now(); return true; }
        if (text == "started" || text == "startup" || text == "ready") { out = started(); return true; }
        char* end = nullptr;
        const double s = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || s < 0 || s > 86400) return false;
        if (*end == 's') ++end;
        if (*end) return false;
        out = delay(s);
        return true;
    }
    std::string str() const {
        if (kind == Kind::Started) return "after startup";
        if (kind == Kind::Delay) {
            char b[32];
            std::snprintf(b, sizeof b, "after %gs", seconds);
            return b;
        }
        return "now";
    }
};

/** An option waiting for its time (IOHandler::pendingOptions). */
struct PendingOption {
    std::string key, value;
    OptionTiming when;
    double waited = 0;      // Delay: guest seconds since it was set
};

/**
 * The options a device holds back (header-only, so a plugin needs nothing
 * more to link). A device that can tell when it has started holds one,
 * forwards IOHandler::setOptionWhen / pendingOptions to it, and calls
 * started() when its startup has finished and elapse() as guest time passes.
 * Each option is applied once, through the device's setOption. Thread
 * safe: a front end may queue while the emulation runs.
 */
class DeferredOptions {
public:
    void queue(const std::string& key, const std::string& value, const OptionTiming& when) {
        std::lock_guard<std::mutex> g(m_lock);
        // A later setting of the same key replaces a waiting one
        for (auto it = m_pending.begin(); it != m_pending.end();)
            it = it->key == key ? m_pending.erase(it) : it + 1;
        m_pending.push_back({key, value, when, 0});
    }
    /** An option set at once: one waiting for the same key is dropped. */
    void cancel(const std::string& key) {
        std::lock_guard<std::mutex> g(m_lock);
        for (auto it = m_pending.begin(); it != m_pending.end();)
            it = it->key == key ? m_pending.erase(it) : it + 1;
    }
    /** The device's startup has finished: the "started" options apply. */
    template <class Apply> void started(Apply apply) { run(apply, 0, true); }
    /** Guest time passed: the delays that are up apply. */
    template <class Apply> void elapse(double seconds, Apply apply) { run(apply, seconds, false); }

    std::vector<PendingOption> pending() const {
        std::lock_guard<std::mutex> g(m_lock);
        return m_pending;
    }
    bool empty() const {
        std::lock_guard<std::mutex> g(m_lock);
        return m_pending.empty();
    }
    /** The last held option that the device refused when its time came ("" if none). */
    std::string lastError() const {
        std::lock_guard<std::mutex> g(m_lock);
        return m_error;
    }

private:
    /** `apply(key, value, err)` -> bool: the device's setOption. */
    template <class Apply> void run(Apply apply, double seconds, bool started) {
        std::vector<PendingOption> due;
        {
            std::lock_guard<std::mutex> g(m_lock);
            if (m_pending.empty()) return;
            for (auto it = m_pending.begin(); it != m_pending.end();) {
                bool now = false;
                if (it->when.kind == OptionTiming::Kind::Started) now = started;
                else if (it->when.kind == OptionTiming::Kind::Delay) {
                    it->waited += seconds;
                    now = it->waited >= it->when.seconds;
                }
                if (now) {
                    due.push_back(*it);
                    it = m_pending.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& o : due) {
            std::string err;
            if (!apply(o.key, o.value, err)) {
                std::lock_guard<std::mutex> g(m_lock);
                m_error = o.key + "=" + o.value + " (" + o.when.str() + "): " + err;
            }
        }
    }

    mutable std::mutex m_lock;
    std::vector<PendingOption> m_pending;
    std::string m_error;
};
