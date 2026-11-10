cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(EdgeStream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# System Dependencies
find_package(OpenCV 4 REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(Threads REQUIRED)

# CUDA Toolkit Discovery (Optional: enables CUDA execution provider if available)
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    message(STATUS "CUDA Toolkit found: ${CUDAToolkit_VERSION}")
    set(CUDA_LIB CUDA::cudart)
    set(CUDA_INC ${CUDAToolkit_INCLUDE_DIRS})
elseif(EXISTS "/usr/local/cuda")
    message(STATUS "Found CUDA at /usr/local/cuda")
    set(CUDA_INC "/usr/local/cuda/include")
    find_library(CUDA_LIB NAMES cudart HINTS "/usr/local/cuda/lib64" "/usr/local/cuda/lib")
else()
    message(STATUS "CUDA Toolkit not detected; building with CPU/OpenCV backend support.")
    set(CUDA_LIB "")
    set(CUDA_INC "")
endif()

# ONNX Runtime Root Resolution
# Allows user override via -DORT_ROOT=<path> or $ORT_ROOT environment variable.
if(NOT DEFINED ORT_ROOT)
    if(DEFINED ENV{ORT_ROOT})
        set(ORT_ROOT "$ENV{ORT_ROOT}")
    else()
        # Look for locally unpacked onnxruntime archive directory in ./onnxruntime
        file(GLOB ORT_LOCAL_DIRS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/onnxruntime/onnxruntime*")
        list(FILTER ORT_LOCAL_DIRS EXCLUDE REGEX "\\.(tgz|tar\\.gz|zip)$")
        if(ORT_LOCAL_DIRS)
            list(GET ORT_LOCAL_DIRS 0 FIRST_ORT_DIR)
            set(ORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/${FIRST_ORT_DIR}")
        else()
            set(ORT_ROOT "/usr/local")
        endif()
    endif()
endif()
message(STATUS "Using ONNX Runtime root: ${ORT_ROOT}")

find_path(ORT_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    HINTS "${ORT_ROOT}/include" "/usr/local/include" "/usr/include"
    REQUIRED
)

find_library(ORT_LIB
    NAMES onnxruntime
    HINTS "${ORT_ROOT}/lib" "/usr/local/lib" "/usr/lib" "/usr/lib/x86_64-linux-gnu"
    REQUIRED
)

# Configure dynamic runtime library paths (RPATH) to resolve dependencies cleanly
get_filename_component(ORT_LIB_DIR "${ORT_LIB}" DIRECTORY)
set(APP_RPATH "${ORT_LIB_DIR}")
if(CUDA_LIB)
    if(TARGET CUDA::cudart)
        # CUDAToolkit target is handled automatically by CMake
    else()
        get_filename_component(CUDA_LIB_DIR "${CUDA_LIB}" DIRECTORY)
        list(APPEND APP_RPATH "${CUDA_LIB_DIR}")
    endif()
endif()

# Fetch GoogleTest
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
fetchcontent_makeavailable(googletest)
enable_testing()

# Source definitions
set(CORE_SOURCES
    src/database.cpp
    src/detector.cpp
    src/tracker.cpp
    src/zoneMonitor.cpp
)

# Main Pipeline Application
add_executable(main main.cpp ${CORE_SOURCES})
target_include_directories(main PRIVATE
    include
    ${ORT_INCLUDE_DIR}
    ${OpenCV_INCLUDE_DIRS}
    ${SQLite3_INCLUDE_DIRS}
    ${CUDA_INC}
)
target_link_libraries(main PRIVATE
    ${ORT_LIB}
    ${OpenCV_LIBS}
    ${SQLite3_LIBRARIES}
    ${CUDA_LIB}
    yaml-cpp::yaml-cpp
    Threads::Threads
)
set_target_properties(main PROPERTIES
    BUILD_RPATH "${APP_RPATH}"
    INSTALL_RPATH "${APP_RPATH}"
)
target_compile_options(main PRIVATE
    $<$<CONFIG:Debug,GDebug>:-g -O0 -march=native>
    $<$<CONFIG:Release,GRelease>:-g -O3 -march=native>
)

# Automated Verification Test Suite
add_executable(run_tests tests/tests.cpp ${CORE_SOURCES})
target_include_directories(run_tests PRIVATE
    include
    ${ORT_INCLUDE_DIR}
    ${OpenCV_INCLUDE_DIRS}
    ${SQLite3_INCLUDE_DIRS}
    ${CUDA_INC}
)
target_link_libraries(run_tests PRIVATE
    GTest::gtest_main
    ${ORT_LIB}
    ${OpenCV_LIBS}
    ${SQLite3_LIBRARIES}
    ${CUDA_LIB}
    yaml-cpp::yaml-cpp
    Threads::Threads
)
set_target_properties(run_tests PROPERTIES
    BUILD_RPATH "${APP_RPATH}"
    INSTALL_RPATH "${APP_RPATH}"
)
target_compile_definitions(run_tests PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_options(run_tests PRIVATE
    $<$<CONFIG:Debug,GDebug>:-g -O0 -march=native>
    $<$<CONFIG:Release,GRelease>:-g -O3 -march=native>
)

include(GoogleTest)
gtest_discover_tests(run_tests)

# Optional Trial Benchmarking Target
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/trial.cpp")
    add_executable(trials trial.cpp ${CORE_SOURCES})
    target_include_directories(trials PRIVATE
        include
        ${ORT_INCLUDE_DIR}
        ${OpenCV_INCLUDE_DIRS}
        ${SQLite3_INCLUDE_DIRS}
        ${CUDA_INC}
    )
    target_link_libraries(trials PRIVATE
        ${ORT_LIB}
        ${OpenCV_LIBS}
        ${SQLite3_LIBRARIES}
        ${CUDA_LIB}
        yaml-cpp::yaml-cpp
        Threads::Threads
    )
    set_target_properties(trials PROPERTIES
        BUILD_RPATH "${APP_RPATH}"
        INSTALL_RPATH "${APP_RPATH}"
    )
    target_compile_options(trials PRIVATE
        $<$<CONFIG:Debug,GDebug>:-g -O0 -march=native>
        $<$<CONFIG:Release,GRelease>:-g -O3 -march=native>
    )
endif()
