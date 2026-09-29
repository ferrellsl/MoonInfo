#  MoonInfo: the Moon Info website as a Windows program.
#
#  Build (Visual Studio 2019, x64):
#    cmake -S . -B build -G "Visual Studio 16 2019" -A x64
#    cmake --build build --config Release
#  Produces build/Release/MoonInfo.exe (self-contained: static runtime, no
#  DLLs) with the Moon images copied beside it in build/Release/images, and
#  mooninfo-test.exe, which prints the calculations for checking.

cmake_minimum_required (VERSION 3.15)
project (MOONINFO VERSION 1.2 LANGUAGES C CXX)

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

#  Location.cpp uses C++/WinRT (Windows.Devices.Geolocation); the Windows 10
#  SDK 10.0.19041's C++/WinRT needs these switches with Visual Studio 2019.
set_source_files_properties (src/Location.cpp PROPERTIES COMPILE_OPTIONS "/permissive-;/Zc:twoPhase-;/bigobj;/await")

#  Prints the calculations for a time and place, to check them against the
#  website, or tests finding the location.
add_executable (mooninfo-test src/Test.cpp src/Location.cpp)
target_link_libraries (mooninfo-test mooncalc winhttp windowsapp)

#  --------------------------------------------------------------------------
#  The program: plain Win32 (it also runs under Wine).
set (MOONINFO_RES_DIR "${PROJECT_SOURCE_DIR}/res")
configure_file ("${PROJECT_SOURCE_DIR}/res/MoonInfo.rc.in" "${PROJECT_BINARY_DIR}/MoonInfo.rc" @ONLY)

add_executable (MoonInfo WIN32 src/MoonInfo.cpp src/Location.cpp src/Location.h res/resource.h
                              "${PROJECT_BINARY_DIR}/MoonInfo.rc" res/MoonInfo.manifest)
target_include_directories (MoonInfo PRIVATE "${PROJECT_SOURCE_DIR}/res" "${PROJECT_BINARY_DIR}")
target_link_libraries (MoonInfo mooncalc user32 gdi32 comctl32 ole32 windowscodecs uxtheme dwmapi
                       advapi32 winhttp windowsapp)

#  The Moon images beside the program (they aren't in the repository: copy
#  the images folder from the release zip into the project folder).
if (EXISTS "${PROJECT_SOURCE_DIR}/images")
  add_custom_command (TARGET MoonInfo POST_BUILD
                      COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/images" "$<TARGET_FILE_DIR:MoonInfo>/images")
else()
  message (WARNING "No images folder: MoonInfo will run without the Moon's picture (see README.md)")
endif()
