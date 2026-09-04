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
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/configs/camera_periodic_linux.json"
    "${CMAKE_CURRENT_BINARY_DIR}/camera_periodic_config.json"
    COPYONLY)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/configs/camera_control_linux.json"
    "${CMAKE_CURRENT_BINARY_DIR}/camera_control_config.json"
    COPYONLY)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/configs/screen_linux.json"
    "${CMAKE_CURRENT_BINARY_DIR}/screen_config.json"
    COPYONLY)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/configs/adc_mqtt_linux.json"
    "${CMAKE_CURRENT_BINARY_DIR}/adc_mqtt_config.json"
    COPYONLY)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/configs/yolo_linux.json"
    "${CMAKE_CURRENT_BINARY_DIR}/yolo_config.json"
    COPYONLY)

# miniGateway remains a third-party subproject. Its own targets are emitted
# into build/miniGateway; the DS18B20 plugin below is emitted beside the
# executable so the generated configuration can load it with ./lib....so.
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniGateway"
    "${CMAKE_CURRENT_BINARY_DIR}/miniGateway")

# Rockchip librga is kept in this repository as arm64 Debian packages under
# third_party/rga. Extract it into build/ so the cross build never writes into
# the host sysroot and all deployable runtime files stay below build/.
set(RGA_ROOT "" CACHE PATH
    "Pre-extracted arm64 RGA root containing include/rga and lib/librga")
set(RGA_PACKAGE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rga"
    CACHE PATH "Directory containing the arm64 librga Debian packages")
if(RGA_ROOT STREQUAL "")
    find_program(DPKG_DEB_EXECUTABLE dpkg-deb REQUIRED)
    set(RGA_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/rga-sysroot")
    set(RGA_DEV_DEB "${RGA_PACKAGE_DIR}/librga-dev_2.2.0-1_arm64.deb")
    set(RGA_RUNTIME_DEB "${RGA_PACKAGE_DIR}/librga2_2.2.0-1_arm64.deb")
    if(NOT EXISTS "${RGA_DEV_DEB}" OR NOT EXISTS "${RGA_RUNTIME_DEB}")
        message(FATAL_ERROR
            "Rockchip RGA packages were not found. Set RGA_ROOT to an "
            "extracted arm64 librga root or set RGA_PACKAGE_DIR. Missing: "
            "${RGA_DEV_DEB}; ${RGA_RUNTIME_DEB}")
    endif()
    file(MAKE_DIRECTORY "${RGA_STAGE_DIR}")
    execute_process(
        COMMAND "${DPKG_DEB_EXECUTABLE}" -x "${RGA_RUNTIME_DEB}" "${RGA_STAGE_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND "${DPKG_DEB_EXECUTABLE}" -x "${RGA_DEV_DEB}" "${RGA_STAGE_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
    set(RGA_ROOT "${RGA_STAGE_DIR}/usr")
endif()

find_path(RGA_INCLUDE_DIR rga/im2d.h
    PATHS "${RGA_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
find_library(RGA_LIBRARY NAMES rga
    PATHS "${RGA_ROOT}/lib/aarch64-linux-gnu" "${RGA_ROOT}/lib"
    NO_DEFAULT_PATH REQUIRED)
add_library(RGA::RGA SHARED IMPORTED GLOBAL)
set_target_properties(RGA::RGA PROPERTIES
    IMPORTED_LOCATION "${RGA_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${RGA_INCLUDE_DIR}")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway")
file(GLOB RGA_RUNTIME_LIBRARIES
    "${RGA_ROOT}/lib/aarch64-linux-gnu/librga.so*")
if(NOT RGA_RUNTIME_LIBRARIES)
    message(FATAL_ERROR "No arm64 librga runtime library found below ${RGA_ROOT}")
endif()
file(COPY ${RGA_RUNTIME_LIBRARIES}
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    FOLLOW_SYMLINK_CHAIN)

# RKNN runtime/API for the RK3566 NPU. The default points at the
# repository-local Rockchip runtime, but callers may override RKNN_ROOT when
# using a different SDK revision.
set(RKNN_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rknpu2/runtime/RK356X/Linux/librknn_api"
    CACHE PATH "RKNN API root for RK356X Linux")
find_path(RKNN_INCLUDE_DIR rknn_api.h
    PATHS "${RKNN_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
find_library(RKNN_LIBRARY NAMES rknnrt
    PATHS "${RKNN_ROOT}/aarch64" NO_DEFAULT_PATH REQUIRED)
add_library(RKNN::RT SHARED IMPORTED GLOBAL)
set_target_properties(RKNN::RT PROPERTIES
    IMPORTED_LOCATION "${RKNN_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${RKNN_INCLUDE_DIR}")
configure_file(
    "${RKNN_LIBRARY}"
    "${CMAKE_CURRENT_BINARY_DIR}/miniGateway/librknnrt.so"
    COPYONLY)

# ONNX Runtime CPU backend for the YOLO plugin.  The repository-local
# package contains AArch64 headers and shared libraries, so do not search the
# host system and accidentally link an x86_64 runtime into the cross build.
set(ONNXRUNTIME_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime"
    CACHE PATH "AArch64 ONNX Runtime package root")
find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    PATHS "${ONNXRUNTIME_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime
    PATHS "${ONNXRUNTIME_ROOT}/lib" "${ONNXRUNTIME_ROOT}/lib64"
    NO_DEFAULT_PATH REQUIRED)
add_library(ONNXRuntime::Runtime SHARED IMPORTED GLOBAL)
set_target_properties(ONNXRuntime::Runtime PROPERTIES
    IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}")

file(GLOB ONNXRUNTIME_RUNTIME_LIBRARIES
    "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so*"
    "${ONNXRUNTIME_ROOT}/lib/libonnxruntime_providers_shared.so*"
    "${ONNXRUNTIME_ROOT}/lib64/libonnxruntime.so*"
    "${ONNXRUNTIME_ROOT}/lib64/libonnxruntime_providers_shared.so*")
if(NOT ONNXRUNTIME_RUNTIME_LIBRARIES)
    message(FATAL_ERROR
        "No AArch64 ONNX Runtime shared libraries found below ${ONNXRUNTIME_ROOT}")
endif()
file(COPY ${ONNXRUNTIME_RUNTIME_LIBRARIES}
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    FOLLOW_SYMLINK_CHAIN)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway/model")
foreach(YOLO_MODEL IN ITEMS
        yolov5s_rk3566.rknn
        yolov5n_rk3566.rknn
        yolov5n.onnx)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/yolo/model/${YOLO_MODEL}")
        message(FATAL_ERROR "Missing YOLO model: plugins/yolo/model/${YOLO_MODEL}")
    endif()
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/plugins/yolo/model/${YOLO_MODEL}"
        "${CMAKE_CURRENT_BINARY_DIR}/miniGateway/model/${YOLO_MODEL}"
        COPYONLY)
endforeach()

add_library(camera_capture_support STATIC
    plugins/camera/camera_capture.c)
target_include_directories(camera_capture_support PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/camera")
target_link_libraries(camera_capture_support PUBLIC RGA::RGA)
target_compile_options(camera_capture_support PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion)

add_library(gateway_camera SHARED
    plugins/camera/camera.cpp)
target_include_directories(gateway_camera PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/camera"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniGateway/include")
target_link_libraries(gateway_camera
    PUBLIC gateway_core
    PRIVATE camera_capture_support nlohmann_json::nlohmann_json)
set_target_properties(gateway_camera PROPERTIES
    OUTPUT_NAME "gateway_camera"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    BUILD_RPATH "$ORIGIN"
    BUILD_RPATH_USE_ORIGIN ON
    INSTALL_RPATH "$ORIGIN")
target_compile_options(gateway_camera PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion)

add_library(screen_fb_support STATIC
    plugins/screen/screen_fb.c)
target_include_directories(screen_fb_support PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/screen")
target_compile_options(screen_fb_support PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion)

add_library(gateway_screen SHARED
    plugins/screen/screen.cpp)
target_include_directories(gateway_screen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/screen"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniGateway/include")
target_link_libraries(gateway_screen
    PUBLIC gateway_core
    PRIVATE screen_fb_support nlohmann_json::nlohmann_json)
set_target_properties(gateway_screen PROPERTIES
    OUTPUT_NAME "gateway_screen"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    BUILD_RPATH_USE_ORIGIN ON
    INSTALL_RPATH "$ORIGIN")
target_compile_options(gateway_screen PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion)

add_library(gateway_adc SHARED
    plugins/adc/adc.cpp)
target_include_directories(gateway_adc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/adc"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniGateway/include")
target_link_libraries(gateway_adc
    PUBLIC gateway_core
    PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(gateway_adc PROPERTIES
    OUTPUT_NAME "gateway_adc"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    BUILD_RPATH "$ORIGIN"
    BUILD_RPATH_USE_ORIGIN ON
    INSTALL_RPATH "$ORIGIN")
target_compile_options(gateway_adc PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion)

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

add_library(gateway_yolo SHARED
    plugins/yolo/yolo.cpp)
target_include_directories(gateway_yolo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/yolo"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniGateway/include"
    "${RKNN_INCLUDE_DIR}"
    "${ONNXRUNTIME_INCLUDE_DIR}")
target_link_libraries(gateway_yolo
    PUBLIC gateway_core
    PRIVATE nlohmann_json::nlohmann_json RKNN::RT ONNXRuntime::Runtime m)
set_target_properties(gateway_yolo PROPERTIES
    OUTPUT_NAME "gateway_yolo"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniGateway"
    BUILD_RPATH "$ORIGIN"
    BUILD_RPATH_USE_ORIGIN ON
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN")
target_compile_options(gateway_yolo PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion)

add_dependencies(miniGateway
    gateway_ds18b20 gateway_camera gateway_screen gateway_adc gateway_yolo)

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

add_executable(camera_test
    tools/camera_test.c)
target_include_directories(camera_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/camera")
target_link_libraries(camera_test PRIVATE camera_capture_support)
set_target_properties(camera_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
    BUILD_RPATH "$ORIGIN/../miniGateway"
    BUILD_RPATH_USE_ORIGIN ON
    INSTALL_RPATH "$ORIGIN/../miniGateway")
target_compile_options(camera_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion)

add_executable(screen_test
    tools/screen_test.c)
target_include_directories(screen_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/screen")
target_link_libraries(screen_test PRIVATE screen_fb_support)
set_target_properties(screen_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools")
target_compile_options(screen_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion)

add_executable(ds18b20_test
    tools/ds18b20_test.c)
set_target_properties(ds18b20_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools")
target_compile_options(ds18b20_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion)

add_custom_target(ds18b20_artifacts ALL
    DEPENDS ds18b20_kernel_module gateway_ds18b20 gateway_camera gateway_adc
            gateway_yolo miniGateway ds18b20_test camera_test screen_test
            gateway_screen)
