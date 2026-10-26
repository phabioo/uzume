#pragma once
// The tools of the debug MCP server (ADR 0032 D3): app, plugins, game. Each group registers its tools in the registry; the
// handlers reach the app through the PlayerController on the GUI thread (plugin work is queued onto the plugin thread, game
// work onto the emulation thread, never touched from here). The UI-control group works on the GUI thread on the QML items.
#include "mcp_registry.h"

namespace uzume::ui {
class PlayerController;
}

namespace uzume::mcp {

// Everything the debug server offers: installAppTools + installPluginTools + installGameTools + installUiTools.
void installTools(McpToolRegistry& registry, ui::PlayerController& player);

void installAppTools(McpToolRegistry& registry, ui::PlayerController& player);
void installPluginTools(McpToolRegistry& registry, ui::PlayerController& player);
void installGameTools(McpToolRegistry& registry, ui::PlayerController& player);
// reports_list, report_get, report_file: read-only problem reports on the connected server (tools_reports.cpp).
void installReportTools(McpToolRegistry& registry, ui::PlayerController& player);
// ui_tree, ui_click, ui_type, ui_key, ui_scroll, navigate, game_start, game_stop, app_screenshot (tools_ui.cpp).
void installUiTools(McpToolRegistry& registry, ui::PlayerController& player);

}  // namespace uzume::mcp
