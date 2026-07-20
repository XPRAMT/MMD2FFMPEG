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

add_library(mmd2ffmpeg_dmo SHARED
    src/dmo_driver.cpp
    src/mmd2ffmpeg_dmo.def
)
target_compile_features(mmd2ffmpeg_dmo PRIVATE cxx_std_20)
target_compile_definitions(mmd2ffmpeg_dmo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mmd2ffmpeg_dmo PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(mmd2ffmpeg_dmo PRIVATE ole32 oleaut32 uuid strmiids dmoguids user32)
set_target_properties(mmd2ffmpeg_dmo PROPERTIES OUTPUT_NAME "mmd2ffmpeg_dmo")

add_executable(vfw_smoke tests/vfw_smoke.cpp)
target_compile_features(vfw_smoke PRIVATE cxx_std_20)
target_compile_definitions(vfw_smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(vfw_smoke PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(vfw_smoke PRIVATE vfw32)

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
