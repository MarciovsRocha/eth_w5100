
#include "eth-w5100-ctrl.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "eth-w5100-hal.h"
#include "eth-w5100-reg.h"
#include "freertos/FreeRTOS.h"	// IWYU pragma: keep
#include "freertos/task.h"

static const char *const TAG = "w5100_ctrl";

esp_err_t w5100_init( void )
{
	w5100_internal_init();
	wiz_w_byte( MR0, MR_RST );

	const int64_t deadline = esp_timer_get_time() + CONFIG_W5100_CMD_TIMEOUT_MS * 1000LL;
	while ( wiz_r_byte( MR0 ) )
		if ( esp_timer_get_time() > deadline )
		{
			ESP_LOGE( TAG, "W5100 did not leave reset (MR = 0x%02x). Check MISO, power and /RESET", wiz_r_byte( MR0 ) );
			return ESP_ERR_TIMEOUT;
		}

	// The reset also restarts the PLL, which takes up to 10 ms to lock (datasheet section 7)
	vTaskDelay( pdMS_TO_TICKS( 10 ) + 1 );

	// All 8 KB of RX and TX memory to socket 0, the only one used in MACRAW mode
	wiz_w_byte( RMSR, 3 );
	wiz_w_byte( TMSR, 3 );

	// A floating or shorted MISO reads 0x00/0xFF: the register read back catches a chip that is not there
	const uint8_t rmsr = wiz_r_byte( RMSR ), tmsr = wiz_r_byte( TMSR );
	if ( rmsr != 3 || tmsr != 3 )
	{
		ESP_LOGE( TAG, "W5100 not answering on SPI (RMSR = 0x%02x, TMSR = 0x%02x). Check wiring, CS and clock", rmsr, tmsr );
		return ESP_ERR_INVALID_RESPONSE;
	}

	return ESP_OK;
}

void w5100_setMAC( const uint8_t *const mac )
{
	wiz_w_cont( SHAR0, mac, 6 );
}
