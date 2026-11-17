# Copyright 2026 Gerald Guyomard
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.22)
project(spatialcontainer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dawn (WebGPU) -----------------------------------------------------------
# Minimal download: a depth-1 clone of a pinned Chromium release branch, then only the
# third-party deps this configuration needs (Vulkan backend, no tests/tools/GL/D3D/protobuf),
# each fetched at depth 1 at the exact commit Dawn pins as a submodule. The checkout lives in
# the module directory so all build variants share it.
set(DAWN_GIT_BRANCH "chromium/7999")
set(DAWN_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/dawn")
set(DAWN_REQUIRED_DEPS
    abseil-cpp
    jinja2
    markupsafe
    spirv-headers/src
    spirv-tools/src
    vulkan-headers/src
    vulkan-utility-libraries/src
    webgpu-headers/src
)

find_package(Git REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(dawn_git)
    execute_process(COMMAND ${GIT_EXECUTABLE} ${ARGN}
        WORKING_DIRECTORY ${DAWN_SOURCE_DIR}
        RESULT_VARIABLE result OUTPUT_VARIABLE output OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed")
    endif()
    set(GIT_OUTPUT "${output}" PARENT_SCOPE)
endfunction()

if(NOT EXISTS "${DAWN_SOURCE_DIR}/CMakeLists.txt")
    message(STATUS "Cloning Dawn ${DAWN_GIT_BRANCH} (depth 1)")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} clone --depth 1 --single-branch --branch ${DAWN_GIT_BRANCH}
                https://dawn.googlesource.com/dawn ${DAWN_SOURCE_DIR}
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Failed to clone Dawn")
    endif()
endif()

foreach(dep ${DAWN_REQUIRED_DEPS})
    set(dep_dir "${DAWN_SOURCE_DIR}/third_party/${dep}")
    if(EXISTS "${dep_dir}/.fetched")
        continue()
    endif()
    dawn_git(ls-tree HEAD "third_party/${dep}")
    string(REGEX MATCH "commit ([0-9a-f]+)" _ "${GIT_OUTPUT}")
    set(dep_commit "${CMAKE_MATCH_1}")
    dawn_git(config -f .gitmodules --get "submodule.third_party/${dep}.url")
    set(dep_url "${GIT_OUTPUT}")
    message(STATUS "Fetching Dawn dependency ${dep} @ ${dep_commit} (depth 1)")
    file(REMOVE_RECURSE "${dep_dir}")
    file(MAKE_DIRECTORY "${dep_dir}")
    foreach(step "init -q" "remote add origin ${dep_url}" "fetch -q --depth 1 origin ${dep_commit}"
                 "checkout -q FETCH_HEAD")
        separate_arguments(step_args UNIX_COMMAND "${step}")
        execute_process(COMMAND ${GIT_EXECUTABLE} ${step_args} WORKING_DIRECTORY "${dep_dir}"
            RESULT_VARIABLE result)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Failed to fetch ${dep}: git ${step}")
        endif()
    endforeach()
    file(TOUCH "${dep_dir}/.fetched")
endforeach()

set(DAWN_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_MONOLITHIC_LIBRARY STATIC CACHE STRING "" FORCE)
set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
set(DAWN_ENABLE_OPENGLES OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_DESKTOP_GL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_NULL OFF CACHE BOOL "" FORCE)
set(DAWN_USE_GLFW OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_PROTOBUF OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_SPIRV_VALIDATION OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_CMD_TOOLS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_SPV_READER OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_GLSL_VALIDATOR OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_IR_BINARY OFF CACHE BOOL "" FORCE)
add_subdirectory(${DAWN_SOURCE_DIR} "${CMAKE_CURRENT_BINARY_DIR}/dawn" EXCLUDE_FROM_ALL)

# --- OpenXR loader (org.khronos.openxr:openxr_loader_for_android prefab) ------
find_package(OpenXR REQUIRED CONFIG)

# --- Vulkan shaders (SPIR-V via the NDK's glslc) -------------------------------
# Each shader becomes a comma-separated list of 32-bit words included by vulkan_renderer.cpp.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(NDK_HOST_TAG darwin-x86_64)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(NDK_HOST_TAG windows-x86_64)
else()
    set(NDK_HOST_TAG linux-x86_64)
endif()
find_program(GLSLC glslc HINTS "${ANDROID_NDK}/shader-tools/${NDK_HOST_TAG}" REQUIRED)
set(SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")
set(SHADER_INCS)
foreach(shader scene.vert lit.frag unlit.frag)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}")
    set(out "${SHADER_OUT_DIR}/${shader}.inc")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${GLSLC}" --target-env=vulkan1.1 -mfmt=num -o "${out}" "${src}"
        DEPENDS "${src}"
        COMMENT "glslc ${shader}"
        VERBATIM)
    list(APPEND SHADER_INCS "${out}")
endforeach()
add_custom_target(spatialcontainer_shaders DEPENDS ${SHADER_INCS})

# --- App ----------------------------------------------------------------------
add_library(spatialcontainer SHARED
    main.cpp
    xr_app.cpp
    scene.cpp
    webgpu_renderer.cpp
    vulkan_renderer.cpp
)
add_dependencies(spatialcontainer spatialcontainer_shaders)
target_include_directories(spatialcontainer PRIVATE "${SHADER_OUT_DIR}")
target_compile_definitions(spatialcontainer PRIVATE XR_USE_PLATFORM_ANDROID)
# Each backend sees only its own graphics API in openxr_platform.h.
set_source_files_properties(webgpu_renderer.cpp PROPERTIES
    COMPILE_DEFINITIONS XR_USE_GRAPHICS_API_OPENGL_ES)
set_source_files_properties(vulkan_renderer.cpp PROPERTIES
    COMPILE_DEFINITIONS XR_USE_GRAPHICS_API_VULKAN)
target_link_libraries(spatialcontainer PRIVATE
    OpenXR::openxr_loader
    dawn::webgpu_dawn
    vulkan
    EGL
    GLESv3
    android
    log
)
