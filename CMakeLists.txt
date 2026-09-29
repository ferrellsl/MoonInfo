#  MoonInfo: the Moon Info website as a Windows program.
#
#  Build (Visual Studio 2019, x64):
#    cmake -S . -B build -G "Visual Studio 16 2019" -A x64
#    cmake --build build --config Release
#  Produces build/Release/MoonInfo.exe (self-contained: static runtime, no
#  DLLs) with the Moon images copied beside it in build/Release/images, and
#  mooninfo-test.exe, which prints the calculations for checking.

cmake_minimum_required (VERSION 3.15)
project (MOONINFO VERSION 1.0 LANGUAGES C CXX)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

#  Static C/C++ runtime, so the program doesn't need the Visual C++ runtime DLLs.
set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if (MSVC)
  add_compile_definitions (_CRT_SECURE_NO_WARNINGS)
endif()

configure_file ("${PROJECT_SOURCE_DIR}/src/config.h.in" "${PROJECT_BINARY_DIR}/config.h")

#  Astronomy Engine (C), (c) 2019-2023 Don Cross, MIT licence.
add_library (astronomy STATIC third_party/astronomy/astronomy.c third_party/astronomy/astronomy.h)
target_include_directories (astronomy PUBLIC "${PROJECT_SOURCE_DIR}/third_party/astronomy")

add_library (mooncalc STATIC src/MoonCalc.cpp src/MoonCalc.h)
target_link_libraries (mooncalc PUBLIC astronomy)
target_include_directories (mooncalc PUBLIC "${PROJECT_SOURCE_DIR}/src")

#  Prints the calculations for a time and place, to check them against the
#  website.
add_executable (mooninfo-test src/Test.cpp src/Location.cpp)
target_link_libraries (mooninfo-test mooncalc winhttp windowsapp)

#  --------------------------------------------------------------------------
#  The program, built with GraphApp (http://enchantia.com/graphapp/).
set (GRAPHAPP_ROOT "C:/GraphApp" CACHE PATH "GraphApp folder (with src/app.h and graphapp.lib)")
find_path (GRAPHAPP_INCLUDE_DIR app.h PATHS "${GRAPHAPP_ROOT}/src" NO_DEFAULT_PATH)
find_library (GRAPHAPP_LIBRARY NAMES graphapp PATHS "${GRAPHAPP_ROOT}" NO_DEFAULT_PATH)
if (NOT (GRAPHAPP_INCLUDE_DIR AND GRAPHAPP_LIBRARY))
  message (FATAL_ERROR "GraphApp not found in GRAPHAPP_ROOT (${GRAPHAPP_ROOT})")
endif()

set (MOONINFO_RES_DIR "${PROJECT_SOURCE_DIR}/res")
configure_file ("${PROJECT_SOURCE_DIR}/res/MoonInfo.rc.in" "${PROJECT_BINARY_DIR}/MoonInfo.rc" @ONLY)

#  WIN32: GraphApp's library supplies WinMain, which calls main().
add_executable (MoonInfo WIN32 src/MoonInfo.cpp src/Location.cpp src/Location.h res/resource.h
                              "${PROJECT_BINARY_DIR}/MoonInfo.rc")
target_include_directories (MoonInfo PRIVATE "${GRAPHAPP_INCLUDE_DIR}" "${PROJECT_SOURCE_DIR}/res" "${PROJECT_BINARY_DIR}")
#  Location.cpp uses C++/WinRT (Windows.Devices.Geolocation).
set_source_files_properties (src/Location.cpp PROPERTIES COMPILE_OPTIONS "/permissive-;/Zc:twoPhase-;/bigobj;/await")
target_link_libraries (MoonInfo mooncalc "${GRAPHAPP_LIBRARY}"
                       user32 gdi32 comdlg32 shell32 ole32 winmm winhttp windowsapp dwmapi)

#  The Moon images beside the program (they aren't in the repository: copy
#  the images folder from the release zip into the project folder).
if (EXISTS "${PROJECT_SOURCE_DIR}/images")
  add_custom_command (TARGET MoonInfo POST_BUILD
                      COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/images" "$<TARGET_FILE_DIR:MoonInfo>/images")
else()
  message (WARNING "No images folder: MoonInfo will run without the Moon's picture (see README.md)")
endif()
