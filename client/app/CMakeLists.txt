# uzume_app: Executable. The QML module and UI sources come from ../ui (uzume_ui).
# Link scheme and single instance (main.cpp); a library so the test can use it.
add_library(uzume_linkhandler STATIC linkhandler.h linkhandler.cpp)
target_include_directories(uzume_linkhandler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uzume_linkhandler PUBLIC Qt6::Core Qt6::Network)
uzume_warnings(uzume_linkhandler)
uzume_add_test(app_linkhandler tests/linkhandler_test.cpp LIBS uzume_linkhandler)

qt_add_executable(uzume_app main.cpp)
# Window icon from the Qt resource (all platforms), see main.cpp.
qt_add_resources(uzume_app app_icons PREFIX "/uzume/app" BASE icons
  FILES icons/uzume-16.png icons/uzume-24.png icons/uzume-32.png icons/uzume-48.png icons/uzume-64.png
        icons/uzume-128.png icons/uzume-256.png icons/uzume-512.png)
target_link_libraries(uzume_app PRIVATE
  uzume_core uzume_emulation uzume_ui uzume_uiplugin uzume_linkhandler uzume_mcp_tools
  Qt6::Quick Qt6::QuickControls2 Qt6::Multimedia)
uzume_warnings(uzume_app)
uzume_vcpkg_runtime(uzume_app)
# Executable name: Uzume.exe on Windows (the launcher in ../launcher is Uzume.exe at the top level), uzume elsewhere.
if(WIN32)
  set_target_properties(uzume_app PROPERTIES OUTPUT_NAME Uzume)
else()
  set_target_properties(uzume_app PROPERTIES OUTPUT_NAME uzume)
endif()
if(WIN32)
  # GUI subsystem: no console window on start. Qt::EntryPoint maps main() to WinMain. Console output of
  # --version/--help/--version-json goes through printToStdout() in main.cpp (inherited handle or parent console).
  set_target_properties(uzume_app PROPERTIES WIN32_EXECUTABLE ON)
  target_link_libraries(uzume_app PRIVATE shell32)
  # Executable icon (Explorer/taskbar/shortcuts); the .rc is only compiled on Windows.
  target_sources(uzume_app PRIVATE uzume.rc)
  # Application manifest (longPathAware, UTF-8 active code page). MSVC: CMake merges a .manifest source into the linker's
  # manifest (mt.exe); embedding it through the .rc as well would give a duplicate resource. Other toolchains: uzume.rc.
  if(MSVC)
    target_sources(uzume_app PRIVATE uzume.manifest)
  endif()
endif()

if(BUILD_TESTING)
  add_test(NAME app_smoke COMMAND uzume_app --smoke-test)
  set_tests_properties(app_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    TIMEOUT 60)
endif()
