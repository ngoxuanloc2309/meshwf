# splc_port - SimplePLC + ST67 start-up glue of the meshwf board.
#
# Target name is meshwf_splc_port on purpose: `splc_port` is already the Layer 3.5
# target inside libs/simple_plc.
#
# OBJECT library, not STATIC: it defines __wrap_MX_FREERTOS_Init(), which only the
# linker's --wrap option references, so a static archive could drop it depending
# on link order. app/CMakeLists.txt adds $<TARGET_OBJECTS:meshwf_splc_port> to the
# executable and passes -Wl,--wrap=MX_FREERTOS_Init.
#
# This directory also holds splcopts.h (libs/CMakeLists.txt sets SPLC_OPTS_DIR here).
add_library(meshwf_splc_port OBJECT
    splc_port.c
    splc_wifi_port.c
)

target_include_directories(meshwf_splc_port PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(meshwf_splc_port PRIVATE
    splc
    st67w611_port
    stm32cubemx
)