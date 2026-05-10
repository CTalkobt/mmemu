# Skill: Machine Management in mmemu

This skill details how to manage multiple independent machine instances and load binary images.

## Workflow

1.  **List Available Machine Types:** Use `list_machines` to see all supported hardware types (e.g., `c64`, `vic20`, `pet2001`, `rawMega65`).
2.  **Create a Machine Instance:** Use `create_machine` with a `machine_type` (the hardware type).
    - Optionally provide a `machine_id` (user-chosen instance name, e.g., `"test-session"`)
    - If `machine_id` is omitted, the system auto-generates one as `<type>_<n>` (e.g., `"c64_1"`, `"c64_2"`)
3.  **List Running Instances:** Use `list_instances` to see all active machine instances with their types and display names.
4.  **Load Program Image:** Use `load_image` to load a `.prg` or `.bin` file into a machine's memory. Set `auto_start=true` to jump to the start address immediately.
5.  **Attach Cartridge:** Use `attach_cartridge` for machines like the C64 to load specialized software that uses ROM mapping.
6.  **Reset/Reboot:** Use `reset_machine` to return a machine to its power-on state while keeping it initialized.
7.  **Close an Instance:** Use `destroy_machine` to release a machine instance and free its resources.

## Multi-Machine Sessions

- You can have multiple **independent instances** of the same machine type running simultaneously in a single MCP session.
  - Example: `"c64_1"` and `"c64_2"` are two separate C64 instances with independent state, memory, and breakpoints.
- Always provide the correct `machine_id` (instance ID, not the hardware type) when calling any tool to ensure you're addressing the right instance.
- Use `list_instances` to see which instances are currently active.

## Example: Creating Two Independent C64 Instances

```
1. list_machines → returns available types including "c64"
2. create_machine(machine_type="c64") → returns instance_id "c64_1"
3. create_machine(machine_type="c64") → returns instance_id "c64_2"
4. load_image(machine_id="c64_1", path="game1.prg")
5. load_image(machine_id="c64_2", path="game2.prg")
6. step_cpu(machine_id="c64_1") → steps c64_1 only
7. step_cpu(machine_id="c64_2") → steps c64_2 only (independent state)
8. destroy_machine(machine_id="c64_1") → closes c64_1; c64_2 continues running
```

## Tools Used
- `list_machines` — List available hardware types
- `list_instances` — List running machine instances
- `create_machine` — Create a new machine instance (machine_type required, machine_id optional)
- `destroy_machine` — Close an instance
- `load_image` — Load program into a machine's memory
- `attach_cartridge` — Attach a cartridge ROM to a machine
- `reset_machine` — Reset a machine to power-on state
