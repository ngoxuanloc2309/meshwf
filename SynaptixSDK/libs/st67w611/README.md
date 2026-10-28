# ST67W611 Wi-Fi Library

Reusable C library for ST67W611 modules connected through SPI.

The library exposes the stable st67w611_* interface and hides the
X-CUBE-ST67W61 vendor API, vendor spi_port_* symbols, Wi-Fi lifecycle,
state machine, callbacks, recovery, and diagnostics.

Pinned vendor dependency:

    Release: v1.3.0
    Commit:  572b72dcb0c5bf505c088536e8196bdb5011ba12

## Architecture

    Application / network manager
                |
            wifi_driver          GatewayV2 integration layer
                |
            st67w611              reusable core facade
                |
        private vendor bridge
                |
          st67w611_port           board SPI/GPIO/DMA/reset adapter
                |
              HAL / BSP

| Layer | Responsibility |
| --- | --- |
| st67w611 | lifecycle, state, vendor adapter, scan, errors, recovery |
| st67w611_port | SPI, DMA, CS, RDY, reset and board callbacks |
| st67w611_lwip | optional ST67-to-lwIP adapter for T02 |
| wifi_driver | GatewayV2 integration with network manager |
| wifi_netif | GatewayV2 lwIP netif integration |

Applications must not include or call W6X_*, W61_*, spi_port_*, or vendor
netif functions.

## Requirements

- C11 compiler and CMake 3.22 or newer.
- Platform HAL and FreeRTOS required by the pinned vendor package.
- X-CUBE-ST67W61 at the pinned version.
- A consumer board adapter implementing st67w611_port_ops_t.

The core does not contain board-specific SPI code. GatewayV2 provides its
adapter in ports/st67w611_port; another project should provide its own.

## CMake integration

    set(ST67W611_ARCH T02 CACHE STRING "ST67 architecture (T01 or T02)")

    add_subdirectory(libs/st67w611)
    add_subdirectory(ports/st67w611_port)

    target_link_libraries(my_application PRIVATE
        st67w611
        st67w611_port
    )

ST67W611_ARCH must be exactly T01 or T02.

For T02 with lwIP:

    add_subdirectory(libs/st67w611/adapters/lwip)
    target_link_libraries(my_application PRIVATE st67w611_lwip)

Override the vendor location when required:

    -DST67W611_VENDOR_DIR=/path/to/x-cube-st67w61

## Consumer port adapter

The consumer implements one st67w611_port_ops_t. This is the only place
that should include HAL, CubeMX headers, SPI handles, GPIO definitions, DMA
code, or board reset logic.

Mandatory callbacks:

    init
    deinit
    transfer
    transfer_dma
    cancel_transfer
    set_cs
    read_ready
    hardware_reset
    delay_ms

Optional callbacks:

    set_enable
    set_boot
    set_wakeup
    get_diagnostics

Contract:

- transfer() is blocking and returns after completion, error, or timeout.
- transfer_dma() only reports whether DMA was accepted.
- The completion callback is called exactly once for a live transaction.
- cancel_transfer() stops an active DMA transaction before recovery.
- hardware_reset() performs a real module reset.
- The port must not call application callbacks directly.
- The first version supports one active ST67W611 module per MCU.

The library provides the vendor-compatible spi_port_* implementation
internally. A consumer does not implement or call those functions.

## Minimal application setup

    #include "st67w611.h"
    #include "st67w611_port.h"  /* supplied by the consumer board adapter */

    extern const st67w611_port_ops_t st67w611_port_ops;

    static void wifi_event_callback(st67w611_event_t event,
                                    void *event_data,
                                    void *user_context)
    {
        (void)event_data;
        (void)user_context;

        switch (event) {
        case ST67W611_EVENT_CONNECTED:
            /* Link connected. */
            break;
        case ST67W611_EVENT_GOT_IP:
            /* IP configuration is available. */
            break;
        case ST67W611_EVENT_DISCONNECTED:
            /* Link disconnected. */
            break;
        case ST67W611_EVENT_ERROR:
            /* Inspect diagnostics. */
            break;
        default:
            break;
        }
    }

    static const st67w611_config_t wifi_config = {
        .port_ops = &st67w611_port_ops,
        .port_context = NULL,
        .event_cb = wifi_event_callback,
        .user_context = NULL,
        .capabilities = NULL,
        .recovery = {
            .max_transport_retries = 2U,
            .retry_delay_ms = 100U,
            .reset_timeout_ms = 3000U
        }
    };

    void wifi_init(void)
    {
        if (st67w611_init(&wifi_config) != ST67W611_OK) {
            return;
        }
        (void)st67w611_start();
    }

    void wifi_connect(void)
    {
        (void)st67w611_connect("MyNetwork", "MyPassword");
    }

st67w611_init() registers configuration and callbacks. It does not start the
vendor stack or connect to an access point. st67w611_start() brings the
module to ST67W611_STATE_READY.

## Lifecycle and events

    OFF -> INITIALIZING -> READY -> CONNECTING -> CONNECTED
                                               \-> READY
    ERROR -> RESETTING -> INITIALIZING -> READY

connect() is valid from READY. Exclusive operations return
ST67W611_ERR_BUSY when another operation is active.

Events are READY, CONNECTING, CONNECTED, GOT_IP, DISCONNECTED, and ERROR.
event_data is valid only during the callback; copy data that must be retained.

## Scan

    static void scan_callback(st67w611_status_t status,
                              const st67w611_scan_result_t *result,
                              void *context)
    {
        (void)context;
        if (status != ST67W611_OK) {
            /* Scan failed. */
            return;
        }
        if (result == NULL) {
            /* Scan completed. */
            return;
        }
        /* result contains SSID, RSSI, channel and MAC. */
    }

    (void)st67w611_scan(scan_callback, NULL);

Callback contract:

    status == OK, result != NULL -> one access point
    status == OK, result == NULL -> scan complete
    status != OK                 → scan failed

Results are copied by the library and are valid only during the callback.
Only one scan may be active at a time.

## T01 and T02

Select one architecture at configure time:

    -DST67W611_ARCH=T01
    -DST67W611_ARCH=T02

T01 keeps networking inside the ST67 vendor stack and does not require
st67w611_lwip.

T02 uses a host-side lwIP netif. The consumer owns the netif lifecycle:

    #include "st67w611_lwip.h"

    st67w611_lwip_callbacks_t callbacks = {
        .link_sta_up = on_wifi_link_up,
        .link_sta_down = on_wifi_link_down,
        .link_ap_up = NULL,
        .link_ap_down = NULL,
        .rx_sta_notify = NULL,
        .rx_ap_notify = NULL
    };

    (void)st67w611_lwip_attach(&wifi_netif, &callbacks);
    /* ... */
    (void)st67w611_lwip_detach(&wifi_netif);

For T02, IP and ping are available through st67w611_lwip_get_ip() and
st67w611_lwip_ping(). st67w611_get_rssi() returns
ST67W611_ERR_UNSUPPORTED for the pinned vendor release.

## Reset, recovery, and diagnostics

    st67w611_status_t status = st67w611_reset();

    st67w611_diagnostics_t diagnostics;
    (void)st67w611_get_diagnostics(&diagnostics);

Reset stops the vendor stack, performs hardware reset through the port,
registers callbacks again, restarts the vendor/Wi-Fi stack, and returns READY
or ERROR. Retries are bounded by max_transport_retries and retry_delay_ms.

Useful fields include state, last_error, state_transition_count,
callback_count, timeout_count, transport_error_count, spi_transfer_count,
spi_timeout_count, spi_error_count, dma_error_count, retry_count, and
reset_count.

## Error handling

Always check returned statuses. Use st67w611_status_string(status) for text.

| Status | Meaning |
| --- | --- |
| ST67W611_OK | Operation succeeded |
| ST67W611_ERR_INVALID_ARG | Invalid argument or missing port callback |
| ST67W611_ERR_INVALID_STATE | Lifecycle operation is not valid |
| ST67W611_ERR_NOT_READY | Module is not ready |
| ST67W611_ERR_BUSY | Another exclusive operation is active |
| ST67W611_ERR_TIMEOUT | Operation or transport timeout |
| ST67W611_ERR_TRANSPORT | SPI, DMA, or port failure |
| ST67W611_ERR_VENDOR | Vendor stack rejected the operation |
| ST67W611_ERR_UNSUPPORTED | Capability is unavailable |

## GatewayV2 integration

wifi_driver is not part of the reusable library. It maps st67w611 events
and operations to GatewayV2's network manager and application state.

    Only ST67 API      -> st67w611 + st67w611_port
    ST67 with lwIP     -> st67w611 + st67w611_port + st67w611_lwip
    GatewayV2 policy   -> add wifi_driver and wifi_netif

Do not move GatewayV2 failover, DHCP policy, scan JSON, or MQTT logic into
the reusable core.

## Porting checklist

- SPI is initialized before st67w611_start().
- CS polarity and timing match the module schematic.
- RDY GPIO and both edge notifications are wired.
- DMA completion and error callbacks call the registered completion callback.
- DMA cancellation stops the peripheral before recovery.
- D-cache maintenance is implemented where required.
- Hardware reset meets module timing requirements.
- All mandatory callbacks are non-NULL.
- Application code does not include vendor headers.
- Diagnostics remain stable during normal operation.

## Validation

Compile-boundary targets:

    st67w611_api_boundary_test
    st67w611_port_contract_test

GatewayV2 hardware acceptance:

    boot -> READY -> Wi-Fi connect -> DHCP -> DNS/MQTT
         -> scan -> disconnect -> reconnect -> hardware reset -> reconnect

Monitor heap, stack, SPI errors, DMA errors, timeouts, and retries. A
compile-only pass does not prove RF or DMA behavior on the target board.

## Troubleshooting

### Build failure

Check the vendor path, ST67W611_ARCH, st67w611_conf.h, and all mandatory
port callbacks.

### Module does not reach READY

Check SPI initialization order, CS polarity, RDY wiring, DMA/EXTI setup,
power/reset timing, and port diagnostics.

### Wi-Fi connects but no IP is received

For T02, attach the lwIP adapter before connecting and verify link-up and
DHCP handling.

### Internet access fails with a local IP

Verify that the Wi-Fi netif is the default route and software checksum
generation is enabled in lwIP.

### LTE errors

A7680C AT Timeout belongs to the LTE driver and is outside this library.
Use st67w611_get_diagnostics() for ST67 diagnostics.

## Vendor notice

Preserve the X-CUBE-ST67W61 license and notices when redistributing this
package. Do not modify the vendor working tree during the build.
