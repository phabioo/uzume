# MCP tests (QtTest, offscreen): the server over a real socket, and the tools against plugins and the Uzume test core.
get_filename_component(FB_ROM_DIR "${CMAKE_BINARY_DIR}/emulation" ABSOLUTE)

uzume_add_test(mcp_server mcp_server_test.cpp LIBS uzume_mcp Qt6::Network)
target_include_directories(test_mcp_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

uzume_add_test(mcp_tools_testcore mcp_tools_test.cpp LIBS uzume_mcp_tools uzume_ui uzume_core uzume_emulation uzume_plugins TEST_CORE)
target_include_directories(test_mcp_tools_testcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_mcp_tools_testcore PRIVATE FB_TEST_ROM_PATH="${FB_ROM_DIR}/uzume_test.nds")
add_dependencies(test_mcp_tools_testcore uzume_test_rom)
target_include_directories(test_mcp_tools_testcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../emulation/tests)
