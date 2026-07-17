#include "lua_engine.h"
#include "libcore/main/icore.h"
#include "libmem/main/ibus.h"
#include "libdebug/main/debug_context.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <iomanip>

// Try to include Lua headers if available
// Note: lauxlib.h must be included before lualib.h for proper initialization
#if __has_include(<lua5.4/lua.h>)
#define HAVE_LUA 1
extern "C" {
#include <lua5.4/lua.h>
#include <lua5.4/lauxlib.h>
#include <lua5.4/lualib.h>
}
#elif __has_include(<lua.h>)
#define HAVE_LUA 1
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

#ifdef HAVE_LUA

// Lua helper functions for mmemu API
static int lua_read_byte(lua_State* L) {
    IBus* bus = static_cast<IBus*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!bus) {
        lua_pushstring(L, "bus not available");
        lua_error(L);
        return 0;
    }

    uint32_t addr = luaL_checkinteger(L, 1);
    uint8_t val = bus->peek8(addr);
    lua_pushinteger(L, val);
    return 1;
}

static int lua_write_byte(lua_State* L) {
    IBus* bus = static_cast<IBus*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!bus) {
        lua_pushstring(L, "bus not available");
        lua_error(L);
        return 0;
    }

    uint32_t addr = luaL_checkinteger(L, 1);
    uint8_t val = luaL_checkinteger(L, 2);
    bus->write8(addr, val);
    return 0;
}

static int lua_get_register(lua_State* L) {
    ICore* cpu = static_cast<ICore*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!cpu) {
        lua_pushstring(L, "cpu not available");
        lua_error(L);
        return 0;
    }

    const char* regName = luaL_checkstring(L, 1);
    int idx = cpu->regIndexByName(regName);
    if (idx < 0) {
        lua_pushstring(L, ("unknown register: " + std::string(regName)).c_str());
        lua_error(L);
        return 0;
    }

    uint32_t val = cpu->regRead(idx);
    lua_pushinteger(L, val);
    return 1;
}

static int lua_set_register(lua_State* L) {
    ICore* cpu = static_cast<ICore*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!cpu) {
        lua_pushstring(L, "cpu not available");
        lua_error(L);
        return 0;
    }

    const char* regName = luaL_checkstring(L, 1);
    uint32_t val = luaL_checkinteger(L, 2);

    int idx = cpu->regIndexByName(regName);
    if (idx < 0) {
        lua_pushstring(L, ("unknown register: " + std::string(regName)).c_str());
        lua_error(L);
        return 0;
    }

    cpu->regWrite(idx, val);
    return 0;
}

static int lua_get_pc(lua_State* L) {
    ICore* cpu = static_cast<ICore*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!cpu) {
        lua_pushstring(L, "cpu not available");
        lua_error(L);
        return 0;
    }

    lua_pushinteger(L, cpu->pc());
    return 1;
}

static int lua_set_pc(lua_State* L) {
    ICore* cpu = static_cast<ICore*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!cpu) {
        lua_pushstring(L, "cpu not available");
        lua_error(L);
        return 0;
    }

    uint32_t addr = luaL_checkinteger(L, 1);
    cpu->setPc(addr);
    return 0;
}

static int lua_print_hex(lua_State* L) {
    int val = luaL_checkinteger(L, 1);
    std::ostringstream ss;
    ss << "$" << std::hex << std::uppercase << std::setfill('0')
       << std::setw(2) << (val & 0xFF);
    lua_pushstring(L, ss.str().c_str());
    return 1;
}

static int lua_log(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    spdlog::info("[Lua] {}", msg);
    return 0;
}

// Issue #24 Phase 4.2: Machine event hooks
static int lua_on_cycle(lua_State* L) {
    // on_cycle(interval, function_name)
    uint64_t interval = luaL_checkinteger(L, 1);
    const char* funcName = luaL_checkstring(L, 2);

    // Get DebugContext from upvalue
    DebugContext* dbg = static_cast<DebugContext*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!dbg) {
        lua_pushstring(L, "DebugContext not available");
        lua_error(L);
        return 0;
    }

    dbg->luaEvents().registerCycleEvent(interval, funcName);
    return 0;
}

static int lua_on_interrupt(lua_State* L) {
    // on_interrupt(type, function_name)
    const char* type = luaL_checkstring(L, 1);
    const char* funcName = luaL_checkstring(L, 2);

    // Get DebugContext from upvalue
    DebugContext* dbg = static_cast<DebugContext*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!dbg) {
        lua_pushstring(L, "DebugContext not available");
        lua_error(L);
        return 0;
    }

    dbg->luaEvents().registerInterruptEvent(type, funcName);
    return 0;
}

// Issue #24 Phase 4.3: Snapshot integration
static int lua_save_snapshot(lua_State* L) {
    // save_snapshot(label) -> snapshot_id
    const char* label = luaL_checkstring(L, 1);

    DebugContext* dbg = static_cast<DebugContext*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!dbg) {
        lua_pushstring(L, "DebugContext not available");
        lua_error(L);
        return 0;
    }

    int snapshotId = dbg->saveSnapshot(label);
    lua_pushinteger(L, snapshotId);
    return 1;
}

static int lua_load_snapshot(lua_State* L) {
    // load_snapshot(snapshot_id) -> success
    int snapshotId = luaL_checkinteger(L, 1);

    DebugContext* dbg = static_cast<DebugContext*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!dbg) {
        lua_pushstring(L, "DebugContext not available");
        lua_error(L);
        return 0;
    }

    bool success = dbg->restoreSnapshot(snapshotId);
    lua_pushboolean(L, success);
    return 1;
}

static int lua_list_snapshots(lua_State* L) {
    // list_snapshots() -> table of {id, label}
    DebugContext* dbg = static_cast<DebugContext*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!dbg) {
        lua_pushstring(L, "DebugContext not available");
        lua_error(L);
        return 0;
    }

    const auto& snapshots = dbg->snapshots();
    lua_newtable(L);

    for (size_t i = 0; i < snapshots.size(); ++i) {
        lua_newtable(L);  // Create entry table

        lua_pushinteger(L, i);  // id
        lua_setfield(L, -2, "id");

        lua_pushstring(L, snapshots[i].label.c_str());  // label
        lua_setfield(L, -2, "label");

        lua_rawseti(L, -2, i + 1);  // Add to results table
    }

    return 1;
}

#endif // HAVE_LUA

LuaEngine::LuaEngine(ICore* cpu, IBus* bus, DebugContext* dbg)
    : m_cpu(cpu), m_bus(bus), m_dbg(dbg), m_lua(nullptr) {
#ifdef HAVE_LUA
    m_lua = luaL_newstate();
    if (!m_lua) {
        m_lastError = "Failed to create Lua state";
        return;
    }

    // Load standard libraries
    luaL_openlibs(m_lua);
    setupGlobals();
#else
    m_lastError = "Lua support not available. Install lua5.4-dev to enable scripting.";
#endif
}

LuaEngine::~LuaEngine() {
#ifdef HAVE_LUA
    if (m_lua) {
        lua_close(m_lua);
        m_lua = nullptr;
    }
#endif
}

void LuaEngine::setupGlobals() {
#ifdef HAVE_LUA
    if (!m_lua) return;

    // Create mmemu table
    lua_newtable(m_lua);
    int mmemu_table = lua_gettop(m_lua);

    // Memory API
    lua_pushlightuserdata(m_lua, m_bus);
    lua_pushcclosure(m_lua, lua_read_byte, 1);
    lua_setfield(m_lua, mmemu_table, "read_byte");

    lua_pushlightuserdata(m_lua, m_bus);
    lua_pushcclosure(m_lua, lua_write_byte, 1);
    lua_setfield(m_lua, mmemu_table, "write_byte");

    // CPU API
    lua_pushlightuserdata(m_lua, m_cpu);
    lua_pushcclosure(m_lua, lua_get_register, 1);
    lua_setfield(m_lua, mmemu_table, "get_register");

    lua_pushlightuserdata(m_lua, m_cpu);
    lua_pushcclosure(m_lua, lua_set_register, 1);
    lua_setfield(m_lua, mmemu_table, "set_register");

    lua_pushlightuserdata(m_lua, m_cpu);
    lua_pushcclosure(m_lua, lua_get_pc, 1);
    lua_setfield(m_lua, mmemu_table, "get_pc");

    lua_pushlightuserdata(m_lua, m_cpu);
    lua_pushcclosure(m_lua, lua_set_pc, 1);
    lua_setfield(m_lua, mmemu_table, "set_pc");

    // Utility API
    lua_pushcclosure(m_lua, lua_print_hex, 0);
    lua_setfield(m_lua, mmemu_table, "hex");

    lua_pushcclosure(m_lua, lua_log, 0);
    lua_setfield(m_lua, mmemu_table, "log");

    // Event hook API (Issue #24 Phase 4.2)
    lua_pushlightuserdata(m_lua, m_dbg);
    lua_pushcclosure(m_lua, lua_on_cycle, 1);
    lua_setfield(m_lua, mmemu_table, "on_cycle");

    lua_pushlightuserdata(m_lua, m_dbg);
    lua_pushcclosure(m_lua, lua_on_interrupt, 1);
    lua_setfield(m_lua, mmemu_table, "on_interrupt");

    // Snapshot API (Issue #24 Phase 4.3)
    lua_pushlightuserdata(m_lua, m_dbg);
    lua_pushcclosure(m_lua, lua_save_snapshot, 1);
    lua_setfield(m_lua, mmemu_table, "save_snapshot");

    lua_pushlightuserdata(m_lua, m_dbg);
    lua_pushcclosure(m_lua, lua_load_snapshot, 1);
    lua_setfield(m_lua, mmemu_table, "load_snapshot");

    lua_pushlightuserdata(m_lua, m_dbg);
    lua_pushcclosure(m_lua, lua_list_snapshots, 1);
    lua_setfield(m_lua, mmemu_table, "list_snapshots");

    // Set global mmemu table
    lua_setglobal(m_lua, "mmemu");
#endif
}

void LuaEngine::setupMachineAPI() {
    // Extensible for future machine-specific APIs
}

void LuaEngine::setupDebuggerAPI() {
    // Extensible for future debugger APIs
}

bool LuaEngine::executeString(const std::string& code) {
#ifdef HAVE_LUA
    if (!m_lua) {
        m_lastError = "Lua engine not initialized";
        return false;
    }

    int result = luaL_dostring(m_lua, code.c_str());
    if (result != LUA_OK) {
        m_lastError = lua_tostring(m_lua, -1);
        lua_pop(m_lua, 1);
        return false;
    }

    return true;
#else
    m_lastError = "Lua support not available. Install lua5.4-dev to enable scripting.";
    return false;
#endif
}

bool LuaEngine::executeFile(const std::string& path) {
#ifdef HAVE_LUA
    if (!m_lua) {
        m_lastError = "Lua engine not initialized";
        return false;
    }

    // Read file
    std::ifstream file(path);
    if (!file) {
        m_lastError = "Cannot open file: " + path;
        return false;
    }

    std::string code((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    return executeString(code);
#else
    m_lastError = "Lua support not available. Install lua5.4-dev to enable scripting.";
    return false;
#endif
}

bool LuaEngine::callFunction(const std::string& name) {
#ifdef HAVE_LUA
    if (!m_lua) {
        m_lastError = "Lua engine not initialized";
        return false;
    }

    lua_getglobal(m_lua, name.c_str());
    if (!lua_isfunction(m_lua, -1)) {
        lua_pop(m_lua, 1);
        m_lastError = "Function not found: " + name;
        return false;
    }

    int result = lua_pcall(m_lua, 0, 0, 0);
    if (result != LUA_OK) {
        m_lastError = lua_tostring(m_lua, -1);
        lua_pop(m_lua, 1);
        return false;
    }

    return true;
#else
    m_lastError = "Lua support not available. Install lua5.4-dev to enable scripting.";
    return false;
#endif
}

// Breakpoint action implementation

LuaBreakpointAction::LuaBreakpointAction(const std::string& scriptCode)
    : m_scriptCode(scriptCode) {}

LuaBreakpointAction::~LuaBreakpointAction() {}

bool LuaBreakpointAction::execute(ICore* cpu, IBus* bus, DebugContext* dbg) {
    LuaEngine engine(cpu, bus, dbg);
    if (!engine.executeString(m_scriptCode)) {
        m_lastError = engine.getLastError();
        spdlog::warn("[Lua Breakpoint] {}", m_lastError);
        return true; // Continue execution on error
    }
    return true; // Continue execution
}
