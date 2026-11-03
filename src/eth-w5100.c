
#include "eth-w5100.h"

#include "esp_check.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "eth-w5100-hal.h"
#include "eth-w5100-if.h"
#include "lwip/ip4_addr.h"

#include <stdlib.h>
#include <string.h>

static const char *const __unused TAG = "eth_main";

static struct
{
	esp_eth_config_t eth_config;
	esp_netif_t *eth_netif;
	esp_eth_handle_t eth_handle;
	esp_eth_netif_glue_handle_t eth_glue;
	esp_event_handler_instance_t hostname_evt_hdl;
	struct w5100_config_t w5100_cfg;
	bool ll_initialized;
	char *hostname;
} *eth_cfgs;

static esp_err_t eth_enable_static_ip( const struct eth_static_ip *const sip )
{
	esp_netif_ip_info_t old_ip_info;

	ESP_RETURN_ON_ERROR( esp_netif_get_ip_info( eth_cfgs->eth_netif, &old_ip_info ), TAG, "get ip info failed" );
	esp_err_t ret = esp_netif_dhcpc_stop( eth_cfgs->eth_netif );
	ESP_RETURN_ON_FALSE( ret == ESP_OK || ret == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED, ret, TAG, "dhcp client stop failed" );
	ESP_RETURN_ON_ERROR( esp_netif_set_old_ip_info( eth_cfgs->eth_netif, &old_ip_info ), TAG, "set old ip info failed" );
	ESP_RETURN_ON_ERROR( esp_netif_set_ip_info( eth_cfgs->eth_netif, &sip->net ), TAG, "set ip info failed" );

	if ( !ip4_addr_isany_val( sip->p_dns ) )
		ESP_RETURN_ON_ERROR(
			esp_netif_set_dns_info( eth_cfgs->eth_netif, ESP_NETIF_DNS_MAIN, &( esp_netif_dns_info_t ) { .ip.u_addr.ip4 = sip->p_dns } ),
			TAG,
			"set main dns failed" );
	if ( !ip4_addr_isany_val( sip->s_dns ) )
		ESP_RETURN_ON_ERROR(
			esp_netif_set_dns_info( eth_cfgs->eth_netif, ESP_NETIF_DNS_BACKUP, &( esp_netif_dns_info_t ) { .ip.u_addr.ip4 = sip->s_dns } ),
			TAG,
			"set backup dns failed" );
	if ( !ip4_addr_isany_val( sip->f_dns ) )
		ESP_RETURN_ON_ERROR(
			esp_netif_set_dns_info( eth_cfgs->eth_netif, ESP_NETIF_DNS_FALLBACK, &( esp_netif_dns_info_t ) { .ip.u_addr.ip4 = sip->f_dns } ),
			TAG,
			"set fallback dns failed" );

	ESP_LOGI(
		TAG,
		"Static IPv4: IP " IPSTR ", MASK " IPSTR ", GW " IPSTR,
		IP2STR( &sip->net.ip ),
		IP2STR( &sip->net.netmask ),
		IP2STR( &sip->net.gw ) );

	return ESP_OK;
}

static void eth_event_handler_hostname( void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data )
{
	if ( esp_netif_set_hostname( eth_cfgs->eth_netif, eth_cfgs->hostname ) != ESP_OK )
		ESP_LOGW( TAG, "could not set hostname '%s'", eth_cfgs->hostname );
}

void eth_deinit( void )
{
	if ( !eth_cfgs )
		return;

	if ( eth_cfgs->eth_handle )
	{
		esp_eth_stop( eth_cfgs->eth_handle );
		if ( eth_cfgs->eth_glue )
			esp_eth_del_netif_glue( eth_cfgs->eth_glue );
		ESP_ERROR_CHECK( esp_eth_driver_uninstall( eth_cfgs->eth_handle ) );
	}
	if ( eth_cfgs->eth_config.phy )
		eth_cfgs->eth_config.phy->del( eth_cfgs->eth_config.phy );
	if ( eth_cfgs->eth_config.mac )
		eth_cfgs->eth_config.mac->del( eth_cfgs->eth_config.mac );
	if ( eth_cfgs->hostname_evt_hdl )
		esp_event_handler_instance_unregister( ETH_EVENT, ETHERNET_EVENT_START, eth_cfgs->hostname_evt_hdl );
	if ( eth_cfgs->eth_netif )
		esp_netif_destroy( eth_cfgs->eth_netif );
	if ( eth_cfgs->ll_initialized )
		eth_cfgs->w5100_cfg.deinit();
	free( eth_cfgs->hostname );
	free( eth_cfgs );
	eth_cfgs = NULL;
}

esp_err_t eth_init( const struct eth_ifconfig *const cfg )
{
	esp_err_t ret = ESP_OK;

	ESP_RETURN_ON_FALSE( cfg, ESP_ERR_INVALID_ARG, TAG, "config can't be null" );
	ESP_RETURN_ON_FALSE(
		cfg->w5100_cfg.init && cfg->w5100_cfg.deinit && cfg->w5100_cfg.ll_hw_reset && cfg->w5100_cfg.read && cfg->w5100_cfg.write,
		ESP_ERR_INVALID_ARG,
		TAG,
		"all w5100_cfg callbacks are required" );
	ESP_RETURN_ON_FALSE( !eth_cfgs, ESP_ERR_INVALID_STATE, TAG, "already initialized" );
	ESP_RETURN_ON_FALSE( eth_cfgs = calloc( 1, sizeof *eth_cfgs ), ESP_ERR_NO_MEM, TAG, "no mem" );

	ESP_GOTO_ON_FALSE(
		eth_cfgs->eth_netif = esp_netif_new( &( esp_netif_config_t )ESP_NETIF_DEFAULT_ETH() ),
		ESP_FAIL,
		err,
		TAG,
		"esp_netif_new failed" );

	if ( cfg->hostname[ 0 ] )
	{
		ESP_GOTO_ON_FALSE( eth_cfgs->hostname = strndup( cfg->hostname, sizeof cfg->hostname ), ESP_ERR_NO_MEM, err, TAG, "no mem" );
		ESP_GOTO_ON_ERROR(
			esp_event_handler_instance_register(
				ETH_EVENT,
				ETHERNET_EVENT_START,
				&eth_event_handler_hostname,
				NULL,
				&eth_cfgs->hostname_evt_hdl ),
			err,
			TAG,
			"register hostname handler failed" );
	}

	if ( cfg->sip.net.ip.addr )
		ESP_GOTO_ON_ERROR( eth_enable_static_ip( &cfg->sip ), err, TAG, "static ip failed" );

	memcpy( &eth_cfgs->w5100_cfg, &cfg->w5100_cfg, sizeof cfg->w5100_cfg );
	pre_init_setup( &eth_cfgs->w5100_cfg );

	eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
	mac_config.rx_task_stack_size = CONFIG_W5100_RX_TASK_STACK_SIZE;
	mac_config.rx_task_prio = CONFIG_W5100_RX_TASK_PRIORITY;

	esp_eth_mac_t *mac = esp_eth_mac_new_w5100( &mac_config );
	esp_eth_phy_t *phy = esp_eth_phy_new_w5100( NULL );	 // No PHY pins connected
	eth_cfgs->eth_config = ( esp_eth_config_t )ETH_DEFAULT_CONFIG( mac, phy );
	ESP_GOTO_ON_FALSE( mac && phy, ESP_ERR_NO_MEM, err, TAG, "no mem for MAC/PHY" );

	eth_cfgs->w5100_cfg.init();
	eth_cfgs->ll_initialized = true;

	ESP_GOTO_ON_ERROR( esp_eth_driver_install( &eth_cfgs->eth_config, &eth_cfgs->eth_handle ), err, TAG, "driver install failed" );

	uint8_t mac_addr[ 6 ];
	ESP_GOTO_ON_ERROR( esp_read_mac( mac_addr, ESP_MAC_ETH ), err, TAG, "read mac failed" );
	ESP_GOTO_ON_ERROR( esp_eth_ioctl( eth_cfgs->eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr ), err, TAG, "set mac failed" );

	/* attach Ethernet driver to TCP/IP stack */
	ESP_GOTO_ON_FALSE( eth_cfgs->eth_glue = esp_eth_new_netif_glue( eth_cfgs->eth_handle ), ESP_ERR_NO_MEM, err, TAG, "netif glue failed" );
	ESP_GOTO_ON_ERROR( esp_netif_attach( eth_cfgs->eth_netif, eth_cfgs->eth_glue ), err, TAG, "netif attach failed" );
	/* start Ethernet driver state machine */
	ESP_GOTO_ON_ERROR( esp_eth_start( eth_cfgs->eth_handle ), err, TAG, "start failed" );

	return ESP_OK;

err:
	eth_deinit();
	return ret;
}

esp_netif_t *eth_get_netif( void )
{
	return eth_cfgs ? eth_cfgs->eth_netif : NULL;
}

esp_eth_handle_t eth_get_handle( void )
{
	return eth_cfgs ? eth_cfgs->eth_handle : NULL;
}
