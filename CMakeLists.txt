cmake_minimum_required(VERSION 3.13)
project(ESPressioMeshAdapters LANGUAGES CXX)

add_library(ESPressio-MeshAdapters INTERFACE)
target_include_directories(ESPressio-MeshAdapters INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(ESPressio-MeshAdapters INTERFACE cxx_std_17)
