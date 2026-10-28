# app - the ONLY module that attaches SynaptixSDK to the firmware executable.
#
# IMPORTANT: plain signature (no PRIVATE/PUBLIC) on purpose. The CubeMX file
# cmake/stm32cubemx/CMakeLists.txt links ${CMAKE_PROJECT_NAME} with the plain
# signature, and CMake forbids mixing plain and keyword forms on one target.

target_link_libraries(${CMAKE_PROJECT_NAME}
    sx_libs
    sx_components
    sx_services
    sx_ports
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # FreeRTOS implementation of sx_os_*() (empty unless SX_OS_USE_FREERTOS = 1)
    $<TARGET_OBJECTS:splc_platform_freertos>
    # SimplePLC + ST67 tasks, started from __wrap_MX_FREERTOS_Init()
    $<TARGET_OBJECTS:meshwf_splc_port>
)

# main.c still calls MX_FREERTOS_Init(); the linker routes it to
# __wrap_MX_FREERTOS_Init() (ports/splc_port), which runs the CubeMX original
# first. No CubeMX file is edited.
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -Wl,--wrap=MX_FREERTOS_Init
)