# components - chip-level drivers. Exposes one INTERFACE target: sx_components.
#
#   st67  MQTT client of the ST67W611 module + TLS certificate store
#   rtc   external RTC RX8130CE on I2C1 (decision 57); the SimplePLC hooks that use it
#         live in ports/splc_port/splc_rtc_port.c
add_subdirectory(st67)
add_subdirectory(rtc)

add_library(sx_components INTERFACE)
target_link_libraries(sx_components INTERFACE sx_st67 sx_rtc)