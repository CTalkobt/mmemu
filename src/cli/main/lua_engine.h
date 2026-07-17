#pragma once

#include <string>
#include <functional>
#include <memory>

// Forward declare Lua state if Lua is available
#if __has_include(<lua5.4/lua.h>) || __has_include(<lua.h>)
struct lua_State;
#else
// Stub type when Lua is not available
typedef void lua_State;
#endif

class ICore;
class IBus;
class DebugContext;

/// Lua scripting engine for mmemu automation and custom breakpoint actions
class LuaEngine {
public:
    /// Create a new Lua engine for a given machine context
    LuaEngine(ICore* cpu, IBus* bus, DebugContext* dbg);
    ~LuaEngine();

    /// Execute a Lua script from a string
    /// Returns true on success, false on error (check getLastError())
    bool executeString(const std::string& code);

    /// Execute a Lua script from a file
    /// Returns true on success, false on error
    bool executeFile(const std::string& path);

    /// Call a Lua function by name with no arguments
    bool callFunction(const std::string& name);

    /// Set the last error message
    const std::string& getLastError() const { return m_lastError; }

    /// Get the Lua state for advanced usage
    lua_State* getLuaState() { return m_lua; }

private:
    ICore* m_cpu;
    IBus* m_bus;
    DebugContext* m_dbg;
    lua_State* m_lua;
    std::string m_lastError;

    void setupGlobals();
    void setupMachineAPI();
    void setupDebuggerAPI();
};

/// Breakpoint action backed by Lua script
class LuaBreakpointAction {
public:
    LuaBreakpointAction(const std::string& scriptCode);
    ~LuaBreakpointAction();

    /// Execute this breakpoint action in a machine context
    /// Returns true if execution should continue, false to stay paused
    bool execute(ICore* cpu, IBus* bus, DebugContext* dbg);

    const std::string& getLastError() const { return m_lastError; }

private:
    std::string m_scriptCode;
    std::string m_lastError;
};
