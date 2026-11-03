// W5100 Ethernet example: brings the chip up over SPI, gets an IPv4 address by DHCP and pings the gateway.
// A ping that comes back proves both directions of the driver (TX and RX) end to end.
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "eth-w5100-spi.h"
#include "eth-w5100.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "ping/ping_sock.h"

#include <inttypes.h>

#define GOT_IP_BIT	  BIT0
#define PING_DONE_BIT BIT1

static const char *TAG = "example";
static EventGroupHandle_t s_events;
static esp_ip4_addr_t s_gateway;

static void on_eth_event( void *arg, esp_event_base_t base, int32_t id, void *data )
{
	switch ( id )
	{
		case ETHERNET_EVENT_CONNECTED:
			ESP_LOGI( TAG, "Ethernet link up" );
			break;
		case ETHERNET_EVENT_DISCONNECTED:
			ESP_LOGW( TAG, "Ethernet link down" );
			break;
		case ETHERNET_EVENT_START:
			ESP_LOGI( TAG, "Ethernet started" );
			break;
		case ETHERNET_EVENT_STOP:
			ESP_LOGI( TAG, "Ethernet stopped" );
			break;
		default:
			break;
	}
}

static void on_got_ip( void *arg, esp_event_base_t base, int32_t id, void *data )
{
	const ip_event_got_ip_t *event = data;

	ESP_LOGI( TAG, "IP " IPSTR ", mask " IPSTR ", gateway " IPSTR, IP2STR( &event->ip_info.ip ), IP2STR( &event->ip_info.netmask ), IP2STR( &event->ip_info.gw ) );
	s_gateway = event->ip_info.gw;
	xEventGroupSetBits( s_events, GOT_IP_BIT );
}

static void on_ping_success( esp_ping_handle_t hdl, void *args )
{
	uint32_t seqno, elapsed_ms;
	esp_ping_get_profile( hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof seqno );
	esp_ping_get_profile( hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof elapsed_ms );
	ESP_LOGI( TAG, "reply from " IPSTR ": seq=%" PRIu32 " time=%" PRIu32 " ms", IP2STR( &s_gateway ), seqno, elapsed_ms );
}

static void on_ping_timeout( esp_ping_handle_t hdl, void *args )
{
	uint32_t seqno;
	esp_ping_get_profile( hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof seqno );
	ESP_LOGW( TAG, "no reply from " IPSTR ": seq=%" PRIu32, IP2STR( &s_gateway ), seqno );
}

static void on_ping_end( esp_ping_handle_t hdl, void *args )
{
	uint32_t sent, received;
	esp_ping_get_profile( hdl, ESP_PING_PROF_REQUEST, &sent, sizeof sent );
	esp_ping_get_profile( hdl, ESP_PING_PROF_REPLY, &received, sizeof received );
	ESP_LOGI( TAG, "ping: %" PRIu32 " sent, %" PRIu32 " received", sent, received );
	*( uint32_t * )args = received;
	xEventGroupSetBits( s_events, PING_DONE_BIT );
}

void app_main( void )
{
	s_events = xEventGroupCreate();

	ESP_ERROR_CHECK( esp_netif_init() );
	ESP_ERROR_CHECK( esp_event_loop_create_default() );
	ESP_ERROR_CHECK( esp_event_handler_register( ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL ) );
	ESP_ERROR_CHECK( esp_event_handler_register( IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL ) );

	// Each W5100 access is one 4-byte frame, so no DMA is needed
	ESP_ERROR_CHECK( spi_bus_initialize(
		SPI3_HOST,
		&( const spi_bus_config_t ) {
			.miso_io_num = CONFIG_EXAMPLE_W5100_MISO_GPIO,
			.mosi_io_num = CONFIG_EXAMPLE_W5100_MOSI_GPIO,
			.sclk_io_num = CONFIG_EXAMPLE_W5100_SCLK_GPIO,
			.quadwp_io_num = -1,
			.quadhd_io_num = -1,
			.max_transfer_sz = 4 },
		SPI_DMA_DISABLED ) );

	struct w5100_spi_config spi = W5100_SPI_DEFAULT_CONFIG( SPI3_HOST, CONFIG_EXAMPLE_W5100_CS_GPIO );
	spi.rst_gpio = CONFIG_EXAMPLE_W5100_RST_GPIO;
	spi.clock_hz = CONFIG_EXAMPLE_W5100_SPI_CLOCK_KHZ * 1000;

	struct eth_ifconfig ifcfg = { .hostname = "w5100-example" };
	w5100_spi_get_ops( &spi, &ifcfg.w5100_cfg );

	esp_err_t err = eth_init( &ifcfg );
	if ( err != ESP_OK )
	{
		ESP_LOGE( TAG, "W5100 init failed: %s. Check the wiring in docs/wiring.md", esp_err_to_name( err ) );
		return;
	}

	ESP_LOGI( TAG, "waiting for DHCP..." );
	if ( !( xEventGroupWaitBits( s_events, GOT_IP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS( 30000 ) ) & GOT_IP_BIT ) )
	{
		ESP_LOGE( TAG, "no DHCP address in 30 s: check the cable and the router" );
		return;
	}

	uint32_t replies = 0;
	esp_ping_config_t ping_cfg = ESP_PING_DEFAULT_CONFIG();
	ping_cfg.count = CONFIG_EXAMPLE_PING_COUNT;
	ping_cfg.target_addr.type = IPADDR_TYPE_V4;
	ping_cfg.target_addr.u_addr.ip4.addr = s_gateway.addr;
	esp_ping_handle_t ping;
	ESP_ERROR_CHECK( esp_ping_new_session(
		&ping_cfg,
		&( const esp_ping_callbacks_t ) { .cb_args = &replies, .on_ping_success = on_ping_success, .on_ping_timeout = on_ping_timeout, .on_ping_end = on_ping_end },
		&ping ) );
	ESP_ERROR_CHECK( esp_ping_start( ping ) );
	xEventGroupWaitBits( s_events, PING_DONE_BIT, pdFALSE, pdTRUE, portMAX_DELAY );
	esp_ping_delete_session( ping );

	if ( replies )
		ESP_LOGI( TAG, "W5100 OK: TX and RX working" );
	else
		ESP_LOGE( TAG, "gateway did not answer: TX or RX path is not working" );
}
