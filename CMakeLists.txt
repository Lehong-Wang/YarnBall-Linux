cmake_minimum_required(VERSION 3.24)
project(YarnBall LANGUAGES C CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CUDA_STANDARD 17)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# RTX 50-series (sm_120) requires CUDA Toolkit 12.8+. Earlier toolkits do
# not know about sm_120 and will hard-fail at configure on `native`.
find_package(CUDAToolkit 12.8 REQUIRED)

# `project(... LANGUAGES CUDA)` auto-initializes CMAKE_CUDA_ARCHITECTURES
# to `52` under policy CMP0104, so a bare `if(NOT DEFINED)` never fires.
# Distinguish auto-init from explicit `-D` via the cache entry's TYPE:
#   auto-init -> type=STRING (with helpstring "CUDA architectures")
#   user -D   -> type=UNINITIALIZED
# (probed on CMake 3.28). Auto-detect only when the user didn't set it.
get_property(_cuda_arch_type CACHE CMAKE_CUDA_ARCHITECTURES PROPERTY TYPE)
if(NOT _cuda_arch_type STREQUAL "UNINITIALIZED")
    if(CUDAToolkit_VERSION VERSION_LESS 12.8)
        set(CMAKE_CUDA_ARCHITECTURES 86)
    else()
        # Detect from nvcc's bundled query helper. CMake's `native` is
        # unreliable when no GPU is accessible at configure time
        # (containers, CI, WSL2 quirks). Multi-arch fallback covers
        # Turing..Blackwell so binaries still load via PTX JIT.
        if(EXISTS ${CUDAToolkit_BIN_DIR}/__nvcc_device_query)
            execute_process(
                COMMAND ${CUDAToolkit_BIN_DIR}/__nvcc_device_query
                OUTPUT_VARIABLE _gpu_arch
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _arch_rc)
            if(_arch_rc EQUAL 0 AND _gpu_arch MATCHES "^[0-9]+$")
                set(CMAKE_CUDA_ARCHITECTURES ${_gpu_arch})
            else()
                set(CMAKE_CUDA_ARCHITECTURES "75;86;89;120")
            endif()
        else()
            set(CMAKE_CUDA_ARCHITECTURES "75;86;89;120")
        endif()
    endif()
endif()
message(STATUS "CUDA architectures: ${CMAKE_CUDA_ARCHITECTURES}")

# ---------- system / distro packages ----------
find_package(OpenGL   REQUIRED)
find_package(glfw3    REQUIRED)
find_package(assimp   REQUIRED)
find_package(Freetype REQUIRED)
find_package(glm      REQUIRED)
find_package(Eigen3   REQUIRED)
find_package(Threads  REQUIRED)
find_package(OpenMP   REQUIRED)

# jsoncpp packaging varies. Try the CMake config first, fall back to pkg-config.
find_package(jsoncpp CONFIG QUIET)
if(NOT jsoncpp_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
endif()

# stb headers — Ubuntu 24.04's libstb-dev installs to /usr/include/stb/
# (subdirectory). The source uses `#include "stb_image.h"` (no prefix),
# so we add the stb subdir explicitly to the include path.
find_path(STB_INCLUDE_DIR stb_image.h
    PATHS /usr/include/stb /usr/local/include/stb /usr/include /usr/local/include
    REQUIRED)

# ---------- vendored / fetched ----------
include(FetchContent)

FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11
    GIT_TAG v2.4.2)
FetchContent_MakeAvailable(cli11)

# Source uses ImGui::Begin/Text/TreeNode/Checkbox/SliderFloat only — no
# DockBuilder, no ConfigFlags_DockingEnable. Plain master tag is sufficient.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG v1.91.5)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)

# GLAD must be the glad1 ABI (<glad/glad.h> + gladLoadGLLoader). Auto-generate
# into third_party/glad/ via Python's `glad` package if missing — keeps fresh
# clones self-bootstrapping without committing generated source. Same recipe
# as scripts/install-linux-deps.sh; the script just runs it eagerly.
set(GLAD_DIR ${CMAKE_SOURCE_DIR}/third_party/glad)
if(NOT EXISTS ${GLAD_DIR}/src/glad.c OR NOT EXISTS ${GLAD_DIR}/include/glad/glad.h)
    find_package(Python3 COMPONENTS Interpreter)
    if(NOT Python3_FOUND)
        message(FATAL_ERROR
            "Missing third_party/glad/ and python3 not available. "
            "Run scripts/install-linux-deps.sh first.")
    endif()
    set(GLAD_VENV ${CMAKE_SOURCE_DIR}/.venv-glad)
    if(NOT EXISTS ${GLAD_VENV}/bin/glad)
        message(STATUS "Bootstrapping GLAD: creating venv at ${GLAD_VENV}")
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -m venv ${GLAD_VENV}
            RESULT_VARIABLE _venv_rc)
        if(NOT _venv_rc EQUAL 0)
            message(FATAL_ERROR "Failed to create venv (rc=${_venv_rc}). Install python3-venv: sudo apt install python3-venv")
        endif()
        execute_process(
            COMMAND ${GLAD_VENV}/bin/pip install --quiet glad
            RESULT_VARIABLE _pip_rc)
        if(NOT _pip_rc EQUAL 0)
            message(FATAL_ERROR "Failed to install glad in venv (rc=${_pip_rc}). Network unreachable?")
        endif()
    endif()
    # Stage on the same filesystem as the destination so the final
    # file(RENAME) doesn't cross devices (matters on tmpfs build dirs).
    set(_glad_tmp ${CMAKE_SOURCE_DIR}/third_party/.glad_gen)
    file(REMOVE_RECURSE ${_glad_tmp})
    message(STATUS "Bootstrapping GLAD: generating glad1 GL 4.6 core")
    execute_process(
        COMMAND ${GLAD_VENV}/bin/glad
            --generator c --spec gl --profile core --api gl=4.6
            --extensions= --out-path ${_glad_tmp}
        RESULT_VARIABLE _glad_rc)
    if(NOT _glad_rc EQUAL 0 OR NOT EXISTS ${_glad_tmp}/src/glad.c)
        message(FATAL_ERROR "GLAD generation failed (rc=${_glad_rc})")
    endif()
    file(REMOVE_RECURSE ${GLAD_DIR})
    file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party)
    file(RENAME ${_glad_tmp} ${GLAD_DIR})
endif()
add_library(glad STATIC ${GLAD_DIR}/src/glad.c)
target_include_directories(glad PUBLIC ${GLAD_DIR}/include)

# ---------- YarnBall static library ----------
set(YARNBALL_HOST_SOURCES
    KittenEngine/KittenEngine/opt/asa047.cpp
    KittenEngine/KittenEngine/opt/compass_search.cpp
    KittenEngine/KittenEngine/opt/lbfgs.c
    KittenEngine/KittenEngine/opt/math.cpp
    KittenEngine/KittenEngine/opt/praxis.cpp
    KittenEngine/KittenEngine/opt/svd/svd.cpp
    KittenEngine/KittenEngine/opt/toms178.cpp

    KittenEngine/KittenEngine/src/Algo.cpp
    KittenEngine/KittenEngine/src/ComputeBuffer.cpp
    KittenEngine/KittenEngine/src/Font.cpp
    KittenEngine/KittenEngine/src/FrameBuffer.cpp
    KittenEngine/KittenEngine/src/Gizmos.cpp
    KittenEngine/KittenEngine/src/KittenAssets.cpp
    KittenEngine/KittenEngine/src/KittenInit.cpp
    KittenEngine/KittenEngine/src/KittenPreprocessor.cpp
    KittenEngine/KittenEngine/src/KittenRendering.cpp
    KittenEngine/KittenEngine/src/Mesh.cpp
    KittenEngine/KittenEngine/src/MeshMoments.cpp
    KittenEngine/KittenEngine/src/Shader.cpp
    KittenEngine/KittenEngine/src/StopWatch.cpp
    KittenEngine/KittenEngine/src/Texture.cpp
    KittenEngine/KittenEngine/src/Timer.cpp

    KittenEngine/YarnBall/io/fiberMesher.cpp
    KittenEngine/YarnBall/io/reader.cpp
    KittenEngine/YarnBall/io/fileExport.cpp
    KittenEngine/YarnBall/io/jsonBuilder.cpp
    KittenEngine/YarnBall/io/render.cpp

    KittenEngine/YarnBall/sim/statistics.cpp
    KittenEngine/YarnBall/sim/step.cpp)

set(YARNBALL_CUDA_SOURCES
    KittenEngine/KittenEngine/KittenGpuLBVH/lbvh.cu
    KittenEngine/YarnBall/sim/iteration.cu
    KittenEngine/YarnBall/sim/cosserat.cu
    KittenEngine/YarnBall/sim/collision.cu
    KittenEngine/YarnBall/YarnBall.cu)

add_library(YarnBall STATIC
    ${YARNBALL_HOST_SOURCES}
    ${YARNBALL_CUDA_SOURCES})

# Engine includes. The submodule has its own KittenEngine/includes/ tree
# that would shadow the parent — intentionally not added here.
target_include_directories(YarnBall PUBLIC
    ${CMAKE_SOURCE_DIR}/KittenEngine/KittenEngine/includes
    ${CMAKE_SOURCE_DIR}/KittenEngine/KittenEngine
    ${CMAKE_SOURCE_DIR}/KittenEngine
    ${STB_INCLUDE_DIR})

target_link_libraries(YarnBall PUBLIC
    CUDA::cudart
    OpenMP::OpenMP_CXX
    Threads::Threads
    glm::glm
    Eigen3::Eigen
    glad
    assimp::assimp
    Freetype::Freetype
    OpenGL::GL
    glfw
    imgui)

if(TARGET JsonCpp::JsonCpp)
    target_link_libraries(YarnBall PUBLIC JsonCpp::JsonCpp)
elseif(TARGET jsoncpp_lib)
    target_link_libraries(YarnBall PUBLIC jsoncpp_lib)
elseif(TARGET jsoncpp_static)
    target_link_libraries(YarnBall PUBLIC jsoncpp_static)
else()
    target_link_libraries(YarnBall PUBLIC PkgConfig::JSONCPP)
endif()

# OpenMP must reach both compile and link of CUDA TUs:
# KittenGpuLBVH/lbvh.cu uses `#pragma omp parallel for` and `#pragma omp atomic`
# inside CUDA-compiled code. -Xcompiler=-fopenmp routes the flag to the host
# compiler used by NVCC; OpenMP::OpenMP_CXX carries -fopenmp to the link step.
target_compile_options(YarnBall PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--extended-lambda>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcudafe=--diag_suppress=esa_on_defaulted_function_ignored>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-fopenmp>
    $<$<COMPILE_LANGUAGE:CXX>:-fopenmp>)


# ---------- Gui executable ----------
add_executable(Gui KittenEngine/main.cpp)
target_link_libraries(Gui PRIVATE YarnBall CLI11::CLI11)

# ---------- resource staging ----------
# Engine paths in the source are cwd-relative (e.g. `loadDirectory("resources")`,
# `loadDirectory("KittenEngine/shaders")`). Stage them next to the binary so
# `cd build && ./Gui ...` works.
add_custom_command(TARGET Gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/KittenEngine/configs
        $<TARGET_FILE_DIR:Gui>/configs
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/KittenEngine/resources
        $<TARGET_FILE_DIR:Gui>/resources
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/KittenEngine/KittenEngine/shaders
        $<TARGET_FILE_DIR:Gui>/KittenEngine/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/KittenEngine/KittenEngine/fonts
        $<TARGET_FILE_DIR:Gui>/KittenEngine/fonts
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:Gui>/frames
    COMMENT "Staging resources, configs, shaders, fonts next to Gui binary")
