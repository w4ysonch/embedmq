cmake_minimum_required(VERSION 3.14)
project(embedmq C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- PAL selection -----------------------------------------------
# Default to Linux. Override with -DEMBEDMQ_PAL=freertos or =none
set(EMBEDMQ_PAL "linux" CACHE STRING "PAL backend: linux | freertos | none")

# ---- Library target ----------------------------------------------
add_library(embedmq STATIC
    src/embedmq.c
    src/embedmq_hash.c
    src/embedmq_queue.c
    pal/${EMBEDMQ_PAL}/embedmq_pal.c
)

target_include_directories(embedmq PUBLIC include)
target_include_directories(embedmq PRIVATE src pal)

# Map PAL name → compile definition
string(TOUPPER "${EMBEDMQ_PAL}" _PAL_UPPER)
target_compile_definitions(embedmq PRIVATE EMBEDMQ_PAL_${_PAL_UPPER})

if(EMBEDMQ_PAL STREQUAL "linux")
    target_link_libraries(embedmq PRIVATE pthread)
endif()

target_compile_options(embedmq PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)

# ---- Examples ----------------------------------------------------
option(EMBEDMQ_BUILD_EXAMPLES "Build examples" ON)
if(EMBEDMQ_BUILD_EXAMPLES)
    add_executable(example_basic examples/basic.c)
    target_link_libraries(example_basic PRIVATE embedmq)

    add_executable(benchmark examples/benchmark.c)
    target_link_libraries(benchmark PRIVATE embedmq pthread)

    add_executable(example_basic_cpp examples/basic_cpp.cpp)
    target_link_libraries(example_basic_cpp PRIVATE embedmq)
endif()

# ---- Tests -------------------------------------------------------
option(EMBEDMQ_BUILD_TESTS "Build tests" ON)
if(EMBEDMQ_BUILD_TESTS)
    enable_testing()
    add_executable(test_embedmq tests/test_embedmq.c)
    target_link_libraries(test_embedmq PRIVATE embedmq)
    add_test(NAME embedmq_tests COMMAND test_embedmq)

    add_executable(test_embedmq_cpp tests/test_embedmq_cpp.cpp)
    target_link_libraries(test_embedmq_cpp PRIVATE embedmq)
    add_test(NAME embedmq_cpp_tests COMMAND test_embedmq_cpp)
endif()
