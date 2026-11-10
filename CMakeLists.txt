# Plugin SKSE de Transformaciones. El .dll va a Data/SKSE/Plugins.

cmake_minimum_required(VERSION 3.21)

project(Transformaciones VERSION 0.1.0 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES src/plugin.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui d3d11 dxgi d3dcompiler)
