# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.20)

find_package(Vulkan 1.3.230 REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_executable(CORNELL_GLSLANG NAMES glslangValidator glslang
             HINTS "$ENV{VULKAN_SDK}/bin" REQUIRED)

add_executable(venus-cornell main.cpp scene.hpp)
target_compile_features(venus-cornell PRIVATE cxx_std_17)
target_link_libraries(venus-cornell PRIVATE Vulkan::Vulkan SDL3::SDL3 PNG::PNG)
target_include_directories(venus-cornell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

foreach(shader raster.vert raster.mesh raster.frag fullscreen.vert ray.frag)
    string(REPLACE "." "_" symbol "${shader}")
    set(spirv "${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${symbol}.hpp")
    add_custom_command(
        OUTPUT "${header}"
        BYPRODUCTS "${spirv}"
        COMMAND "${CORNELL_GLSLANG}" --target-env vulkan1.2 -V
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}" -o "${spirv}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${spirv}" "-DOUTPUT=${header}"
                "-DSYMBOL=${symbol}" -P "${CMAKE_CURRENT_SOURCE_DIR}/embed-spirv.cmake"
        DEPENDS "shaders/${shader}" shaders/shared.glsl embed-spirv.cmake
        VERBATIM)
    target_sources(venus-cornell PRIVATE "${header}")
endforeach()
