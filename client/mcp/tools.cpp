#include "mcp_tools.h"

namespace uzume::mcp {

void installTools(McpToolRegistry& registry, ui::PlayerController& player) {
  installAppTools(registry, player);
  installPluginTools(registry, player);
  installGameTools(registry, player);
  installReportTools(registry, player);
  installUiTools(registry, player);
  installVariantTools(registry, player);
}

}  // namespace uzume::mcp
