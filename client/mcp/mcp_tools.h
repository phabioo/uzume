#pragma once
// The tools of the debug MCP server (ADR 0032 D3): app, plugins, game. Each group registers its tools in the registry; the
// handlers reach the app through the PlayerController on the GUI thread (plugin work is queued onto the plugin thread, game
// work onto the emulation thread, never touched from here). The UI-control group works on the GUI thread on the QML items.
#include <QJsonObject>
#include <QString>

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
// game_state_save, game_state_load, game_memory_write (tools_emulation.cpp): raw save states kept in memory (no server) and memory
// writes, both through the emulation command queue between two frames. Unlike the read-only game tools they change the game.
void installEmulationTools(McpToolRegistry& registry, ui::PlayerController& player);
// states_list, states_save, states_load, states_delete, rewind (tools_states.cpp, 0.20 package 4): the app's real save states and rewind.
void installStatesTools(McpToolRegistry& registry, ui::PlayerController& player);
// reports_list, report_get, report_file: read-only problem reports on the connected server (tools_reports.cpp).
void installReportTools(McpToolRegistry& registry, ui::PlayerController& player);
// ui_tree, ui_click, ui_type, ui_key, ui_scroll, navigate, game_start, game_stop, app_screenshot (tools_ui.cpp).
void installUiTools(McpToolRegistry& registry, ui::PlayerController& player);
// tools_list, tool_prepare, variants_list, variant_presets, variant_build, variant_status, variant_cancel, variant_delete, variant_files,
// variant_start, run_variant_build, plugin_install_file, plugin_set_enabled (tools_variants.cpp, Uzume 0.21).
void installVariantTools(McpToolRegistry& registry, ui::PlayerController& player);
// runs_list, run_set_rules, run_events, developer_mode, plugin_link, file_dialog_pick (tools_runs.cpp, Uzume 0.20 package 6).
void installRunTools(McpToolRegistry& registry, ui::PlayerController& player);

// run_recap (tools_runs_recap.cpp, Uzume 0.22): the computed run recap. validateRunRecapArgs is the pure part (tests).
void installRunRecapTools(McpToolRegistry& registry, ui::PlayerController& player);
QString validateRunRecapArgs(const QJsonObject& args);  // "" = fine, else the message

// server_sign_in (tools_together.cpp): add a server and sign in with a password; self-signed certificates only with a matching
// trust_fingerprint. The two helpers are the pure parts (tests).
void installTogetherTools(McpToolRegistry& registry, ui::PlayerController& player);
QString validateServerSignInArgs(const QJsonObject& args);  // "" = fine, else the message
bool serverFingerprintTrusted(const QString& observed, const QString& trusted);

}  // namespace uzume::mcp
