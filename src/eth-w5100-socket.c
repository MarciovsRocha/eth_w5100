
#include "eth-w5100-socket.h"

#include "esp_eth_spec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "eth-w5100-hal.h"
#include "eth-w5100-reg.h"

#include <inttypes.h>

static const char *const TAG = "w5100_socket";

static int64_t deadline_us( void )
{
	return esp_timer_get_time() + CONFIG_W5100_CMD_TIMEOUT_MS * 1000LL;
}

static uint16_t read_uint16_reg( const uint16_t addr )
{
	uint16_t reg;

	wiz_r_cont( addr, ( uint8_t *const )&reg, 2 );

	return __builtin_bswap16( reg );
}

static void write_uint16_reg( const uint16_t addr, const uint16_t data )
{
	wiz_w_cont( addr, ( const uint8_t *const )&( uint16_t ) { __builtin_bswap16( data ) }, 2 );
}

// The chip may update these 16-bit counters between the two byte reads: read until two reads agree
static uint16_t getS0_XX_XSR( const uint16_t addr )
{
	uint16_t first_read, second_read;

	do
		wiz_r_cont( addr, ( uint8_t *const )&first_read, 2 ), wiz_r_cont( addr, ( uint8_t *const )&second_read, 2 );
	while ( first_read != second_read );

	return __builtin_bswap16( first_read );
}

static esp_err_t exec_cmd( const uint8_t cmd )
{
	wiz_w_byte( S0_CR, cmd );

	const int64_t deadline = deadline_us();
	while ( wiz_r_byte( S0_CR ) )
		if ( esp_timer_get_time() > deadline )
		{
			ESP_LOGE( TAG, "socket command 0x%02x timed out", cmd );
			return ESP_ERR_TIMEOUT;
		}

	return ESP_OK;
}

static void write_tx_buf( const uint16_t ptr, const uint8_t *data, const uint16_t len )
{
	uint16_t dst_mask = ptr & SMASK;
	uint16_t dst_ptr = SBUFBASEADDRESS + dst_mask;

	if ( dst_mask + len <= SSIZE )
		wiz_w_cont( dst_ptr, data, len );
	else
	{
		uint16_t size = SSIZE - dst_mask;
		wiz_w_cont( dst_ptr, data, size );
		data += size;
		size = len - size;
		wiz_w_cont( SBUFBASEADDRESS, data, size );
	}
}

static void read_rx_buf( const uint16_t src_addr, uint8_t *dst, const uint16_t len )
{
	uint16_t src_mask = src_addr & RMASK;
	uint16_t real_src_addr = RBUFBASEADDRESS + src_mask;

	if ( src_mask + len <= RSIZE )
		wiz_r_cont( real_src_addr, dst, len );
	else
	{
		uint16_t size = RSIZE - src_mask;
		wiz_r_cont( real_src_addr, dst, size );
		dst += size;
		size = len - size;
		wiz_r_cont( RBUFBASEADDRESS, dst, size );
	}
}

esp_err_t w5100_socket_open( const bool mac_filter )
{
	// The datasheet only describes writing Sn_MR before OPEN, so the MAC filter is changed by reopening the socket
	for ( int attempt = 0; attempt < 3; ++attempt )
	{
		w5100_socket_close();
		wiz_w_byte( S0_MR, S0_MR_MACRAW | ( mac_filter ? S0_MR_MF : 0 ) );

		if ( exec_cmd( S0_CR_OPEN ) == ESP_OK && wiz_r_byte( S0_SR ) == SOCK_MACRAW )
			return ESP_OK;
	}

	ESP_LOGE( TAG, "could not open socket 0 in MACRAW mode (SR = 0x%02x)", wiz_r_byte( S0_SR ) );
	return ESP_FAIL;
}

void w5100_socket_close( void )
{
	exec_cmd( S0_CR_CLOSE );
	wiz_w_byte( S0_IR, 0xFF );
}

esp_err_t w5100_socket_send( const struct w5100_tx_part *const parts, const size_t count )
{
	uint32_t total = 0;

	for ( size_t i = 0; i < count; ++i )
		total += parts[ i ].len;

	// A MACRAW frame must go out in a single SEND, so it has to fit the TX buffer at once
	if ( !total || total > SSIZE )
		return ESP_ERR_INVALID_SIZE;

	const int64_t deadline = deadline_us();
	while ( getS0_XX_XSR( S0_TX_FSR0 ) < total )
		if ( esp_timer_get_time() > deadline )
		{
			ESP_LOGE( TAG, "no room in TX buffer for %" PRIu32 " bytes", total );
			return ESP_ERR_TIMEOUT;
		}

	uint16_t ptr = read_uint16_reg( S0_TX_WR0 );

	for ( size_t i = 0; i < count; ++i )
	{
		write_tx_buf( ptr, parts[ i ].buf, ( uint16_t )parts[ i ].len );
		ptr += parts[ i ].len;
	}

	write_uint16_reg( S0_TX_WR0, ptr );

	esp_err_t ret = exec_cmd( S0_CR_SEND );
	if ( ret != ESP_OK )
		return ret;

	while ( !( wiz_r_byte( S0_IR ) & S0_IR_SEND_OK ) )
		if ( esp_timer_get_time() > deadline )
		{
			ESP_LOGE( TAG, "SEND_OK not raised" );
			return ESP_ERR_TIMEOUT;
		}

	// Clear only our bit: the RX side never touches S0_IR
	wiz_w_byte( S0_IR, S0_IR_SEND_OK );

	return ESP_OK;
}

esp_err_t w5100_socket_rx_peek( uint16_t *const frame_len )
{
	const uint16_t received = getS0_XX_XSR( S0_RX_RSR0 );

	if ( !received )
		return ESP_ERR_NOT_FOUND;

	// Every MACRAW frame is preceded by its length, big endian, counting these 2 bytes
	uint16_t header;
	read_rx_buf( read_uint16_reg( S0_RX_RD0 ), ( uint8_t * )&header, 2 );
	header = __builtin_bswap16( header );

	if ( header < 2 + ETH_HEADER_LEN || header > 2 + ETH_MAX_PACKET_SIZE || header > received )
	{
		ESP_LOGW( TAG, "RX ring out of sync (frame header %u, %u bytes received)", header, received );
		return ESP_ERR_INVALID_SIZE;
	}

	*frame_len = header - 2;

	return ESP_OK;
}

esp_err_t w5100_socket_rx_consume( uint8_t *const buf, const uint16_t frame_len )
{
	const uint16_t ptr = read_uint16_reg( S0_RX_RD0 ) + 2;

	if ( buf )
		read_rx_buf( ptr, buf, frame_len );

	write_uint16_reg( S0_RX_RD0, ptr + frame_len );

	return exec_cmd( S0_CR_RECV );
}
