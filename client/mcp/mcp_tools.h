#pragma once
// The tools of the debug MCP server (ADR 0032 D3): app, plugins, game. Each group registers its tools in the registry; the
// handlers reach the app through the PlayerController on the GUI thread (plugin work is queued onto the plugin thread, game
// work onto the emulation thread, never touched from here). PR 2 adds the UI-control group the same way.
#include "mcp_registry.h"

namespace uzume::ui {
class PlayerController;
}

namespace uzume::mcp {

// Everything the debug server offers: installAppTools + installPluginTools + installGameTools.
void installTools(McpToolRegistry& registry, ui::PlayerController& player);

void installAppTools(McpToolRegistry& registry, ui::PlayerController& player);
void installPluginTools(McpToolRegistry& registry, ui::PlayerController& player);
void installGameTools(McpToolRegistry& registry, ui::PlayerController& player);

}  // namespace uzume::mcp
