
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct w5100_tx_part
{
	const uint8_t *buf;
	uint32_t len;
};

// Socket 0 in MACRAW mode. mac_filter: receive only frames addressed to our MAC and broadcasts.
esp_err_t w5100_socket_open( const bool mac_filter );
void w5100_socket_close( void );
// False once the chip has lost its configuration, for example after a power loss on the shield.
bool w5100_socket_is_open( void );

// Writes the parts back to back as one Ethernet frame and waits until the chip has sent it.
esp_err_t w5100_socket_send( const struct w5100_tx_part *const parts, const size_t count );

// ESP_OK and the length of the next frame, ESP_ERR_NOT_FOUND when none is waiting,
// ESP_ERR_INVALID_SIZE when the RX ring is out of sync (the socket must be reopened).
esp_err_t w5100_socket_rx_peek( uint16_t *const frame_len );
// Copies the frame announced by w5100_socket_rx_peek() into buf (NULL drops it) and frees its space in the RX ring.
esp_err_t w5100_socket_rx_consume( uint8_t *const buf, const uint16_t frame_len );
