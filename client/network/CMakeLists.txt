# Network sources of the core (hub client, signaling later). Belongs to the uzume_core target.
target_sources(uzume_core PRIVATE
  hubprotocol.cpp
  hubhttp.cpp
  hubconnection.cpp
  hublibrary.cpp
  romdownloader.cpp
  saveapi.cpp
  stateapi.cpp
  savesync.cpp
  sessiontypes.cpp
  sessionapi.cpp
  hubsocket.cpp
  hubsystems.cpp
  firmwareprovisioner.cpp
  coreprovisioner.cpp
  toolsclient.cpp
  variantsclient.cpp
  coreupdater.cpp
  corechoice.cpp
  gameuploader.cpp
  romregistrar.cpp
  romsource.cpp
  pluginhub.cpp
  runtypes.cpp
  runclient.cpp
  runrecap.cpp
  connectioncheck.cpp
  connectionprobes.cpp
  reportuploader.cpp
  accountapi.cpp
  adminapi.cpp
  personalapi.cpp
  screenshotapi.cpp
  screenshotqueue.cpp
  discordpresence.cpp
  chatapi.cpp
  errorsender.cpp
  githubissues.cpp
)
target_link_libraries(uzume_core PUBLIC Qt6::WebSockets)
target_include_directories(uzume_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Developer tool against a real hub (pairing, library, saves, Sessions: session-share / session-watch).
add_executable(uzume_cli cli/main.cpp cli/sessioncommands.cpp cli/sessioncommands.h)
target_link_libraries(uzume_cli PRIVATE uzume_core uzume_media)
uzume_warnings(uzume_cli)
uzume_vcpkg_runtime(uzume_cli)
set_target_properties(uzume_cli PROPERTIES OUTPUT_NAME uzume-cli)

set(FB_TESTDATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata")
set(FB_FAKEHUB tests/fakehub.cpp tests/fakehub.h)

uzume_add_test(network_hub_flow tests/hub_flow_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_library_rom tests/library_rom_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_savesync tests/savesync_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_sessions tests/sessions_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_hub_features tests/hub_features_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_coreprovisioner tests/coreprovisioner_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_coreupdater tests/coreupdater_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_pluginhub tests/pluginhub_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_pluginstore_sync tests/pluginstore_sync_test.cpp ${FB_FAKEHUB} LIBS uzume_core uzume_plugins)
uzume_add_test(network_state_api tests/state_api_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_runs tests/runclient_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_recap tests/runrecap_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_report_upload tests/report_upload_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_errors tests/errors_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_local_roms tests/local_roms_test.cpp ${FB_FAKEHUB} LIBS uzume_core uzume_emulation)
uzume_add_test(network_accounts tests/accounts_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_personal tests/personal_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_screenshots tests/screenshots_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_tools_variants tests/tools_variants_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
uzume_add_test(network_corechoice tests/corechoice_test.cpp LIBS uzume_core)
uzume_add_test(network_connectioncheck tests/connectioncheck_test.cpp LIBS uzume_core)
uzume_add_test(network_connectionprobes tests/connectionprobes_test.cpp ${FB_FAKEHUB} LIBS uzume_core)
if(BUILD_TESTING)
  foreach(t network_hub_flow network_library_rom network_savesync network_sessions network_hub_features network_coreprovisioner network_tools_variants network_coreupdater network_pluginhub network_pluginstore_sync network_state_api network_runs network_recap network_personal network_screenshots network_local_roms network_report_upload network_connectionprobes network_accounts network_errors)
    target_compile_definitions(test_${t} PRIVATE FB_TEST_DATA_DIR="${FB_TESTDATA_DIR}")
  endforeach()
  # The longest test (about 110 s on Windows CI) starts first under ctest -j: a fresh CI build has no cost data.
  set_tests_properties(network_savesync PROPERTIES COST 1000)
endif()
uzume_add_test(network_discord tests/discord_presence_test.cpp LIBS uzume_core)
