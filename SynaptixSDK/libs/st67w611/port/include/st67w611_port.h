#ifndef ST67W611_PORT_H
#define ST67W611_PORT_H

#include "st67w611.h"

void st67w611_port_configure(const st67w611_port_ops_t *ops, void *context);
void st67w611_port_get_diagnostics(st67w611_port_diagnostics_t *diagnostics);

#endif
