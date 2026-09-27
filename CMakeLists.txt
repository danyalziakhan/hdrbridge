cmake_minimum_required(VERSION 3.20)
project(hdrbridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(minhook STATIC
    deps/minhook/src/buffer.c
    deps/minhook/src/hook.c
    deps/minhook/src/trampoline.c
    deps/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC deps/minhook/include)

add_library(hdrbridge SHARED src/hdrbridge.cpp src/trace.cpp src/tonemap.cpp src/hdrbridge.rc)
target_include_directories(hdrbridge PRIVATE deps/reshade/include deps/imgui)
target_link_libraries(hdrbridge PRIVATE minhook dxgi setupapi)
target_compile_definitions(hdrbridge PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(hdrbridge PRIVATE /W3 /utf-8)
set_target_properties(hdrbridge PROPERTIES SUFFIX ".addon64")
