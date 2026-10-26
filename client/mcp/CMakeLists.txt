# Debug MCP server (ADR 0032): uzume_mcp = HTTP/JSON-RPC server and tool registry (Qt only); uzume_mcp_tools = the tools of
# the app, plugins and the running game (needs uzume_ui, which owns the McpController and therefore links uzume_mcp).
add_library(uzume_mcp STATIC mcp_registry.h mcp_registry.cpp mcp_server.h mcp_server.cpp)
target_include_directories(uzume_mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uzume_mcp PUBLIC cxx_std_20)
target_link_libraries(uzume_mcp PUBLIC Qt6::Core Qt6::Network)
uzume_warnings(uzume_mcp)

add_library(uzume_mcp_tools STATIC mcp_tools.h tools.cpp tools_util.h tools_util.cpp tools_app.cpp tools_plugins.cpp tools_game.cpp tools_ui.cpp tools_reports.cpp)
target_link_libraries(uzume_mcp_tools PUBLIC uzume_mcp uzume_ui Qt6::Quick Qt6::Concurrent)
uzume_warnings(uzume_mcp_tools)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
