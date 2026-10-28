# components - chip-level drivers. Exposes one INTERFACE target: sx_components.
#
# Empty for now. components/rtc/sx_rtc.{c,h} are empty placeholders: the external
# RTC (I2C1) hooks of decision 57 (get_epoch / set_epoch / is_synced +
# plc_rtc_register()) go here. When they exist:
#
#   add_library(sx_rtc STATIC rtc/sx_rtc.c)
#   target_include_directories(sx_rtc PUBLIC ${CMAKE_CURRENT_LIST_DIR}/rtc)
#   target_link_libraries(sx_rtc PUBLIC splc PRIVATE stm32cubemx)
#   target_link_libraries(sx_components INTERFACE sx_rtc)
add_subdirectory(st67)

add_library(sx_components INTERFACE)
target_link_libraries(sx_components INTERFACE sx_st67)