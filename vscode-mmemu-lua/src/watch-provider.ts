import * as vscode from 'vscode';
import { MmemuDebugger } from './debugger';

/**
 * Watch Expression interface
 */
export interface WatchExpression {
    id: string;
    expression: string;
    value?: string;
    error?: string;
    enabled: boolean;
    lastUpdated?: Date;
}

/**
 * Watch Expression TreeItem
 */
class WatchExpressionItem extends vscode.TreeItem {
    constructor(
        public readonly expression: WatchExpression,
        public readonly collapsibleState: vscode.TreeItemCollapsibleState = vscode.TreeItemCollapsibleState.None
    ) {
        super(expression.expression, collapsibleState);
        this.updateDisplay();
    }

    private updateDisplay(): void {
        if (this.expression.error) {
            this.label = `${this.expression.expression}`;
            this.description = `✗ Error`;
            this.iconPath = new vscode.ThemeIcon('error');
            this.tooltip = this.expression.error;
        } else if (this.expression.value !== undefined) {
            this.label = `${this.expression.expression}`;
            this.description = this.expression.value;
            this.iconPath = new vscode.ThemeIcon('variable');
            this.tooltip = `Value: ${this.expression.value}`;
        } else {
            this.label = `${this.expression.expression}`;
            this.description = '─';
            this.iconPath = new vscode.ThemeIcon('watch');
            this.tooltip = 'Waiting for evaluation';
        }
    }
}

/**
 * TreeDataProvider for Watch Expressions
 */
export class WatchExpressionProvider implements vscode.TreeDataProvider<WatchExpression>, vscode.Disposable {
    private expressions: Map<string, WatchExpression> = new Map();
    private nextId = 0;
    private mmemuDebugger: MmemuDebugger;
    private disposables: vscode.Disposable[] = [];

    private _onDidChangeTreeData: vscode.EventEmitter<WatchExpression | undefined | null | void> =
        new vscode.EventEmitter<WatchExpression | undefined | null | void>();
    readonly onDidChangeTreeData: vscode.Event<WatchExpression | undefined | null | void> =
        this._onDidChangeTreeData.event;

    constructor(mmemuDebugger: MmemuDebugger) {
        this.mmemuDebugger = mmemuDebugger;
        this.setupCommands();
    }

    /**
     * Setup watch expression commands
     */
    private setupCommands(): void {
        // Add watch expression
        const addCmd = vscode.commands.registerCommand(
            'mmemu.addWatchExpression',
            (selectedExpr?: string) => this.addExpressionFromUser(selectedExpr)
        );
        this.disposables.push(addCmd);

        // Remove watch expression
        const removeCmd = vscode.commands.registerCommand(
            'mmemu.removeWatchExpression',
            (expr: WatchExpression) => this.removeExpression(expr.id)
        );
        this.disposables.push(removeCmd);

        // Clear all watches
        const clearCmd = vscode.commands.registerCommand(
            'mmemu.clearWatchExpressions',
            () => this.clearAllExpressions()
        );
        this.disposables.push(clearCmd);

        // Edit watch expression
        const editCmd = vscode.commands.registerCommand(
            'mmemu.editWatchExpression',
            (expr: WatchExpression) => this.editExpression(expr)
        );
        this.disposables.push(editCmd);
    }

    /**
     * Get root elements
     */
    getChildren(element?: WatchExpression): WatchExpression[] {
        if (element) return [];
        return Array.from(this.expressions.values());
    }

    /**
     * Get tree item for element
     */
    getTreeItem(element: WatchExpression): vscode.TreeItem {
        return new WatchExpressionItem(element);
    }

    /**
     * Add expression from user input
     */
    private async addExpressionFromUser(selectedExpr?: string): Promise<void> {
        const expr = selectedExpr || await vscode.window.showInputBox({
            prompt: 'Enter Lua expression to watch',
            placeHolder: 'e.g., mmemu.get_register("A")',
        });

        if (expr) {
            this.addExpression(expr);
        }
    }

    /**
     * Add a watch expression
     */
    addExpression(expression: string): void {
        const watchExpr: WatchExpression = {
            id: `watch_${this.nextId++}`,
            expression,
            value: undefined,
            error: undefined,
            enabled: true,
        };

        this.expressions.set(watchExpr.id, watchExpr);
        this._onDidChangeTreeData.fire(undefined);
    }

    /**
     * Remove a watch expression
     */
    removeExpression(id: string): void {
        this.expressions.delete(id);
        this._onDidChangeTreeData.fire(undefined);
    }

    /**
     * Clear all watch expressions
     */
    clearAllExpressions(): void {
        this.expressions.clear();
        this._onDidChangeTreeData.fire(undefined);
    }

    /**
     * Edit a watch expression
     */
    private async editExpression(expr: WatchExpression): Promise<void> {
        const newExpr = await vscode.window.showInputBox({
            prompt: 'Edit expression',
            value: expr.expression,
        });

        if (newExpr && newExpr !== expr.expression) {
            expr.expression = newExpr;
            expr.value = undefined;
            expr.error = undefined;
            this._onDidChangeTreeData.fire(expr);
        }
    }

    /**
     * Refresh all watch expressions with current values
     */
    async refreshAll(): Promise<void> {
        const promises: Promise<void>[] = [];

        for (const expr of this.expressions.values()) {
            promises.push(this.evaluateExpression(expr));
        }

        await Promise.all(promises);
        this._onDidChangeTreeData.fire(undefined);
    }

    /**
     * Evaluate a single expression
     */
    private async evaluateExpression(expr: WatchExpression): Promise<void> {
        if (!expr.enabled) return;

        try {
            expr.value = await this.mmemuDebugger.evaluateLua(expr.expression);
            expr.error = undefined;
            expr.lastUpdated = new Date();
        } catch (error) {
            expr.value = undefined;
            expr.error = error instanceof Error ? error.message : String(error);
        }
    }

    /**
     * Get all expressions
     */
    getExpressions(): WatchExpression[] {
        return Array.from(this.expressions.values());
    }

    /**
     * Toggle expression enabled state
     */
    toggleExpression(id: string): void {
        const expr = this.expressions.get(id);
        if (expr) {
            expr.enabled = !expr.enabled;
            this._onDidChangeTreeData.fire(expr);
        }
    }

    /**
     * Dispose of resources
     */
    dispose(): void {
        this.disposables.forEach(d => d.dispose());
        this._onDidChangeTreeData.dispose();
    }
}
