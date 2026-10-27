# uzume_plugins: plugin runtime of the Player (docs/architecture/12-plugins.md 16.3-16.7, ADR 0023):
# manifests, plugin manager, Lua sandbox and plugin host on a dedicated plugin thread. No UI, no Hub.

# Vendored Lua 5.4.7, compiled as C++ (lua_error unwinds with C++ exceptions, destructors run). No warning flags.
set(FB_LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.4.7)
set(FB_LUA_SOURCES
  lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmathlib.c
  lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c
  lvm.c lzio.c)
list(TRANSFORM FB_LUA_SOURCES PREPEND ${FB_LUA_DIR}/)
add_library(uzume_lua STATIC ${FB_LUA_SOURCES})
set_source_files_properties(${FB_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
target_include_directories(uzume_lua PUBLIC ${FB_LUA_DIR})
target_compile_features(uzume_lua PUBLIC cxx_std_20)
if(MSVC)
  target_compile_definitions(uzume_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(uzume_lua PRIVATE /w)
else()
  target_compile_options(uzume_lua PRIVATE -w)
endif()
set_target_properties(uzume_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(uzume_plugins STATIC manifest.cpp plugin_value.cpp lua_sandbox.cpp plugin_engine.cpp plugin_ui.cpp plugin_api.cpp
  plugin_host.cpp plugin_manager.cpp fbplugin.cpp plugin_delivery.cpp run_bridge.cpp run_session_bridge.cpp plugin_language.cpp plugin_variants.cpp)
target_include_directories(uzume_plugins PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uzume_plugins PUBLIC cxx_std_20)
target_link_libraries(uzume_plugins PUBLIC uzume_lua uzume_core uzume_formats Qt6::Core)
uzume_warnings(uzume_plugins)

uzume_add_test(plugins_manifest tests/manifest_test.cpp LIBS uzume_plugins)
target_compile_definitions(test_plugins_manifest PRIVATE FB_MANIFEST_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/../../server/internal/pluginpkg/testdata/manifests")  # shared with the Hub
uzume_add_test(plugins_sandbox tests/sandbox_test.cpp LIBS uzume_plugins)
uzume_add_test(plugins_manager tests/manager_test.cpp LIBS uzume_plugins)
uzume_add_test(plugins_host tests/host_test.cpp LIBS uzume_plugins)
uzume_add_test(plugins_engine_budget tests/engine_budget_test.cpp LIBS uzume_plugins)
uzume_add_test(plugins_ui tests/ui_test.cpp LIBS uzume_plugins)
uzume_add_test(plugins_video tests/video_test.cpp LIBS uzume_plugins)
uzume_add_test(plugins_language tests/language_test.cpp LIBS uzume_plugins)
uzume_add_test(plugins_variants tests/variants_test.cpp LIBS uzume_plugins)

# .fbplugin reading and signature, Hub delivery (against the fake Hub of network/tests).
uzume_add_test(plugins_fbplugin tests/fbplugin_test.cpp LIBS uzume_plugins)
target_compile_definitions(test_plugins_fbplugin PRIVATE FB_PLUGIN_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata")
get_filename_component(FB_NETTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../network/tests" ABSOLUTE)
uzume_add_test(plugins_delivery tests/delivery_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h LIBS uzume_plugins)
target_include_directories(test_plugins_delivery PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_plugins_delivery PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# fb.run against the real RunClient + RunSessionBridge + fake Hub.
uzume_add_test(plugins_run tests/run_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h LIBS uzume_plugins)
target_include_directories(test_plugins_run PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_plugins_run PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")
