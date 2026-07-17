# Phase 6.5.2: Advanced Debugging Features

## Overview

Enhance the VS Code extension with four powerful debugging capabilities:
1. **Call Stack Display** — Trace function hierarchy
2. **Hover Tooltips** — Inspect variables on hover
3. **Integrated Test Runner** — Run Lua tests within VS Code
4. **Performance Visualization** — Profile execution with charts

## Architecture

### 1. Call Stack Display

```
┌─────────────────────────────────────┐
│ Call Stack (Debug Sidebar)          │
├─────────────────────────────────────┤
│ ▼ test_memory_operations            │
│   ├─ patterns.fill()                │
│   │  ├─ stdlib.hex()                │
│   │  └─ mmemu.write_byte()          │
│   └─ test_utils.assert_equal()      │
│ ▼ main()                            │
│   └─ [Lua runtime]                  │
└─────────────────────────────────────┘
```

**Implementation**:
- TreeDataProvider for call stack
- Parse Lua stack traces
- Navigate to function definitions
- Show line numbers and arguments
- Right-click → "Go to Source"

**Files**:
- `src/call-stack-provider.ts` (200 lines)

### 2. Hover Tooltips

```
Variable hover in editor:
  mmemu.get_register("A")──────────────┐
                                       │
                     "A register value: $55"
                     
  mmemu.get_pc()────────────────────────┐
                                        │
                     "Program counter: $FCE2"
```

**Implementation**:
- HoverProvider for .lua files
- Extract variable/expression at cursor
- Evaluate and show result
- Format with context

**Files**:
- `src/hover-provider.ts` (150 lines)

### 3. Integrated Test Runner

```
┌─────────────────────────────────────┐
│ Test Explorer (Activity Bar)        │
├─────────────────────────────────────┤
│ ▼ test_suite.lua                    │
│   ├─ ✓ test_memory_operations      │
│   ├─ ✓ test_register_access        │
│   ├─ ✗ test_pattern_fill (FAILED)  │
│   └─ ⊖ test_device_io             │
│ ▼ integration_tests.lua             │
│   ├─ ⊘ test_sid_synthesis         │
│   └─ ⊘ test_dma_copy              │
└─────────────────────────────────────┘

Click test → Run immediately
View output → Pass/fail details
Re-run failed tests
Profile test execution
```

**Implementation**:
- TestExplorerProvider
- Parse test functions from scripts
- Execute tests via mmemu
- Display results with timing
- Quick re-run failed tests

**Files**:
- `src/test-explorer-provider.ts` (300 lines)

### 4. Performance Visualization

```
┌─────────────────────────────────────┐
│ Performance Profile                 │
├─────────────────────────────────────┤
│ Test: test_memory_operations        │
│ Time: 42ms                          │
│                                     │
│ Function Breakdown:                 │
│ ┌─ patterns.fill()      12ms (29%) │
│ ├─ stdlib.hex()          8ms (19%) │
│ ├─ test_utils.assert()  15ms (36%) │
│ └─ mmemu.write_byte()    7ms (17%) │
│                                     │
│ Breakdown by Category:              │
│ ├─ Memory Ops: 18ms (43%)          │
│ ├─ CPU Regs:   12ms (29%)          │
│ └─ Formatting:  12ms (29%)         │
└─────────────────────────────────────┘
```

**Implementation**:
- WebView panel for charts
- Collect timing data from profiler
- Render bar charts and pie charts
- Export performance reports

**Files**:
- `src/perf-visualizer.ts` (200 lines)
- `src/perf-chart.html` (150 lines)

## Implementation Plan

### Phase 6.5.2.1: Call Stack Display (Day 1-2)

**Files to create**:
- `src/call-stack-provider.ts` — TreeDataProvider

**Features**:
1. Parse Lua call stack on breakpoint
2. TreeItem per stack frame
3. Navigate to source location
4. Show arguments and local vars
5. Click to jump to frame

**Integration Points**:
- Listen to debugger `breakpoint` events
- Update call stack in sidebar
- Create view container if needed

**Example Test**:
```lua
function level1()
  level2()  -- Breakpoint here: shows stack
end

function level2()
  level3()
end

function level3()
  mmemu.write_byte(0x100, 0x42)
end
```

### Phase 6.5.2.2: Hover Tooltips (Day 2-3)

**Files to create**:
- `src/hover-provider.ts` — HoverProvider

**Features**:
1. Hover over expression → evaluate
2. Show result in tooltip
3. Format with type info
4. Cache results (don't re-eval while typing)
5. Handle errors gracefully

**Integration Points**:
- `vscode.languages.registerHoverProvider()`
- Call debugger.evaluateLua()
- Format result with mmemu context

**Example**:
```lua
local addr = 0x100
mmemu.write_byte(addr, 0x42)  -- Hover over 'addr': shows "$100"
local value = mmemu.read_byte(addr)  -- Hover: shows "$42"
```

### Phase 6.5.2.3: Test Runner (Day 3-4)

**Files to create**:
- `src/test-explorer-provider.ts` — TestTreeDataProvider

**Features**:
1. Scan .lua files for `function test_*()`
2. Display in tree explorer
3. Run individual tests
4. Show pass/fail status
5. Display timing per test
6. Quick re-run failed
7. Show output on failure

**Integration Points**:
- Register TestExplorerProvider
- Hook into TestController API
- Call debugger to run tests
- Parse test results

**Test Detection**:
```lua
-- Detected as tests:
function test_memory_operations() ... end
function test_register_access() ... end
function test_device_io() ... end

-- Not detected:
function helper_function() ... end
local test_var = 42
```

### Phase 6.5.2.4: Performance Visualization (Day 4-5)

**Files to create**:
- `src/perf-visualizer.ts` — Chart renderer
- `src/perf-chart.html` — WebView UI

**Features**:
1. Collect timing from profiler module
2. Render bar chart (functions)
3. Render pie chart (categories)
4. Export as CSV/JSON
5. Compare runs side-by-side
6. Identify bottlenecks

**Integration Points**:
- WebViewPanel for visualization
- Parse profiler output
- Generate charts with Canvas/SVG
- Interactive tooltips

**Data Format**:
```json
{
  "test": "test_memory_operations",
  "totalTime": 42,
  "functions": [
    { "name": "patterns.fill", "time": 12, "percent": 29 },
    { "name": "stdlib.hex", "time": 8, "percent": 19 },
    ...
  ],
  "categories": [
    { "name": "Memory Ops", "time": 18, "percent": 43 },
    ...
  ]
}
```

## UI Integration

### package.json Changes

**New Views**:
```json
{
  "views": {
    "debug": [
      {
        "id": "mmemu.callStack",
        "name": "Call Stack"
      },
      {
        "id": "mmemu.testExplorer",
        "name": "Test Explorer"
      },
      {
        "id": "mmemu.performanceProfile",
        "name": "Performance Profile"
      }
    ]
  }
}
```

**New Commands**:
```json
{
  "command": "mmemu.runTest",
  "title": "Run Test"
},
{
  "command": "mmemu.runAllTests",
  "title": "Run All Tests"
},
{
  "command": "mmemu.runFailedTests",
  "title": "Re-run Failed Tests"
},
{
  "command": "mmemu.showPerformanceProfile",
  "title": "Show Performance Profile"
}
```

## Data Structures

### CallStackFrame

```typescript
interface CallStackFrame {
  id: string;
  functionName: string;
  fileName: string;
  lineNumber: number;
  arguments?: {name: string, value: string}[];
  locals?: {name: string, value: string}[];
}
```

### TestCase

```typescript
interface TestCase {
  id: string;
  name: string;
  file: string;
  line: number;
  status: 'pass' | 'fail' | 'skip' | 'pending';
  duration: number;
  error?: string;
  output?: string;
}
```

### PerformanceData

```typescript
interface PerformanceData {
  test: string;
  totalTime: number;
  functions: {name: string, time: number, percent: number}[];
  categories: {name: string, time: number, percent: number}[];
}
```

## Testing Strategy

### Unit Tests

```typescript
describe('Call Stack Provider', () => {
  test('parses call stack from debugger', () => {
    // Mock breakpoint event with stack trace
    // Verify frames extracted correctly
  });
  
  test('displays function names with line numbers', () => {
    // Verify TreeItem labels
  });
});

describe('Hover Provider', () => {
  test('evaluates expression at cursor', () => {
    // Hover over variable
    // Verify tooltip shows value
  });
  
  test('formats numbers as hex/binary', () => {
    // Hover over number
    // Verify formatting
  });
});

describe('Test Explorer', () => {
  test('discovers test functions', () => {
    // Parse test script
    // Verify all tests found
  });
  
  test('runs individual test', () => {
    // Execute test
    // Verify pass/fail status
  });
});

describe('Performance Visualizer', () => {
  test('renders performance chart', () => {
    // Generate chart from timing data
    // Verify SVG output
  });
});
```

## Performance Targets

- Call stack update: <50ms
- Hover tooltip: <200ms (cached)
- Test discovery: <500ms per file
- Test execution: <1s per test
- Chart rendering: <100ms

## Success Criteria

✅ Call stack displays on breakpoint
✅ Hover tooltips show values instantly
✅ Test explorer discovers and runs tests
✅ Performance data visualized in charts
✅ All features integrate seamlessly
✅ No performance regressions

## Timeline

- **Day 1-2**: Call Stack Display
- **Day 2-3**: Hover Tooltips
- **Day 3-4**: Test Runner
- **Day 4-5**: Performance Visualization
- **Day 5-6**: Integration testing, refinement
- **Day 6**: Documentation, packaging

## Deliverables

1. ✅ Call Stack TreeDataProvider
2. ✅ Hover Expression Provider
3. ✅ Test Explorer with runner
4. ✅ Performance Visualizer
5. ✅ Updated package.json
6. ✅ Comprehensive tests
7. ✅ User documentation
8. ✅ Updated extension package

## Example Workflow

```
1. Open test_suite.lua in VS Code
2. Hover over mmemu.get_register("A") → Shows: $55
3. Set breakpoint in test function
4. Run test from Test Explorer
5. Hit breakpoint → Call Stack shows:
   ├─ test_memory_operations (line 42)
   ├─ patterns.fill (line 100)
   └─ mmemu.write_byte (line 1)
6. Test completes
7. View Performance Profile → See timing breakdown
8. Re-run failed tests with one click
```

---

**Phase 6.5.2 Status**: Design Complete → Ready for Implementation
