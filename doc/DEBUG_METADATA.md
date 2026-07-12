# Debug Metadata Format Specification

## Overview

The debug metadata format provides structured variable and type information emitted by the cc45 compiler (or other compatible compilers) to enable enhanced debugging capabilities in mmemu. This format allows the debugger to:

1. Display variable names and values in a human-readable format
2. Track variable scope (parameters, locals, globals)
3. Support struct field inspection
4. Map source-level symbols to memory locations
5. Correlate debugging information with source files and line numbers

## Format Specification

### Variable Metadata Lines

Variable metadata is emitted as assembly comments in the following format:

```
; .debug_var: function_name var_name offset=N size=N type=TYPE scope=SCOPE [src_line=N] [src_file=FILE] [name=DISPLAY_NAME]
```

#### Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `function_name` | string | Yes | Name of the function (with `_` prefix for mangled names, `@global` for global scope) |
| `var_name` | string | Yes | Internal/compiler variable name (may use prefixes like `@_`, `__vr0`) |
| `offset` | decimal | Yes | Byte offset (frame-relative for local/parameter, absolute for global) |
| `size` | decimal | Yes | Size in bytes |
| `type` | string | Yes | Type identifier (see Type Identifiers section) |
| `scope` | enum | Yes | One of: `parameter`, `local`, `global` |
| `src_line` | decimal | Optional | Source code line number where declared (-1 if unknown) |
| `src_file` | string | Optional | Source filename |
| `name` | string | Optional | User-friendly display name (if different from `var_name`) |

#### Type Identifiers

Standard type identifiers:
- `int8`, `I8` — signed 8-bit integer
- `int16`, `I16` — signed 16-bit integer
- `int32`, `I32` — signed 32-bit integer
- `uint8`, `U8` — unsigned 8-bit integer
- `uint16`, `U16` — unsigned 16-bit integer
- `uint32`, `U32` — unsigned 32-bit integer
- `char` — character type
- `ptr` or `T*` — pointer type
- `struct_NAME` — struct type reference
- Custom type names followed by `[...]` — array types

#### Examples

Simple local variable:
```
; .debug_var: _main x offset=10 size=2 type=int16 scope=local
```

Function parameter with source location:
```
; .debug_var: _make_point @_p_x offset=10 size=2 type=int16 scope=parameter src_line=12 src_file=main.c name=p.x
```

Global counter:
```
; .debug_var: @global counter offset=0x1000 size=1 type=uint8 scope=global name=tick_counter
```

Temporary register variable:
```
; .debug_var: _calculate __vr0 offset=4 size=2 type=int16 scope=local name=result
```

### Struct Definition Lines

Struct field metadata is emitted as:

```
; .debug_struct: struct_name field1:offset:size:type field2:offset:size:type ...
```

#### Examples

```
; .debug_struct: Point x:0:2:int16 y:2:2:int16
; .debug_struct: Rectangle top_left:0:4:struct_Point bottom_right:4:4:struct_Point
```

## Integration with mmemu

### Parser

The `DebugMetadataParser` class provides:

- `parseVariableLine()` — Parse variable metadata from assembly comment
- `parseStructLine()` — Parse struct definition from assembly comment
- `isDebugMetadataLine()` — Check if a line contains debug metadata

### Registry

The `DebugMetadataRegistry` class collects and queries metadata:

```cpp
// Add parsed metadata
registry.addVariable(debugVar);
registry.addStructFields(structFields);

// Query metadata
auto vars = registry.getVariablesForFunction("_main");
auto fields = registry.getStructFields("Point");
```

### VariableSymbolTable Integration

The `VariableSymbolTable` can load debug metadata directly:

```cpp
VariableSymbolTable table;
table.loadFromDebugMetadata("program.asm");
```

This converts debug metadata into `VariableSymbol` objects for seamless integration with existing debugger infrastructure.

## Compiler Integration (cc45)

To enable debug metadata emission in cc45:

1. Add a compiler flag or option (e.g., `-g` or `--debug-metadata`)
2. When emitting assembly, for each variable in the symbol table:
   - Generate a `; .debug_var:` line after the function prologue
   - Include function name, variable name, frame offset, size, type, and scope
   - Add optional source location information from the source map

### Example cc45 Transformation

Input C code:
```c
void make_point(int x, int y) {
    int temp = x + y;
    ...
}
```

Generated assembly with debug metadata:
```asm
_make_point:
    ; .debug_var: _make_point @_p_x offset=10 size=2 type=int16 scope=parameter src_line=1 src_file=main.c name=x
    ; .debug_var: _make_point @_p_y offset=12 size=2 type=int16 scope=parameter src_line=1 src_file=main.c name=y
    ; .debug_var: _make_point __vr0 offset=4 size=2 type=int16 scope=local src_line=2 name=temp
    ; ... function body
```

## CLI Commands

### Load Debug Metadata

```
load-debug-symbols <file>
```

Loads debug metadata from an assembly file and populates the variable symbol table.

### Display Variables

```
vars [function_name]
```

List all variables in a function (or all functions if not specified).

Output example:
```
Function: _main
  @_p_x (x) [parameter]: offset=10, size=2, type=int16, value=0x0123
  __vr0 (result) [local]: offset=4, size=2, type=int16, value=0x0456
```

## MCP Tools

The MCP server can provide tools for:

- `load_debug_metadata` — Load and parse debug metadata from a file
- `inspect_variable` — Get current value of a variable
- `list_variables` — List all variables in a function

## Future Enhancements

1. **Optimization Level Correlation** — Track which variables are optimized out
2. **DW_AT Compatibility** — Subset of DWARF semantics for broader compiler support
3. **Hot Reload** — Dynamic symbol table updates without restarting
4. **Source Mapping** — Clickable links to source code locations
5. **Struct Inspector** — Interactive navigation of struct hierarchies

## Implementation Checklist

- [x] Define metadata format specification
- [x] Implement `DebugMetadataParser` class
- [x] Implement `DebugMetadataRegistry` class
- [x] Integrate with `VariableSymbolTable`
- [ ] Add CLI commands for loading and displaying metadata
- [ ] Add MCP tools for metadata inspection
- [ ] Add GUI support for variable inspection
- [ ] Create issue in cc45 repository for compiler changes
- [ ] Document assembler output format for other compilers

## Testing

Unit tests for metadata parsing are located in:
- `src/libtoolchain/test/test_debug_metadata.cpp`

Tests cover:
- Parsing simple variables
- Parsing parameters with source locations
- Parsing global variables
- Parsing struct definitions
- Registry operations (add, query, clear)
- Type parsing and conversion
