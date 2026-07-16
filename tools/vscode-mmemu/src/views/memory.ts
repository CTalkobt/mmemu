import * as vscode from "vscode";
import { MMemuClient } from "../client";

/**
 * Memory tree view provider.
 */
export class MemoryProvider implements vscode.TreeDataProvider<MemoryItem> {
  private getClient: () => MMemuClient | undefined;
  private onDidChangeTreeData = new vscode.EventEmitter<MemoryItem | undefined>();
  readonly onDidChangeTreeData = this.onDidChangeTreeData.event;

  private watchedRegions: MemoryRegion[] = [
    { name: "Zero Page", start: 0x0000, size: 0x0100 },
    { name: "Stack", start: 0x0100, size: 0x0100 },
    { name: "User Program", start: 0x0800, size: 0x7800 },
    { name: "I/O Registers", start: 0xd000, size: 0x1000 },
  ];

  constructor(getClient: () => MMemuClient | undefined) {
    this.getClient = getClient;
  }

  getTreeItem(element: MemoryItem): vscode.TreeItem {
    return element;
  }

  async getChildren(element?: MemoryItem): Promise<MemoryItem[]> {
    const client = this.getClient();
    if (!client) {
      return [];
    }

    try {
      if (!element) {
        // Root level - list watched regions
        return this.watchedRegions.map(
          (r) =>
            new MemoryItem(
              r.name,
              `$${r.start.toString(16).padStart(4, "0")}-$${(
                r.start +
                r.size -
                1
              )
                .toString(16)
                .padStart(4, "0")}`,
              vscode.TreeItemCollapsibleState.Collapsed,
              r.start,
              r.size
            )
        );
      } else {
        // Show memory contents
        try {
          const data = await client.readMemory(element.address!, Math.min(element.size!, 256));
          const items: MemoryItem[] = [];

          for (let i = 0; i < data.length; i += 16) {
            const chunk = data.slice(i, Math.min(i + 16, data.length));
            const hex = Array.from(chunk)
              .map((b) => b.toString(16).padStart(2, "0"))
              .join(" ");
            const addr = element.address! + i;

            items.push(
              new MemoryItem(
                `$${addr.toString(16).padStart(4, "0")}`,
                hex,
                vscode.TreeItemCollapsibleState.None,
                addr,
                chunk.length
              )
            );
          }

          return items;
        } catch (err) {
          return [];
        }
      }
    } catch (err) {
      console.error("Error loading memory:", err);
      return [];
    }
  }

  refresh(): void {
    this.onDidChangeTreeData.fire(undefined);
  }
}

/**
 * Memory region.
 */
interface MemoryRegion {
  name: string;
  start: number;
  size: number;
}

/**
 * Memory tree item.
 */
class MemoryItem extends vscode.TreeItem {
  address?: number;
  size?: number;

  constructor(
    label: string,
    description: string,
    collapsibleState: vscode.TreeItemCollapsibleState,
    address?: number,
    size?: number
  ) {
    super(label, collapsibleState);
    this.description = description;
    this.address = address;
    this.size = size;
    this.contextValue = "memory";
  }
}
