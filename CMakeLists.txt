cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
project(EdgeStream)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4 REQUIRED)
find_package(SQLite3 REQUIRED)

if (NOT_MAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# install GTest
include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)

fetchcontent_makeavailable(googletest)

enable_testing()

add_executable(main main.cpp src/database.cpp)
add_executable(run_tests tests/tests.cpp src/database.cpp)

# Safely explicitly map the new opencv5 include folder
target_include_directories(main PRIVATE
        ${SQLite3_INCLUDE_DIRS}
        ${OpenCV_INCLUDE_DIRS}
)

# Link binaries
target_link_libraries(main PRIVATE
        ${OpenCV_LIBS}
        ${SQLite3_LIBRARIES}
)

target_compile_options(main PRIVATE
        $<$<CONFIG:Debug,GDebug>:-g -O0 -march=native>
        $<$<CONFIG:Release,GRelease>:-g -O3 -march=native>
)

# Safely explicitly map the new opencv5 include folder
target_include_directories(run_tests PRIVATE
        ${OpenCV_INCLUDE_DIRS}
        ${SQLite3_INCLUDE_DIRS}
)

target_link_libraries(run_tests PRIVATE
        GTest::gtest_main
        ${OpenCV_LIBS}
        ${SQLite3_LIBRARIES}
        pthread
)

target_compile_options(run_tests PRIVATE
        $<$<CONFIG:Debug,GDebug>:-g -O0 -march=native>
        $<$<CONFIG:Release,GRelease>:-g -O3 -march=native>
)

include(GoogleTest)
gtest_discover_tests(run_tests)