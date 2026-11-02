# components/rtc - driver of the external RTC RX8130CE (I2C1) and the calendar <-> UTC epoch
# conversion, see sx_ex_rtc.h.
#
# Uses the library's sx_i2c.h / sx_time.h (target splc, same as components/st67), so splc is a
# PUBLIC dependency: sx_ex_rtc.h includes sx_i2c.h.
add_library(sx_rtc STATIC
    sx_ex_rtc.c
)

target_include_directories(sx_rtc PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(sx_rtc PUBLIC splc PRIVATE stm32cubemx)