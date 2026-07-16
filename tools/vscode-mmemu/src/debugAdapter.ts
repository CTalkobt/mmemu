import * as vscode from "vscode";

/**
 * Factory for MEGA65 debug adapter.
 */
export class MMemuDebugAdapterDescriptorFactory
  implements vscode.DebugAdapterDescriptorFactory
{
  createDebugAdapterDescriptor(
    session: vscode.DebugSession,
    _executable: vscode.DebugAdapterExecutable | undefined
  ): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
    // For now, return a simple implementation
    // In a full implementation, this would spawn a debug adapter process

    const config = session.configuration;
    const host = config.host || "localhost";
    const port = config.port || 6502;

    // Create a socket descriptor to connect directly
    return new vscode.DebugAdapterServer(port, host);
  }
}
