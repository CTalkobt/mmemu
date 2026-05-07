# Completed Items

## UI / Debugging Features
[x] Pop-up Modal Calculator
  [x] - Base conversion
  [x] - Basic Calculator Operations
    ( + - * / ^(power) square root, x^2, nth root, x^n, 
      sin, cos, sec, csc, tan, atn
      arc sin, arc cos.... 
      negate, ... 
    )
  [x] - 1's complement, 2's compliment
  [x] - Float / Integer mode. 
  [x] - Memory mode (8 results) 
  [x] - Ans (aka Rcl 0) button - Recall prior result. 
  [x] - Rcl n button to recall nth prior result. 
  [x] - Set precision display for int or floating point.

## Toolchain Migration
[x] Migrate 45GS02 test suite from KickAssembler (.asm) to ca45 (.s format)
  [x] - Remove legacy KickAssembler .asm files
  [x] - Convert test programs to .s format
  [x] - Update Makefile test targets
  [x] - Update validate.py for ca45 assembler
  [x] - Update documentation with ca45 project link

## Assembler Support & Selection
[ ] Add ca45 assembler backend support for GUI, CLI, and MCP (similar to KickAssembler)
  [ ] - Implement `Ca45AssemblerBackend` in libtoolchain
  [ ] - Support ca45 `.s` file format and `.prg` output
  [ ] - Parse ca45 symbol output (if available)
  [ ] - Register ca45 backend with toolchain registry
  [ ] - Add assembler selection mechanism (config, command-line, or file extension detection)
  [ ] - Update CLI `asm` command to support assembler selection
  [ ] - Update GUI assemble dialog with assembler picker
  [ ] - Update MCP assembler tool with backend selection
  [ ] - Document assembler backend architecture and how to add new assemblers
