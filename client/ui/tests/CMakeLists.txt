# UI tests (QtTest, offscreen). Source of the fake hub helper: network/tests (test code only).
set(FB_UI_TEST_LIBS uzume_ui uzume_uiplugin uzume_core uzume_emulation Qt6::Quick Qt6::QuickControls2 Qt6::QuickTest)
get_filename_component(FB_NETTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../network/tests" ABSOLUTE)
get_filename_component(FB_ROM_DIR "${CMAKE_BINARY_DIR}/emulation" ABSOLUTE)

uzume_add_test(ui_input input_test.cpp LIBS uzume_ui uzume_core uzume_emulation)
uzume_add_test(ui_library_sort library_sort_test.cpp LIBS uzume_ui uzume_core uzume_emulation)
uzume_add_test(ui_audio audio_test.cpp LIBS uzume_ui uzume_core uzume_emulation)

# Live save = full game restart (GameSession::restartWithSave) with the fake libretro core from emulation/tests.
uzume_add_test(ui_live_restart live_restart_test.cpp LIBS uzume_ui uzume_core uzume_emulation)
target_compile_definitions(test_ui_live_restart PRIVATE FB_FAKE_CORE_PATH="$<TARGET_FILE:uzume_fake_core>")
add_dependencies(test_ui_live_restart uzume_fake_core)

uzume_add_test(ui_screens screens_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_screens PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_screens PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Start screen, switch user and My account (0.18 package 7) against the FakeHub.
uzume_add_test(ui_start_screens start_screens_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_start_screens PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_start_screens PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Game view with a real core and the homebrew test ROM (SKIP/77 without UZUME_MELONDS_DS_CORE).
uzume_add_test(ui_game game_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS} NEEDS_CORE)
target_include_directories(test_ui_game PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_game PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata"
  FB_TEST_ROM_PATH="${FB_ROM_DIR}/uzume_test.nds")
add_dependencies(test_ui_game uzume_test_rom)

# Save sync with a real core: conflict dialog (3d), decide later / use Hub version.
uzume_add_test(ui_save save_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS} NEEDS_CORE)
target_include_directories(test_ui_save PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_save PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata"
  FB_TEST_ROM_PATH="${FB_ROM_DIR}/uzume_test.nds")
add_dependencies(test_ui_save uzume_test_rom)

# sessions in the library, Session panel, multiview, diagnostics (FakeHub incl. WSS). Tests with a running
# game need the core (they QSKIP without it, the rest runs).
uzume_add_test(ui_sessions sessions_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS} NEEDS_CORE)
target_include_directories(test_ui_sessions PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_sessions PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata"
  FB_TEST_ROM_PATH="${FB_ROM_DIR}/uzume_test.nds")
add_dependencies(test_ui_sessions uzume_test_rom)

# invite redemption, user_disabled, upload, firmware state/materialization, core warnings, Settings (no core needed).
uzume_add_test(ui_hub_features hub_features_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_hub_features PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_hub_features PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Connection check dialog (6e): opened from Settings, the four outcomes, keys, Copy / Add to problem report / Edit Hub address….
# Scripted probes through PlayerController::Options::connectionProbes (no network); FakeHub for the paired Hub profile.
uzume_add_test(ui_connection_check connection_check_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_connection_check PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_connection_check PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# "Report a problem" (0.17): dialog state machine, attachments, upload against the FakeHub, Save as file (no core needed).
uzume_add_test(ui_report report_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_report PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_report PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Admin area (0.18 package 8): SERVER group in AppNav, server screen and Status page, the admin request helper with
# "Confirm it's you" (park, ask, replay, cancel, lock), downloads and uploads, UzTable / UzMetric; FakeHub with admin_v1.
uzume_add_test(ui_admin admin_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_admin PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_admin PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Users (People, Invites, Devices) and Reports pages of the admin area (0.18 package 8) against FakeHub's admin directory fixtures.
uzume_add_test(ui_admin_users admin_users_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_admin_users PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_admin_users PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Admin area, Systems & cores and server Plugins pages (0.18 package 8): "No core", Use this, switching, automatic update, firmware,
# plugin index, upload -> stage -> offer, remove; FakeHub with fixed admin answers.
uzume_add_test(ui_admin_systems admin_systems_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_admin_systems PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_admin_systems PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Server settings (0.18 package 8, 3q-app-1 to 3q-app-7): Settings for "Server", the seven pages, restart line, backups with polling,
# restore / rollback through Confirm it's you, metrics token shown once, diagnostics bundle download.
uzume_add_test(ui_admin_settings admin_settings_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_admin_settings PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_admin_settings PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Admin area, rest (0.18 package 8): Library and Saves admin parts, Runs actions, What's new, setup assistant, server problems and the parity
# edits of the Users and Systems pages; FakeHub with fixed answers for the admin routes.
uzume_add_test(ui_admin_library admin_library_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_admin_library PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_admin_library PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# SettingsRow in isolation (grid, dot slot, More / Less, segment -> select, disabled reason, narrow); no hub, no core.
uzume_add_test(ui_settings_row settings_row_ui_test.cpp LIBS ${FB_UI_TEST_LIBS})

# Design system blocks (0.18 package 3): every Uz* block in Sumi and Kinari, token facts, focus ring, keyboard behaviour,
# dialog busy state, popover closing; no hub, no core.
uzume_add_test(ui_design_system design_system_ui_test.cpp LIBS ${FB_UI_TEST_LIBS})

# Emulation page (3e, settings hierarchy, core options) and Controllers page (3f) with SDL virtual joysticks.
# The test with the real core QSKIPs without UZUME_MELONDS_DS_CORE.
uzume_add_test(ui_emulation_controllers emulation_controllers_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS} uzume_input uzume_sdl3 NEEDS_CORE)
target_include_directories(test_ui_emulation_controllers PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_emulation_controllers PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Controllers glyph sets, Button labels (Auto + override per device), drawn arrows, input test grid and layout at 1280 / 960
# (SDL virtual joysticks, no core needed).
uzume_add_test(ui_controllers_glyphs controllers_glyphs_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS} uzume_input uzume_sdl3)
target_include_directories(test_ui_controllers_glyphs PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_controllers_glyphs PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")
# Same test at devicePixelRatio 2 (icons are painted in device pixels; the arrows must stay equal). Own executable
# (same source) so the CI rerun step finds test_<ctest name>.exe.
uzume_add_test(ui_controllers_glyphs_hidpi controllers_glyphs_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS} uzume_input uzume_sdl3)
target_include_directories(test_ui_controllers_glyphs_hidpi PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_controllers_glyphs_hidpi PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")
set_tests_properties(ui_controllers_glyphs_hidpi PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=2")

# Save history, restore, snapshots, slot picker in the detail pane (no core needed).
uzume_add_test(ui_save_history save_history_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_save_history PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_save_history PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Library revision 3c-2..3c-4: sort, Ready first, SAVE summary, saves view, upload confirmation, column layout (no core needed).
uzume_add_test(ui_library_saves library_saves_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_library_saves PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_library_saves PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Library at scale: 500/1000/2000 games, load and reload with sync states stay fast (generous bound, no core needed).
uzume_add_test(ui_library_scale library_scale_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_library_scale PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_library_scale PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Screenshots of the out-of-game screens (design v4); skipped without UZUME_SCREENSHOT_DIR.
uzume_add_test(ui_screenshots_shell screenshots_shell_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_screenshots_shell PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_screenshots_shell PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Round 3 Player A artboards (3a-3, 3c-6, 3c-7, 3p-3); skipped without UZUME_SCREENSHOT_DIR.
uzume_add_test(ui_screenshots_player_a screenshots_player_a_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_screenshots_player_a PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_screenshots_player_a PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# First-load layout of the shell (round 3 P8, P9): no overlap of the Library header and the sidebar user block.
uzume_add_test(ui_shell_layout shell_layout_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_shell_layout PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_shell_layout PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Screen layouts (0.6 D12) and the diagnostics model (0.6 D5-D11): pure logic, no Qt Quick.
uzume_add_test(ui_screenlayout screenlayout_test.cpp LIBS uzume_ui uzume_core uzume_emulation)
uzume_add_test(ui_diagnostics diagnostics_test.cpp LIBS uzume_ui uzume_core uzume_emulation)

# GPU-direct encoding of the own Session (ADR 0019): GpuEncodeBridge and GameSession on the fake hardware core from
# emulation/tests and the fake CUDA API (testutil/fake_cuda.h); needs a GL 3.3 context (SKIP/77 without one, except on
# Linux CI where it must run). Same wiring as emulation_hw_render for FB_FAKE_HW_CORE_PATH.
uzume_add_test(ui_gpu_encode gpu_encode_test.cpp LIBS uzume_ui uzume_core uzume_emulation uzume_media Qt6::Gui)
target_compile_definitions(test_ui_gpu_encode PRIVATE FB_FAKE_HW_CORE_PATH="$<TARGET_FILE:uzume_fake_hw_core>")
add_dependencies(test_ui_gpu_encode uzume_fake_hw_core)
set_tests_properties(ui_gpu_encode PROPERTIES SKIP_RETURN_CODE 77)
# Backend choice of GpuEncodeBridge (ADR 0027 D6) with fake captures: pure, no GL.
uzume_add_test(ui_gpu_encode_select gpu_encode_select_test.cpp LIBS uzume_ui uzume_core uzume_emulation uzume_media)

# Zero-copy display (ADR 0027): GameSession + GameView with a fake platform display on the fake hardware core and the
# software scene graph; needs a GL 3.3 context (SKIP/77 without one, except on Linux CI where it must run).
uzume_add_test(ui_gpu_display gpu_display_test.cpp LIBS uzume_ui uzume_core uzume_emulation uzume_media Qt6::Gui Qt6::Quick)
target_compile_definitions(test_ui_gpu_display PRIVATE FB_FAKE_HW_CORE_PATH="$<TARGET_FILE:uzume_fake_hw_core>")
add_dependencies(test_ui_gpu_display uzume_fake_hw_core)
set_tests_properties(ui_gpu_display PROPERTIES SKIP_RETURN_CODE 77)

# Screenshots of the game view with fake data (0.6): QSKIP unless UZUME_SCREENSHOT_DIR is set; no core needed.
uzume_add_test(ui_screenshots_game screenshots_game_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_screenshots_game PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_screenshots_game PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Speed-up button, speed select, Space (preview game, no core).
uzume_add_test(ui_fast_forward fast_forward_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_fast_forward PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_fast_forward PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# "← Library" keeps the game loaded in the background: strip, Resume/Quit, confirmation, input (preview game, no core).
uzume_add_test(ui_background_game background_game_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_background_game PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_background_game PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# In-game view revision (0.7.x, 3g-2/3r-2/3t-2/3x-2): header zones, game controls, Reset popover, one diagnostics toggle and
# its overlay position, panel following the tile selection, picker anchoring, "Now running" strip in the sidebar (preview game, no core).
uzume_add_test(ui_ingame ingame_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_ingame PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_ingame PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# The four tests that need a core, once more against the in-repo Uzume test core (emulation/tests/test_core.cpp, flavor
# melondsds: option keys of the product profile). Same sources, own executables (the CI rerun step looks up
# test_<ctest name>); they run the game / Session / save / options cases instead of SKIPping, so merge-gating CI needs no
# real emulator. The NEEDS_CORE originals above stay for the nightly run with real cores.
foreach(t IN ITEMS game save sessions emulation_controllers)
  set(extra_libs "")
  if(t STREQUAL "game")
    set(src game_test.cpp)
  elseif(t STREQUAL "save")
    set(src save_ui_test.cpp)
  elseif(t STREQUAL "sessions")
    set(src sessions_ui_test.cpp)
  else()
    set(src emulation_controllers_test.cpp)
    set(extra_libs uzume_input uzume_sdl3)
  endif()
  uzume_add_test(ui_${t}_testcore ${src} ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
    LIBS ${FB_UI_TEST_LIBS} ${extra_libs} TEST_CORE)
  target_include_directories(test_ui_${t}_testcore PRIVATE ${FB_NETTEST_DIR})
  target_compile_definitions(test_ui_${t}_testcore PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")
  if(NOT t STREQUAL "emulation_controllers")
    target_compile_definitions(test_ui_${t}_testcore PRIVATE FB_TEST_ROM_PATH="${FB_ROM_DIR}/uzume_test.nds")
    add_dependencies(test_ui_${t}_testcore uzume_test_rom)
  endif()
endforeach()

# Plugins in the Player UI: Settings > Plugins and the Plugin console tab (preview game, no core). With
# UZUME_SCREENSHOT_DIR it also saves PNGs for the review.
uzume_add_test(ui_plugins plugins_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_plugins PRIVATE ${FB_NETTEST_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/tests)
target_compile_definitions(test_ui_plugins PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Plugin UI slots (sidebar entry + screen, Settings "Adds", panel tabs, HUD, notices, tile strips) in the preview game.
# With UZUME_SCREENSHOT_DIR it also saves PNGs for the review.
uzume_add_test(ui_plugin_slots plugin_slots_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_plugin_slots PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_plugin_slots PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Plugin hotkeys in Controllers > Hotkeys (groups, conflicts, capture, persistence).
uzume_add_test(ui_plugin_hotkeys plugin_hotkeys_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_plugin_hotkeys PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_plugin_hotkeys PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Plugin game detail section + "Plugins" status row in the Library detail column (real Lua plugin in the shell context).
uzume_add_test(ui_plugin_detail plugin_detail_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_plugin_detail PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_plugin_detail PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Plugin UI building blocks (PluginBlock / PluginMapView) in isolation: every block dark and light, actions, confirmation,
# map selection / filter / clusters / keyboard. With UZUME_SCREENSHOT_DIR it also saves PNGs for the review.
uzume_add_test(ui_plugin_blocks plugin_blocks_ui_test.cpp LIBS ${FB_UI_TEST_LIBS})

# Plugins end to end against the Uzume test core: EmulationRunner + memory adapter + PluginHost + fb.codec.pk6_decrypt.
uzume_add_test(plugins_e2e_testcore plugins_e2e_test.cpp LIBS uzume_ui uzume_core uzume_emulation uzume_plugins TEST_CORE)
target_compile_definitions(test_plugins_e2e_testcore PRIVATE FB_TEST_ROM_PATH="${FB_ROM_DIR}/uzume_test.nds")
add_dependencies(test_plugins_e2e_testcore uzume_test_rom)
target_include_directories(test_plugins_e2e_testcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../emulation/tests)

# Plugin UI runtime in the PluginsController (shell context, models, actions, toasts) and the image://fbplugin rules.
uzume_add_test(ui_plugins_runtime plugins_runtime_ui_test.cpp LIBS uzume_ui uzume_core uzume_emulation uzume_plugins Qt6::Quick)
target_include_directories(test_ui_plugins_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/tests)

# Runs screen: sidebar entry, invite/join, detail, Start a run, Play run (fake Hub, synthetic plugin).
uzume_add_test(ui_runs runs_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_runs PRIVATE ${FB_NETTEST_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/tests)
target_compile_definitions(test_ui_runs PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Same layout tests with a noticeably wider application font (UZUME_TEST_WIDE_FONT, testsupport.h): catches items
# that only fit with Linux font metrics (Windows CI renders wider; 18 px letter spacing is about the Windows fallback font).
# Not on Windows: its fonts are already the wide case, extra spacing there tests something no user sees.
if(NOT WIN32)
  foreach(t ui_shell_layout ui_library_saves ui_library_overview ui_ingame ui_plugin_slots ui_settings_row ui_design_system ui_screens ui_plugins ui_connection_check ui_report ui_admin ui_admin_users ui_admin_systems ui_admin_settings ui_admin_library ui_runs)
    add_test(NAME ${t}_widefont COMMAND test_${t})
    set_tests_properties(${t}_widefont PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;UZUME_TEST_WIDE_FONT=18")
  endforeach()
  # The test-core variants keep their core environment (merge CI has no real core).
  foreach(t ui_sessions_testcore ui_emulation_controllers_testcore)
    get_test_property(${t} ENVIRONMENT core_env)
    add_test(NAME ${t}_widefont COMMAND test_${t})
    set_tests_properties(${t}_widefont PROPERTIES ENVIRONMENT "${core_env};UZUME_TEST_WIDE_FONT=18")
  endforeach()
endif()

# First start, join and set-up flow and the people on this PC (OnboardingController, AccountsController) against the fake
# server; no window.
uzume_add_test(ui_start_accounts start_accounts_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_start_accounts PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_start_accounts PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# The package 10 screens: "On this PC" view, Share with others dialogs, Stop sharing, ConnectionScreen child states.
uzume_add_test(ui_share share_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_share PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_share PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# "On this PC and sharing" (0.18 package 10): child server process, hidden admin, Share with others, Stop sharing; fake
# server script + FakeHub, no window.
uzume_add_test(ui_localserver localserver_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_localserver PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_localserver PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Favourites, collections, play time and last played in the Library and on the game page (Uzume 0.19, no core needed).
uzume_add_test(ui_personal personal_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_personal PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_personal PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# The readable Library: status chips with counts, List / Grid, one problem sentence, Remove with Undo (readable pass, PR 5).
uzume_add_test(ui_library_overview library_overview_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_library_overview PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_library_overview PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")

# Screenshots: the strip and viewer of the game page, the capture path with its pending queue (Uzume 0.19, no core needed).
uzume_add_test(ui_screenshots screenshots_ui_test.cpp ${FB_NETTEST_DIR}/fakehub.cpp ${FB_NETTEST_DIR}/fakehub.h
  LIBS ${FB_UI_TEST_LIBS})
target_include_directories(test_ui_screenshots PRIVATE ${FB_NETTEST_DIR})
target_compile_definitions(test_ui_screenshots PRIVATE FB_TEST_DATA_DIR="${FB_NETTEST_DIR}/testdata")
