# SynaptixSDK - firmware modules of the meshwf Wi-Fi IO board.
#
# The root CMakeLists.txt only does add_subdirectory(SynaptixSDK). Every module
# below defines its own targets; only app/ touches the firmware executable
# ${CMAKE_PROJECT_NAME}. Order matters: a module may link targets of the
# modules before it.
#
#   libs        third-party / vendored code   (st67w611, simple_plc)
#   components  chip-level drivers            (sx_rtc, ...)
#   services    middleware                    (sx_mqtt, sx_http, ...)
#   ports       board glue                    (st67w611_port, splc_port)
#   app         attaches everything to ${CMAKE_PROJECT_NAME}

add_subdirectory(libs)
add_subdirectory(components)
add_subdirectory(services)
add_subdirectory(ports)
add_subdirectory(app)