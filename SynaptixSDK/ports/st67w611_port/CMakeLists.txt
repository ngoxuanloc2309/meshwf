# st67w611_port - board adapter of libs/st67w611 for the meshwf board (SPI1).
#
# Added by SynaptixSDK/ports/CMakeLists.txt; libs/CMakeLists.txt has already
# forced ST67W611_ARCH=T01 (AT mode, no lwIP). It links `splc` because
# st67w611_spi_port.c uses sx_spi_* / sx_gpio_* of simple_plc's Layer 0.
add_library(st67w611_port STATIC
    st67w611_spi_port.c
    st67w611_wifi_switch.c
    st67w611_soft_reset.c
)

set(ST67_SPLC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/simple_plc)

target_include_directories(st67w611_port PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(st67w611_port PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/st67w611/port/include
    ${ST67_SPLC_DIR}
    ${ST67_SPLC_DIR}/config
    ${ST67_SPLC_DIR}/components/spi
    ${ST67_SPLC_DIR}/components/gpio
)

target_link_libraries(st67w611_port
    PUBLIC  st67w611 splc
    PRIVATE stm32cubemx
)