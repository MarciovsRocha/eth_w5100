// W5100 Ethernet self-test. Each stage needs the previous ones; the result of every stage is printed at the end.
//   1 SPI   the chip answers on the SPI bus (eth_init)
//   2 DHCP  address, gateway and DNS server obtained from the network
//   3 PING  the gateway answers ICMP: TX and RX of the driver work
//   4 DNS   a public name resolves: the DNS server is reachable
//   5 TCP   HTTP HEAD to that host: a TCP connection over the internet
//   6 MQTT  a message published to the broker comes back on our own subscription
// Afterwards the interface stays up, so the board can also be pinged from a PC.
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "eth-w5100-spi.h"
#include "eth-w5100.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mqtt_client.h"
#include "ping/ping_sock.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GOT_IP_BIT	   BIT0
#define PING_DONE_BIT  BIT1
#define MQTT_ECHO_BIT  BIT2
#define MQTT_ERROR_BIT BIT3

enum stage { ST_SPI, ST_DHCP, ST_PING, ST_DNS, ST_TCP, ST_MQTT, ST_COUNT };

static const char *const STAGE_NAMES[ ST_COUNT ] = { "SPI", "DHCP", "PING", "DNS", "TCP", "MQTT" };

static struct
{
	const char *status;	 // PASS, FAIL or SKIP
	char detail[ 96 ];
} s_results[ ST_COUNT ];

static const char *TAG = "selftest";
static EventGroupHandle_t s_events;
static esp_netif_ip_info_t s_ip_info;
static char s_topic[ 48 ], s_payload[ 48 ];

static void result( enum stage st, const char *status, const char *fmt, ... )
{
	va_list args;
	va_start( args, fmt );
	vsnprintf( s_results[ st ].detail, sizeof s_results[ st ].detail, fmt, args );
	va_end( args );
	s_results[ st ].status = status;
	ESP_LOGI( TAG, "[%s] %s: %s", STAGE_NAMES[ st ], status, s_results[ st ].detail );
}

static void on_eth_event( void *arg, esp_event_base_t base, int32_t id, void *data )
{
	if ( id == ETHERNET_EVENT_CONNECTED )
		ESP_LOGI( TAG, "Ethernet link up" );
	else if ( id == ETHERNET_EVENT_DISCONNECTED )
		ESP_LOGW( TAG, "Ethernet link down" );
}

static void on_got_ip( void *arg, esp_event_base_t base, int32_t id, void *data )
{
	s_ip_info = ( ( const ip_event_got_ip_t * )data )->ip_info;
	xEventGroupSetBits( s_events, GOT_IP_BIT );
}

static void on_ping_end( esp_ping_handle_t hdl, void *args )
{
	uint32_t received;
	esp_ping_get_profile( hdl, ESP_PING_PROF_REPLY, &received, sizeof received );
	*( uint32_t * )args = received;
	xEventGroupSetBits( s_events, PING_DONE_BIT );
}

static void on_ping_success( esp_ping_handle_t hdl, void *args )
{
	uint32_t seqno, elapsed_ms;
	esp_ping_get_profile( hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof seqno );
	esp_ping_get_profile( hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof elapsed_ms );
	ESP_LOGI( TAG, "  reply seq=%" PRIu32 " time=%" PRIu32 " ms", seqno, elapsed_ms );
}

static void on_ping_timeout( esp_ping_handle_t hdl, void *args )
{
	uint32_t seqno;
	esp_ping_get_profile( hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof seqno );
	ESP_LOGW( TAG, "  no reply seq=%" PRIu32, seqno );
}

static bool stage_spi( void )
{
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

	struct eth_ifconfig ifcfg = { .hostname = "w5100-selftest" };
	w5100_spi_get_ops( &spi, &ifcfg.w5100_cfg );

	esp_err_t err = eth_init( &ifcfg );
	if ( err != ESP_OK )
	{
		result( ST_SPI, "FAIL", "%s: check wiring (docs/wiring.md), then lower the SPI clock", esp_err_to_name( err ) );
		return false;
	}

	result( ST_SPI, "PASS", "W5100 answered at %d kHz", CONFIG_EXAMPLE_W5100_SPI_CLOCK_KHZ );
	return true;
}

static bool stage_dhcp( void )
{
	if ( !( xEventGroupWaitBits( s_events, GOT_IP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS( 30000 ) ) & GOT_IP_BIT ) )
	{
		result( ST_DHCP, "FAIL", "no address in 30 s: check the cable, the LINK LED and the router's DHCP" );
		return false;
	}

	esp_netif_dns_info_t dns = { 0 };
	esp_netif_get_dns_info( eth_get_netif(), ESP_NETIF_DNS_MAIN, &dns );
	result(
		ST_DHCP,
		"PASS",
		"IP " IPSTR " GW " IPSTR " DNS " IPSTR,
		IP2STR( &s_ip_info.ip ),
		IP2STR( &s_ip_info.gw ),
		IP2STR( &dns.ip.u_addr.ip4 ) );
	return true;
}

static bool stage_ping( void )
{
	uint32_t replies = 0;
	esp_ping_config_t ping_cfg = ESP_PING_DEFAULT_CONFIG();
	ping_cfg.count = CONFIG_EXAMPLE_PING_COUNT;
	ping_cfg.target_addr.type = IPADDR_TYPE_V4;
	ping_cfg.target_addr.u_addr.ip4.addr = s_ip_info.gw.addr;

	esp_ping_handle_t ping;
	ESP_ERROR_CHECK( esp_ping_new_session(
		&ping_cfg,
		&( const esp_ping_callbacks_t ) { .cb_args = &replies, .on_ping_success = on_ping_success, .on_ping_timeout = on_ping_timeout, .on_ping_end = on_ping_end },
		&ping ) );
	ESP_ERROR_CHECK( esp_ping_start( ping ) );
	xEventGroupWaitBits( s_events, PING_DONE_BIT, pdFALSE, pdTRUE, portMAX_DELAY );
	esp_ping_delete_session( ping );

	if ( !replies )
	{
		result( ST_PING, "FAIL", "gateway " IPSTR " did not answer: TX or RX path not working", IP2STR( &s_ip_info.gw ) );
		return false;
	}

	result( ST_PING, "PASS", "%" PRIu32 "/%d replies from " IPSTR, replies, CONFIG_EXAMPLE_PING_COUNT, IP2STR( &s_ip_info.gw ) );
	return true;
}

static bool stage_dns( struct sockaddr_in *const addr )
{
	const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct addrinfo *res = NULL;
	const int err = getaddrinfo( CONFIG_EXAMPLE_TEST_HOST, "80", &hints, &res );

	if ( err || !res )
	{
		result( ST_DNS, "FAIL", "'%s' did not resolve (error %d): no internet or no DNS server", CONFIG_EXAMPLE_TEST_HOST, err );
		return false;
	}

	*addr = *( const struct sockaddr_in * )res->ai_addr;
	freeaddrinfo( res );
	result( ST_DNS, "PASS", "%s = %s", CONFIG_EXAMPLE_TEST_HOST, inet_ntoa( addr->sin_addr ) );
	return true;
}

static void stage_tcp( const struct sockaddr_in *const addr )
{
	const int sock = socket( AF_INET, SOCK_STREAM, IPPROTO_IP );
	const struct timeval timeout = { .tv_sec = 10 };
	char response[ 64 ] = { 0 };

	setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout );
	setsockopt( sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout );

	if ( connect( sock, ( const struct sockaddr * )addr, sizeof *addr ) != 0 )
	{
		result( ST_TCP, "FAIL", "connect to %s:80 failed (errno %d)", CONFIG_EXAMPLE_TEST_HOST, errno );
		close( sock );
		return;
	}

	char request[ 128 ];
	const int len = snprintf( request, sizeof request, "HEAD / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", CONFIG_EXAMPLE_TEST_HOST );
	const int64_t start_us = esp_timer_get_time();
	const bool sent = send( sock, request, len, 0 ) == len;
	const int received = sent ? recv( sock, response, sizeof response - 1, 0 ) : -1;
	const int elapsed_ms = ( int )( ( esp_timer_get_time() - start_us ) / 1000 );
	close( sock );

	if ( received <= 0 || strncmp( response, "HTTP/", 5 ) )
	{
		result( ST_TCP, "FAIL", "no HTTP answer from %s (errno %d)", CONFIG_EXAMPLE_TEST_HOST, errno );
		return;
	}

	response[ strcspn( response, "\r\n" ) ] = '\0';
	result( ST_TCP, "PASS", "'%s' in %d ms", response, elapsed_ms );
}

static void on_mqtt( void *arg, esp_event_base_t base, int32_t id, void *data )
{
	const esp_mqtt_event_handle_t ev = data;

	switch ( ( esp_mqtt_event_id_t )id )
	{
		case MQTT_EVENT_CONNECTED:
			ESP_LOGI( TAG, "  connected, subscribing to %s", s_topic );
			esp_mqtt_client_subscribe_single( ev->client, s_topic, 1 );
			break;
		case MQTT_EVENT_SUBSCRIBED:
			ESP_LOGI( TAG, "  subscribed, publishing '%s'", s_payload );
			esp_mqtt_client_publish( ev->client, s_topic, s_payload, 0, 1, 0 );
			break;
		case MQTT_EVENT_DATA:
			if ( ev->topic_len == ( int )strlen( s_topic ) && !memcmp( ev->topic, s_topic, ev->topic_len ) &&
				 ev->data_len == ( int )strlen( s_payload ) && !memcmp( ev->data, s_payload, ev->data_len ) )
				xEventGroupSetBits( s_events, MQTT_ECHO_BIT );
			break;
		case MQTT_EVENT_ERROR:
			xEventGroupSetBits( s_events, MQTT_ERROR_BIT );
			break;
		default:
			break;
	}
}

static void stage_mqtt( void )
{
	if ( !CONFIG_EXAMPLE_MQTT_URI[ 0 ] )
	{
		result( ST_MQTT, "SKIP", "no broker configured (Example configuration -> MQTT broker URI)" );
		return;
	}

	// A topic per board, so several boards (or a public broker) do not mix their messages
	uint8_t mac[ 6 ];
	esp_read_mac( mac, ESP_MAC_ETH );
	snprintf( s_topic, sizeof s_topic, "eth_w5100/selftest/%02x%02x%02x%02x%02x%02x", mac[ 0 ], mac[ 1 ], mac[ 2 ], mac[ 3 ], mac[ 4 ], mac[ 5 ] );
	snprintf( s_payload, sizeof s_payload, "hello from W5100 at %" PRId64 " ms", esp_timer_get_time() / 1000 );

	const esp_mqtt_client_config_t cfg = { .broker.address.uri = CONFIG_EXAMPLE_MQTT_URI };
	esp_mqtt_client_handle_t client = esp_mqtt_client_init( &cfg );
	if ( !client )
	{
		result( ST_MQTT, "FAIL", "invalid broker URI '%s'", CONFIG_EXAMPLE_MQTT_URI );
		return;
	}

	ESP_LOGI( TAG, "  broker %s", CONFIG_EXAMPLE_MQTT_URI );
	esp_mqtt_client_register_event( client, ESP_EVENT_ANY_ID, on_mqtt, NULL );
	const int64_t start_us = esp_timer_get_time();
	esp_mqtt_client_start( client );

	const EventBits_t bits = xEventGroupWaitBits( s_events, MQTT_ECHO_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS( 20000 ) );
	const int elapsed_ms = ( int )( ( esp_timer_get_time() - start_us ) / 1000 );

	esp_mqtt_client_stop( client );
	esp_mqtt_client_destroy( client );

	if ( bits & MQTT_ECHO_BIT )
		result( ST_MQTT, "PASS", "message came back on %s in %d ms", s_topic, elapsed_ms );
	else
		result(
			ST_MQTT,
			"FAIL",
			"%s in 20 s: check the broker URI, port 1883 and the broker's access rules",
			( xEventGroupGetBits( s_events ) & MQTT_ERROR_BIT ) ? "connection error" : "no echo" );
}

void app_main( void )
{
	s_events = xEventGroupCreate();
	for ( int i = 0; i < ST_COUNT; ++i )
	{
		s_results[ i ].status = "SKIP";
		strcpy( s_results[ i ].detail, "previous stage failed" );
	}

	ESP_ERROR_CHECK( esp_netif_init() );
	ESP_ERROR_CHECK( esp_event_loop_create_default() );
	ESP_ERROR_CHECK( esp_event_handler_register( ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL ) );
	ESP_ERROR_CHECK( esp_event_handler_register( IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL ) );

	ESP_LOGI( TAG, "W5100 self-test: SCLK %d MISO %d MOSI %d CS %d RST %d", CONFIG_EXAMPLE_W5100_SCLK_GPIO, CONFIG_EXAMPLE_W5100_MISO_GPIO,
			  CONFIG_EXAMPLE_W5100_MOSI_GPIO, CONFIG_EXAMPLE_W5100_CS_GPIO, CONFIG_EXAMPLE_W5100_RST_GPIO );

	if ( stage_spi() && stage_dhcp() )
	{
		struct sockaddr_in addr;

		stage_ping();
		if ( stage_dns( &addr ) )
			stage_tcp( &addr );
		stage_mqtt();
	}

	int failed = 0;
	ESP_LOGI( TAG, "==================== W5100 SELF-TEST ====================" );
	for ( int i = 0; i < ST_COUNT; ++i )
	{
		ESP_LOGI( TAG, "%d %-4s %-4s %s", i + 1, STAGE_NAMES[ i ], s_results[ i ].status, s_results[ i ].detail );
		failed += !strcmp( s_results[ i ].status, "FAIL" );
	}
	if ( failed )
		ESP_LOGE( TAG, "RESULT: %d stage(s) FAILED", failed );
	else
		ESP_LOGI( TAG, "RESULT: all stages passed" );

	if ( !strcmp( s_results[ ST_DHCP ].status, "PASS" ) )
		ESP_LOGI( TAG, "Interface stays up: try 'ping " IPSTR "' from a PC on the same network", IP2STR( &s_ip_info.ip ) );
}
