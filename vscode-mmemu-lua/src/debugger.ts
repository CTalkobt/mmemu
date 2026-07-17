import * as net from 'net';
import { EventEmitter } from 'events';

export interface Breakpoint {
    file: string;
    line: number;
    id?: string;
}

export interface RegisterState {
    A: number;
    X: number;
    Y: number;
    PC: number;
    SP: number;
    P: number;  // Processor status
}

export class MmemuDebugger extends EventEmitter {
    private socket: net.Socket | null = null;
    private host: string = 'localhost';
    private port: number = 9999;
    private breakpoints: Map<string, Breakpoint> = new Map();
    private responseHandlers: Map<string, (data: string) => void> = new Map();
    private lastResponse: string = '';

    async connect(host: string, port: number): Promise<void> {
        this.host = host;
        this.port = port;

        return new Promise((resolve, reject) => {
            this.socket = net.createConnection({ host, port }, () => {
                console.log(`Connected to mmemu at ${host}:${port}`);
                this.emit('connected');
                resolve();
            });

            this.socket.on('data', (data) => this.handleData(data.toString()));
            this.socket.on('error', (error) => {
                console.error(`Connection error: ${error}`);
                this.emit('error', error);
                reject(error);
            });
            this.socket.on('close', () => {
                console.log('Disconnected from mmemu');
                this.emit('disconnected');
            });

            setTimeout(() => reject(new Error('Connection timeout')), 5000);
        });
    }

    disconnect(): void {
        if (this.socket) {
            this.socket.destroy();
            this.socket = null;
        }
    }

    async runScript(scriptPath: string): Promise<void> {
        return this.executeCommand(`script run ${scriptPath}`);
    }

    async stepInto(): Promise<void> {
        return this.executeCommand('step');
    }

    async continue(): Promise<void> {
        return this.executeCommand('run');
    }

    async toggleBreakpoint(file: string, line: number): Promise<void> {
        const key = `${file}:${line}`;
        if (this.breakpoints.has(key)) {
            this.breakpoints.delete(key);
            // Remove breakpoint from mmemu
            await this.executeCommand(`break clear ${line}`);
        } else {
            const bp: Breakpoint = { file, line };
            this.breakpoints.set(key, bp);
            // Set breakpoint in mmemu (using Lua line number)
            await this.executeCommand(`break $${line.toString(16).toUpperCase()} action ""`);
        }
    }

    async inspectVariable(varName: string): Promise<string> {
        const response = await this.executeCommand(`script eval "return mmemu.hex(${varName} or 0)"`);
        return response.trim();
    }

    async getRegisters(): Promise<RegisterState> {
        const response = await this.executeCommand('r');
        // Parse response like: "A: $00  X: $00  Y: $00  SP: $FD  PC: $FCE2  P: $24"
        const regs: Partial<RegisterState> = {};

        const aMatch = response.match(/A:\s*\$([0-9A-Fa-f]+)/);
        const xMatch = response.match(/X:\s*\$([0-9A-Fa-f]+)/);
        const yMatch = response.match(/Y:\s*\$([0-9A-Fa-f]+)/);
        const pcMatch = response.match(/PC:\s*\$([0-9A-Fa-f]+)/);
        const spMatch = response.match(/SP:\s*\$([0-9A-Fa-f]+)/);
        const pMatch = response.match(/P:\s*\$([0-9A-Fa-f]+)/);

        if (aMatch) regs.A = parseInt(aMatch[1], 16);
        if (xMatch) regs.X = parseInt(xMatch[1], 16);
        if (yMatch) regs.Y = parseInt(yMatch[1], 16);
        if (pcMatch) regs.PC = parseInt(pcMatch[1], 16);
        if (spMatch) regs.SP = parseInt(spMatch[1], 16);
        if (pMatch) regs.P = parseInt(pMatch[1], 16);

        return regs as RegisterState;
    }

    async readMemory(address: number, size: number): Promise<number[]> {
        const result: number[] = [];
        const addrHex = address.toString(16).toUpperCase().padStart(4, '0');

        for (let i = 0; i < size; i += 16) {
            const response = await this.executeCommand(`m $${addrHex}`);
            // Parse hex dump response
            const matches = response.match(/([0-9A-Fa-f]{2})/g);
            if (matches) {
                for (const match of matches) {
                    result.push(parseInt(match, 16));
                    if (result.length >= size) break;
                }
            }
            if (result.length >= size) break;
        }

        return result.slice(0, size);
    }

    private async executeCommand(command: string): Promise<string> {
        if (!this.socket) {
            throw new Error('Not connected to mmemu');
        }

        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                reject(new Error(`Command timeout: ${command}`));
            }, 5000);

            const handler = (data: string) => {
                clearTimeout(timeout);
                resolve(data);
            };

            this.responseHandlers.set(command, handler);
            this.socket!.write(`${command}\n`);
        });
    }

    private handleData(data: string): void {
        this.lastResponse += data;

        // Check for prompt (indicates command completion)
        if (this.lastResponse.includes('>')) {
            // Extract response (everything before the prompt)
            const response = this.lastResponse.split('>')[0];
            this.lastResponse = '';

            // Try to match with pending handler
            for (const [cmd, handler] of this.responseHandlers.entries()) {
                handler(response);
                this.responseHandlers.delete(cmd);
                break;
            }

            // Check for breakpoint notification
            if (response.includes('Break') || response.includes('breakpoint')) {
                const lineMatch = response.match(/line\s+(\d+)/i);
                if (lineMatch) {
                    const line = parseInt(lineMatch[1], 10);
                    this.emit('breakpoint', line);
                }
            }
        }
    }
}
