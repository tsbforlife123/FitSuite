cmake_minimum_required(VERSION 3.20)
project(FitSuite VERSION 0.2.3 LANGUAGES C CXX)
if(WIN32)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FITSUITE_BUILD_TESTS "Build the core unit tests" ON)
option(FITSUITE_BUILD_APP "Build the Windows desktop app (Windows targets only)" ${WIN32})
option(FITSUITE_BUILD_HARNESS "Build the Linux UI development harness (X11/GLX)" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---- core: platform-independent math, analysis and storage -----------------------------------
add_library(fitcore STATIC
  core/src/stats.cpp
  core/src/timeutil.cpp
  core/src/mouse_transform.cpp
  core/src/mouse_analysis.cpp
  core/src/mouse_fit.cpp
  core/src/mouse_schedule.cpp
  core/src/mouse_compare.cpp
  core/src/frame.cpp
  core/src/motion.cpp
  core/src/profile.cpp
  core/src/serialize.cpp
  core/src/store.cpp
  core/src/rawaccel_export.cpp
  core/src/game_activation.cpp
  core/src/mouse_check.cpp
  core/src/aim_report.cpp
  core/src/sha256.cpp
  core/src/update.cpp
)
target_include_directories(fitcore PUBLIC core/include third_party)

# ---- third-party UI ------------------------------------------------------------------------------
if(FITSUITE_BUILD_APP OR FITSUITE_BUILD_HARNESS)
  add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/implot/implot.cpp
    third_party/implot/implot_items.cpp
  )
  target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends third_party/implot)
  target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
  if(NOT MSVC)
    target_compile_options(imgui PRIVATE -w)
  endif()

  # Fonts are embedded into the binary (Inter, IBM Plex Mono — SIL OFL 1.1).
  set(FONT_SRC ${CMAKE_CURRENT_BINARY_DIR}/embedded_fonts.cpp)
  add_custom_command(
    OUTPUT ${FONT_SRC}
    COMMAND ${CMAKE_COMMAND} -DOUT=${FONT_SRC} -DFONT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_fonts.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_fonts.cmake
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Inter-Medium.ttf
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Inter-SemiBold.ttf
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/IBMPlexMono-Regular.ttf
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/IBMPlexMono-Medium.ttf
    COMMENT "Embedding fonts")

  # This version's release notes (docs/releases/v<version>.md) are built in, for the "What's new" box.
  set(NOTES_MD ${CMAKE_CURRENT_SOURCE_DIR}/docs/releases/v${PROJECT_VERSION}.md)
  set(FITSUITE_RELEASE_NOTES "")
  if(EXISTS ${NOTES_MD})
    file(READ ${NOTES_MD} FITSUITE_RELEASE_NOTES)
    string(STRIP "${FITSUITE_RELEASE_NOTES}" FITSUITE_RELEASE_NOTES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${NOTES_MD})
  endif()
  set(NOTES_SRC ${CMAKE_CURRENT_BINARY_DIR}/release_notes.cpp)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/release_notes.cpp.in ${NOTES_SRC} @ONLY)

  # ---- app: UI pages, session flows and test scenes (platform independent) ----------------------
  add_library(fitapp STATIC
    app/app.cpp
    app/theme.cpp
    app/widgets.cpp
    app/page_overview.cpp
    app/page_mousefit.cpp
    app/page_framefit.cpp
    app/page_motionfit.cpp
    app/page_profiles.cpp
    app/page_settings.cpp
  app/page_setup.cpp
  app/setup_flow.cpp
    app/mouse_test.cpp
    app/motion_test.cpp
    app/scene.cpp
    ${FONT_SRC}
    ${NOTES_SRC}
  )
  target_include_directories(fitapp PUBLIC app)
  target_link_libraries(fitapp PUBLIC fitcore imgui)
endif()

if(FITSUITE_BUILD_APP)
  if(NOT WIN32)
    message(FATAL_ERROR "FITSUITE_BUILD_APP requires a Windows target (use the llvm-mingw toolchain file or MSVC).")
  endif()
  add_executable(FitSuite WIN32
    platform/win32/main_win32.cpp
    platform/win32/raw_input.cpp
    platform/win32/display.cpp
    platform/win32/sysinfo.cpp
    platform/win32/presentmon.cpp
    platform/win32/platform_win32.cpp
    platform/win32/net_win32.cpp
    platform/win32/fitsuite.rc
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
  )
  target_include_directories(FitSuite PRIVATE platform/win32)
  target_compile_definitions(FitSuite PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_link_libraries(FitSuite PRIVATE fitapp d3d11 dxgi d3dcompiler dxva2 setupapi hid cfgmgr32 shell32 ole32 dwmapi winhttp
                                         winmm comdlg32 advapi32 user32 gdi32 version)
  if(MINGW)
    target_link_options(FitSuite PRIVATE -static -municode)
  endif()
  if(NOT MSVC)
    # Win32 structs are routinely initialised as {sizeof s}; that is intended.
    target_compile_options(FitSuite PRIVATE -Wno-missing-field-initializers)
  else()
    # fitsuite.rc embeds its own manifest (DPI awareness, UTF-8, common controls).
    target_link_options(FitSuite PRIVATE /MANIFEST:NO)
  endif()
endif()

if(FITSUITE_BUILD_HARNESS)
  add_executable(fitsuite_harness
    harness/linux/main_harness.cpp
    harness/linux/platform_harness.cpp
    tests/sim_user.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp
  )
  target_include_directories(fitsuite_harness PRIVATE harness/linux tests)
  target_compile_definitions(fitsuite_harness PRIVATE FITSUITE_HARNESS=1)
  target_link_libraries(fitsuite_harness PRIVATE fitapp X11 dl)
endif()

if(FITSUITE_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_executable(fitsuite_tests
    tests/test_main.cpp
    tests/sim_user.cpp
    tests/test_stats.cpp
    tests/test_transform.cpp
    tests/test_analysis.cpp
    tests/test_fit.cpp
    tests/test_compare.cpp
    tests/test_schedule.cpp
    tests/test_frame.cpp
    tests/test_profile.cpp
    tests/test_motion.cpp
    tests/test_rawaccel_export.cpp
    tests/test_game_activation.cpp
    tests/test_check.cpp
    tests/test_rounds.cpp
    tests/test_update.cpp
  )
  target_include_directories(fitsuite_tests PRIVATE tests third_party/doctest)
  target_link_libraries(fitsuite_tests PRIVATE fitcore)
  add_test(NAME fitsuite_tests COMMAND fitsuite_tests)
endif()
