import * as vscode from "vscode";
import { MMemuDebugAdapterDescriptorFactory } from "./debugAdapter";
import { VariablesProvider } from "./views/variables";
import { MemoryProvider } from "./views/memory";
import { MMemuClient } from "./client";

let client: MMemuClient | undefined;
let outputChannel: vscode.OutputChannel;

export function activate(context: vscode.ExtensionContext) {
  outputChannel = vscode.window.createOutputChannel("MEGA65 Emulator");

  log("MEGA65 Emulator extension activated");

  // Register debug adapter
  const factory = new MMemuDebugAdapterDescriptorFactory();
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("mmemu", factory)
  );

  // Register view providers
  const variablesProvider = new VariablesProvider(getClient);
  vscode.window.registerTreeDataProvider("mmemu.variables", variablesProvider);

  const memoryProvider = new MemoryProvider(getClient);
  vscode.window.registerTreeDataProvider("mmemu.memory", memoryProvider);

  // Register commands
  context.subscriptions.push(
    vscode.commands.registerCommand("mmemu.connectEmulator", () =>
      connectEmulator(context)
    )
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("mmemu.disconnect", disconnectEmulator)
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("mmemu.readMemory", readMemory)
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("mmemu.dumpMemory", dumpMemory)
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("mmemu.setBreakpoint", setBreakpoint)
  );

  log("MEGA65 Emulator extension ready");
}

export function deactivate() {
  if (client) {
    client.close();
  }
  outputChannel.dispose();
}

function getClient(): MMemuClient | undefined {
  return client;
}

async function connectEmulator(context: vscode.ExtensionContext) {
  try {
    const config = vscode.workspace.getConfiguration("mmemu");
    const host = config.get<string>("host") || "localhost";
    const port = config.get<number>("port") || 6502;
    const timeout = config.get<number>("timeout") || 5000;

    log(`Connecting to ${host}:${port}...`);

    client = new MMemuClient({ host, port, timeout });

    const regs = await client.readRegisters();
    log(`Connected! PC=${formatAddr(regs.PC)}`);

    vscode.window.showInformationMessage(
      `Connected to mmemu at ${host}:${port}`
    );
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    log(`Connection failed: ${msg}`);
    vscode.window.showErrorMessage(`Failed to connect: ${msg}`);
    client = undefined;
  }
}

function disconnectEmulator() {
  if (client) {
    client.close();
    client = undefined;
    log("Disconnected from emulator");
    vscode.window.showInformationMessage("Disconnected from mmemu");
  }
}

async function readMemory() {
  if (!client) {
    vscode.window.showErrorMessage("Not connected to emulator");
    return;
  }

  const addr = await vscode.window.showInputBox({
    prompt: "Enter memory address",
    value: "$2000",
    validateInput: (v) => (v.length > 0 ? "" : "Address required"),
  });

  if (!addr) return;

  try {
    const memory = await client.readMemory(addr, 256);
    const formatted = formatMemory(memory, parseAddr(addr));
    log(`Memory at ${addr}:\n${formatted}`);
    outputChannel.show();
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    vscode.window.showErrorMessage(`Read failed: ${msg}`);
  }
}

async function dumpMemory() {
  if (!client) {
    vscode.window.showErrorMessage("Not connected to emulator");
    return;
  }

  const uri = await vscode.window.showSaveDialog({
    defaultUri: vscode.Uri.file("memory.bin"),
    filters: { "Binary files": ["bin"], "All files": ["*"] },
  });

  if (!uri) return;

  const addr = await vscode.window.showInputBox({
    prompt: "Start address",
    value: "$0000",
  });

  if (!addr) return;

  const size = await vscode.window.showInputBox({
    prompt: "Size in bytes",
    value: "65536",
  });

  if (!size) return;

  try {
    const memory = await client.readMemory(addr, parseInt(size));
    await vscode.workspace.fs.writeFile(uri, memory);
    vscode.window.showInformationMessage(`Dumped ${memory.length} bytes to ${uri.fsPath}`);
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    vscode.window.showErrorMessage(`Dump failed: ${msg}`);
  }
}

async function setBreakpoint() {
  if (!client) {
    vscode.window.showErrorMessage("Not connected to emulator");
    return;
  }

  const addr = await vscode.window.showInputBox({
    prompt: "Breakpoint address",
    value: "$2000",
  });

  if (!addr) return;

  try {
    const success = await client.setBreakpoint(addr);
    if (success) {
      vscode.window.showInformationMessage(`Breakpoint set at ${addr}`);
    } else {
      vscode.window.showErrorMessage("Failed to set breakpoint");
    }
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    vscode.window.showErrorMessage(`Breakpoint failed: ${msg}`);
  }
}

function log(msg: string) {
  outputChannel.appendLine(`[${new Date().toLocaleTimeString()}] ${msg}`);
}

function formatAddr(addr: number | undefined): string {
  return addr !== undefined ? `$${addr.toString(16).toUpperCase().padStart(4, "0")}` : "????";
}

function parseAddr(s: string): number {
  s = s.trim();
  if (s.startsWith("$")) return parseInt(s.slice(1), 16);
  if (s.startsWith("0x")) return parseInt(s.slice(2), 16);
  return parseInt(s, 10);
}

function formatMemory(data: Buffer, addr: number): string {
  const lines: string[] = [];
  for (let i = 0; i < data.length; i += 16) {
    const chunk = data.slice(i, Math.min(i + 16, data.length));
    const hex = Array.from(chunk).map((b) => b.toString(16).padStart(2, "0")).join(" ");
    const ascii = Array.from(chunk)
      .map((b) => (b >= 32 && b < 127 ? String.fromCharCode(b) : "."))
      .join("");
    lines.push(`${(addr + i).toString(16).padStart(4, "0")}  ${hex.padEnd(48)} ${ascii}`);
  }
  return lines.join("\n");
}
