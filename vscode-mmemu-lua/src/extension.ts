import * as vscode from 'vscode';
import { MmemuDebugger } from './debugger';
import { ConsoleProvider } from './debug-console';
import { WatchExpressionProvider } from './watch-provider';
import { CallStackProvider } from './call-stack-provider';
import { HoverProviderManager } from './hover-provider';
import { TestExplorerProvider } from './test-explorer-provider';
import { PerformanceVisualizer } from './perf-visualizer';

let mmemuDebugger: MmemuDebugger | undefined;
let statusBar: vscode.StatusBarItem;
let consoleProvider: ConsoleProvider | undefined;
let watchProvider: WatchExpressionProvider | undefined;
let callStackProvider: CallStackProvider | undefined;
let hoverProviderManager: HoverProviderManager | undefined;
let testExplorerProvider: TestExplorerProvider | undefined;
let perfVisualizer: PerformanceVisualizer | undefined;

export function activate(context: vscode.ExtensionContext) {
    console.log('mmemu Lua extension activated');

    // Create status bar item
    statusBar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
    statusBar.text = '$(debug-disconnect) mmemu: disconnected';
    statusBar.show();
    context.subscriptions.push(statusBar);

    // Initialize debugger
    mmemuDebugger = new MmemuDebugger();

    // Initialize console provider
    consoleProvider = new ConsoleProvider(mmemuDebugger);
    context.subscriptions.push(consoleProvider);

    // Initialize watch expression provider
    watchProvider = new WatchExpressionProvider(mmemuDebugger);
    vscode.window.registerTreeDataProvider('mmemu.watchExpressions', watchProvider);
    context.subscriptions.push(watchProvider);

    // Initialize call stack provider
    callStackProvider = new CallStackProvider(mmemuDebugger);
    vscode.window.registerTreeDataProvider('mmemu.callStack', callStackProvider);
    context.subscriptions.push(callStackProvider);

    // Initialize hover provider
    hoverProviderManager = new HoverProviderManager(mmemuDebugger);
    context.subscriptions.push(hoverProviderManager);

    // Initialize test explorer provider
    testExplorerProvider = new TestExplorerProvider(mmemuDebugger);
    vscode.window.registerTreeDataProvider('mmemu.testExplorer', testExplorerProvider);
    context.subscriptions.push(testExplorerProvider);

    // Initialize performance visualizer
    perfVisualizer = new PerformanceVisualizer(mmemuDebugger);
    context.subscriptions.push(perfVisualizer);

    // Register commands
    registerCommand(context, 'mmemu.debug', cmdStartDebugger);
    registerCommand(context, 'mmemu.runScript', cmdRunScript);
    registerCommand(context, 'mmemu.stepInto', cmdStepInto);
    registerCommand(context, 'mmemu.continue', cmdContinue);
    registerCommand(context, 'mmemu.inspect', cmdInspect);
    registerCommand(context, 'mmemu.setBreakpoint', cmdSetBreakpoint);
    registerCommand(context, 'mmemu.showRegisters', cmdShowRegisters);
    registerCommand(context, 'mmemu.showMemory', cmdShowMemory);
    registerCommand(context, 'mmemu.viewTestHistory', () => cmdViewTestHistory(testExplorerProvider));
    registerCommand(context, 'mmemu.clearTestHistory', () => cmdClearTestHistory(testExplorerProvider));
    registerCommand(context, 'mmemu.exportTestHistory', () => cmdExportTestHistory(testExplorerProvider));
    registerCommand(context, 'mmemu.compareTestStatus', () => cmdCompareTestStatus(testExplorerProvider));
    registerCommand(context, 'mmemu.toggleTestHistoryDisplay', () => cmdToggleTestHistoryDisplay(testExplorerProvider));

    // Listen for debugger state changes
    if (mmemuDebugger) {
        mmemuDebugger.on('connected', () => {
            statusBar.text = '$(debug-connect) mmemu: connected';
        });
        mmemuDebugger.on('disconnected', () => {
            statusBar.text = '$(debug-disconnect) mmemu: disconnected';
        });
        mmemuDebugger.on('breakpoint', async (line: number) => {
            statusBar.text = `$(debug-pause) mmemu: paused at line ${line}`;
            // Refresh watch expressions when breakpoint is hit
            if (watchProvider) {
                await watchProvider.refreshAll();
            }
        });
    }

    // Auto-connect if configured
    const config = vscode.workspace.getConfiguration('mmemu');
    if (config.get('autoConnect')) {
        cmdStartDebugger();
    }
}

export function deactivate() {
    if (mmemuDebugger) {
        mmemuDebugger.disconnect();
    }
}

function registerCommand(context: vscode.ExtensionContext, id: string, handler: (...args: any[]) => any) {
    context.subscriptions.push(vscode.commands.registerCommand(id, handler));
}

async function cmdStartDebugger() {
    if (!mmemuDebugger) return;

    try {
        const config = vscode.workspace.getConfiguration('mmemu');
        const host = config.get<string>('host') || 'localhost';
        const port = config.get<number>('port') || 9999;

        await mmemuDebugger.connect(host, port);
        vscode.window.showInformationMessage(`Connected to mmemu at ${host}:${port}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to connect to mmemu: ${error}`);
    }
}

async function cmdRunScript() {
    if (!mmemuDebugger) return;

    const editor = vscode.window.activeTextEditor;
    if (!editor) {
        vscode.window.showErrorMessage('No active editor');
        return;
    }

    try {
        const scriptPath = editor.document.uri.fsPath;
        await mmemuDebugger.runScript(scriptPath);
        vscode.window.showInformationMessage(`Script executed: ${scriptPath}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to run script: ${error}`);
    }
}

async function cmdStepInto() {
    if (!mmemuDebugger) return;

    try {
        await mmemuDebugger.stepInto();
    } catch (error) {
        vscode.window.showErrorMessage(`Step failed: ${error}`);
    }
}

async function cmdContinue() {
    if (!mmemuDebugger) return;

    try {
        await mmemuDebugger.continue();
    } catch (error) {
        vscode.window.showErrorMessage(`Continue failed: ${error}`);
    }
}

async function cmdInspect() {
    if (!mmemuDebugger) return;

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
        const value = await mmemuDebugger.inspectVariable(varName);
        vscode.window.showInformationMessage(`${varName} = ${value}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to inspect variable: ${error}`);
    }
}

async function cmdSetBreakpoint() {
    if (!mmemuDebugger) return;

    const editor = vscode.window.activeTextEditor;
    if (!editor) {
        vscode.window.showErrorMessage('No active editor');
        return;
    }

    const line = editor.selection.active.line + 1;
    try {
        await mmemuDebugger.toggleBreakpoint(editor.document.uri.fsPath, line);
        vscode.window.showInformationMessage(`Breakpoint toggled at line ${line}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to set breakpoint: ${error}`);
    }
}

async function cmdShowRegisters() {
    if (!mmemuDebugger) return;

    try {
        const registers = await mmemuDebugger.getRegisters();
        const message = Object.entries(registers)
            .map(([name, value]) => `${name}: $${(value as number).toString(16).toUpperCase().padStart(2, '0')}`)
            .join('\n');
        vscode.window.showInformationMessage(`Registers:\n${message}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to get registers: ${error}`);
    }
}

async function cmdShowMemory() {
    if (!mmemuDebugger) return;

    const input = await vscode.window.showInputBox({
        prompt: 'Enter memory address (hex)',
        placeHolder: '0x100'
    });

    if (!input) return;

    try {
        const addr = parseInt(input, 16);
        const bytes = await mmemuDebugger.readMemory(addr, 256);
        const hex = bytes.map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ');
        vscode.window.showInformationMessage(`Memory at 0x${addr.toString(16).toUpperCase()}:\n${hex}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to read memory: ${error}`);
    }
}

async function cmdViewTestHistory(provider: TestExplorerProvider | undefined) {
    if (!provider) return;

    const tests = provider.getAllTests();
    const testNames = tests.map(t => ({label: t.name, description: t.file}));

    const selected = await vscode.window.showQuickPick(testNames, {
        placeHolder: 'Select test to view history'
    });

    if (!selected) return;

    const persistence = (provider as any).persistence;
    const history = persistence.getTestHistory(selected.label);

    if (history.length === 0) {
        vscode.window.showInformationMessage(`No history for ${selected.label}`);
        return;
    }

    const message = history.reverse().map((run: any, i: number) =>
        `${i + 1}. ${run.status.toUpperCase()} (${run.gitHash}) - ${run.duration}ms - ${run.timestamp}`
    ).join('\n');

    vscode.window.showInformationMessage(`History for ${selected.label}:\n${message}`);
}

async function cmdClearTestHistory(provider: TestExplorerProvider | undefined) {
    if (!provider) return;

    const confirm = await vscode.window.showWarningMessage(
        'Clear all test history?',
        'Clear', 'Cancel'
    );

    if (confirm === 'Clear') {
        const persistence = (provider as any).persistence;
        await persistence.clearAllHistory();
        vscode.window.showInformationMessage('Test history cleared');
        provider.clearAllTests();
    }
}

async function cmdExportTestHistory(provider: TestExplorerProvider | undefined) {
    if (!provider) return;

    const uri = await vscode.window.showSaveDialog({
        defaultUri: vscode.Uri.file('test-history.csv'),
        filters: {'CSV': ['csv']}
    });

    if (!uri) return;

    const persistence = (provider as any).persistence;
    const csv = persistence.exportAllAsCSV();

    try {
        await vscode.workspace.fs.writeFile(uri, Buffer.from(csv, 'utf-8'));
        vscode.window.showInformationMessage(`Test history exported to ${uri.fsPath}`);
    } catch (error) {
        vscode.window.showErrorMessage(`Failed to export: ${error}`);
    }
}

async function cmdCompareTestStatus(provider: TestExplorerProvider | undefined) {
    if (!provider) return;

    const tests = provider.getAllTests();
    const testNames = tests.map(t => ({label: t.name, description: t.file}));

    const selected = await vscode.window.showQuickPick(testNames, {
        placeHolder: 'Select test to compare'
    });

    if (!selected) return;

    const persistence = (provider as any).persistence;
    const history = persistence.getTestHistory(selected.label);
    const changes = persistence.getStatusChangeCount(selected.label);

    let message = `Test: ${selected.label}\n`;
    message += `Total runs: ${history.length}\n`;
    message += `Status changes: ${changes}\n\n`;

    if (history.length > 1) {
        const first = history[0];
        const last = history[history.length - 1];
        message += `First run: ${first.status} (${first.gitHash})\n`;
        message += `Last run: ${last.status} (${last.gitHash})`;
    }

    vscode.window.showInformationMessage(message);
}

async function cmdToggleTestHistoryDisplay(provider: TestExplorerProvider | undefined) {
    if (!provider) return;

    const config = vscode.workspace.getConfiguration('mmemu');
    const current = config.get<boolean>('showTestHistory') ?? true;

    await config.update('showTestHistory', !current, vscode.ConfigurationTarget.Workspace);
    vscode.window.showInformationMessage(
        `Test history display ${!current ? 'enabled' : 'disabled'}`
    );
}
