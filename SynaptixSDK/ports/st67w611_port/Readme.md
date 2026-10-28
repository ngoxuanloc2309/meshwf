# st67w611_port — SPI / chân điều khiển ST67W611 cho board meshwf (STM32H523)

Cài đặt `st67w611_port_ops_t` của `libs/st67w611` (lấy từ repo `wg_ue`) trên SPI1.
Truyền SPI và chân CS đi qua `sx_spi` của simple_plc (`SynaptixSDK/libs/simple_plc`); RDY (EXTI), WAKEUP, EN, BOOT và đường DMA tuỳ chọn vẫn gọi HAL vì `sx_spi`/`sx_gpio` chưa có ngắt và DMA.
Không sửa file CubeMX nào. Lõi vendor (`libs/x-cube-st67w61`) và facade `libs/st67w611` nằm trong repo này. Dùng chế độ AT (không lwIP): `ST67W611_ARCH=T01`.

## File
| File | Việc |
|---|---|
| `st67w611_spi_port.h` | API công khai: `meshwf_st67_port_ops`, `meshwf_st67_port_on_rdy_edge()`, `meshwf_st67_port_get_pins()` |
| `st67w611_spi_port_conf.h` | Đấu chân và tuỳ chọn (mọi macro đều ghi đè được bằng `-D` hoặc `ST67_PORT_USER_CONF`) |
| `st67w611_spi_port.c` | Cài đặt |
| `st67w611_wifi_switch.{c,h}` | `meshwf_st67_wifi_switch(ssid, pw)`: đổi AP. Kiểm tham số TRƯỚC khi ngắt link; READY nối thẳng; CONNECTED ngắt rồi nối (ngắt lỗi thì trả lỗi, không nối); CONNECTING/ERROR cố ngắt, bỏ qua lỗi từ chối rồi nối |
| `st67w611_soft_reset.{c,h}` | `meshwf_st67_soft_reset()`: bọc mỏng `st67w611_soft_reset()` của facade (AT+RST, không cần chân EN) |
| `CMakeLists.txt` | Target `st67w611_port` (chưa cấu hình thử, top-level chưa add SynaptixSDK) |

## Dùng
```c
st67w611_config_t cfg = { .port_ops = &meshwf_st67_port_ops, /* ... */ };
st67w611_init(&cfg);   /* driver tự gọi ops->init() */
```
Thêm `st67w611_spi_port.c` vào target, include dir của port + `libs/st67w611/include` + `libs/st67w611/port/include` + `libs/simple_plc/components/{spi,gpio}`; target phải link `splc_platform_stm32h5` (cung cấp `sx_spi_*`, `sx_gpio_*`).
`MX_GPIO_Init()` và `MX_SPI1_Init()` phải chạy trước (CubeMX đã làm trước scheduler).

## Port tự xử lý những chỗ `.ioc` hiện tại chưa khớp vendor
| Vấn đề trong `.ioc` | Port làm gì | Tắt/đổi |
|---|---|---|
| PB0 (RDY) chỉ cạnh lên, vendor cần cả cạnh lên và xuống | `HAL_GPIO_Init` lại PB0 là `IT_RISING_FALLING`, đặt ưu tiên NVIC 5 (≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`, kiểm lúc biên dịch) | `ST67_RDY_RECONFIGURE` |
| Chưa có `EXTI0_IRQHandler` | Port định nghĩa `EXTI0_IRQHandler` | `ST67_DEFINE_RDY_IRQ_HANDLER=0` nếu sau này CubeMX sinh handler (nếu không: trùng ký hiệu khi link) |
| `HAL_GPIO_EXTI_*_Callback` | Port định nghĩa; nếu file khác đã định nghĩa thì đặt `ST67_DEFINE_EXTI_CALLBACKS=0` và gọi `meshwf_st67_port_on_rdy_edge()` | |
| SPI1 `MasterKeepIOState` tắt ⇒ SCK/MOSI thả nổi giữa các lần truyền khi CS đang tích cực | `sx_spi_init()` bật `MasterKeepIOState` rồi gọi `HAL_SPI_Init` (luôn bật, không còn tuỳ chọn `ST67_KEEP_SPI_IO_STATE`) | — |
| Tốc độ SPI | `ST67_SPI_CLOCK_HZ = 0` (mặc định) giữ nguyên CubeMX (16 MHz, mode 0); khác 0 thì `sx_spi` chọn bộ chia nhanh nhất không vượt giá trị này | `ST67_SPI_CLOCK_HZ`, `ST67_SPI_MODE` |
| SPI1 chưa gắn DMA (GPDMA ch0/1 đang của USART2) | `transfer_dma()` chạy polling rồi báo hoàn tất ngay (vendor chấp nhận) | `ST67_SPI_USE_DMA=1` sau khi gắn 2 kênh GPDMA cho SPI1_TX/RX trong CubeMX |
| Không có chân EN / BOOT | Đặt nhãn `ST67_EN` / `ST67_BOOT` cho chân đó trong CubeMX thì port tự dùng; thiếu EN thì `hardware_reset()` trả `-1` | |

**Board này không có EN ⇒ KHÔNG gọi `st67w611_reset()`** (nó `W6X_DeInit` rồi hardware reset thất bại, driver dừng hẳn và `start` không chạy lại được). Dùng `st67w611_soft_reset()`. Module treo cứng không trả lời AT thì chỉ chân EN cứu được.

**Chân PB1 `ST67W_32K`:** module đã có thạch anh ngoài nên không cần xung 32 kHz từ MCU; port không dùng chân này.

CS tích cực mức CAO (giống mẫu ST và `wg_ue`), khoảng nghỉ tối thiểu 2 µs giữa hai lần chọn, chờ 20 µs sau khi chọn — do `sx_spi_select/deselect` thực hiện (đếm bằng DWT; nếu DWT không chạy thì dùng vòng NOP, không treo).
Vì `sx_spi` chỉ có buffer dummy 256 B nên port vẫn giữ `s_zeros`/`s_sink` 2 KB để truyền RX-only/TX-only tới `ST67_SPI_MAX_XFER`.

## Đã kiểm
- Biên dịch ARM (`-Wall -Wextra -Wshadow`) trên HAL thật của meshwf + header thật của `libs/st67w611` (lấy từ `wg_ue`): 6 cấu hình (mặc định, CS tích cực thấp, `ST67_SPI_CLOCK_HZ`, EN+BOOT, DMA, tự giữ callback EXTI/không re-arm RDY) đều sạch. Mặc định: port 856 B flash + `sx_spi` 884 B, RAM 4 436 B (so với port gọi HAL trực tiếp trước đây: 1 044 B flash, 4 144 B RAM; chênh RAM chủ yếu là `sx_spi_t` 256 B dummy buffer không dùng tới).
- Harness PC (HAL/FreeRTOS giả, `sx_spi` thật, ASan+UBSan), 3 cấu hình (mặc định, 1 MHz, CS tích cực thấp): 69 kiểm tra, 0 lỗi, gồm: init lỗi thì port không dùng được, thứ tự CS idle trước khi thành output, RX-only 1536 B (dài hơn dummy của `sx_spi`), TX-only, giới hạn 2048 B, timeout/lỗi vào bộ đếm chẩn đoán, callback chỉ gọi khi thành công, polarity/setup/gap của CS, DWT không chạy vẫn thoát vòng chờ, deinit trả CS về mức nghỉ. 8 đột biến, mọi đột biến đều bị bắt (3 ca sống sót ở vòng đầu đã bổ sung test).

## CHƯA kiểm / chưa biết (không thể kiểm khi không có board)
- Chạy thật trên board: chưa. Mọi thứ trên là mô hình giả của HAL.
- Chân EN/RESET/BOOT của ST67 trên PCB (chưa có trong `.ioc`). Thiếu EN ⇒ không reset được module bằng phần cứng.
- Tốc độ SPI: CubeMX đặt prescaler 4 (16 MHz); `wg_ue` đang chạy prescaler 128 trên H563. Chưa biết 16 MHz có ổn trên PCB này.
- Cực CS và độ trễ 20 µs lấy từ ST/`wg_ue`, chưa đo trên PCB này.
- Với `ST67_SPI_USE_DMA=0`: `libs/st67w611/port/src/st67w611_vendor_bridge.c` khởi động timer 500 ms sau mỗi `transfer_dma` thành công. Nếu timer hết hạn đúng lúc một `transfer_dma` khác đang chạy (cỡ 1 ms với khung 2 KB), bridge sẽ huỷ nhầm và đánh dấu peer lỗi. Xác suất thấp, biến mất khi dùng DMA thật.
- Chưa nối vào CMake gốc, chưa link toàn bộ firmware (mới `-fsyntax-only` ARM từng file với vendor thật, `ST67_ARCH` 1 và 2).
- `st67w611_soft_reset()` / connect-sau-lỗi / `meshwf_st67_wifi_switch` mới chạy trên mô hình giả (vendor + RTOS giả, PC); chưa chạy module thật. Chưa biết: callback `APP_wifi_cb` còn nguyên sau `W6X_Reset` không; sau sai mật khẩu vendor có phát thêm DISCONNECTED không.
- `ST67_SPI_CLOCK_HZ`: khi khác 0, mode/bit order lấy từ `ST67_SPI_MODE` (mặc định mode 0, MSB) — khớp CubeMX hiện tại, chưa kiểm với module thật.