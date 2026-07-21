cmake_minimum_required(VERSION 3.25)
project(MMD2FFMPEG LANGUAGES CXX)

# Release assets are installed by end users without Visual Studio, so link the
# Microsoft C/C++ runtime statically rather than requiring a separate redist.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(mmd2ffmpeg_dmo SHARED
    src/dmo_driver.cpp
    src/mmd2ffmpeg_dmo.rc
    src/mmd2ffmpeg_dmo.def
)
target_compile_features(mmd2ffmpeg_dmo PRIVATE cxx_std_20)
target_compile_definitions(mmd2ffmpeg_dmo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mmd2ffmpeg_dmo PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(mmd2ffmpeg_dmo PRIVATE ole32 oleaut32 uuid strmiids dmoguids user32 shell32 comctl32)
set_target_properties(mmd2ffmpeg_dmo PROPERTIES OUTPUT_NAME "mmd2ffmpeg_dmo")

add_executable(mmd2ffmpeg_cleanup WIN32 src/cleanup.cpp)
target_compile_features(mmd2ffmpeg_cleanup PRIVATE cxx_std_20)
target_compile_definitions(mmd2ffmpeg_cleanup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mmd2ffmpeg_cleanup PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(mmd2ffmpeg_cleanup PRIVATE shell32)

add_executable(MMDLocaleLauncher WIN32
    src/mmd_locale_launcher.cpp
    src/mmd_locale_launcher.rc
)
target_compile_features(MMDLocaleLauncher PRIVATE cxx_std_20)
target_compile_definitions(MMDLocaleLauncher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(MMDLocaleLauncher PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(MMDLocaleLauncher PRIVATE ole32 shell32 comdlg32 dwmapi)

add_executable(dmo_smoke tests/dmo_smoke.cpp)
target_compile_features(dmo_smoke PRIVATE cxx_std_20)
target_compile_definitions(dmo_smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(dmo_smoke PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(dmo_smoke PRIVATE ole32 uuid strmiids dmoguids msdmo)

add_executable(dmo_compare tests/dmo_compare.cpp)
target_compile_features(dmo_compare PRIVATE cxx_std_20)
target_compile_definitions(dmo_compare PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(dmo_compare PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(dmo_compare PRIVATE ole32 uuid strmiids dmoguids msdmo quartz)

add_executable(property_page_smoke tests/property_page_smoke.cpp)
target_compile_features(property_page_smoke PRIVATE cxx_std_20)
target_compile_definitions(property_page_smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(property_page_smoke PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(property_page_smoke PRIVATE ole32 uuid)
