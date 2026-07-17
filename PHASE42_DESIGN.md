# Issue #24 Phase 4.2: Machine Events - Design Document

## Overview

Phase 4.2 extends Lua scripting with machine event hooks, enabling Lua code to be triggered by:
1. **CPU Cycle Events** — Execute Lua every N CPU cycles
2. **Interrupt Events** — Execute Lua on IRQ/NMI/BRK
3. **Periodic Events** — Execute Lua at fixed time intervals

## Lua API Design

### Cycle Event Hook
```lua
-- Register handler to execute every 10,000 cycles
mmemu.on_cycle(10000, "my_cycle_handler")

function my_cycle_handler()
    local pc = mmemu.get_pc()
    mmemu.log("Cycle event at PC=$" .. string.format("%04X", pc))
end
```

### Interrupt Event Hook
```lua
-- Register handler for IRQ
mmemu.on_interrupt("IRQ", "on_irq_handler")

function on_irq_handler()
    mmemu.log("IRQ triggered")
    -- Return true to continue, false to pause
    return true
end

-- Similar for NMI and BRK
mmemu.on_interrupt("NMI", "on_nmi_handler")
mmemu.on_interrupt("BRK", "on_brk_handler")
```

### Periodic Event Hook (Future)
```lua
-- Register handler every 33ms (1 frame at 30fps)
mmemu.on_timer(33, "on_frame")

function on_frame()
    mmemu.log("Frame tick")
end
```

## Architecture

### LuaEventRegistry (new class)

Stores and manages registered event handlers:

```cpp
class LuaEventRegistry {
public:
    // Cycle event registration
    void registerCycleEvent(uint64_t interval, const std::string& functionName);
    
    // Interrupt event registration
    void registerInterruptEvent(const std::string& type, const std::string& functionName);
    
    // Check if cycle event should fire
    bool shouldFireCycleEvent(uint64_t currentCycle);
    
    // Get registered interrupt handler
    const std::string* getInterruptHandler(const std::string& type) const;
    
    // Clear all events
    void clear();
    
private:
    struct CycleEvent {
        uint64_t interval;
        std::string functionName;
        uint64_t lastFired = 0;
    };
    
    std::vector<CycleEvent> m_cycleEvents;
    std::map<std::string, std::string> m_interruptHandlers;  // type -> function name
};
```

### Integration Points

1. **DebugContext::onStep()** — Check cycle events after each step
2. **ICore interrupt handling** — Call Lua handler on IRQ/NMI
3. **LuaEngine** — Execute registered handlers with error handling

### Event Handler Execution Flow

```
CPU executes instruction
  ↓
DebugContext::onStep() called
  ↓
Check: should_fire_cycle_event?
  ├─ YES: Create LuaEngine, execute handler
  └─ NO: Continue
  ↓
Breakpoint check (existing)
  ↓
Continue execution
```

## Implementation Phases

### Phase 4.2.1: Cycle Events (2 hours)
- LuaEventRegistry class
- `on_cycle()` Lua API
- Integration in DebugContext::onStep()
- Unit tests for cycle tracking

### Phase 4.2.2: Interrupt Events (2 hours)
- `on_interrupt()` Lua API for IRQ/NMI/BRK
- Hook into ICore interrupt handling
- Test interrupt event firing
- Example scripts

### Phase 4.2.3: Periodic Events (1 hour)
- Timer infrastructure (if needed)
- `on_timer()` Lua API
- Integration with event loop

## Example Use Cases

### Instruction Counter
```lua
local count = 0

function count_instructions()
    count = count + 1
end

mmemu.on_cycle(1, "count_instructions")
```

### IRQ Tracer
```lua
function trace_irq()
    local a = mmemu.get_register("A")
    local x = mmemu.get_register("X")
    mmemu.log("IRQ! A=$" .. string.format("%02X", a) .. 
              " X=$" .. string.format("%02X", x))
end

mmemu.on_interrupt("IRQ", "trace_irq")
```

### Periodic State Inspection
```lua
function check_state()
    local sp = mmemu.get_register("SP")
    if sp < 0x80 then
        mmemu.log("Stack overflow warning: SP=$" .. string.format("%02X", sp))
    end
end

mmemu.on_cycle(50000, "check_state")  -- Every 50k cycles
```

## Testing Strategy

### Unit Tests
- LuaEventRegistry cycle tracking accuracy
- Handler registration/lookup
- Multiple concurrent events

### Integration Tests
- Cycle events fire at correct intervals
- Interrupt events fire on IRQ/NMI
- Error handling in handlers

### Example Scripts
- `examples/lua/cycle_counter.lua`
- `examples/lua/interrupt_tracer.lua`
- `examples/lua/performance_monitor.lua`

## Error Handling

- Handler execution errors logged but don't crash debugger
- Failed handler doesn't prevent subsequent handlers
- Registry survives handler errors

## Performance Considerations

- Cycle event check: O(1) with interval-based tracking
- Interrupt handler lookup: O(1) map lookup
- Zero overhead when no events registered
- Minimal overhead for registered events

## Files to Modify/Create

### New Files
- `src/libdebug/main/lua_event_registry.h`
- `src/libdebug/main/lua_event_registry.cpp`
- `examples/lua/cycle_counter.lua`
- `examples/lua/interrupt_tracer.lua`

### Modified Files
- `src/cli/main/lua_engine.h` — Add event registry pointer
- `src/cli/main/lua_engine.cpp` — Add `on_cycle()` and `on_interrupt()` APIs
- `src/libdebug/main/debug_context.h` — Add event registry
- `src/libdebug/main/debug_context.cpp` — Check/fire cycle events in onStep()
- `src/plugins/6502/main/cpu6502.cpp` — Hook interrupt handlers (if needed)

## Success Criteria

✅ Cycle events fire at correct intervals
✅ Interrupt events fire on IRQ/NMI
✅ All 660 tests passing
✅ Example scripts demonstrate all features
✅ No performance regression without events
✅ Graceful error handling

## Timeline

- **Phase 4.2.1 (Cycle Events)**: 2 hours
- **Phase 4.2.2 (Interrupt Events)**: 2 hours
- **Phase 4.2.3 (Periodic Events)**: 1 hour
- **Total Phase 4.2**: 5 hours
