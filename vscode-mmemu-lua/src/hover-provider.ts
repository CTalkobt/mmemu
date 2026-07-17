import * as vscode from 'vscode';
import { MmemuDebugger } from './debugger';

/**
 * Hover Provider for Lua expressions
 * Shows variable values and expression results on hover
 */
export class LuaHoverProvider implements vscode.HoverProvider, vscode.Disposable {
    private mmemuDebugger: MmemuDebugger;
    private disposables: vscode.Disposable[] = [];
    private resultCache: Map<string, {value: string, timestamp: number}> = new Map();
    private cacheTimeout = 5000; // 5 seconds

    constructor(mmemuDebugger: MmemuDebugger) {
        this.mmemuDebugger = mmemuDebugger;
        this.setupCacheInvalidation();
    }

    /**
     * Provide hover information
     */
    async provideHover(
        document: vscode.TextDocument,
        position: vscode.Position,
        _token: vscode.CancellationToken
    ): Promise<vscode.Hover | undefined> {
        try {
            // Get word at cursor
            const wordRange = document.getWordRangeAtPosition(position);
            if (!wordRange) return undefined;

            let expression = document.getText(wordRange);

            // Try to get full expression (handle . and [ ] access)
            const line = document.lineAt(position.line);
            const expanded = this.expandExpression(line.text, position.character);
            if (expanded.length > expression.length) {
                expression = expanded;
            }

            // Check cache first
            const cached = this.getFromCache(expression);
            if (cached) {
                return this.createHover(expression, cached);
            }

            // Evaluate expression
            let value: string;
            try {
                value = await this.mmemuDebugger.evaluateLua(expression);
            } catch {
                // If evaluation fails, try as a variable name
                return undefined;
            }

            // Cache result
            this.cacheResult(expression, value);

            return this.createHover(expression, value);
        } catch (error) {
            return undefined;
        }
    }

    /**
     * Expand expression to include property/method access
     */
    private expandExpression(line: string, cursorPos: number): string {
        let start = cursorPos;
        let end = cursorPos;

        // Find start of expression (alphanumeric, _, $, or .)
        while (start > 0 && /[\w_$.]/.test(line[start - 1])) {
            start--;
        }

        // Find end of expression
        while (end < line.length && /[\w_$.\[\]"]/.test(line[end])) {
            end++;
        }

        return line.substring(start, end);
    }

    /**
     * Create hover content
     */
    private createHover(expression: string, value: string): vscode.Hover {
        const hover = new vscode.MarkdownString();

        // Format value based on type
        const formatted = this.formatValue(value);

        hover.appendMarkdown(`**\`${expression}\`**\n\n`);
        hover.appendMarkdown(`Value: ${formatted}\n`);

        // Add type information if available
        const typeInfo = this.inferType(value);
        if (typeInfo) {
            hover.appendMarkdown(`\nType: ${typeInfo}`);
        }

        return new vscode.Hover(hover);
    }

    /**
     * Format value for display
     */
    private formatValue(value: string): string {
        if (!value) return '(empty)';

        // Hex value
        if (value.startsWith('$')) {
            const hex = value.substring(1);
            const decimal = parseInt(hex, 16);
            const binary = decimal.toString(2).padStart(8, '0');
            return `\`${value}\` (${decimal}, 0b${binary})`;
        }

        // Binary value
        if (value.startsWith('0b')) {
            const binary = value.substring(2);
            const decimal = parseInt(binary, 2);
            const hex = decimal.toString(16).toUpperCase();
            return `\`${value}\` (${decimal}, $${hex})`;
        }

        // Number
        if (/^\d+$/.test(value)) {
            const decimal = parseInt(value);
            const hex = decimal.toString(16).toUpperCase();
            const binary = decimal.toString(2);
            return `\`${value}\` ($${hex}, 0b${binary})`;
        }

        // String
        if (value.includes(' ') || value.includes('\n')) {
            return `\`"${value}"\``;
        }

        // Default
        return `\`${value}\``;
    }

    /**
     * Infer type from value
     */
    private inferType(value: string): string | undefined {
        if (value.startsWith('$')) return 'number (hex)';
        if (value.startsWith('0b')) return 'number (binary)';
        if (/^\d+$/.test(value)) return 'number';
        if (value === 'true' || value === 'false') return 'boolean';
        if (value.includes(' ') || value.includes('\n')) return 'string';
        return undefined;
    }

    /**
     * Get result from cache
     */
    private getFromCache(key: string): string | undefined {
        const entry = this.resultCache.get(key);
        if (!entry) return undefined;

        // Check if expired
        const now = Date.now();
        if (now - entry.timestamp > this.cacheTimeout) {
            this.resultCache.delete(key);
            return undefined;
        }

        return entry.value;
    }

    /**
     * Cache evaluation result
     */
    private cacheResult(key: string, value: string): void {
        this.resultCache.set(key, {
            value,
            timestamp: Date.now(),
        });
    }

    /**
     * Setup cache invalidation
     */
    private setupCacheInvalidation(): void {
        // Clear cache on breakpoint (values might have changed)
        this.mmemuDebugger.on('breakpoint', () => {
            this.resultCache.clear();
        });

        // Clear cache on disconnect
        this.mmemuDebugger.on('disconnected', () => {
            this.resultCache.clear();
        });
    }

    /**
     * Clear cache
     */
    clearCache(): void {
        this.resultCache.clear();
    }

    /**
     * Dispose
     */
    dispose(): void {
        this.disposables.forEach(d => d.dispose());
        this.resultCache.clear();
    }
}

/**
 * Provider registration helper
 */
export class HoverProviderManager implements vscode.Disposable {
    private provider: LuaHoverProvider;
    private disposables: vscode.Disposable[] = [];

    constructor(mmemuDebugger: MmemuDebugger) {
        this.provider = new LuaHoverProvider(mmemuDebugger);

        // Register hover provider for Lua files
        const hoverReg = vscode.languages.registerHoverProvider(
            { language: 'lua-mmemu' },
            this.provider
        );
        this.disposables.push(hoverReg);
    }

    /**
     * Get the provider instance
     */
    getProvider(): LuaHoverProvider {
        return this.provider;
    }

    /**
     * Dispose
     */
    dispose(): void {
        this.provider.dispose();
        this.disposables.forEach(d => d.dispose());
    }
}
