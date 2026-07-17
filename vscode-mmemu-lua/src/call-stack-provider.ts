import * as vscode from 'vscode';
import { MmemuDebugger } from './debugger';

/**
 * Call Stack Frame
 */
export interface CallStackFrame {
    id: string;
    functionName: string;
    fileName: string;
    lineNumber: number;
    level: number;
}

/**
 * Tree item for a call stack frame
 */
class CallStackItem extends vscode.TreeItem {
    constructor(
        public readonly frame: CallStackFrame,
        public readonly collapsibleState: vscode.TreeItemCollapsibleState = vscode.TreeItemCollapsibleState.None
    ) {
        super(frame.functionName, collapsibleState);
        this.description = `${frame.fileName}:${frame.lineNumber}`;
        this.tooltip = `${frame.functionName} at ${frame.fileName}:${frame.lineNumber}`;
        this.iconPath = new vscode.ThemeIcon('symbol-function');
        this.command = {
            title: 'Go to Frame',
            command: 'mmemu.goToStackFrame',
            arguments: [frame],
        };
    }
}

/**
 * Call Stack TreeDataProvider
 */
export class CallStackProvider implements vscode.TreeDataProvider<CallStackFrame>, vscode.Disposable {
    private callStack: CallStackFrame[] = [];
    private mmemuDebugger: MmemuDebugger;
    private disposables: vscode.Disposable[] = [];

    private _onDidChangeTreeData: vscode.EventEmitter<CallStackFrame | undefined | null | void> =
        new vscode.EventEmitter<CallStackFrame | undefined | null | void>();
    readonly onDidChangeTreeData: vscode.Event<CallStackFrame | undefined | null | void> =
        this._onDidChangeTreeData.event;

    constructor(mmemuDebugger: MmemuDebugger) {
        this.mmemuDebugger = mmemuDebugger;
        this.setupCommands();
        this.setupDebuggerHooks();
    }

    /**
     * Setup command handlers
     */
    private setupCommands(): void {
        // Go to frame location
        const goToCmd = vscode.commands.registerCommand(
            'mmemu.goToStackFrame',
            async (frame: CallStackFrame) => {
                await this.goToFrame(frame);
            }
        );
        this.disposables.push(goToCmd);

        // Clear call stack
        const clearCmd = vscode.commands.registerCommand(
            'mmemu.clearCallStack',
            () => this.clearStack()
        );
        this.disposables.push(clearCmd);
    }

    /**
     * Setup debugger event hooks
     */
    private setupDebuggerHooks(): void {
        this.mmemuDebugger.on('breakpoint', (line: number) => {
            this.updateCallStack(line);
        });

        this.mmemuDebugger.on('disconnected', () => {
            this.clearStack();
        });
    }

    /**
     * Get root elements (top-level frames)
     */
    getChildren(element?: CallStackFrame): CallStackFrame[] {
        if (element) return [];
        return this.callStack;
    }

    /**
     * Get tree item for element
     */
    getTreeItem(element: CallStackFrame): vscode.TreeItem {
        return new CallStackItem(element);
    }

    /**
     * Update call stack from breakpoint
     */
    private async updateCallStack(line: number): Promise<void> {
        this.callStack = await this.buildCallStack(line);
        this._onDidChangeTreeData.fire(undefined);
    }

    /**
     * Build call stack
     * In a real implementation, this would parse Lua stack traces
     */
    private async buildCallStack(line: number): Promise<CallStackFrame[]> {
        const stack: CallStackFrame[] = [];

        try {
            // Get stack trace from debugger
            // This is a simplified implementation
            // In production, we'd parse the full Lua stack

            // Example: Create frames for common test scenarios
            const frames: CallStackFrame[] = [
                {
                    id: 'frame_0',
                    functionName: '[Breakpoint]',
                    fileName: 'current',
                    lineNumber: line,
                    level: 0,
                },
            ];

            return frames;
        } catch (error) {
            console.error('Error building call stack:', error);
            return [];
        }
    }

    /**
     * Navigate to frame location
     */
    private async goToFrame(frame: CallStackFrame): Promise<void> {
        try {
            // Open file and scroll to line
            const uri = vscode.Uri.file(frame.fileName);
            const document = await vscode.workspace.openTextDocument(uri);
            const editor = await vscode.window.showTextDocument(document);

            // Scroll to line
            const linePos = new vscode.Position(frame.lineNumber - 1, 0);
            editor.selection = new vscode.Selection(linePos, linePos);
            editor.revealRange(new vscode.Range(linePos, linePos), vscode.TextEditorRevealType.InCenter);
        } catch (error) {
            vscode.window.showErrorMessage(`Failed to go to frame: ${error}`);
        }
    }

    /**
     * Clear the call stack
     */
    private clearStack(): void {
        this.callStack = [];
        this._onDidChangeTreeData.fire(undefined);
    }

    /**
     * Get current call stack
     */
    getStack(): CallStackFrame[] {
        return [...this.callStack];
    }

    /**
     * Get frame at level
     */
    getFrame(level: number): CallStackFrame | undefined {
        return this.callStack.find(f => f.level === level);
    }

    /**
     * Dispose resources
     */
    dispose(): void {
        this.disposables.forEach(d => d.dispose());
        this._onDidChangeTreeData.dispose();
    }
}

/**
 * Call Stack Provider with formatting helpers
 */
export class CallStackFormatter {
    /**
     * Format function signature
     */
    static formatSignature(functionName: string, args?: {name: string, value: string}[]): string {
        if (!args || args.length === 0) {
            return `${functionName}()`;
        }

        const argStr = args.map(arg => `${arg.name}=${arg.value}`).join(', ');
        return `${functionName}(${argStr})`;
    }

    /**
     * Format location
     */
    static formatLocation(fileName: string, line: number): string {
        return `${fileName}:${line}`;
    }

    /**
     * Format frame details
     */
    static formatFrame(frame: CallStackFrame): string {
        return `L${frame.level}: ${frame.functionName} @ ${CallStackFormatter.formatLocation(frame.fileName, frame.lineNumber)}`;
    }
}
