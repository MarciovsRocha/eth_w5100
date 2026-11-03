
#pragma once

#include "driver/spi_master.h"
#include "eth-w5100-cfg.h"

#include <stdbool.h>

// Ready-made lower layer (struct w5100_config_t) for a W5100 on an ESP32 SPI master bus.
// The bus itself (MISO/MOSI/SCLK) must be initialized with spi_bus_initialize() by the application.
struct w5100_spi_config
{
	spi_host_device_t host;
	int cs_gpio;			 // W5100 /SCS (D10 on Arduino Ethernet shields)
	int rst_gpio;			 // W5100 /RESET, -1 when not connected (a software reset is always done).
							 // Keep -1 on Arduino shields: their RESET pin is a 5 V supervisor input.
	bool rst_active_high;	 // true when /RESET is driven through an inverting transistor
	int spi_en_gpio;		 // W5100 SEN, -1 when the board derives it from /SCS (Arduino shields)
	int clock_hz;			 // W5100 accepts up to 14 MHz; keep it lower with long jumper wires
};

#define W5100_SPI_DEFAULT_CONFIG( spi_host, cs )                                                                                            \
	{                                                                                                                                       \
		.host = ( spi_host ), .cs_gpio = ( cs ), .rst_gpio = -1, .rst_active_high = false, .spi_en_gpio = -1, .clock_hz = 4 * 1000 * 1000 \
	}

/**
 * Fills ops with callbacks that talk to the W5100 described by cfg. Only one W5100 is supported.
 * The SPI device is added to the bus when the driver calls ops->init.
 */
void w5100_spi_get_ops( const struct w5100_spi_config *const cfg, struct w5100_config_t *const ops );
