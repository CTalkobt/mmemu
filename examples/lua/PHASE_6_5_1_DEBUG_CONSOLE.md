# Phase 6.5.1: Debug Console & Watch Expressions

## Overview

Enhance VS Code extension with interactive Lua debugging capabilities:
1. **Debug Console**: Lua REPL for expression evaluation
2. **Watch Expressions**: Monitor variables/expressions continuously
3. **Expression Evaluator**: Complex expression support with mmemu API

## Architecture

### Debug Console

```
┌─────────────────────────────────────┐
│ Debug Console (Bottom Panel)         │
├─────────────────────────────────────┤
│ > mmemu.get_register("A")            │
│ $55                                  │
│ > local x = 42                       │
│ nil                                  │
│ > stdlib.hex(x)                      │
│ $2A                                  │
│ > _                                  │
└─────────────────────────────────────┘
```

**Features**:
- Lua expression input
- Real-time evaluation
- History navigation (↑/↓)
- Result display with formatting
- mmemu API access
- Error messages with context

### Watch Expressions Panel

```
┌─────────────────────────────────────┐
│ Watch Expressions                   │
├─────────────────────────────────────┤
│ ▼ mmemu.get_register("A")   $55     │
│ ▼ mmemu.get_pc()            $FCE2  │
│ ▼ x * 2                      84     │
│ ▼ stdlib.hex(mmemu.read_byte(0x100))│
│   Script error: ...                 │
│ + Add expression                    │
└─────────────────────────────────────┘
```

**Features**:
- Add arbitrary expressions
- Auto-update on step/breakpoint
- Display results with formatting
- Show errors inline
- Remove/edit expressions
- Persistent across session

## Implementation Plan

### Phase 6.5.1.1: Debug Console (Week 1)

**Files to modify**:
- `src/extension.ts` — Console panel UI
- `src/debugger.ts` — Expression evaluation
- `package.json` — Output channel registration

**Commands**:
```typescript
mmemu.evaluateExpression(expr: string): Promise<string>
mmemu.clearConsole(): void
mmemu.showConsole(): void
```

**Output Channel Integration**:
```typescript
const outputChannel = vscode.window.createOutputChannel('mmemu Lua');
outputChannel.appendLine('> mmemu.get_register("A")');
outputChannel.appendLine('$55');
```

**Implementation Steps**:
1. Create OutputChannel in extension
2. Add console input via command palette
3. Implement expression evaluator in debugger.ts
4. Parse mmemu API calls
5. Format results (hex, binary, objects)
6. Add error handling

### Phase 6.5.1.2: Watch Expressions (Week 1-2)

**Files to modify**:
- `src/extension.ts` — Watch panel UI
- `src/debugger.ts` — Batch evaluation
- `package.json` — View container registration

**TreeDataProvider**:
```typescript
class WatchExpressionProvider implements vscode.TreeDataProvider<WatchExpression> {
    getChildren(element?: WatchExpression): WatchExpression[] { }
    getTreeItem(element: WatchExpression): vscode.TreeItem { }
    addExpression(expr: string): void { }
    removeExpression(id: string): void { }
    refreshAll(): void { }
}
```

**Lifecycle**:
1. User adds expression → stored in TreeDataProvider
2. Debugger pauses/steps → calls refreshAll()
3. Each expression re-evaluated
4. Results updated in panel
5. Errors displayed inline

**Implementation Steps**:
1. Create TreeDataProvider for watch expressions
2. Add "Add Watch Expression" command
3. Implement batch evaluation
4. Update on breakpoint/step events
5. Add edit/remove UI
6. Persist expressions to settings

## UI Integration

### Command Palette Commands

```json
{
  "mmemu.addWatchExpression": {
    "title": "mmemu: Add Watch Expression",
    "category": "Debug"
  },
  "mmemu.removeWatchExpression": {
    "title": "mmemu: Remove Watch Expression"
  },
  "mmemu.evaluateInConsole": {
    "title": "mmemu: Evaluate in Console"
  },
  "mmemu.clearConsole": {
    "title": "mmemu: Clear Console"
  }
}
```

### Sidebar Integration

```json
{
  "views": {
    "debug": [
      {
        "id": "mmemu.watchExpressions",
        "name": "Watch Expressions",
        "when": "debugType == mmemu-lua"
      }
    ]
  }
}
```

### Keybindings

```json
{
  "command": "mmemu.addWatchExpression",
  "key": "ctrl+shift+w",
  "mac": "cmd+shift+w",
  "when": "debugType == mmemu-lua"
}
```

## Expression Evaluation

### Supported Syntax

```lua
-- Basic values
42
"hello"
true

-- mmemu API
mmemu.get_register("A")
mmemu.get_pc()
mmemu.read_byte(0x100)

-- stdlib functions
stdlib.hex(0xFF)
stdlib.binary(0x55)

-- Complex expressions
mmemu.get_register("A") * 2
stdlib.hex(mmemu.read_byte(0x100))

-- Variables (in scope)
x + y
result.field
```

### Error Handling

```
Expression: mmemu.read_byte(invalid_addr)
Error: attempt to index a nil value
Context: at debugger.ts:142
Suggestion: Use numeric address like 0x100
```

## Data Structures

### WatchExpression Interface

```typescript
interface WatchExpression {
  id: string;                    // Unique ID
  expression: string;            // User expression
  value?: string;                // Latest result
  error?: string;                // Error message if failed
  lastUpdated?: Date;            // Last evaluation time
  enabled: boolean;              // Can be disabled temporarily
}
```

### ConsoleState Interface

```typescript
interface ConsoleState {
  history: string[];             // Input history
  watchExpressions: WatchExpression[];  // Persisted watches
  lastOutput: string;            // Last console output
}
```

## Testing Strategy

### Unit Tests (Jest)

```typescript
describe('Debug Console', () => {
  test('evaluateExpression with mmemu.get_register', async () => {
    const result = await debugger.evaluateExpression('mmemu.get_register("A")');
    expect(result).toMatch(/^\$[0-9A-F]{2}$/);
  });

  test('evaluateExpression with stdlib.hex', async () => {
    const result = await debugger.evaluateExpression('stdlib.hex(0xFF)');
    expect(result).toBe('$FF');
  });

  test('error on invalid expression', async () => {
    const result = await debugger.evaluateExpression('invalid_function()');
    expect(result).toContain('Error');
  });
});

describe('Watch Expressions', () => {
  test('addExpression and getChildren', () => {
    provider.addExpression('mmemu.get_register("A")');
    const items = provider.getChildren();
    expect(items).toHaveLength(1);
  });

  test('refreshAll updates all expressions', async () => {
    provider.addExpression('mmemu.get_pc()');
    await provider.refreshAll();
    const items = provider.getChildren();
    expect(items[0].value).toBeDefined();
  });
});
```

### Integration Tests

```typescript
describe('Integration: Console + Watch', () => {
  test('console expression appears in watch', async () => {
    await cmdEvaluateInConsole('mmemu.get_register("A")');
    provider.addExpression('mmemu.get_register("A")');
    expect(provider.getChildren()[0].value).toBe('$55');
  });

  test('watch updates on debugger breakpoint', async () => {
    provider.addExpression('mmemu.get_register("X")');
    await debugger.stepInto();
    // Should see updated X value
  });
});
```

## File Changes

### New Files
- `src/debug-console.ts` (250 lines) — Console UI and input
- `src/watch-provider.ts` (300 lines) — TreeDataProvider for watches

### Modified Files
- `src/extension.ts` — Register console and watch panels (+150 lines)
- `src/debugger.ts` — Add evaluateExpression method (+100 lines)
- `package.json` — Add commands, views, keybindings

**Total new code**: ~700 lines

## Timeline

- **Day 1-2**: Debug Console implementation
- **Day 3-4**: Watch Expressions panel
- **Day 5**: Integration testing and refinement
- **Day 6**: Documentation and examples

## Success Criteria

✅ Debug console evaluates Lua expressions in real-time
✅ Watch expressions update on step/breakpoint
✅ mmemu API fully accessible from console
✅ Error messages clear and helpful
✅ Performance: <100ms per evaluation
✅ UI responsive with 10+ watch expressions
✅ Settings persisted across sessions
✅ All tests passing (30+ new tests)

## Example Usage

### Debug Console Workflow

```
1. Set breakpoint at line with mmemu.write_byte()
2. Hit breakpoint
3. Open Debug Console (Ctrl+Alt+Y or View → Debug Console)
4. Type: mmemu.get_register("A")
5. See result: $55
6. Type: stdlib.hex(mmemu.read_byte(0x100))
7. See result: $42
8. Continue execution
```

### Watch Expressions Workflow

```
1. In Debug sidebar, click "Watch Expressions"
2. Click "+" to add expression
3. Enter: mmemu.get_register("A")
4. Enter: mmemu.get_pc()
5. Enter: stdlib.binary(mmemu.read_byte(0x100))
6. Set breakpoint
7. Hit breakpoint
8. Watch expressions auto-update to current values
9. Continue and watch values change
```

## Deliverables

1. ✅ Debug Console with Lua REPL
2. ✅ Watch Expressions panel
3. ✅ Expression Evaluator
4. ✅ Unit + integration tests
5. ✅ Documentation & examples
6. ✅ Updated extension package

## Notes

- Console uses VS Code OutputChannel for stdout
- Watch expressions are WebView-based for rich formatting
- Both features work offline (no server dependency)
- State persisted via VS Code workspace settings
- Compatible with all mmemu machines (C64, VIC-20, MEGA65)

---

**Phase 6.5.1 Status**: Design Complete → Ready for Implementation
