# Phase 4.2.2 & 4.3 Implementation Plan

## Phase 4.2.2: Interrupt Event Hooks

### Scope
Register and execute Lua handlers for interrupt events (IRQ/NMI/BRK).

### Implementation Strategy

**Approach 1: Manual Callbacks (Current)**
- Add utility method `DebugContext::fireInterruptEvent(type)` for scripts to call
- Allows Lua scripts to manually trigger interrupt handlers
- Provides foundation for automatic hooking in future phases
- No CPU code modification needed

**Approach 2: Automatic Integration (Future)**
- Modify CPU step function to call interrupt handlers
- Hook into actual IRQ/NMI/BRK events in instruction execution
- Requires changes to 6502/45GS02 CPU cores
- Deferred to Phase 5

### Files to Modify
- `src/libdebug/main/debug_context.h` — Add `fireInterruptEvent()` method
- `src/libdebug/main/debug_context.cpp` — Implement handler execution
- `src/cli/main/lua_engine.cpp` — Expose as `mmemu.fire_interrupt()` (optional)
- `examples/lua/` — Example scripts

### Example Usage (Phase 4.2.2)
```lua
-- Register interrupt handler
mmemu.on_interrupt("IRQ", "on_irq_handler")

function on_irq_handler()
    mmemu.log("IRQ event fired!")
    return true  -- Continue execution
end

-- Manually trigger (for testing)
mmemu.fire_interrupt("IRQ")  -- Call registered handler
```

---

## Phase 4.3: Snapshot Integration

### Scope
Add Lua API for saving/loading machine state snapshots.

### Implementation Strategy

**Lua API Design**
```lua
-- Save snapshot with label
local snapshot_id = mmemu.save_snapshot("my_checkpoint")

-- Load snapshot by ID
mmemu.load_snapshot(snapshot_id)

-- List snapshots (optional)
local snapshots = mmemu.list_snapshots()
```

### Integration
- Use existing DebugContext snapshot functionality
- Snapshots store CPU/RAM/Cartridge state
- Return snapshot ID to Lua for later restoration
- Example: Save state before test, restore on failure

### Files to Modify
- `src/cli/main/lua_engine.cpp` — Add snapshot API
- `src/libdebug/main/debug_context.h` — Expose snapshot methods
- `examples/lua/` — Example scripts

### Example Usage (Phase 4.3)
```lua
-- Save initial state
initial = mmemu.save_snapshot("startup")

-- Run test
mmemu.log("Running test...")
-- ... test code ...

-- Restore if needed
mmemu.load_snapshot(initial)
```

---

## Implementation Order

1. **Phase 4.2.2** (This session)
   - Implement `fireInterruptEvent()` in DebugContext
   - Add Lua API exposure (optional for 4.2.2)
   - Unit tests
   - Example scripts

2. **Phase 4.3** (This session)
   - Implement `save_snapshot()` and `load_snapshot()` Lua APIs
   - Wire up to DebugContext snapshot methods
   - Example scripts for checkpoint patterns
   - Unit tests

3. **Future (Phase 5)**
   - Automatic interrupt event hooking in CPU cores
   - Direct device I/O access from Lua
   - Performance profiling integration

---

## Testing Strategy

### Phase 4.2.2
- Unit tests for interrupt handler execution
- Test that handlers can be called and return values respected
- Example script demonstrating interrupt pattern

### Phase 4.3
- Unit tests for snapshot save/restore
- Verify CPU/RAM state preservation
- Example script showing checkpoint patterns

---

## Success Criteria

### Phase 4.2.2
✅ Register interrupt handlers via Lua API
✅ Execute handlers programmatically
✅ Error handling (handler not found, Lua errors)
✅ All 660+ tests passing

### Phase 4.3
✅ Save snapshots from Lua
✅ Restore snapshots by ID
✅ Verify state integrity
✅ All 660+ tests passing

---

## Timeline Estimate

- **Phase 4.2.2**: 1-2 hours (interrupt handler execution + examples)
- **Phase 4.3**: 2-3 hours (snapshot API + examples + tests)
- **Total**: 3-5 hours for both phases
