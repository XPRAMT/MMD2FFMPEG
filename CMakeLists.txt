cmake_minimum_required(VERSION 3.25)
project(MMD2FFMPEG LANGUAGES CXX)

add_library(mmd2ffmpeg SHARED
    src/driver.cpp
    src/mmd2ffmpeg.def
)

target_compile_features(mmd2ffmpeg PRIVATE cxx_std_20)
target_compile_definitions(mmd2ffmpeg PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mmd2ffmpeg PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(mmd2ffmpeg PRIVATE vfw32 winmm shell32)
set_target_properties(mmd2ffmpeg PROPERTIES OUTPUT_NAME "mmd2ffmpeg")

add_executable(vfw_smoke tests/vfw_smoke.cpp)
target_compile_features(vfw_smoke PRIVATE cxx_std_20)
target_compile_definitions(vfw_smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(vfw_smoke PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(vfw_smoke PRIVATE vfw32)
