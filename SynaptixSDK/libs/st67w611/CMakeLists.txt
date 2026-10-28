# CMake build configuration for the standalone ST67W611 Wi-Fi Driver Library
add_library(st67w611 STATIC)

set(ST67W611_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../x-cube-st67w61"
    CACHE PATH "X-CUBE-ST67W61 vendor directory")
set(ST67W611_ARCH "T02" CACHE STRING "ST67 architecture (T01 or T02)")
set_property(CACHE ST67W611_ARCH PROPERTY STRINGS T01 T02)

if(NOT ST67W611_ARCH STREQUAL "T01" AND NOT ST67W611_ARCH STREQUAL "T02")
    message(FATAL_ERROR "ST67W611_ARCH must be T01 or T02")
endif()

if(ST67W611_ARCH STREQUAL "T01")
    target_compile_definitions(st67w611 PUBLIC ST67_ARCH=1)
else()
    target_compile_definitions(st67w611 PUBLIC ST67_ARCH=2)
endif()

if(NOT EXISTS "${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver")
    message(FATAL_ERROR "ST67W611 vendor driver not found: ${ST67W611_VENDOR_DIR}")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/vendor-version.cmake)
message(STATUS "st67w611 vendor ${ST67W611_VENDOR_RELEASE} (${ST67W611_VENDOR_COMMIT})")

# The user must provide the include path for st67w611_conf.h in their project.
# Example: target_include_directories(st67w611 PUBLIC ${CMAKE_SOURCE_DIR}/Core/Inc)

target_sources(st67w611 PRIVATE
    # Vendor Core
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Core/w6x_sys.c
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Core/w6x_wifi.c
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Core/w6x_net.c
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Core/w6x_netif.c
    # Vendor Driver/W61_bus
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Driver/W61_bus/w61_io.c
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Driver/W61_bus/spi_iface.c
    # Vendor Driver/W61_at
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Driver/W61_at/w61_at_common.c
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Driver/W61_at/w61_at_sys.c
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Driver/W61_at/w61_at_wifi.c
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Driver/W61_at/w61_at_net.c
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Driver/W61_at/modem_cmd_handler.c
    # Vendor Utils
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Utils/Logging/logging.c
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Utils/Misc/common_parser.c
    
    ${CMAKE_CURRENT_SOURCE_DIR}/src/st67w611.c
    ${CMAKE_CURRENT_SOURCE_DIR}/port/src/st67w611_port.c
    ${CMAKE_CURRENT_SOURCE_DIR}/port/src/st67w611_vendor_bridge.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/backend/st67w611_backend_${ST67W611_ARCH}.c
)

# Only our facade and port/config headers are public. Vendor headers remain
# private implementation details of this target.
target_include_directories(st67w611 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(st67w611 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/backend
    ${CMAKE_CURRENT_SOURCE_DIR}/port/include
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Api
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Core
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Core/Inc
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Driver/W61_at
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Driver/W61_bus
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Utils/Logging
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Utils/Misc
)

# NOTE: The parent project provides the RTOS and HAL through the stm32cubemx target.
# The concrete SPI/GPIO implementation is supplied by the consumer port target.
# Vendor sources remain private implementation details of this target.
target_link_libraries(st67w611 PRIVATE stm32cubemx)
