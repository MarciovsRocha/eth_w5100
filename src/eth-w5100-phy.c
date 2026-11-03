
#include "esp_check.h"
#include "eth-w5100-if.h"

#include <stdlib.h>

// The W5100 integrates its PHY but its register map has no PHY status register (no link, speed or duplex
// bits, unlike the W5200/W5500). Speed and duplex come from the OPMODE2-0 straps, which Arduino shields tie
// to 000 (autonegotiation), so the negotiated result is only visible on the LINK/100M/FULLD LEDs.
// The link is therefore reported up, as 100 Mbps full duplex, once the driver starts.
typedef struct
{
	esp_eth_phy_t parent;
	esp_eth_mediator_t *eth;
	eth_link_t link;
} phy_w5100_t;

static const char *TAG = "w5100_eth_phy";

static esp_err_t ephy_w5100_set_mediator( esp_eth_phy_t *phy, esp_eth_mediator_t *mediator )
{
	ESP_RETURN_ON_FALSE( mediator, ESP_ERR_INVALID_ARG, TAG, "can't set mediator to null" );
	__containerof( phy, phy_w5100_t, parent )->eth = mediator;

	return ESP_OK;
}

static esp_err_t ephy_w5100_set_link( esp_eth_phy_t *phy, eth_link_t link )
{
	phy_w5100_t *w5100 = __containerof( phy, phy_w5100_t, parent );

	if ( w5100->link != link )
	{
		w5100->link = link;
		ESP_RETURN_ON_ERROR( w5100->eth->on_state_changed( w5100->eth, ETH_STATE_LINK, ( void * )link ), TAG, "change link failed" );
	}

	return ESP_OK;
}

static esp_err_t ephy_w5100_get_link( esp_eth_phy_t *phy )
{
	phy_w5100_t *w5100 = __containerof( phy, phy_w5100_t, parent );

	if ( w5100->link != ETH_LINK_UP )
	{
		ESP_RETURN_ON_ERROR( w5100->eth->on_state_changed( w5100->eth, ETH_STATE_SPEED, ( void * )ETH_SPEED_100M ), TAG, "change speed failed" );
		ESP_RETURN_ON_ERROR( w5100->eth->on_state_changed( w5100->eth, ETH_STATE_DUPLEX, ( void * )ETH_DUPLEX_FULL ), TAG, "change duplex failed" );
		return ephy_w5100_set_link( phy, ETH_LINK_UP );
	}

	return ESP_OK;
}

static esp_err_t ephy_w5100_reset( esp_eth_phy_t *phy )
{
	return ESP_OK;
}

static esp_err_t ephy_w5100_reset_hw( esp_eth_phy_t *phy )
{
	return ESP_OK;
}

// Autonegotiation runs in the chip and cannot be controlled over SPI
static esp_err_t ephy_w5100_autonego_ctrl( esp_eth_phy_t *phy, eth_phy_autoneg_cmd_t cmd, bool *autonego_en_stat )
{
	if ( autonego_en_stat )
		*autonego_en_stat = false;

	return ESP_OK;
}

static esp_err_t ephy_w5100_pwrctl( esp_eth_phy_t *phy, bool enable )
{
	return ESP_OK;
}

static esp_err_t ephy_w5100_set_addr( esp_eth_phy_t *phy, uint32_t addr )
{
	return ESP_OK;
}

static esp_err_t ephy_w5100_get_addr( esp_eth_phy_t *phy, uint32_t *addr )
{
	*addr = 0;
	return ESP_OK;
}

static esp_err_t ephy_w5100_del( esp_eth_phy_t *phy )
{
	free( __containerof( phy, phy_w5100_t, parent ) );

	return ESP_OK;
}

static esp_err_t ephy_w5100_init( esp_eth_phy_t *phy )
{
	__containerof( phy, phy_w5100_t, parent )->link = ETH_LINK_DOWN;
	return ESP_OK;
}

static esp_err_t ephy_w5100_deinit( esp_eth_phy_t *phy )
{
	return ESP_OK;
}

static esp_err_t ephy_w5100_advertise_pause_ability( esp_eth_phy_t *phy, uint32_t ability )
{
	return ESP_OK;
}

static esp_err_t ephy_w5100_loopback( esp_eth_phy_t *phy, bool enable )
{
	return ESP_OK;
}

static esp_err_t ephy_w5100_set_speed( esp_eth_phy_t *phy, eth_speed_t speed )
{
	return ESP_OK;
}

static esp_err_t ephy_w5100_set_duplex( esp_eth_phy_t *phy, eth_duplex_t duplex )
{
	return ESP_OK;
}

esp_eth_phy_t *esp_eth_phy_new_w5100( const eth_phy_config_t *const phy_config )
{
	phy_w5100_t *w5100 = calloc( 1, sizeof( phy_w5100_t ) );
	ESP_RETURN_ON_FALSE( w5100, NULL, TAG, "no mem for PHY instance" );
	w5100->link = ETH_LINK_DOWN;  // ETH_LINK_UP is 0, so calloc alone would start "up"
	w5100->parent.set_mediator = ephy_w5100_set_mediator;
	w5100->parent.reset = ephy_w5100_reset;
	w5100->parent.reset_hw = ephy_w5100_reset_hw;
	w5100->parent.init = ephy_w5100_init;
	w5100->parent.deinit = ephy_w5100_deinit;
	w5100->parent.autonego_ctrl = ephy_w5100_autonego_ctrl;
	w5100->parent.get_link = ephy_w5100_get_link;
	w5100->parent.set_link = ephy_w5100_set_link;
	w5100->parent.pwrctl = ephy_w5100_pwrctl;
	w5100->parent.set_addr = ephy_w5100_set_addr;
	w5100->parent.get_addr = ephy_w5100_get_addr;
	w5100->parent.advertise_pause_ability = ephy_w5100_advertise_pause_ability;
	w5100->parent.loopback = ephy_w5100_loopback;
	w5100->parent.set_speed = ephy_w5100_set_speed;
	w5100->parent.set_duplex = ephy_w5100_set_duplex;
	w5100->parent.del = ephy_w5100_del;

	return &( w5100->parent );
}
