import * as vscode from 'vscode';
import { MmemuDebugger } from './debugger';

/**
 * Performance data
 */
export interface PerformanceData {
    test: string;
    totalTime: number;
    functions: Array<{name: string, time: number, percent: number}>;
    categories: Array<{name: string, time: number, percent: number}>;
    timestamp: number;
}

/**
 * Performance Visualizer
 */
export class PerformanceVisualizer implements vscode.Disposable {
    private webviewPanel: vscode.WebviewPanel | undefined;
    private mmemuDebugger: MmemuDebugger;
    private performanceData: PerformanceData[] = [];
    private disposables: vscode.Disposable[] = [];

    constructor(mmemuDebugger: MmemuDebugger) {
        this.mmemuDebugger = mmemuDebugger;
        this.setupCommands();
    }

    /**
     * Setup commands
     */
    private setupCommands(): void {
        const showCmd = vscode.commands.registerCommand(
            'mmemu.showPerformanceProfile',
            () => this.show()
        );
        this.disposables.push(showCmd);

        const clearCmd = vscode.commands.registerCommand(
            'mmemu.clearPerformanceData',
            () => this.clearData()
        );
        this.disposables.push(clearCmd);

        const exportCmd = vscode.commands.registerCommand(
            'mmemu.exportPerformanceData',
            () => this.exportData()
        );
        this.disposables.push(exportCmd);
    }

    /**
     * Show performance visualizer
     */
    show(): void {
        if (!this.webviewPanel) {
            this.webviewPanel = vscode.window.createWebviewPanel(
                'mmemuPerformance',
                'mmemu Performance Profile',
                vscode.ViewColumn.Two,
                { enableScripts: true }
            );

            this.webviewPanel.onDidDispose(() => {
                this.webviewPanel = undefined;
            });
        }

        this.webviewPanel.webview.html = this.getWebviewContent();
        this.webviewPanel.reveal();
    }

    /**
     * Add performance data
     */
    addPerformanceData(data: PerformanceData): void {
        this.performanceData.push(data);

        if (this.webviewPanel) {
            this.webviewPanel.webview.html = this.getWebviewContent();
        }
    }

    /**
     * Get webview content
     */
    private getWebviewContent(): string {
        const latest = this.performanceData[this.performanceData.length - 1];

        if (!latest) {
            return this.getEmptyContent();
        }

        return `
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Performance Profile</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }

        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
            padding: 20px;
            background: var(--vscode-editor-background);
            color: var(--vscode-editor-foreground);
        }

        .container {
            max-width: 900px;
            margin: 0 auto;
        }

        h1 { font-size: 24px; margin-bottom: 10px; }
        .info { color: var(--vscode-descriptionForeground); margin-bottom: 20px; }

        .grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
            margin-bottom: 30px;
        }

        .chart-container {
            border: 1px solid var(--vscode-panel-border);
            border-radius: 4px;
            padding: 15px;
            background: var(--vscode-panel-background);
        }

        .chart-title {
            font-weight: 600;
            margin-bottom: 15px;
            font-size: 14px;
            color: var(--vscode-descriptionForeground);
        }

        .bar-chart {
            display: flex;
            flex-direction: column;
            gap: 10px;
        }

        .bar {
            display: flex;
            align-items: center;
            gap: 10px;
        }

        .bar-label {
            min-width: 120px;
            font-size: 12px;
            text-overflow: ellipsis;
            white-space: nowrap;
            overflow: hidden;
        }

        .bar-bg {
            flex: 1;
            height: 24px;
            background: var(--vscode-progressBar-background);
            border-radius: 2px;
            position: relative;
            overflow: hidden;
        }

        .bar-fill {
            height: 100%;
            background: #007acc;
            display: flex;
            align-items: center;
            justify-content: flex-end;
            padding-right: 5px;
            color: white;
            font-size: 11px;
            font-weight: 600;
        }

        .pie-chart {
            width: 200px;
            height: 200px;
            border-radius: 50%;
            display: flex;
            align-items: center;
            justify-content: center;
            background: conic-gradient(${this.generateConicGradient(latest.categories)});
            position: relative;
            margin: 0 auto;
        }

        .pie-legend {
            display: flex;
            flex-direction: column;
            gap: 8px;
            margin-top: 15px;
        }

        .legend-item {
            display: flex;
            align-items: center;
            gap: 8px;
            font-size: 12px;
        }

        .legend-color {
            width: 12px;
            height: 12px;
            border-radius: 2px;
        }

        .export-btn {
            background: var(--vscode-button-background);
            color: var(--vscode-button-foreground);
            border: none;
            padding: 8px 16px;
            border-radius: 4px;
            cursor: pointer;
            font-size: 12px;
        }

        .export-btn:hover {
            background: var(--vscode-button-hoverBackground);
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>Performance Profile</h1>
        <div class="info">
            <strong>Test:</strong> ${latest.test}<br>
            <strong>Total Time:</strong> ${latest.totalTime}ms
        </div>

        <div class="grid">
            <div class="chart-container">
                <div class="chart-title">Functions by Time</div>
                <div class="bar-chart">
                    ${latest.functions.map(f => `
                        <div class="bar">
                            <div class="bar-label">${f.name}</div>
                            <div class="bar-bg">
                                <div class="bar-fill" style="width: ${f.percent}%">
                                    ${f.percent > 5 ? f.percent + '%' : ''}
                                </div>
                            </div>
                            <div style="min-width: 40px; text-align: right; font-size: 12px;">${f.time}ms</div>
                        </div>
                    `).join('')}
                </div>
            </div>

            <div class="chart-container">
                <div class="chart-title">Categories</div>
                <div class="pie-chart"></div>
                <div class="pie-legend">
                    ${latest.categories.map((c, i) => `
                        <div class="legend-item">
                            <div class="legend-color" style="background: ${this.getCategoryColor(i)}"></div>
                            <span>${c.name}: ${c.time}ms (${c.percent}%)</span>
                        </div>
                    `).join('')}
                </div>
            </div>
        </div>

        <button class="export-btn">Export Data</button>
    </div>

    <script>
        const vscode = acquireVsCodeApi();
        document.querySelector('.export-btn').addEventListener('click', () => {
            vscode.postMessage({ command: 'export' });
        });
    </script>
</body>
</html>
        `;
    }

    /**
     * Get empty content
     */
    private getEmptyContent(): string {
        return `
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Performance Profile</title>
    <style>
        body {
            font-family: system-ui;
            padding: 20px;
            background: var(--vscode-editor-background);
            color: var(--vscode-editor-foreground);
            display: flex;
            align-items: center;
            justify-content: center;
            min-height: 100vh;
        }
        .message {
            text-align: center;
            color: var(--vscode-descriptionForeground);
        }
    </style>
</head>
<body>
    <div class="message">
        <h2>No Performance Data</h2>
        <p>Run tests to collect performance data</p>
    </div>
</body>
</html>
        `;
    }

    /**
     * Generate conic gradient for pie chart
     */
    private generateConicGradient(categories: Array<{name: string, time: number, percent: number}>): string {
        const colors = ['#007acc', '#20c997', '#fd7e14', '#dc3545'];
        let gradient = '';
        let start = 0;

        for (let i = 0; i < categories.length; i++) {
            const percent = categories[i].percent;
            const color = colors[i % colors.length];
            gradient += `${color} ${start}deg ${start + (percent * 3.6)}deg`;
            if (i < categories.length - 1) gradient += ', ';
            start += percent * 3.6;
        }

        return gradient;
    }

    /**
     * Get category color
     */
    private getCategoryColor(index: number): string {
        const colors = ['#007acc', '#20c997', '#fd7e14', '#dc3545', '#6610f2', '#e83e8c'];
        return colors[index % colors.length];
    }

    /**
     * Clear performance data
     */
    private clearData(): void {
        this.performanceData = [];
        if (this.webviewPanel) {
            this.webviewPanel.webview.html = this.getEmptyContent();
        }
    }

    /**
     * Export performance data
     */
    private async exportData(): Promise<void> {
        if (this.performanceData.length === 0) {
            vscode.window.showInformationMessage('No performance data to export');
            return;
        }

        // Export as JSON
        const json = JSON.stringify(this.performanceData, null, 2);
        const uri = await vscode.window.showSaveDialog({
            defaultUri: vscode.Uri.file('performance_data.json'),
            filters: {'JSON': ['json']},
        });

        if (uri) {
            await vscode.workspace.fs.writeFile(uri, Buffer.from(json));
            vscode.window.showInformationMessage('Performance data exported');
        }
    }

    /**
     * Dispose
     */
    dispose(): void {
        this.webviewPanel?.dispose();
        this.disposables.forEach(d => d.dispose());
    }
}
