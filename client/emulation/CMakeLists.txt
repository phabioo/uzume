# uzume_emulation: EmulatorBackend, LibretroBackend (core loaded via QLibrary, no linking),
# EmulationRunner, system manifests (Qt resource) and CoreLocator.
add_library(uzume_emulation STATIC
  emulation_runner.cpp
  frame_stats.cpp
  core_locator.cpp
  libretro_backend.cpp
  system_manifest.cpp
  firmware_materializer.cpp
  core_options.cpp
  hw_render.cpp
  vk_render.cpp
  memory_map.cpp
  memory_snapshot.cpp
  game_identity.cpp)
# Systems and core profiles are data only (ADR 0020 D6): a new manifests/{systems,cores}/*.json needs no CMake edit.
file(GLOB FB_MANIFEST_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS manifests/systems/*.json manifests/cores/*.json)
qt_add_resources(uzume_emulation uzume_manifests
  PREFIX "/uzume/emulation"
  FILES ${FB_MANIFEST_FILES})
target_include_directories(uzume_emulation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(uzume_emulation SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libretro)
# Vulkan hardware rendering (ADR 0034): headers only, the loader is opened at runtime (QLibrary), nothing is linked.
# Linux: libvulkan-dev; Windows: vcpkg port vulkan-headers. Without them vk_render.cpp compiles to "unavailable".
find_path(UZUME_VULKAN_INCLUDE_DIR vulkan/vulkan.h)
set(UZUME_VULKAN_TEST_DEFS "")
set(UZUME_VULKAN_TEST_INCLUDES "")
if(UZUME_VULKAN_INCLUDE_DIR)
  message(STATUS "Vulkan headers: ${UZUME_VULKAN_INCLUDE_DIR} (loaded at runtime, not linked)")
  target_compile_definitions(uzume_emulation PUBLIC UZUME_HAVE_VULKAN=1)
  set(UZUME_VULKAN_TEST_DEFS UZUME_HAVE_VULKAN=1)
  if(NOT UZUME_VULKAN_INCLUDE_DIR IN_LIST CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
    target_include_directories(uzume_emulation SYSTEM PRIVATE ${UZUME_VULKAN_INCLUDE_DIR})
    set(UZUME_VULKAN_TEST_INCLUDES ${UZUME_VULKAN_INCLUDE_DIR})
  endif()
else()
  message(STATUS "Vulkan headers not found: Vulkan hardware rendering is compiled out")
  target_compile_definitions(uzume_emulation PUBLIC UZUME_HAVE_VULKAN=0)
endif()
target_compile_features(uzume_emulation PUBLIC cxx_std_20)
target_link_libraries(uzume_emulation PUBLIC uzume_inputmap Qt6::Core Qt6::Gui ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(uzume_emulation PRIVATE winmm)
endif()
uzume_warnings(uzume_emulation)

if(BUILD_TESTING)
  # Generator for the homebrew test ROM (plain C++, also runs on Windows CI).
  add_executable(uzume_make_test_rom tools/make_test_rom.cpp)
  uzume_warnings(uzume_make_test_rom)
  uzume_vcpkg_runtime(uzume_make_test_rom)

  # The test ROM is generated only in the build directory, never in the repo.
  set(FB_TEST_ROM ${CMAKE_CURRENT_BINARY_DIR}/uzume_test.nds)
  add_custom_command(OUTPUT ${FB_TEST_ROM}
    COMMAND uzume_make_test_rom ${FB_TEST_ROM}
    DEPENDS uzume_make_test_rom
    COMMENT "Generating NDS homebrew test ROM"
    VERBATIM)
  add_custom_target(uzume_test_rom ALL DEPENDS ${FB_TEST_ROM})

  # Without core: manifest parsing, CoreLocator.
  uzume_add_test(emulation_manifest tests/manifest_test.cpp LIBS uzume_emulation)
  target_compile_definitions(test_emulation_manifest PRIVATE FB_MANIFEST_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/manifests")

  uzume_add_test(emulation_frame_stats tests/frame_stats_test.cpp LIBS uzume_emulation)
  uzume_add_test(emulation_firmware tests/firmware_test.cpp LIBS uzume_emulation)
  uzume_add_test(emulation_launch_options tests/launch_options_test.cpp LIBS uzume_emulation)
  uzume_add_test(emulation_core_options tests/core_options_test.cpp LIBS uzume_emulation NEEDS_CORE)
  uzume_add_test(emulation_core_options_testcore tests/core_options_test.cpp LIBS uzume_emulation TEST_CORE)

  # Plugin memory access: pure map/identity tests, and snapshots against the test core's synthetic memory map.
  uzume_add_test(emulation_memory_map tests/memory_map_test.cpp LIBS uzume_emulation)
  uzume_add_test(emulation_game_identity tests/game_identity_test.cpp LIBS uzume_emulation)

  # Test-only libretro MODULEs. They link no vcpkg DLLs, so skip vcpkg's z-applocal post-link step (it is attached in
  # the toolchain's add_library override and read at call time, hence the function-local override). With it, Ninja
  # runs one z-applocal per module in parallel on Windows and they race on the shared vcpkg bin directory ("The
  # process cannot access the file because it is being used by another process"). No-op off Windows/vcpkg.
  function(uzume_add_test_module target)
    set(VCPKG_APPLOCAL_DEPS OFF)
    add_library(${target} MODULE ${ARGN})
  endfunction()

  # Fake libretro core (SAVE_RAM only) for the save persistence tests.
  uzume_add_test_module(uzume_fake_core tests/fake_core.cpp)
  target_include_directories(uzume_fake_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(uzume_fake_core PROPERTIES PREFIX "" OUTPUT_NAME uzume_fake_core)
  uzume_add_test(emulation_save_ram tests/save_ram_test.cpp LIBS uzume_emulation)
  target_compile_definitions(test_emulation_save_ram PRIVATE FB_FAKE_CORE_PATH="$<TARGET_FILE:uzume_fake_core>")
  add_dependencies(test_emulation_save_ram uzume_fake_core)
  uzume_add_test(emulation_frame_pacer tests/frame_pacer_test.cpp LIBS uzume_emulation)
  uzume_add_test(emulation_fast_forward tests/fast_forward_test.cpp LIBS uzume_emulation)
  target_compile_definitions(test_emulation_fast_forward PRIVATE FB_FAKE_CORE_PATH="$<TARGET_FILE:uzume_fake_core>")
  add_dependencies(test_emulation_fast_forward uzume_fake_core)

  # Fake cores for the probe tests: with and without SET_SUPPORT_NO_GAME (the latter crashes on retro_load_game(NULL)).
  foreach(variant IN ITEMS 0 1)
    set(tgt uzume_fake_nogame_core_${variant})
    uzume_add_test_module(${tgt} tests/fake_nogame_core.cpp)
    target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${tgt} PRIVATE FB_DECLARE_NO_GAME=${variant})
    set_target_properties(${tgt} PROPERTIES PREFIX "" OUTPUT_NAME ${tgt})
  endforeach()
  uzume_add_test(emulation_core_probe tests/core_probe_test.cpp LIBS uzume_emulation)
  target_compile_definitions(test_emulation_core_probe PRIVATE
    FB_FAKE_NOGAME_CORE_PATH="$<TARGET_FILE:uzume_fake_nogame_core_0>"
    FB_FAKE_NOGAME_DECLARED_CORE_PATH="$<TARGET_FILE:uzume_fake_nogame_core_1>")
  add_dependencies(test_emulation_core_probe uzume_fake_nogame_core_0 uzume_fake_nogame_core_1)

  # Fake libretro core with OpenGL hardware rendering + test (needs QGuiApplication and a GL 3.3 context; SKIP/77
  # without one, except on Linux CI where it must run). Runs on the offscreen QPA with GLX (under xvfb) or EGL.
  uzume_add_test_module(uzume_fake_hw_core tests/fake_hw_core.cpp)
  target_include_directories(uzume_fake_hw_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(uzume_fake_hw_core PROPERTIES PREFIX "" OUTPUT_NAME uzume_fake_hw_core)
  uzume_add_test(emulation_hw_render tests/hw_render_test.cpp LIBS uzume_emulation Qt6::Gui)
  target_compile_definitions(test_emulation_hw_render PRIVATE FB_FAKE_HW_CORE_PATH="$<TARGET_FILE:uzume_fake_hw_core>")
  add_dependencies(test_emulation_hw_render uzume_fake_hw_core)
  set_tests_properties(emulation_hw_render PROPERTIES SKIP_RETURN_CODE 77)

  # Uzume test core (libretro MODULE, test only: not installed, not packaged, not part of the Player). Merge-gating
  # replacement for the real emulator cores in the tests below; see tests/test_core.cpp.
  uzume_add_test_module(uzume_test_core tests/test_core.cpp)
  target_include_directories(uzume_test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${UZUME_TESTUTIL_DIR})
  set_target_properties(uzume_test_core PROPERTIES PREFIX "" OUTPUT_NAME uzume_test_core)
  # Vulkan render mode of the test core: headers only (no loader link); the core gets every function from the frontend.
  target_include_directories(uzume_test_core SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libretro ${UZUME_VULKAN_TEST_INCLUDES})
  target_compile_definitions(uzume_test_core PRIVATE ${UZUME_VULKAN_TEST_DEFS})
  uzume_warnings(uzume_test_core)
  uzume_add_test(emulation_test_core tests/test_core_test.cpp LIBS uzume_emulation TEST_CORE TEST_CORE_FLAVOR generic)
  target_include_directories(test_emulation_test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(test_emulation_test_core PRIVATE FB_TEST_CORE_PATH="$<TARGET_FILE:uzume_test_core>")
  add_dependencies(test_emulation_test_core uzume_test_rom)
  target_compile_definitions(test_emulation_test_core PRIVATE FB_TEST_ROM_PATH="${FB_TEST_ROM}")

  # Vulkan hardware rendering (ADR 0034) against the test core's "vulkan" render mode, readback on lavapipe or any other
  # Vulkan device. SKIP/77 without a device, except with UZUME_REQUIRE_VULKAN=1 (Linux CI).
  uzume_add_test(emulation_vk_render tests/vk_render_test.cpp LIBS uzume_emulation Qt6::Gui TEST_CORE TEST_CORE_FLAVOR generic)
  target_include_directories(test_emulation_vk_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(test_emulation_vk_render PRIVATE FB_TEST_CORE_PATH="$<TARGET_FILE:uzume_test_core>"
                                                              FB_TEST_ROM_PATH="${FB_TEST_ROM}")
  add_dependencies(test_emulation_vk_render uzume_test_rom)
  set_tests_properties(emulation_vk_render PROPERTIES SKIP_RETURN_CODE 77)

  # Input mapping chain (Uzume input -> system profile -> libretro mask) ending in the test core, e.g. ZR -> R2.
  uzume_add_test(emulation_test_core_input tests/test_core_input_test.cpp LIBS uzume_emulation uzume_input uzume_sdl3 TEST_CORE TEST_CORE_FLAVOR generic)
  target_include_directories(test_emulation_test_core_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(test_emulation_test_core_input PRIVATE FB_TEST_CORE_PATH="$<TARGET_FILE:uzume_test_core>")

  # With core (SKIP/77 without UZUME_MELONDS_DS_CORE).
  uzume_add_test(emulation_core tests/core_test.cpp LIBS uzume_emulation NEEDS_CORE)
  target_compile_definitions(test_emulation_core PRIVATE FB_TEST_ROM_PATH="${FB_TEST_ROM}")
  add_dependencies(test_emulation_core uzume_test_rom)

  # DeSmuME smoke test (SKIP/77 without UZUME_DESMUME_CORE).
  uzume_add_test(emulation_core_desmume tests/core_desmume_test.cpp LIBS uzume_emulation NEEDS_DESMUME_CORE)
  target_compile_definitions(test_emulation_core_desmume PRIVATE FB_TEST_ROM_PATH="${FB_TEST_ROM}")
  add_dependencies(test_emulation_core_desmume uzume_test_rom)

  uzume_add_test(emulation_core_speed tests/core_speed_test.cpp LIBS uzume_emulation NEEDS_CORE)
  target_compile_definitions(test_emulation_core_speed PRIVATE FB_TEST_ROM_PATH="${FB_TEST_ROM}")
  add_dependencies(test_emulation_core_speed uzume_test_rom)
  # Timing test (fps ratios): must not share the CPU with the parallel tests.
  set_tests_properties(emulation_core_speed PROPERTIES RUN_SERIAL TRUE)

  # melonDS DS OpenGL renderer through hardware-frame readback (SKIP/77 without core or GL 3.3 context).
  uzume_add_test(emulation_core_gl tests/core_gl_test.cpp LIBS uzume_emulation Qt6::Gui NEEDS_CORE)
  target_compile_definitions(test_emulation_core_gl PRIVATE FB_TEST_ROM_PATH="${FB_TEST_ROM}")
  add_dependencies(test_emulation_core_gl uzume_test_rom)

  # The same tests against the Uzume test core (flavors with the option keys of the product core profiles): always
  # run, never SKIP (the GL one only without a GL 3.3 context). The real-core tests above run in the nightly workflow.
  foreach(variant IN ITEMS core core_speed core_gl)
    set(src tests/${variant}_test.cpp)
    set(libs uzume_emulation)
    if(variant STREQUAL "core_gl")
      list(APPEND libs Qt6::Gui)
    endif()
    uzume_add_test(emulation_${variant}_testcore ${src} LIBS ${libs} TEST_CORE)
    target_compile_definitions(test_emulation_${variant}_testcore PRIVATE FB_TEST_ROM_PATH="${FB_TEST_ROM}")
    add_dependencies(test_emulation_${variant}_testcore uzume_test_rom)
  endforeach()
  set_tests_properties(emulation_core_gl_testcore PROPERTIES SKIP_RETURN_CODE 77)
  # Timing test (fps ratios): must not share the CPU with the parallel tests.
  set_tests_properties(emulation_core_speed_testcore PROPERTIES RUN_SERIAL TRUE)
  uzume_add_test(emulation_core_desmume_testcore tests/core_desmume_test.cpp LIBS uzume_emulation TEST_CORE TEST_CORE_FLAVOR desmume)
  target_compile_definitions(test_emulation_core_desmume_testcore PRIVATE FB_TEST_ROM_PATH="${FB_TEST_ROM}")
  add_dependencies(test_emulation_core_desmume_testcore uzume_test_rom)

  uzume_add_test(emulation_state_cheats_testcore tests/state_cheats_test.cpp LIBS uzume_emulation TEST_CORE)
  target_include_directories(test_emulation_state_cheats_testcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(test_emulation_state_cheats_testcore PRIVATE FB_TEST_ROM_PATH="${FB_TEST_ROM}")
  add_dependencies(test_emulation_state_cheats_testcore uzume_test_rom)

  uzume_add_test(emulation_memory_snapshot_testcore tests/memory_snapshot_test.cpp LIBS uzume_emulation TEST_CORE)
  target_include_directories(test_emulation_memory_snapshot_testcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(test_emulation_memory_snapshot_testcore PRIVATE FB_TEST_ROM_PATH="${FB_TEST_ROM}")
  add_dependencies(test_emulation_memory_snapshot_testcore uzume_test_rom)
endif()
