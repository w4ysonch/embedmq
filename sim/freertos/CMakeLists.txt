# ---------------------------------------------------------------------
# embedmq FreeRTOS POSIX simulator test
#
# Pulls FreeRTOS-Kernel via FetchContent and builds the embedmq core +
# FreeRTOS PAL + test harness against the GCC_POSIX port — runs on a host
# machine with no hardware. Enabled from the top-level build with
#   -DEMBEDMQ_BUILD_FREERTOS_SIM=ON
# ---------------------------------------------------------------------

include(FetchContent)

FetchContent_Declare(freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG        V11.1.0   # pinned; do not track master
)

# FreeRTOSConfig.h lives in this directory
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

set(FREERTOS_HEAP "4"         CACHE STRING "" FORCE)   # heap_4.c
set(FREERTOS_PORT "GCC_POSIX" CACHE STRING "" FORCE)   # native host simulation

FetchContent_MakeAvailable(freertos_kernel)

add_executable(test_embedmq_freertos
    main.c
    ${EMBEDMQ_ROOT}/src/embedmq.c
    ${EMBEDMQ_ROOT}/src/embedmq_hash.c
    ${EMBEDMQ_ROOT}/src/embedmq_queue.c
    ${EMBEDMQ_ROOT}/pal/freertos/embedmq_pal.c
)

target_compile_definitions(test_embedmq_freertos PRIVATE EMBEDMQ_PAL_FREERTOS)

target_include_directories(test_embedmq_freertos PRIVATE
    ${EMBEDMQ_ROOT}/include
    ${EMBEDMQ_ROOT}/src
    ${EMBEDMQ_ROOT}/pal
)

target_link_libraries(test_embedmq_freertos PRIVATE
    freertos_kernel
    freertos_config
)
