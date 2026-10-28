# services - middleware. Exposes one INTERFACE target: sx_services.
#
# Empty for now. services/mqtt/sx_mqtt.{c,h} and services/http/sx_http.{c,h} are
# empty placeholders (what they will be is still open). Same pattern as
# components/: one STATIC library per service, linked into sx_services.
add_library(sx_services INTERFACE)