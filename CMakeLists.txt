cmake_minimum_required(VERSION 3.20)

project(rkGateway LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The kernel headers/build tree must be supplied by the caller. Do not use a
# repository-local default because the module must be built against the exact
# kernel tree used by the target board.
if(NOT DEFINED KERNEL_HEADERS OR KERNEL_HEADERS STREQUAL "")
    message(FATAL_ERROR
        "KERNEL_HEADERS must be provided, for example: "
        "-DKERNEL_HEADERS=/path/to/linux-headers")
endif()
set(KERNEL_HEADERS "${KERNEL_HEADERS}" CACHE PATH
    "Linux kernel headers/build tree supplied by the caller")
set(KERNEL_ARCH "arm64" CACHE STRING "Linux kernel architecture")
set(KERNEL_CROSS_COMPILE "aarch64-linux-gnu-" CACHE STRING
    "Linux kernel CROSS_COMPILE prefix")

if(NOT EXISTS "${KERNEL_HEADERS}/Makefile")
    message(FATAL_ERROR "KERNEL_HEADERS does not contain a kernel Makefile: ${KERNEL_HEADERS}")
endif()

# Use the board-oriented configuration by default. Users can still override
# GATEWAY_CONFIG at configure time with -DGATEWAY_CONFIG=... .
set(GATEWAY_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/configs/ds18b20_linux.json"
    CACHE STRING "Gateway JSON configuration")

# miniGateway remains a third-party subproject. Its own targets are emitted
# into build/miniGateway; the DS18B20 plugin below is emitted beside the
# executable so the generated configuration can load it with ./lib....so.
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniGateway"
    "${CMAKE_CURRENT_BINARY_DIR}/miniGateway")

add_library(gateway_ds18b20 SHARED
    plugins/ds18b20/ds18b20.cpp)
target_include_directories(gateway_ds18b20 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ds18b20"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniGateway/include")
target_link_libraries(gateway_ds18b20
    PUBLIC gateway_core
    PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(gateway_ds18b20 PROPERTIES
    OUTPUT_NAME "gateway_ds18b20"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    BUILD_RPATH_USE_ORIGIN ON
    INSTALL_RPATH "$ORIGIN")
target_compile_options(gateway_ds18b20 PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion)

add_dependencies(miniGateway gateway_ds18b20)

set(KERNEL_MODULE_BUILD_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/kernel/ds18b20")
set(KERNEL_MODULE
    "${KERNEL_MODULE_BUILD_DIR}/ds18b20.ko")
add_custom_command(
    OUTPUT "${KERNEL_MODULE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${KERNEL_MODULE_BUILD_DIR}/src"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/drivers/ds18b20/ds18b20.c"
        "${KERNEL_MODULE_BUILD_DIR}/src/ds18b20.c"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/drivers/ds18b20/Makefile"
        "${KERNEL_MODULE_BUILD_DIR}/src/Makefile"
    COMMAND ${CMAKE_MAKE_PROGRAM} -C "${KERNEL_HEADERS}"
        ARCH=${KERNEL_ARCH}
        CROSS_COMPILE=${KERNEL_CROSS_COMPILE}
        M=${KERNEL_MODULE_BUILD_DIR}/src
        modules
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${KERNEL_MODULE_BUILD_DIR}/src/ds18b20.ko"
        "${KERNEL_MODULE}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/drivers/ds18b20/ds18b20.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/drivers/ds18b20/Makefile"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building DS18B20 kernel module"
    VERBATIM)
add_custom_target(ds18b20_kernel_module ALL DEPENDS "${KERNEL_MODULE}")

add_executable(ds18b20_test
    tools/ds18b20_test.c)
set_target_properties(ds18b20_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools")
target_compile_options(ds18b20_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion)

add_custom_target(ds18b20_artifacts ALL
    DEPENDS ds18b20_kernel_module gateway_ds18b20 miniGateway ds18b20_test)
