// Isolation test, independent of the eth_w5100 driver. Set a free address on your network below.
//
// A: the chip's own TCP/IP answers ARP and ping, no SPI traffic
// B: MACRAW, no SPI traffic during the window, then the ring is read
// C: MACRAW, polled every 10 ms like the driver
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_SCLK 18
#define PIN_CS   17

static const uint8_t MAC[] = { 0x02, 0x57, 0x35, 0x31, 0x30, 0x30 };  // locally administered
static const uint8_t GATEWAY[] = { 192, 168, 18, 1 };
static const uint8_t SUBNET[] = { 255, 255, 255, 0 };
static const uint8_t ADDRESS[] = { 192, 168, 18, 250 };

static spi_device_handle_t dev;

static uint8_t xfer(uint8_t op, uint16_t a, uint8_t d)
{
    spi_transaction_t t = { .flags = SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA,
                            .length = 32,
                            .tx_data = { op, a >> 8, a & 0xff, d } };
    ESP_ERROR_CHECK(spi_device_polling_transmit(dev, &t));
    return t.rx_data[3];
}

static uint8_t rd(uint16_t a) { return xfer(0x0F, a, 0); }
static void wr(uint16_t a, uint8_t d) { xfer(0xF0, a, d); }

static void wrn(uint16_t a, const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) wr(a + i, p[i]);
}

static uint16_t rd16(uint16_t a)
{
    uint16_t h = rd(a);
    return h << 8 | rd(a + 1);
}

static void wr16(uint16_t a, uint16_t v)
{
    wr(a, v >> 8);
    wr(a + 1, v & 0xff);
}

static uint16_t rsr(void)
{
    uint16_t x, y;
    do {
        x = rd16(0x426);
        y = rd16(0x426);
    } while (x != y);
    return x;
}

static void cmd(uint8_t c)
{
    wr(0x401, c);
    while (rd(0x401)) {}
}

static void chip_init(void)
{
    wr(0, 0x80);
    while (rd(0)) {}
    vTaskDelay(pdMS_TO_TICKS(20));  // the PLL takes up to 10 ms to lock after a reset
    wr(0x1A, 3);
    wr(0x1B, 3);
    wrn(0x09, MAC, 6);
}

static void macraw_open(void)
{
    cmd(0x10);
    wr(0x402, 0xff);
    wr(0x400, 0x04);
    cmd(0x01);
    printf("SR=0x%02x\n", rd(0x403));
}

static int next_frame(void)
{
    uint16_t ptr = rd16(0x428);
    uint16_t hi = rd(0x6000 + (ptr & 0x1fff)), lo = rd(0x6000 + ((ptr + 1) & 0x1fff));
    uint16_t len = hi << 8 | lo;
    if (len < 16 || len > 1600) {
        printf("BAD header %u\n", len);
        return -1;
    }
    wr16(0x428, ptr + len);
    cmd(0x40);
    return len - 2;
}

void app_main(void)
{
    spi_bus_config_t bus = { .miso_io_num = PIN_MISO, .mosi_io_num = PIN_MOSI, .sclk_io_num = PIN_SCLK,
                             .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4 };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED));
    spi_device_interface_config_t di = { .clock_speed_hz = 4000000, .mode = 0, .spics_io_num = PIN_CS, .queue_size = 1 };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &di, &dev));

    chip_init();
    printf("RMSR=%d TMSR=%d\n", rd(0x1A), rd(0x1B));
    wrn(0x01, GATEWAY, 4);
    wrn(0x05, SUBNET, 4);
    wrn(0x0F, ADDRESS, 4);
    printf("PHASE_A\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(45000));

    chip_init();
    macraw_open();
    printf("PHASE_B\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(16000));
    printf("B rsr=%u frames:", rsr());
    for (int n = 200; n > 0 && rsr(); n--) {
        int l = next_frame();
        if (l < 0) break;
        printf(" %d", l);
    }
    printf("\nB_END\n");

    macraw_open();
    printf("PHASE_C\n");
    fflush(stdout);
    printf("C frames:");
    for (int i = 0; i < 1600; i++) {
        while (rsr()) {
            int l = next_frame();
            if (l < 0) break;
            printf(" %d", l);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("\nC_END\nDONE\n");
    fflush(stdout);
}
