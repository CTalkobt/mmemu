import * as net from "net";

export interface MMemuClientOptions {
  host: string;
  port: number;
  timeout: number;
}

export interface RegisterState {
  A?: number;
  X?: number;
  Y?: number;
  SP?: number;
  PC?: number;
  P?: number;
}

export interface Variable {
  name: string;
  offset: number;
  size: number;
  type: string;
  scope: string;
}

export interface Breakpoint {
  addr: number;
  enabled: boolean;
}

/**
 * Client for communicating with mmemu serial monitor server.
 */
export class MMemuClient {
  private socket: net.Socket | undefined;
  private options: MMemuClientOptions;
  private buffer = "";

  constructor(options: MMemuClientOptions) {
    this.options = options;
  }

  /**
   * Establish connection to server.
   */
  private async connect(): Promise<void> {
    return new Promise((resolve, reject) => {
      this.socket = net.createConnection(this.options.port, this.options.host);

      this.socket.setTimeout(this.options.timeout);
      this.socket.on("connect", () => resolve());
      this.socket.on("error", (err) => reject(err));
      this.socket.on("timeout", () => {
        this.socket?.destroy();
        reject(new Error("Connection timeout"));
      });

      this.socket.on("data", (data) => {
        this.buffer += data.toString();
      });
    });
  }

  /**
   * Send command and get response.
   */
  private async sendCommand(cmd: string): Promise<string> {
    if (!this.socket) {
      await this.connect();
    }

    return new Promise((resolve, reject) => {
      if (!this.socket) {
        reject(new Error("Socket not connected"));
        return;
      }

      const timeout = setTimeout(() => {
        reject(new Error("Command timeout"));
      }, this.options.timeout);

      const onData = () => {
        if (this.buffer.includes("\n")) {
          clearTimeout(timeout);
          this.socket?.off("data", onData);
          const response = this.buffer.split("\n")[0];
          this.buffer = this.buffer.slice(response.length + 1);
          resolve(response);
        }
      };

      this.socket.on("data", onData);
      this.socket.write(cmd + "\n");
    });
  }

  /**
   * Read all CPU registers.
   */
  async readRegisters(): Promise<RegisterState> {
    const response = await this.sendCommand("R");
    return this.parseRegisters(response);
  }

  /**
   * Read memory.
   */
  async readMemory(addr: string | number, length: number = 256): Promise<Buffer> {
    const addrNum = typeof addr === "string" ? parseAddr(addr) : addr;
    const response = await this.sendCommand(`M ${addrNum.toString(16)}`);

    // Parse hex dump response
    const data: number[] = [];
    const lines = response.split("\n");

    for (const line of lines) {
      const parts = line.split(/\s+/);
      for (const part of parts) {
        if (/^[0-9A-F]{2}$/i.test(part)) {
          data.push(parseInt(part, 16));
          if (data.length >= length) break;
        }
      }
      if (data.length >= length) break;
    }

    return Buffer.from(data);
  }

  /**
   * Write memory.
   */
  async writeMemory(addr: string | number, data: Buffer): Promise<void> {
    const addrNum = typeof addr === "string" ? parseAddr(addr) : addr;

    for (let i = 0; i < data.length; i++) {
      const response = await this.sendCommand(
        `S ${(addrNum + i).toString(16)} ${data[i].toString(16)}`
      );
      if (response.toUpperCase().includes("ERROR")) {
        throw new Error(`Write failed at ${(addrNum + i).toString(16)}: ${response}`);
      }
    }
  }

  /**
   * Set program counter.
   */
  async setPC(addr: string | number): Promise<void> {
    const addrNum = typeof addr === "string" ? parseAddr(addr) : addr;
    await this.sendCommand(`G ${addrNum.toString(16)}`);
  }

  /**
   * Disassemble.
   */
  async disassemble(addr: string | number, count: number = 16): Promise<Array<[number, string]>> {
    const addrNum = typeof addr === "string" ? parseAddr(addr) : addr;
    const response = await this.sendCommand(`D ${addrNum.toString(16)} ${count}`);

    const instrs: Array<[number, string]> = [];
    for (const line of response.split("\n")) {
      const parts = line.trim().split(/\s+/);
      if (parts.length >= 2) {
        try {
          const addr = parseInt(parts[0], 16);
          const instr = parts.slice(1).join(" ");
          instrs.push([addr, instr]);
        } catch {
          // Skip parse errors
        }
      }
    }
    return instrs;
  }

  /**
   * Set breakpoint.
   */
  async setBreakpoint(addr: string | number): Promise<boolean> {
    const addrNum = typeof addr === "string" ? parseAddr(addr) : addr;
    const response = await this.sendCommand(`B ${addrNum.toString(16)}`);
    return !response.toUpperCase().includes("ERROR");
  }

  /**
   * List breakpoints.
   */
  async listBreakpoints(): Promise<Breakpoint[]> {
    const response = await this.sendCommand("B");
    const breakpoints: Breakpoint[] = [];

    for (const line of response.split("\n")) {
      const match = /\$([0-9A-F]+)/i.exec(line);
      if (match) {
        breakpoints.push({
          addr: parseInt(match[1], 16),
          enabled: !line.includes("disabled"),
        });
      }
    }

    return breakpoints;
  }

  /**
   * List variables for a function.
   */
  async listVariables(functionName?: string): Promise<Variable[]> {
    const response = await this.sendCommand(functionName ? `V ${functionName}` : "V");
    const variables: Variable[] = [];

    for (const line of response.split("\n")) {
      const match = /(\w+)\s+@([0-9A-F]+)\s+size=(\d+)\s+type=(\w+)\s+scope=(\w+)/i.exec(
        line
      );
      if (match) {
        variables.push({
          name: match[1],
          offset: parseInt(match[2], 16),
          size: parseInt(match[3]),
          type: match[4],
          scope: match[5],
        });
      }
    }

    return variables;
  }

  /**
   * Get help.
   */
  async getHelp(): Promise<string> {
    return this.sendCommand("?");
  }

  /**
   * Close connection.
   */
  close(): void {
    if (this.socket) {
      this.socket.destroy();
      this.socket = undefined;
    }
  }

  private parseRegisters(response: string): RegisterState {
    const regs: RegisterState = {};

    const patterns: Record<string, RegExp> = {
      A: /A=([0-9A-F]+)/i,
      X: /X=([0-9A-F]+)/i,
      Y: /Y=([0-9A-F]+)/i,
      SP: /SP=([0-9A-F]+)/i,
      PC: /PC=([0-9A-F]+)/i,
      P: /P=([0-9A-F]+)/i,
    };

    for (const [key, pattern] of Object.entries(patterns)) {
      const match = pattern.exec(response);
      if (match) {
        regs[key as keyof RegisterState] = parseInt(match[1], 16);
      }
    }

    return regs;
  }
}

function parseAddr(s: string): number {
  s = s.trim();
  if (s.startsWith("$")) return parseInt(s.slice(1), 16);
  if (s.startsWith("0x")) return parseInt(s.slice(2), 16);
  if (/^[0-9A-F]+$/i.test(s)) return parseInt(s, 16);
  return parseInt(s, 10);
}
