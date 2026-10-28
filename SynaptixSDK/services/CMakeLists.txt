# services - middleware. Exposes one INTERFACE target: sx_services.
#
# services/mqtt (sx_mqtt) bridges SimplePLC and the module's MQTT client.
# services/http/sx_http.{c,h} is still an empty placeholder (what it will be is open).
# Same pattern as components/: one STATIC library per service, linked into sx_services.
add_subdirectory(mqtt)

add_library(sx_services INTERFACE)
target_link_libraries(sx_services INTERFACE sx_mqtt)