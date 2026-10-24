#pragma once

// Crash reporting for the bridge: AICrash.log in the game's Logs folder, for exceptions this DLL is involved in.
void McpInstallCrashHandler();
// Must run before the DLL unloads: the handler is registered process-wide.
void McpUninstallCrashHandler();
// Named in the crash log as what was running; "" clears it.
void McpCrashSetContext(const char* text);
