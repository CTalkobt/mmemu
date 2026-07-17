import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs/promises';
import { execSync } from 'child_process';

/**
 * Single test run result
 */
export interface TestRunResult {
    timestamp: string;    // ISO 8601
    gitHash: string;      // Commit hash (7 chars)
    status: 'pass' | 'fail' | 'skip';
    duration: number;     // Milliseconds
    error?: string;       // Error message if failed
}

/**
 * Test history entry
 */
export interface TestHistory {
    file: string;
    history: TestRunResult[];
    lastStatus: 'pass' | 'fail' | 'skip';
    lastGitHash: string;
    statusChanges: number;
}

/**
 * Persistence data structure
 */
interface TestPersistenceData {
    version: string;
    workspace: string;
    tests: Record<string, TestHistory>;
}

/**
 * Test Run Persistence Manager
 * Tracks test results with git hash recording
 */
export class TestPersistence {
    private dataPath: string;
    private data: TestPersistenceData;
    private dirty = false;

    constructor(workspaceRoot?: string) {
        this.dataPath = this.getPersistencePath();
        this.data = {
            version: '1.0',
            workspace: workspaceRoot || vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || '',
            tests: {},
        };
        this.loadData();
    }

    /**
     * Get persistence file path
     */
    private getPersistencePath(): string {
        let basePath: string;

        if (process.platform === 'win32') {
            basePath = process.env.APPDATA || path.join(process.env.USERPROFILE || '', 'AppData', 'Roaming');
        } else if (process.platform === 'darwin') {
            basePath = path.join(process.env.HOME || '', 'Library', 'Application Support');
        } else {
            basePath = path.join(process.env.HOME || '', '.local', 'share');
        }

        const mmemuDir = path.join(basePath, 'mmemu');
        return path.join(mmemuDir, 'test-results.json');
    }

    /**
     * Load test results from file
     */
    private async loadData(): Promise<void> {
        try {
            const content = await fs.readFile(this.dataPath, 'utf-8');
            const loaded = JSON.parse(content);

            // Validate structure
            if (loaded.version && loaded.tests) {
                this.data = loaded;
                this.cleanupOldData();
            }
        } catch (error) {
            // File doesn't exist or is invalid - start fresh
            this.data = {
                version: '1.0',
                workspace: vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || '',
                tests: {},
            };
        }
    }

    /**
     * Save test results to file
     */
    private async saveData(): Promise<void> {
        try {
            const dir = path.dirname(this.dataPath);
            await fs.mkdir(dir, { recursive: true });
            await fs.writeFile(this.dataPath, JSON.stringify(this.data, null, 2));
            this.dirty = false;
        } catch (error) {
            console.error('Failed to save test persistence data:', error);
        }
    }

    /**
     * Record a test run result
     * Returns true if test status changed
     */
    async recordTestRun(testName: string, file: string, result: Omit<TestRunResult, 'gitHash'>): Promise<boolean> {
        // Get current git hash
        const gitHash = this.getGitHash();

        // Add git hash to result
        const fullResult: TestRunResult = {
            ...result,
            gitHash,
        };

        // Check if status changed
        const hasChanged = this.hasStatusChanged(testName, result.status);

        // Initialize test history if needed
        if (!this.data.tests[testName]) {
            this.data.tests[testName] = {
                file,
                history: [],
                lastStatus: result.status,
                lastGitHash: gitHash,
                statusChanges: 0,
            };
        }

        const history = this.data.tests[testName];

        // Record the run
        history.history.push(fullResult);

        // Update status and count changes
        if (hasChanged) {
            history.statusChanges++;
        }

        history.lastStatus = result.status;
        history.lastGitHash = gitHash;

        this.dirty = true;
        await this.saveData();

        return hasChanged;
    }

    /**
     * Check if test status has changed
     */
    private hasStatusChanged(testName: string, newStatus: string): boolean {
        if (!this.data.tests[testName]) return true;
        return this.data.tests[testName].lastStatus !== newStatus;
    }

    /**
     * Get test history
     */
    getTestHistory(testName: string): TestRunResult[] {
        return this.data.tests[testName]?.history || [];
    }

    /**
     * Get test status change count
     */
    getStatusChangeCount(testName: string): number {
        return this.data.tests[testName]?.statusChanges || 0;
    }

    /**
     * Get last git hash for test
     */
    getLastGitHash(testName: string): string | undefined {
        return this.data.tests[testName]?.lastGitHash;
    }

    /**
     * Get last status for test
     */
    getLastStatus(testName: string): 'pass' | 'fail' | 'skip' | undefined {
        return this.data.tests[testName]?.lastStatus;
    }

    /**
     * Get current git commit hash
     */
    private getGitHash(): string {
        try {
            const hash = execSync('git rev-parse --short HEAD', {
                cwd: this.data.workspace,
                encoding: 'utf-8',
                stdio: ['pipe', 'pipe', 'pipe'],
            }).trim();
            return hash;
        } catch {
            return 'unknown';
        }
    }

    /**
     * Clean up old data (>30 days old)
     */
    private cleanupOldData(): void {
        const thirtyDaysAgo = new Date();
        thirtyDaysAgo.setDate(thirtyDaysAgo.getDate() - 30);

        for (const testName in this.data.tests) {
            const history = this.data.tests[testName];

            // Keep at least 10 most recent entries
            if (history.history.length > 10) {
                const filtered = history.history.filter(
                    run => new Date(run.timestamp) > thirtyDaysAgo
                );

                // If we'd delete all old data, keep last 10
                if (filtered.length < 10) {
                    history.history = history.history.slice(-10);
                } else {
                    history.history = filtered;
                }

                this.dirty = true;
            }
        }

        if (this.dirty) {
            this.saveData();
        }
    }

    /**
     * Clear all test history
     */
    async clearAllHistory(): Promise<void> {
        this.data.tests = {};
        this.dirty = true;
        await this.saveData();
    }

    /**
     * Clear history for specific test
     */
    async clearTestHistory(testName: string): Promise<void> {
        delete this.data.tests[testName];
        this.dirty = true;
        await this.saveData();
    }

    /**
     * Get all test names
     */
    getTestNames(): string[] {
        return Object.keys(this.data.tests);
    }

    /**
     * Export history as CSV
     */
    exportAsCSV(testName: string): string {
        const history = this.getTestHistory(testName);

        const rows = [
            'timestamp,git_hash,status,duration_ms,error',
            ...history.map(run =>
                `${run.timestamp},${run.gitHash},${run.status},${run.duration},"${run.error || ''}"`
            ),
        ];

        return rows.join('\n');
    }

    /**
     * Export all history as CSV
     */
    exportAllAsCSV(): string {
        const rows = [
            'test_name,timestamp,git_hash,status,duration_ms,error',
        ];

        for (const testName in this.data.tests) {
            const history = this.data.tests[testName];
            for (const run of history.history) {
                rows.push(
                    `${testName},${run.timestamp},${run.gitHash},${run.status},${run.duration},"${run.error || ''}"`
                );
            }
        }

        return rows.join('\n');
    }

    /**
     * Get test status description
     */
    getStatusDescription(testName: string): string {
        const test = this.data.tests[testName];
        if (!test) return 'No history';

        const lastRun = test.history[test.history.length - 1];
        if (!lastRun) return 'No history';

        const age = Math.round((Date.now() - new Date(lastRun.timestamp).getTime()) / 1000);
        const ageStr = age < 60 ? `${age}s ago` : `${Math.round(age / 60)}m ago`;

        return `${test.lastStatus === 'pass' ? '✓' : '✗'} ${test.lastStatus} (${ageStr}, ${test.lastGitHash})`;
    }
}
