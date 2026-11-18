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

set(VENUS_CORNELL_DATA_DIR "" CACHE PATH
    "Optional directory containing the Cornell and CIE measurement inputs")
set(shaders raster.vert raster.mesh raster.frag fullscreen.vert ray.frag)
if(VENUS_CORNELL_DATA_DIR)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(spectral_inputs
        "${VENUS_CORNELL_DATA_DIR}/cornell.html"
        "${VENUS_CORNELL_DATA_DIR}/boxsource.mat"
        "${VENUS_CORNELL_DATA_DIR}/cie.csv")
    foreach(input IN LISTS spectral_inputs)
        if(NOT EXISTS "${input}")
            message(FATAL_ERROR "Missing Cornell Box measurement input: ${input}")
        endif()
    endforeach()
    set(spectral_table "${CMAKE_CURRENT_BINARY_DIR}/spectral-table.glsl")
    add_custom_command(
        OUTPUT "${spectral_table}"
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/generate-spectral-table.py"
                ${spectral_inputs} "${spectral_table}"
        DEPENDS generate-spectral-table.py ${spectral_inputs}
        VERBATIM)
    list(APPEND shaders ray_measured.frag)
    target_compile_definitions(venus-cornell PRIVATE VENUS_CORNELL_MEASURED=1)
endif()

foreach(shader IN LISTS shaders)
    set(shader_dependencies "shaders/${shader}" shaders/shared.glsl embed-spirv.cmake)
    if(shader STREQUAL "ray_measured.frag")
        list(APPEND shader_dependencies "${spectral_table}")
    endif()
    string(REPLACE "." "_" symbol "${shader}")
    set(spirv "${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${symbol}.hpp")
    add_custom_command(
        OUTPUT "${header}"
        BYPRODUCTS "${spirv}"
        COMMAND "${CORNELL_GLSLANG}" --target-env vulkan1.2 -V "-I${CMAKE_CURRENT_BINARY_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}" -o "${spirv}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${spirv}" "-DOUTPUT=${header}"
                "-DSYMBOL=${symbol}" -P "${CMAKE_CURRENT_SOURCE_DIR}/embed-spirv.cmake"
        DEPENDS ${shader_dependencies}
        VERBATIM)
    target_sources(venus-cornell PRIVATE "${header}")
endforeach()
