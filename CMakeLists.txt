cmake_minimum_required(VERSION 3.16)
project(camera-ctls-rk3588 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt5 REQUIRED COMPONENTS Widgets)

# The one genuinely SoC-specific setting. RK3588 == ISP hardware v30; a different
# Rockchip target needs a flag change here, not a code change.
set(ISP_HW_VERSION "ISP_HW_V30" CACHE STRING "rkaiq ISP hardware version macro")

# librkaiq ships no pkg-config file, so paths are given explicitly.
set(RKAIQ_ROOT "/usr/include/rkaiq" CACHE PATH "rkaiq header root")

set(SOURCES
    src/main.cpp
    src/CameraEnumerator.cpp
    src/AiqController.cpp
    src/V4l2Capture.cpp
    src/PreviewWidget.cpp
    src/LabeledSlider.cpp
    src/CameraPipeline.cpp
    src/MainWindow.cpp
    src/IqOverride.cpp
    src/LensController.cpp
    src/ContrastAutoFocus.cpp
)

add_executable(camera-ctls ${SOURCES})

target_include_directories(camera-ctls PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # imgproc includes "rkaiq/xcore/base/xcam_common.h" by that full path,
    # so the parent of the rkaiq tree must be on the include path too.
    /usr/include
    ${RKAIQ_ROOT}
    ${RKAIQ_ROOT}/uAPI2
    ${RKAIQ_ROOT}/common
    ${RKAIQ_ROOT}/xcore
    ${RKAIQ_ROOT}/algos
    ${RKAIQ_ROOT}/iq_parser
    ${RKAIQ_ROOT}/iq_parser_v2
    ${RKAIQ_ROOT}/isp
)

# Must match the flags librkaiq.so itself was built with, or struct layouts
# can disagree between our translation units and the library.
target_compile_definitions(camera-ctls PRIVATE ${ISP_HW_VERSION} ADD_RK_AIQ)

target_link_libraries(camera-ctls PRIVATE Qt5::Widgets rkaiq pthread)

install(TARGETS camera-ctls RUNTIME DESTINATION bin)
