# uzume_core: Player core without GUI (core/ + network/). Add sources here and in ../network.
add_library(uzume_core STATIC version.cpp credentialstore.cpp profilestore.cpp romcache.cpp localstorage.cpp savestore.cpp statestore.cpp playersettings.cpp firmwarecache.cpp corecache.cpp dsvsave.cpp sdtree.cpp emulationsettings.cpp controllerprofiles.cpp
  filelog.cpp diagbundle.cpp errorfingerprint.cpp errorevents.cpp problemreport.cpp semver.cpp updateindex.cpp updaterollback.cpp updatesig.cpp installroot.cpp updater.cpp localhub.cpp hubsetup.cpp fsutil.cpp localromindex.cpp securitycode.cpp joinlink.cpp localpin.cpp localserverprocess.cpp
  zipextract.cpp toolcache.cpp variantcache.cpp enginerunner.cpp variantbuilder.cpp variantlaunch.cpp)
target_include_directories(uzume_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uzume_core PUBLIC cxx_std_20)
target_link_libraries(uzume_core PUBLIC Qt6::Core Qt6::Concurrent Qt6::Network OpenSSL::Crypto)
if(WIN32)
  target_link_libraries(uzume_core PUBLIC advapi32 shell32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(uzume_core PUBLIC Qt6::DBus)  # Secret Service credential store
endif()
uzume_warnings(uzume_core)

# FVX launcher shim (issue #414, tools/fvx-launcher): a tiny precompiled class that enables 3DS support in the randomizer engine's
# CLI before it runs. Needs a JDK at build time (javac >= 11, jar); without one the app runs the engine as before, without the
# shim. UZUME_REQUIRE_FVX_LAUNCHER=ON (CI jobs that produce shipped binaries) makes a missing JDK a configure error.
option(UZUME_REQUIRE_FVX_LAUNCHER "Fail configure when no JDK is found to build the FVX launcher jar" OFF)
find_package(Java 11 COMPONENTS Development QUIET)
if(Java_FOUND AND Java_JAVAC_EXECUTABLE AND Java_JAR_EXECUTABLE)
  set(UZUME_FVX_LAUNCHER_DIR ${CMAKE_CURRENT_BINARY_DIR}/fvx-launcher)
  set(UZUME_FVX_LAUNCHER_JAR ${UZUME_FVX_LAUNCHER_DIR}/uzume-fvx-launcher.jar)
  set(UZUME_FVX_LAUNCHER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../tools/fvx-launcher/UzumeFvxLauncher.java)
  add_custom_command(
    OUTPUT ${UZUME_FVX_LAUNCHER_JAR}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${UZUME_FVX_LAUNCHER_DIR}/classes
    COMMAND ${CMAKE_COMMAND} -E make_directory ${UZUME_FVX_LAUNCHER_DIR}/classes
    COMMAND ${Java_JAVAC_EXECUTABLE} --release 11 -encoding UTF-8 -d ${UZUME_FVX_LAUNCHER_DIR}/classes ${UZUME_FVX_LAUNCHER_SRC}
    COMMAND ${Java_JAR_EXECUTABLE} --create --file ${UZUME_FVX_LAUNCHER_JAR} -C ${UZUME_FVX_LAUNCHER_DIR}/classes .
    DEPENDS ${UZUME_FVX_LAUNCHER_SRC}
    COMMENT "Building uzume-fvx-launcher.jar"
    VERBATIM)
  add_custom_target(uzume_fvx_launcher_jar DEPENDS ${UZUME_FVX_LAUNCHER_JAR})
  add_dependencies(uzume_core uzume_fvx_launcher_jar)
  qt_add_resources(uzume_core uzume_fvx_launcher
    PREFIX "/uzume/engine" BASE ${UZUME_FVX_LAUNCHER_DIR}
    FILES ${UZUME_FVX_LAUNCHER_JAR})
  target_compile_definitions(uzume_core PUBLIC UZUME_HAVE_FVX_LAUNCHER=1)
  message(STATUS "FVX launcher: building with ${Java_JAVAC_EXECUTABLE}")
elseif(UZUME_REQUIRE_FVX_LAUNCHER)
  message(FATAL_ERROR "UZUME_REQUIRE_FVX_LAUNCHER=ON but no JDK (javac >= 11 and jar) was found; 3DS randomizer builds need the launcher")
else()
  message(STATUS "FVX launcher: no JDK found, the app will run the randomizer engine without it (3DS builds will fail)")
endif()
# Build identity (see client/CMakeLists.txt): only version.cpp sees it, so a changed version rebuilds one file.
set_source_files_properties(version.cpp PROPERTIES COMPILE_DEFINITIONS
  "UZUME_VERSION_STR=\"${UZUME_EFFECTIVE_VERSION}\";UZUME_CHANNEL_STR=\"${UZUME_CHANNEL}\";UZUME_COMMIT_STR=\"${UZUME_COMMIT}\"")

uzume_add_test(core_version tests/core_version_test.cpp LIBS uzume_core)
uzume_add_test(core_profilestore tests/profilestore_test.cpp LIBS uzume_core)
uzume_add_test(core_envvar tests/envvar_test.cpp LIBS uzume_core)
uzume_add_test(core_romcache tests/romcache_test.cpp LIBS uzume_core)
uzume_add_test(core_savestore tests/savestore_test.cpp LIBS uzume_core)
uzume_add_test(core_statestore tests/statestore_test.cpp LIBS uzume_core)
uzume_add_test(core_localstorage tests/localstorage_test.cpp LIBS uzume_core)
uzume_add_test(core_dsvsave tests/dsvsave_test.cpp LIBS uzume_core)
uzume_add_test(core_sdtree tests/sdtree_test.cpp LIBS uzume_core)
uzume_add_test(core_playersettings tests/playersettings_test.cpp LIBS uzume_core)
uzume_add_test(core_emulationsettings tests/emulationsettings_test.cpp LIBS uzume_core)
uzume_add_test(core_controllerprofiles tests/controllerprofiles_test.cpp LIBS uzume_core)
uzume_add_test(core_firmwarecache tests/firmwarecache_test.cpp LIBS uzume_core)
uzume_add_test(core_corecache tests/corecache_test.cpp LIBS uzume_core)
uzume_add_test(core_filelog tests/filelog_test.cpp LIBS uzume_core)
uzume_add_test(core_diagbundle tests/diagbundle_test.cpp LIBS uzume_core)
uzume_add_test(core_problemreport tests/problemreport_test.cpp LIBS uzume_core)
uzume_add_test(core_errors tests/errors_test.cpp LIBS uzume_core)
if(BUILD_TESTING)
  target_compile_definitions(test_core_errors PRIVATE UZUME_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../protocol/fixtures")
endif()
uzume_add_test(core_update tests/update_test.cpp LIBS uzume_core)
uzume_add_test(core_localhub tests/localhub_test.cpp LIBS uzume_core)
uzume_add_test(core_localromindex tests/localromindex_test.cpp LIBS uzume_core)
uzume_add_test(core_fsutil tests/fsutil_test.cpp LIBS uzume_core)
uzume_add_test(core_securitycode tests/securitycode_test.cpp LIBS uzume_core)
uzume_add_test(core_joinlink tests/joinlink_test.cpp LIBS uzume_core)
uzume_add_test(core_localpin tests/localpin_test.cpp LIBS uzume_core)
uzume_add_test(core_localserverprocess tests/localserverprocess_test.cpp LIBS uzume_core)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Secret Service store against an in-test fake service on a private dbus-daemon (SKIP when dbus-daemon is missing).
  uzume_add_test(core_credential_secret_service tests/secret_service_store_test.cpp LIBS uzume_core)
endif()
if(WIN32)
  # Writes/reads/deletes a dummy value in the Windows Credential Manager (Windows CI only).
  uzume_add_test(core_windows_credential_store tests/windows_credential_store_test.cpp LIBS uzume_core)
endif()

# Stand-in for the randomizer engine's CLI (testutil/test_engine.cpp): tests pack it as java/bin/java of a fake engine package.
if(BUILD_TESTING)
  add_executable(uzume_test_engine ${UZUME_TESTUTIL_DIR}/test_engine.cpp)
  uzume_warnings(uzume_test_engine)
  set_target_properties(uzume_test_engine PROPERTIES OUTPUT_NAME uzume_test_engine)
  uzume_vcpkg_runtime(uzume_test_engine)
  uzume_add_test(core_zipextract tests/zipextract_test.cpp LIBS uzume_core)
  uzume_add_test(core_toolcache tests/toolcache_test.cpp LIBS uzume_core)
  uzume_add_test(core_variantcache tests/variantcache_test.cpp LIBS uzume_core)
  uzume_add_test(core_variantbuilder tests/variantbuilder_test.cpp LIBS uzume_core)
  uzume_add_test(core_enginerunner tests/enginerunner_test.cpp LIBS uzume_core)
  add_dependencies(test_core_enginerunner uzume_test_engine)
  target_compile_definitions(test_core_enginerunner PRIVATE FB_TEST_ENGINE_PATH="$<TARGET_FILE:uzume_test_engine>")
  add_dependencies(test_core_variantbuilder uzume_test_engine)
  target_compile_definitions(test_core_variantbuilder PRIVATE FB_TEST_ENGINE_PATH="$<TARGET_FILE:uzume_test_engine>")
  uzume_add_test(core_variantlaunch tests/variantlaunch_test.cpp LIBS uzume_core)
endif()
