if(NOT ST67W611_ARCH STREQUAL "T02")
    return()
endif()

add_library(st67w611_lwip STATIC src/st67w611_lwip.c)
target_include_directories(st67w611_lwip PUBLIC include)
target_include_directories(st67w611_lwip PRIVATE internal)
target_include_directories(st67w611_lwip PRIVATE
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Api
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Core
    ${ST67W611_VENDOR_DIR}/Middlewares/ST/ST67W6X_Network_Driver/Driver/W61_bus
)
target_link_libraries(st67w611_lwip PUBLIC st67w611 middleware_lwip)
