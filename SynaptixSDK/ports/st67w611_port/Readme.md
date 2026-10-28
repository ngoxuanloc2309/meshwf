# st67w611_port — SPI / chân điều khiển ST67W611 cho board meshwf (STM32H523)

Cài đặt `st67w611_port_ops_t` của `libs/st67w611` (lấy từ repo `wg_ue`) trên SPI1.
Không sửa file CubeMX nào. Lõi vendor (`x-cube-st67w61`, `libs/st67w611`) chưa nằm trong repo này.

## File
| File | Việc |
|---|---|
| `st67w611_spi_port.h` | API công khai: `meshwf_st67_port_ops`, `meshwf_st67_port_on_rdy_edge()`, `meshwf_st67_port_get_pins()` |
| `st67w611_spi_port_conf.h` | Đấu chân và tuỳ chọn (mọi macro đều ghi đè được bằng `-D` hoặc `ST67_PORT_USER_CONF`) |
| `st67w611_spi_port.c` | Cài đặt |

## Dùng
```c
st67w611_config_t cfg = { .port_ops = &meshwf_st67_port_ops, /* ... */ };
st67w611_init(&cfg);   /* driver tự gọi ops->init() */
```
Thêm `st67w611_spi_port.c` vào target, include dir của port + `libs/st67w611/include` + `libs/st67w611/port/include`.
`MX_GPIO_Init()` và `MX_SPI1_Init()` phải chạy trước (CubeMX đã làm trước scheduler).

## Port tự xử lý những chỗ `.ioc` hiện tại chưa khớp vendor
| Vấn đề trong `.ioc` | Port làm gì | Tắt/đổi |
|---|---|---|
| PB0 (RDY) chỉ cạnh lên, vendor cần cả cạnh lên và xuống | `HAL_GPIO_Init` lại PB0 là `IT_RISING_FALLING`, đặt ưu tiên NVIC 5 (≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`, kiểm lúc biên dịch) | `ST67_RDY_RECONFIGURE` |
| Chưa có `EXTI0_IRQHandler` | Port định nghĩa `EXTI0_IRQHandler` | `ST67_DEFINE_RDY_IRQ_HANDLER=0` nếu sau này CubeMX sinh handler (nếu không: trùng ký hiệu khi link) |
| `HAL_GPIO_EXTI_*_Callback` | Port định nghĩa; nếu file khác đã định nghĩa thì đặt `ST67_DEFINE_EXTI_CALLBACKS=0` và gọi `meshwf_st67_port_on_rdy_edge()` | |
| SPI1 `MasterKeepIOState` tắt ⇒ SCK/MOSI thả nổi giữa các lần truyền khi CS đang tích cực | Bật `SPI_CFG2.AFCNTR` lúc SPI đang tắt | `ST67_KEEP_SPI_IO_STATE` |
| SPI1 chưa gắn DMA (GPDMA ch0/1 đang của USART2) | `transfer_dma()` chạy polling rồi báo hoàn tất ngay (vendor chấp nhận) | `ST67_SPI_USE_DMA=1` sau khi gắn 2 kênh GPDMA cho SPI1_TX/RX trong CubeMX |
| Không có chân EN / BOOT | Đặt nhãn `ST67_EN` / `ST67_BOOT` cho chân đó trong CubeMX thì port tự dùng; thiếu EN thì `hardware_reset()` trả `-1` | |

CS tích cực mức CAO (giống mẫu ST và `wg_ue`), khoảng nghỉ tối thiểu 2 µs giữa hai lần chọn, chờ 20 µs sau khi chọn.

## Đã kiểm
- Biên dịch ARM (`-Wall -Wextra -Wshadow`) trên HAL thật của meshwf: 6 cấu hình (mặc định, DMA, có EN+BOOT, CS tích cực thấp, tự giữ callback EXTI, không re-arm RDY). Mặc định: 1,1 KB flash, 4,1 KB RAM (2 buffer 2 KB).
- Một file chứa cả `board_wifi_io.h` và conf của port: không xung đột macro (macro chân của port mang tiền tố `ST67SPI_`).
- Harness PC (HAL/FreeRTOS giả, ASan+UBSan), 4 cấu hình chính + 2 phụ: 76 đến 97 kiểm tra, 0 lỗi. 28 đột biến, mọi đột biến đều bị bắt ở cấu hình có biên dịch đoạn code đó.

## CHƯA kiểm / chưa biết (không thể kiểm khi không có board)
- Chạy thật trên board: chưa. Mọi thứ trên là mô hình giả của HAL.
- Chân EN/RESET/BOOT của ST67 trên PCB (chưa có trong `.ioc`). Thiếu EN ⇒ không reset được module bằng phần cứng.
- PB1 `ST67W_32K` đang là GPIO output thường, **không phát được xung**. Nếu module cần xung 32 kHz từ MCU thì phải chuyển sang chân timer/LSCO.
- Tốc độ SPI: CubeMX đặt prescaler 4 (16 MHz); `wg_ue` đang chạy prescaler 128 trên H563. Chưa biết 16 MHz có ổn trên PCB này.
- Cực CS và độ trễ 20 µs lấy từ ST/`wg_ue`, chưa đo trên PCB này.
- Với `ST67_SPI_USE_DMA=0`: `libs/st67w611/port/src/st67w611_vendor_bridge.c` khởi động timer 500 ms sau mỗi `transfer_dma` thành công. Nếu timer hết hạn đúng lúc một `transfer_dma` khác đang chạy (cỡ 1 ms với khung 2 KB), bridge sẽ huỷ nhầm và đánh dấu peer lỗi. Xác suất thấp, biến mất khi dùng DMA thật.
- Chưa nối vào CMake và chưa link với vendor (vendor chưa có trong repo).