#ifndef ST67W611_VENDOR_BRIDGE_H
#define ST67W611_VENDOR_BRIDGE_H

#include "st67w611.h"

void st67w611_vendor_bridge_configure(const st67w611_port_ops_t *ops,
                                      void *context);
void st67w611_vendor_bridge_ready_rising(void);
void st67w611_vendor_bridge_ready_falling(void);

#endif
