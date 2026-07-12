# cc45 Compiler Integration: Debug Metadata Emission

## Summary

This document provides an overview of the cc45 compiler integration work needed to emit debug metadata for mmemu's debugger infrastructure.

## What's Already Done in mmemu

The mmemu debugging infrastructure is **complete and ready** to consume debug metadata:

### 1. ✓ Debug Metadata Format Specification
- **File**: `doc/DEBUG_METADATA.md`
- **Status**: Fully specified and documented
- **Features**:
  - Variable metadata format (function, offset, size, type, scope)
  - Struct definition format
  - Optional source location mapping (src_line, src_file)
  - Type identifier system (int8, int16, uint32, ptr, arrays, structs)

### 2. ✓ Metadata Parser and Registry
- **Location**: `src/libtoolchain/main/debug_metadata.*`
- **Classes**:
  - `DebugMetadataParser` — Parses assembly comments
  - `DebugMetadataRegistry` — Stores and queries metadata
- **Methods**:
  - `parseVariableLine()` — Extracts variable information
  - `parseStructLine()` — Extracts struct field information
  - `isDebugMetadataLine()` — Detects metadata comments

### 3. ✓ Variable Symbol Integration
- **Location**: `src/libtoolchain/main/variable_symbol.*`
- **Capability**: Converts parsed metadata into VariableSymbol objects
- **Method**: `VariableSymbolTable::loadFromDebugMetadata()`

### 4. ✓ CLI Support
- **Commands**:
  - `load-debug-metadata <file>` — Load metadata from assembly file
  - `vars [function]` — Display variables in function or globally
- **Location**: `src/cli/main/cli_interpreter.cpp`

### 5. ✓ GUI Support
- **Widget**: VariablePane (wxListCtrl-based)
- **Location**: `src/gui/main/variable_pane.*`
- **Features**: Function selector dropdown, variable list display

### 6. ✓ MCP Server Support
- **Tools**:
  - `load_debug_metadata` — Load metadata via MCP protocol
  - `list_variables` — Query variables via remote RPC
- **Location**: `src/mcp/main/main.cpp`

### 7. ✓ GDB Protocol Extension
- **New Query Commands**:
  - `qMmemuVariables` — Get variables for function
  - `qMmemuSymbols` — Get symbol table
  - `qMmemuFrame` — Get frame information
- **Location**: `src/cli/main/gdb_server.cpp`
- **Format**: Hex-encoded JSON responses for IDE compatibility

## What Needs to Be Done in cc45

### Phase 1: Code Review and Understanding
**Effort**: 1-2 hours  
**Task**: Understand cc45 CodeGenerator architecture:
- [ ] Review `src/main/CodeGenerator.cpp` (275KB, ~8000 lines)
- [ ] Understand `VarInfo` struct and variable tracking
- [ ] Identify frame offset calculation logic
- [ ] Find strategic emission points for metadata

**Key Files**:
- `src/main/CodeGenerator.cpp` — Main code generator
- `src/main/CodeGenerator.h` — Header with data structures
- Lines ~1257 (parameters), ~1327 (locals), ~1776 (globals)

### Phase 2: Implementation
**Effort**: 2-3 hours  
**Tasks**:

1. **Add Helper Function** (~10 lines)
   - Method: `formatTypeString(type, pointerLevel, arrayDims)`
   - Converts CC45 type info to debug metadata format
   - Examples: `int` → `"int32"`, `int*` → `"int32*"`, `int[10]` → `"int32[10]"`

2. **Add Metadata Emitter** (~25 lines)
   - Method: `emitDebugVariable(...)`
   - Formats and outputs the metadata comment
   - Handles optional fields (src_line, src_file, name)

3. **Integration Points** (~25 lines)
   - After `.func_flags` for parameters and locals
   - In global variable generation
   - Call `emitDebugVariable()` with appropriate info

**Total Code Change**: ~60 lines of C++

### Phase 3: Testing
**Effort**: 1-2 hours  
**Tasks**:

1. **Compile Test Program**
   ```bash
   ./cc45 -c test.c -o test.o45
   ```

2. **Verify Metadata in Assembly**
   ```bash
   grep "\.debug_var:" test.s45
   ```

3. **Integration Test with mmemu**
   ```bash
   ./mmemu-cli
   > create c64
   > load test.prg
   > load-debug-metadata test.s45
   > vars
   ```

4. **Write Unit Tests**
   - Type formatting tests
   - Parameter/local/global variable tests
   - Array and pointer type tests

### Phase 4: Documentation
**Effort**: 30-45 minutes  
**Tasks**:

1. Update `CHANGELOG.md` with new feature
2. Add example in documentation (show input C → output asm)
3. Document in `doc/` folder if needed

## Implementation Roadmap

### Step 1: Create GitHub Issue
Use the template in `doc/CC45_GITHUB_ISSUE_TEMPLATE.md` to create an issue in the cc45 repository:
```
[Feature] Debug Metadata Emission for mmemu Debugger Integration
```

### Step 2: Design Review
Review `doc/CC45_DEBUG_METADATA_INTEGRATION.md` with team:
- Verify approach is sound
- Discuss type formatting strategy
- Identify any additional integration points

### Step 3: Implementation
Implement the three components:
1. Type formatter (trivial)
2. Metadata emitter (simple)
3. Integration points (straightforward)

### Step 4: Testing
- Unit tests for each component
- Integration test with mmemu
- Regression tests (ensure no existing functionality breaks)

### Step 5: Code Review & Merge
- Get peer review on cc45 pull request
- Merge to cc45 main/master branch
- Update cc45 release notes

## Expected Timeline

- **Design Review**: 1 day
- **Implementation**: 1-2 days
- **Testing & Debugging**: 1-2 days
- **Code Review & Merge**: 1 day
- **Total**: 4-6 days

## Success Metrics

After implementation, you should be able to:

```bash
# 1. Compile a program with cc45
$ ./cc45 -c test.c -o test.o45

# 2. Inspect the assembly for metadata comments
$ grep "\.debug_var:" test.s45
; .debug_var: _main x offset=10 size=2 type=int16 scope=local name=x
; .debug_var: _main y offset=12 size=2 type=int16 scope=local name=y

# 3. Load in mmemu and view variables
$ ./mmemu-cli
> create c64
> load test.prg
> load-debug-metadata test.s45
> vars
@global:
  global_counter (int32) at 0x2000

_main:
  x (int16) at offset 10
  y (int16) at offset 12
  result (int16) at offset 8
```

## Files Provided

1. **`doc/DEBUG_METADATA.md`** — Format specification (already in mmemu)
2. **`doc/CC45_DEBUG_METADATA_INTEGRATION.md`** — Detailed implementation plan
3. **`doc/CC45_GITHUB_ISSUE_TEMPLATE.md`** — Ready-to-use GitHub issue
4. **`doc/CC45_INTEGRATION_SUMMARY.md`** — This document

## Next Steps

1. **Review** the implementation plan (`CC45_DEBUG_METADATA_INTEGRATION.md`)
2. **Create** GitHub issue in cc45 repository using the template
3. **Assign** to developer (or self-assign if implementing personally)
4. **Implement** following the phase breakdown
5. **Test** with mmemu integration
6. **Merge** to cc45 main branch

## Benefits

### For Developers
- View variable values by name in mmemu CLI/GUI
- See local and parameter values without external symbol files
- Type-aware display of memory contents
- Stack frame layout visualization

### For Debugging
- Source-level variable inspection
- Automated symbol table generation
- Zero additional build artifacts needed
- Works across all mmemu frontends (CLI, GUI, MCP)

### For IDE Integration
- VS Code extension can display variable info
- CLion integration shows variable types
- GDB protocol provides symbol data to any GDB-compatible IDE

## Questions?

Refer to:
- Implementation plan: `doc/CC45_DEBUG_METADATA_INTEGRATION.md` (line-by-line details)
- Format spec: `doc/DEBUG_METADATA.md` (metadata format)
- Example: Search for `debug_metadata` in mmemu tests

---

**Status**: Ready for implementation  
**Owner**: cc45 maintainer  
**Priority**: Medium (nice-to-have, enables full IDE integration)  
**Estimate**: 4-6 days total effort
