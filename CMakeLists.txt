cmake_minimum_required(VERSION 3.20)
project(DesktopSnow VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SNOW_BUILD_TESTS "Build unit tests" ON)
option(SNOW_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

if(MSVC)
  # Single self-contained exe: static CRT.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8 /W4 /permissive- /EHsc /MP /Zc:__cplusplus)
  if(SNOW_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra)
  if(SNOW_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

if(WIN32)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

# Output everything into <build>/bin so build.ps1 can copy a single folder.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${cfg} CFG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# ---- portable core (simulation, pile model, settings) -----------------------
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/core/*.cpp)
add_library(snowcore STATIC ${CORE_SOURCES})
target_include_directories(snowcore PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ---- unit tests --------------------------------------------------------------
if(SNOW_BUILD_TESTS)
  enable_testing()
  file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
  set(TEST_EXTRA)
  if(EXISTS ${CMAKE_SOURCE_DIR}/src/harness/detect.cpp)
    list(APPEND TEST_EXTRA ${CMAKE_SOURCE_DIR}/src/harness/detect.cpp)
  else()
    list(FILTER TEST_SOURCES EXCLUDE REGEX "test_detect\\.cpp$")
  endif()
  add_executable(snow_tests ${TEST_SOURCES} ${TEST_EXTRA})
  target_link_libraries(snow_tests PRIVATE snowcore)
  if(MINGW)
    target_link_options(snow_tests PRIVATE -static)
  endif()
  add_test(NAME snow_tests COMMAND snow_tests)
endif()

# ---- Windows app + harness ---------------------------------------------------
if(WIN32)
  file(GLOB APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/app/*.cpp)
  add_executable(DesktopSnow WIN32 ${APP_SOURCES} ${CMAKE_SOURCE_DIR}/res/app.rc)
  target_link_libraries(DesktopSnow PRIVATE snowcore
    d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore wtsapi32 powrprof windowscodecs
    shell32 shlwapi ole32 oleaut32 user32 gdi32 advapi32 winmm uuid dbghelp)
  if(MSVC)
    # The manifest (PerMonitorV2 DPI awareness) is embedded through app.rc.
    target_link_options(DesktopSnow PRIVATE /MANIFEST:NO)
  endif()
  if(MINGW)
    target_link_options(DesktopSnow PRIVATE -municode -static)
  endif()

  file(GLOB HARNESS_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/harness/*.cpp)
  if(HARNESS_SOURCES)
    add_executable(SnowHarness ${HARNESS_SOURCES})
    target_include_directories(SnowHarness PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(SnowHarness PRIVATE d3d11 dxgi dwmapi user32 gdi32 winmm shcore ole32 uuid)
    if(MINGW)
      target_link_options(SnowHarness PRIVATE -municode -static)
    endif()
  endif()

  # Shown in the log and in harness results so a report names its build (CI sets the commit).
  if(DEFINED ENV{SNOW_BUILD_ID} AND NOT "$ENV{SNOW_BUILD_ID}" STREQUAL "")
    set(SNOW_BUILD_ID "$ENV{SNOW_BUILD_ID}")
  else()
    set(SNOW_BUILD_ID "dev")
  endif()
  target_compile_definitions(DesktopSnow PRIVATE SNOW_BUILD_ID="${SNOW_BUILD_ID}")
  if(TARGET SnowHarness)
    target_compile_definitions(SnowHarness PRIVATE SNOW_BUILD_ID="${SNOW_BUILD_ID}")
  endif()
endif()
