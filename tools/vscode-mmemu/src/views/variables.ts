import * as vscode from "vscode";
import { MMemuClient } from "../client";

/**
 * Variables tree view provider.
 */
export class VariablesProvider implements vscode.TreeDataProvider<VariableItem> {
  private getClient: () => MMemuClient | undefined;
  private onDidChangeTreeData = new vscode.EventEmitter<VariableItem | undefined>();
  readonly onDidChangeTreeData = this.onDidChangeTreeData.event;

  constructor(getClient: () => MMemuClient | undefined) {
    this.getClient = getClient;
  }

  getTreeItem(element: VariableItem): vscode.TreeItem {
    return element;
  }

  async getChildren(element?: VariableItem): Promise<VariableItem[]> {
    const client = this.getClient();
    if (!client) {
      return [];
    }

    try {
      if (!element) {
        // Root level - list functions
        // For now, return empty (would need to analyze debug metadata)
        return [];
      } else {
        // List variables in function
        const variables = await client.listVariables(element.functionName);
        return variables.map(
          (v) =>
            new VariableItem(
              v.name,
              `${v.name}: ${v.type} @ $${v.offset.toString(16).padStart(4, "0")}`,
              vscode.TreeItemCollapsibleState.None,
              v.offset,
              v.type,
              v.scope
            )
        );
      }
    } catch (err) {
      console.error("Error loading variables:", err);
      return [];
    }
  }

  refresh(): void {
    this.onDidChangeTreeData.fire(undefined);
  }
}

/**
 * Variable tree item.
 */
class VariableItem extends vscode.TreeItem {
  functionName?: string;
  offset?: number;
  type?: string;
  scope?: string;

  constructor(
    label: string,
    description: string,
    collapsibleState: vscode.TreeItemCollapsibleState,
    offset?: number,
    type?: string,
    scope?: string
  ) {
    super(label, collapsibleState);
    this.description = description;
    this.offset = offset;
    this.type = type;
    this.scope = scope;
  }
}
