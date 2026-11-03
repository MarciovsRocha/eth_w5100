
#include "eth-w5100-spi.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"	// IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

// W5100 SPI frame: opcode (0xF0 write / 0x0F read), 16-bit address, data. /SCS must go high after every frame.
#define W_PCK( address, data ) ( __builtin_bswap32( 0xF0000000 | ( uint32_t )( address ) << 8 | ( data ) ) )
#define R_PCK( address )	   ( __builtin_bswap32( 0x0F000000 | ( uint32_t )( address ) << 8 ) )

static const char *const TAG = "w5100_spi";

static struct
{
	struct w5100_spi_config cfg;
	spi_device_handle_t dev;
	SemaphoreHandle_t lock;
} s_spi;

static void spi_lock( void )
{
	xSemaphoreTake( s_spi.lock, portMAX_DELAY );
	// SEN high enables SPI mode (datasheet 1.1). The W5100 does not tri-state MISO on /SCS alone, which is why
	// shared-bus designs (and Arduino shields, through a buffer on /SCS) drop SEN between accesses.
	if ( s_spi.cfg.spi_en_gpio >= 0 )
		gpio_set_level( s_spi.cfg.spi_en_gpio, 1 );
}

static void spi_unlock( void )
{
	if ( s_spi.cfg.spi_en_gpio >= 0 )
		gpio_set_level( s_spi.cfg.spi_en_gpio, 0 );
	xSemaphoreGive( s_spi.lock );
}

static uint32_t transfer( const uint32_t frame )
{
	spi_transaction_t trans = { .flags = SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA, .length = 32 };
	memcpy( trans.tx_data, &frame, 4 );
	ESP_ERROR_CHECK( spi_device_polling_transmit( s_spi.dev, &trans ) );
	return trans.rx_data[ 3 ];
}

static void w5100_spi_init( void )
{
	uint64_t outputs = 0;

	if ( s_spi.cfg.rst_gpio >= 0 )
		outputs |= BIT64( s_spi.cfg.rst_gpio );
	if ( s_spi.cfg.spi_en_gpio >= 0 )
		outputs |= BIT64( s_spi.cfg.spi_en_gpio );
	if ( outputs )
		ESP_ERROR_CHECK( gpio_config( &( const gpio_config_t ) { .pin_bit_mask = outputs, .mode = GPIO_MODE_OUTPUT } ) );
	// Keep the chip out of reset and SEN low until the first access
	if ( s_spi.cfg.rst_gpio >= 0 )
		gpio_set_level( s_spi.cfg.rst_gpio, s_spi.cfg.rst_active_high ? 0 : 1 );
	if ( s_spi.cfg.spi_en_gpio >= 0 )
		gpio_set_level( s_spi.cfg.spi_en_gpio, 0 );

	ESP_ERROR_CHECK( !( s_spi.lock = xSemaphoreCreateMutex() ) );
	ESP_ERROR_CHECK( spi_bus_add_device(
		s_spi.cfg.host,
		&( const spi_device_interface_config_t ) {
			.clock_speed_hz = s_spi.cfg.clock_hz,
			.mode = 0,
			.spics_io_num = s_spi.cfg.cs_gpio,
			.queue_size = 1 },
		&s_spi.dev ) );
	// The bus is dedicated to the W5100: owning it makes every polling transaction cheaper
	ESP_ERROR_CHECK( spi_device_acquire_bus( s_spi.dev, portMAX_DELAY ) );

	ESP_LOGI( TAG, "W5100 on SPI host %d, CS GPIO%d, RST GPIO%d, %d kHz", s_spi.cfg.host + 1, s_spi.cfg.cs_gpio, s_spi.cfg.rst_gpio, s_spi.cfg.clock_hz / 1000 );
}

static void w5100_spi_deinit( void )
{
	spi_lock();
	spi_device_release_bus( s_spi.dev );
	ESP_ERROR_CHECK( spi_bus_remove_device( s_spi.dev ) );
	s_spi.dev = NULL;
	spi_unlock();
	vSemaphoreDelete( s_spi.lock );
	s_spi.lock = NULL;
}

static void w5100_spi_hw_reset( void )
{
	if ( s_spi.cfg.rst_gpio < 0 )
		return;	 // the driver follows with a software reset (MR.RST)

	// /RESET low for at least 2 us, then up to 10 ms for the PLL to lock
	gpio_set_level( s_spi.cfg.rst_gpio, s_spi.cfg.rst_active_high ? 1 : 0 );
	vTaskDelay( pdMS_TO_TICKS( 2 ) + 1 );
	gpio_set_level( s_spi.cfg.rst_gpio, s_spi.cfg.rst_active_high ? 0 : 1 );
	vTaskDelay( pdMS_TO_TICKS( 10 ) + 1 );
}

static void w5100_spi_read( const uint16_t addr, uint8_t *const data_rx, const uint32_t size )
{
	spi_lock();
	for ( uint32_t i = 0; i < size; ++i )
		data_rx[ i ] = ( uint8_t )transfer( R_PCK( addr + i ) );
	spi_unlock();
}

static void w5100_spi_write( const uint16_t addr, const uint8_t *const data_tx, const uint32_t size )
{
	spi_lock();
	for ( uint32_t i = 0; i < size; ++i )
		transfer( W_PCK( addr + i, data_tx[ i ] ) );
	spi_unlock();
}

void w5100_spi_get_ops( const struct w5100_spi_config *const cfg, struct w5100_config_t *const ops )
{
	s_spi.cfg = *cfg;
	*ops = ( struct w5100_config_t ) {
		.init = w5100_spi_init,
		.deinit = w5100_spi_deinit,
		.ll_hw_reset = w5100_spi_hw_reset,
		.read = w5100_spi_read,
		.write = w5100_spi_write,
	};
}
