
#pragma once

#include "esp_err.h"
#include "esp_eth_driver.h"
#include "esp_netif_types.h"
#include "eth-w5100-cfg.h"

struct eth_static_ip
{
	esp_netif_ip_info_t net;
	esp_ip4_addr_t p_dns;
	esp_ip4_addr_t s_dns;
	esp_ip4_addr_t f_dns;
};

struct eth_ifconfig
{
	char hostname[ 32 ];	   // Max 32 characters, empty keeps the esp_netif default
	struct eth_static_ip sip;  // Data is assumed to be in network order. Zero IP means DHCP.
	struct w5100_config_t w5100_cfg;
};

/**
 * Creates the Ethernet esp_netif, installs the W5100 MAC/PHY driver and starts it.
 * esp_netif_init() and esp_event_loop_create_default() must have been called before.
 *
 * @return ESP_OK, or the error of the step that failed (ESP_ERR_TIMEOUT / ESP_ERR_INVALID_RESPONSE when
 *         the chip does not answer on SPI, usually a wiring problem).
 */
esp_err_t eth_init( const struct eth_ifconfig *const cfg );
void eth_deinit( void );

esp_netif_t *eth_get_netif( void );
esp_eth_handle_t eth_get_handle( void );
