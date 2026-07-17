import * as vscode from 'vscode';
import * as path from 'path';
import { MmemuDebugger } from './debugger';

/**
 * Test case representation
 */
export interface TestCase {
    id: string;
    name: string;
    file: string;
    line: number;
    status: 'pass' | 'fail' | 'skip' | 'pending' | 'running';
    duration?: number;
    error?: string;
    output?: string;
}

/**
 * Tree item for test case
 */
class TestItem extends vscode.TreeItem {
    constructor(
        public readonly test: TestCase,
        public readonly collapsibleState: vscode.TreeItemCollapsibleState = vscode.TreeItemCollapsibleState.None
    ) {
        super(test.name, collapsibleState);
        this.updateDisplay();
    }

    private updateDisplay(): void {
        // Icon and description based on status
        switch (this.test.status) {
            case 'pass':
                this.iconPath = new vscode.ThemeIcon('pass');
                this.description = this.test.duration ? `${this.test.duration}ms` : 'passed';
                break;
            case 'fail':
                this.iconPath = new vscode.ThemeIcon('error');
                this.description = `failed: ${this.test.error || 'unknown error'}`;
                break;
            case 'running':
                this.iconPath = new vscode.ThemeIcon('sync');
                this.description = 'running...';
                break;
            case 'skip':
                this.iconPath = new vscode.ThemeIcon('circle-slash');
                this.description = 'skipped';
                break;
            default:
                this.iconPath = new vscode.ThemeIcon('debug-pause');
                this.description = 'pending';
        }

        this.tooltip = `Test: ${this.test.name} (${this.test.file}:${this.test.line})`;
        this.command = {
            title: 'Run Test',
            command: 'mmemu.runTest',
            arguments: [this.test],
        };
    }
}

/**
 * Test Explorer Provider
 */
export class TestExplorerProvider implements vscode.TreeDataProvider<TestCase>, vscode.Disposable {
    private tests: Map<string, TestCase> = new Map();
    private mmemuDebugger: MmemuDebugger;
    private disposables: vscode.Disposable[] = [];
    private testIdCounter = 0;

    private _onDidChangeTreeData: vscode.EventEmitter<TestCase | undefined | null | void> =
        new vscode.EventEmitter<TestCase | undefined | null | void>();
    readonly onDidChangeTreeData: vscode.Event<TestCase | undefined | null | void> =
        this._onDidChangeTreeData.event;

    constructor(mmemuDebugger: MmemuDebugger) {
        this.mmemuDebugger = mmemuDebugger;
        this.setupCommands();
        this.setupFileWatching();
    }

    /**
     * Setup test commands
     */
    private setupCommands(): void {
        // Run single test
        const runCmd = vscode.commands.registerCommand(
            'mmemu.runTest',
            (test: TestCase) => this.runTest(test)
        );
        this.disposables.push(runCmd);

        // Run all tests
        const runAllCmd = vscode.commands.registerCommand(
            'mmemu.runAllTests',
            () => this.runAllTests()
        );
        this.disposables.push(runAllCmd);

        // Run failed tests
        const runFailedCmd = vscode.commands.registerCommand(
            'mmemu.runFailedTests',
            () => this.runFailedTests()
        );
        this.disposables.push(runFailedCmd);

        // Skip test
        const skipCmd = vscode.commands.registerCommand(
            'mmemu.skipTest',
            (test: TestCase) => this.skipTest(test)
        );
        this.disposables.push(skipCmd);
    }

    /**
     * Setup file watching
     */
    private setupFileWatching(): void {
        // Watch for Lua file changes
        const watcher = vscode.workspace.createFileSystemWatcher('**/*.lua');

        watcher.onDidCreate(uri => this.discoverTestsInFile(uri.fsPath));
        watcher.onDidChange(uri => this.discoverTestsInFile(uri.fsPath));
        watcher.onDidDelete(uri => this.removeTestsInFile(uri.fsPath));

        this.disposables.push(watcher);
    }

    /**
     * Get root elements
     */
    getChildren(element?: TestCase): TestCase[] {
        if (element) return [];
        // Group by file
        const grouped = new Map<string, TestCase[]>();
        for (const test of this.tests.values()) {
            if (!grouped.has(test.file)) {
                grouped.set(test.file, []);
            }
            grouped.get(test.file)!.push(test);
        }

        // Return flat list for now (could hierarchically group later)
        return Array.from(this.tests.values());
    }

    /**
     * Get tree item
     */
    getTreeItem(element: TestCase): vscode.TreeItem {
        return new TestItem(element);
    }

    /**
     * Discover tests in a Lua file
     */
    private async discoverTestsInFile(filePath: string): Promise<void> {
        try {
            const document = await vscode.workspace.openTextDocument(filePath);
            const text = document.getText();

            // Find test functions: function test_*()
            const testRegex = /function\s+(test_[\w]+)\s*\(\s*\)/g;
            let match;

            while ((match = testRegex.exec(text)) !== null) {
                const testName = match[1];
                const lineNum = document.positionAt(match.index).line + 1;

                const testId = `test_${this.testIdCounter++}`;
                const test: TestCase = {
                    id: testId,
                    name: testName,
                    file: filePath,
                    line: lineNum,
                    status: 'pending',
                };

                this.tests.set(testId, test);
            }

            this._onDidChangeTreeData.fire(undefined);
        } catch (error) {
            console.error('Error discovering tests:', error);
        }
    }

    /**
     * Remove tests from a file
     */
    private removeTestsInFile(filePath: string): void {
        let removed = false;
        for (const [id, test] of this.tests.entries()) {
            if (test.file === filePath) {
                this.tests.delete(id);
                removed = true;
            }
        }

        if (removed) {
            this._onDidChangeTreeData.fire(undefined);
        }
    }

    /**
     * Run a single test
     */
    private async runTest(test: TestCase): Promise<void> {
        test.status = 'running';
        this._onDidChangeTreeData.fire(test);

        const startTime = Date.now();

        try {
            // Execute the test function
            await this.mmemuDebugger.evaluateLua(`${test.name}()`);

            test.status = 'pass';
            test.duration = Date.now() - startTime;
            test.error = undefined;
        } catch (error) {
            test.status = 'fail';
            test.duration = Date.now() - startTime;
            test.error = error instanceof Error ? error.message : String(error);
        }

        this._onDidChangeTreeData.fire(test);
    }

    /**
     * Run all tests
     */
    private async runAllTests(): Promise<void> {
        const tests = Array.from(this.tests.values());
        for (const test of tests) {
            await this.runTest(test);
        }
    }

    /**
     * Run failed tests
     */
    private async runFailedTests(): Promise<void> {
        const failed = Array.from(this.tests.values()).filter(t => t.status === 'fail');
        for (const test of failed) {
            await this.runTest(test);
        }
    }

    /**
     * Skip test
     */
    private skipTest(test: TestCase): void {
        test.status = 'skip';
        this._onDidChangeTreeData.fire(test);
    }

    /**
     * Get all tests
     */
    getAllTests(): TestCase[] {
        return Array.from(this.tests.values());
    }

    /**
     * Get test statistics
     */
    getStats(): {total: number, passed: number, failed: number, skipped: number} {
        const tests = Array.from(this.tests.values());
        return {
            total: tests.length,
            passed: tests.filter(t => t.status === 'pass').length,
            failed: tests.filter(t => t.status === 'fail').length,
            skipped: tests.filter(t => t.status === 'skip').length,
        };
    }

    /**
     * Clear all tests
     */
    clearAllTests(): void {
        this.tests.clear();
        this._onDidChangeTreeData.fire(undefined);
    }

    /**
     * Dispose
     */
    dispose(): void {
        this.disposables.forEach(d => d.dispose());
        this._onDidChangeTreeData.dispose();
        this.tests.clear();
    }
}
