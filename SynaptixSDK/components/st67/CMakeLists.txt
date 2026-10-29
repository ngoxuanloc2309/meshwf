# components/st67 - MQTT client of the ST67W611 module (vendor AT driver), see st67_mqtt.h.
#
# The facade (libs/st67w611) is Wi-Fi only and does not build the vendor MQTT files, so this
# component compiles them itself with the same include paths and ST67_ARCH as the facade.
# ST67_ARCH arrives through the PUBLIC compile definition of the st67w611 target.
set(ST67_VENDOR_DRV "${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver")

add_library(sx_st67 STATIC
    st67_mqtt.c
    st67_cert_store.c
    ${ST67_VENDOR_DRV}/Core/w6x_mqtt.c
    ${ST67_VENDOR_DRV}/Driver/W61_at/w61_at_mqtt.c
)

target_include_directories(sx_st67 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

# Vendor headers stay private to this component (same list as libs/st67w611).
target_include_directories(sx_st67 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../libs/st67w611/inc
    ${CMAKE_CURRENT_LIST_DIR}/../../libs/st67w611/port/include
    ${ST67_VENDOR_DRV}/Api
    ${ST67_VENDOR_DRV}/Core
    ${ST67_VENDOR_DRV}/Core/Inc
    ${ST67_VENDOR_DRV}/Driver/W61_at
    ${ST67_VENDOR_DRV}/Driver/W61_bus
    ${ST67_VENDOR_DRV}/Utils/Logging
    ${ST67_VENDOR_DRV}/Utils/Misc
)

target_link_libraries(sx_st67 PUBLIC st67w611 splc PRIVATE stm32cubemx)