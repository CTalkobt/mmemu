import * as vscode from 'vscode';
import { MmemuDebugger } from './debugger';

/**
 * Debug Console for Lua expression evaluation
 * Provides interactive REPL for mmemu Lua scripts
 */
export class DebugConsole {
    private outputChannel: vscode.OutputChannel;
    private history: string[] = [];
    private historyIndex = -1;
    private mmemuDebugger: MmemuDebugger;
    private isVisible = false;

    constructor(mmemuDebugger: MmemuDebugger) {
        this.mmemuDebugger = mmemuDebugger;
        this.outputChannel = vscode.window.createOutputChannel('mmemu Lua Console');
    }

    /**
     * Show the debug console
     */
    show(): void {
        this.outputChannel.show();
        this.isVisible = true;
    }

    /**
     * Hide the debug console
     */
    hide(): void {
        this.outputChannel.hide();
        this.isVisible = false;
    }

    /**
     * Clear console output
     */
    clear(): void {
        this.outputChannel.clear();
        this.log('mmemu Lua Console');
        this.log('Type Lua expressions and press Enter to evaluate');
        this.log('');
    }

    /**
     * Log a message to the console
     */
    log(message: string): void {
        this.outputChannel.appendLine(message);
    }

    /**
     * Log formatted output (hex values, etc.)
     */
    logFormatted(label: string, value: string): void {
        const formatted = this.formatOutput(value);
        this.log(`${label}${formatted}`);
    }

    /**
     * Evaluate a Lua expression
     * @param expression The Lua code to evaluate
     * @returns Result as string
     */
    async evaluateExpression(expression: string): Promise<string> {
        try {
            // Add to history
            this.history.push(expression);
            this.historyIndex = -1;

            // Log the expression
            this.log(`> ${expression}`);

            // Evaluate via debugger
            const result = await this.mmemuDebugger.evaluateLua(expression);
            this.log(this.formatOutput(result));
            this.log('');

            return result;
        } catch (error) {
            const errorMsg = error instanceof Error ? error.message : String(error);
            this.logError(errorMsg);
            return '';
        }
    }

    /**
     * Evaluate expression with user input dialog
     */
    async evaluateWithInput(): Promise<void> {
        const input = await vscode.window.showInputBox({
            prompt: 'Enter Lua expression',
            placeHolder: 'e.g., mmemu.get_register("A")',
            valueSelection: [0, 0],
        });

        if (input) {
            await this.evaluateExpression(input);
        }
    }

    /**
     * Get previous command from history
     */
    getPreviousCommand(): string | undefined {
        if (this.history.length === 0) return undefined;
        this.historyIndex = Math.min(this.historyIndex + 1, this.history.length - 1);
        return this.history[this.history.length - 1 - this.historyIndex];
    }

    /**
     * Get next command from history
     */
    getNextCommand(): string | undefined {
        if (this.history.length === 0) return undefined;
        this.historyIndex = Math.max(this.historyIndex - 1, -1);
        if (this.historyIndex === -1) return '';
        return this.history[this.history.length - 1 - this.historyIndex];
    }

    /**
     * Format output for display
     */
    private formatOutput(value: string): string {
        if (!value) return '';

        // Already formatted (hex, binary, etc.)
        if (value.startsWith('$') || value.startsWith('%') || value.includes('0x')) {
            return value;
        }

        // Try to parse as number
        if (/^\d+$/.test(value)) {
            const num = parseInt(value, 10);
            return `${value} (0x${num.toString(16).toUpperCase()}, 0b${num.toString(2)})`;
        }

        // Default: return as-is
        return value;
    }

    /**
     * Log an error message
     */
    private logError(message: string): void {
        this.log(`✗ Error: ${message}`);
    }

    /**
     * Get current history
     */
    getHistory(): string[] {
        return [...this.history];
    }

    /**
     * Clear history
     */
    clearHistory(): void {
        this.history = [];
        this.historyIndex = -1;
    }

    /**
     * Check if console is visible
     */
    isConsoleVisible(): boolean {
        return this.isVisible;
    }
}

/**
 * Console Provider for VS Code
 * Manages console input/output integration
 */
export class ConsoleProvider implements vscode.Disposable {
    private console: DebugConsole;
    private disposables: vscode.Disposable[] = [];

    constructor(mmemuDebugger: MmemuDebugger) {
        this.console = new DebugConsole(mmemuDebugger);
        this.setupCommands();
        this.console.clear();
    }

    /**
     * Setup console commands
     */
    private setupCommands(): void {
        // Show console command
        const showCmd = vscode.commands.registerCommand(
            'mmemu.showConsole',
            () => this.console.show()
        );
        this.disposables.push(showCmd);

        // Hide console command
        const hideCmd = vscode.commands.registerCommand(
            'mmemu.hideConsole',
            () => this.console.hide()
        );
        this.disposables.push(hideCmd);

        // Clear console command
        const clearCmd = vscode.commands.registerCommand(
            'mmemu.clearConsole',
            () => this.console.clear()
        );
        this.disposables.push(clearCmd);

        // Evaluate expression in console
        const evalCmd = vscode.commands.registerCommand(
            'mmemu.evaluateInConsole',
            () => this.console.evaluateWithInput()
        );
        this.disposables.push(evalCmd);
    }

    /**
     * Get the console instance
     */
    getConsole(): DebugConsole {
        return this.console;
    }

    /**
     * Dispose of resources
     */
    dispose(): void {
        this.disposables.forEach(d => d.dispose());
    }
}
