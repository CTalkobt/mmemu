import * as vscode from 'vscode';
import { MmemuDebugger } from './debugger';

let debugger: MmemuDebugger | undefined;
let statusBar: vscode.StatusBarItem;

export function activate(context: vscode.ExtensionContext) {
    console.log('mmemu Lua extension activated');

    // Create status bar item
    statusBar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
    statusBar.text = '$(debug-disconnect) mmemu: disconnected';
    statusBar.show();
    context.subscriptions.push(statusBar);

    // Initialize debugger
    debugger = new MmemuDebugger();

    // Register commands
    registerCommand(context, 'mmemu.debug', cmdStartDebugger);
    registerCommand(context, 'mmemu.runScript', cmdRunScript);
    registerCommand(context, 'mmemu.stepInto', cmdStepInto);
    registerCommand(context, 'mmemu.continue', cmdContinue);
    registerCommand(context, 'mmemu.inspect', cmdInspect);
    registerCommand(context, 'mmemu.setBreakpoint', cmdSetBreakpoint);
    registerCommand(context, 'mmemu.showRegisters', cmdShowRegisters);
    registerCommand(context, 'mmemu.showMemory', cmdShowMemory);

    // Listen for debugger state changes
    if (debugger) {
        debugger.on('connected', () => {
            statusBar.text = '$(debug-connect) mmemu: connected';
        });
        debugger.on('disconnected', () => {
            statusBar.text = '$(debug-disconnect) mmemu: disconnected';
        });
        debugger.on('breakpoint', (line: number) => {
            statusBar.text = `$(debug-pause) mmemu: paused at line ${line}`;
        });
    }

    // Auto-connect if configured
    const config = vscode.workspace.getConfiguration('mmemu');
    if (config.get('autoConnect')) {
        cmdStartDebugger();
    }
}

export function deactivate() {
    if (debugger) {
        debugger.disconnect();
    }
}

function registerCommand(context: vscode.ExtensionContext, id: string, handler: (...args: any[]) => any) {
    context.subscriptions.push(vscode.commands.registerCommand(id, handler));
}

async function cmdStartDebugger() {
    if (!debugger) return;

    try {
        const config = vscode.workspace.getConfiguration('mmemu');
        const host = config.get<string>('host') || 'localhost';
        const port = config.get<number>('port') || 9999;

        await debugger.connect(host, port);
        vscode.window.showInformationMessage(`Connected to mmemu at ${host}:${port}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to connect to mmemu: ${error}`);
    }
}

async function cmdRunScript() {
    if (!debugger) return;

    const editor = vscode.window.activeTextEditor;
    if (!editor) {
        vscode.window.showErrorMessage('No active editor');
        return;
    }

    try {
        const scriptPath = editor.document.uri.fsPath;
        await debugger.runScript(scriptPath);
        vscode.window.showInformationMessage(`Script executed: ${scriptPath}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to run script: ${error}`);
    }
}

async function cmdStepInto() {
    if (!debugger) return;

    try {
        await debugger.stepInto();
    } catch (error) {
        vscode.window.showErrorMessage(`Step failed: ${error}`);
    }
}

async function cmdContinue() {
    if (!debugger) return;

    try {
        await debugger.continue();
    } catch (error) {
        vscode.window.showErrorMessage(`Continue failed: ${error}`);
    }
}

async function cmdInspect() {
    if (!debugger) return;

    const editor = vscode.window.activeTextEditor;
    if (!editor) {
        vscode.window.showErrorMessage('No active editor');
        return;
    }

    const selection = editor.selection;
    const word = editor.document.getWordRangeAtPosition(selection.active);
    if (!word) {
        vscode.window.showErrorMessage('No word at cursor');
        return;
    }

    const varName = editor.document.getText(word);
    try {
        const value = await debugger.inspectVariable(varName);
        vscode.window.showInformationMessage(`${varName} = ${value}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to inspect variable: ${error}`);
    }
}

async function cmdSetBreakpoint() {
    if (!debugger) return;

    const editor = vscode.window.activeTextEditor;
    if (!editor) {
        vscode.window.showErrorMessage('No active editor');
        return;
    }

    const line = editor.selection.active.line + 1;
    try {
        await debugger.toggleBreakpoint(editor.document.uri.fsPath, line);
        vscode.window.showInformationMessage(`Breakpoint toggled at line ${line}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to set breakpoint: ${error}`);
    }
}

async function cmdShowRegisters() {
    if (!debugger) return;

    try {
        const registers = await debugger.getRegisters();
        const message = Object.entries(registers)
            .map(([name, value]) => `${name}: $${(value as number).toString(16).toUpperCase().padStart(2, '0')}`)
            .join('\n');
        vscode.window.showInformationMessage(`Registers:\n${message}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to get registers: ${error}`);
    }
}

async function cmdShowMemory() {
    if (!debugger) return;

    const input = await vscode.window.showInputBox({
        prompt: 'Enter memory address (hex)',
        placeHolder: '0x100'
    });

    if (!input) return;

    try {
        const addr = parseInt(input, 16);
        const bytes = await debugger.readMemory(addr, 256);
        const hex = bytes.map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ');
        vscode.window.showInformationMessage(`Memory at 0x${addr.toString(16).toUpperCase()}:\n${hex}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to read memory: ${error}`);
    }
}
