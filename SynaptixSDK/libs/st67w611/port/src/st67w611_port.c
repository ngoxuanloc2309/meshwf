#include "st67w611_port.h"
#include "st67w611_vendor_bridge.h"
#include <string.h>

static const st67w611_port_ops_t *port_ops;
static void *port_context;
static st67w611_port_diagnostics_t diagnostics;

void st67w611_port_configure(const st67w611_port_ops_t *ops, void *context)
{
    port_ops = ops;
    port_context = context;
    memset(&diagnostics, 0, sizeof(diagnostics));
    st67w611_vendor_bridge_configure(ops, context);
}

void st67w611_port_get_diagnostics(st67w611_port_diagnostics_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (port_ops != NULL && port_ops->get_diagnostics != NULL) {
        (void)port_ops->get_diagnostics(port_context, out);
    }
}
