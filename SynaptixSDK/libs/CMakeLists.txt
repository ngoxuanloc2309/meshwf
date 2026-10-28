# libs - third-party / vendored code. Exposes one INTERFACE target: sx_libs.

# --- ST67W611 Wi-Fi driver: AT mode, NO lwIP (decision 58) ------------------
# T01 = the module runs the TCP/IP stack and sockets (AT). FORCE: the library
# default is T02 (lwIP), which this board has no Flash for.
set(ST67W611_ARCH "T01" CACHE STRING "ST67 architecture (T01 or T02)" FORCE)
add_subdirectory(st67w611)

# --- SimplePLC: library mode ------------------------------------------------
# Target `splc` (INTERFACE). The product options live in
# ports/splc_port/splcopts.h, not in the library (lwIP-style).
set(SPLC_AS_LIBRARY ON CACHE BOOL "Build SimplePLC as a library for a host firmware" FORCE)
set(SPLC_OPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../ports/splc_port" CACHE PATH
    "Directory containing the product's splcopts.h" FORCE)
add_subdirectory(simple_plc)

# libs/nanomodbus (host copy) is not used yet: SimplePLC links its own renamed
# splc_nanomodbus. Add it here when the board needs a Modbus master of its own.

add_library(sx_libs INTERFACE)
target_link_libraries(sx_libs INTERFACE st67w611 splc)