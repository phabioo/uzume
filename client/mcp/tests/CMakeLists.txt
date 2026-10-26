# MCP tests (QtTest, offscreen): the server over a real socket, and the tools against plugins and the Uzume test core.
get_filename_component(FB_ROM_DIR "${CMAKE_BINARY_DIR}/emulation" ABSOLUTE)

uzume_add_test(mcp_server mcp_server_test.cpp LIBS uzume_mcp Qt6::Network)
target_include_directories(test_mcp_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

uzume_add_test(mcp_tools_testcore mcp_tools_test.cpp LIBS uzume_mcp_tools uzume_ui uzume_core uzume_emulation uzume_plugins TEST_CORE)
target_include_directories(test_mcp_tools_testcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_mcp_tools_testcore PRIVATE FB_TEST_ROM_PATH="${FB_ROM_DIR}/uzume_test.nds")
add_dependencies(test_mcp_tools_testcore uzume_test_rom)
target_include_directories(test_mcp_tools_testcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../emulation/tests)

# UI-control tools against the real Main.qml offscreen and a fake hub (no core needed).
get_filename_component(FB_NETTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../network/tests" ABSOLUTE)
get_filename_component(FB_UITEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ui/tests" ABSOLUTE)
uzume_add_test(mcp_ui mcp_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS uzume_mcp_tools uzume_ui uzume_uiplugin uzume_core uzume_emulation Qt6::Quick Qt6::QuickControls2 Qt6::QuickTest)
target_include_directories(test_mcp_ui PRIVATE ${FB_NETTEST_DIR} ${FB_UITEST_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_mcp_ui PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")
