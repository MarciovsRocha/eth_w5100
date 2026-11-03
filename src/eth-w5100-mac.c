
#include "esp_check.h"
#include "esp_log.h"
#include "eth-w5100-ctrl.h"
#include "eth-w5100-if.h"
#include "eth-w5100-socket.h"
#include "freertos/FreeRTOS.h"	// IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define W5100_TSK_RUN	  ( ( uint32_t )0 )
#define W5100_TSK_HOLD_ON ( ( uint32_t )1 )
#define W5100_TSK_GO_ON	  ( ( uint32_t )2 )
#define W5100_TSK_DELETE  ( ( uint32_t )3 )

#define W5100_RX_BURST	   8  // frames read per poll before the task yields
#define W5100_TX_MAX_PARTS 8  // buffers accepted by transmit_ctrl_vargs

#if CONFIG_W5100_RX_TASK_CORE_0
#define W5100_RX_TASK_CORE 0
#elif CONFIG_W5100_RX_TASK_CORE_1
#define W5100_RX_TASK_CORE 1
#else
#define W5100_RX_TASK_CORE tskNO_AFFINITY
#endif

typedef struct
{
	esp_eth_mac_t parent;
	esp_eth_mediator_t *eth;
	TaskHandle_t rx_task_hdl;
	SemaphoreHandle_t rx_task_exited;
	SemaphoreHandle_t lock;	 // socket 0 is shared by the RX task and the lwIP TX path
	uint32_t rx_task_stack_size;
	uint32_t rx_task_prio;
	uint8_t addr[ 6 ];
	bool promiscuous;
	bool socket_open;
	bool multicast_warned;
} emac_w5100_t;

static const char *TAG = "w5100_eth_mac";

static void lock( emac_w5100_t *emac )
{
	xSemaphoreTake( emac->lock, portMAX_DELAY );
}

static void unlock( emac_w5100_t *emac )
{
	xSemaphoreGive( emac->lock );
}

// Call with the lock held
static esp_err_t reopen_socket( emac_w5100_t *emac )
{
	esp_err_t ret = w5100_socket_open( !emac->promiscuous );
	emac->socket_open = ret == ESP_OK;
	return ret;
}

static esp_err_t emac_w5100_write_phy_reg( esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t reg_value )
{
	return ESP_OK;
}

static esp_err_t emac_w5100_read_phy_reg( esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t *reg_value )
{
	*reg_value = 0;
	return ESP_OK;
}

static esp_err_t emac_w5100_set_mediator( esp_eth_mac_t *mac, esp_eth_mediator_t *eth )
{
	ESP_RETURN_ON_FALSE( eth, ESP_ERR_INVALID_ARG, TAG, "can't set mac's mediator to null" );

	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	emac->eth = eth;

	return ESP_OK;
}

static esp_err_t emac_w5100_start( esp_eth_mac_t *mac )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	return pdPASS == xTaskNotify( emac->rx_task_hdl, W5100_TSK_GO_ON, eSetValueWithOverwrite ) ? ESP_OK : ESP_FAIL;
}

static esp_err_t emac_w5100_stop( esp_eth_mac_t *mac )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	return pdPASS == xTaskNotify( emac->rx_task_hdl, W5100_TSK_HOLD_ON, eSetValueWithOverwrite ) ? ESP_OK : ESP_FAIL;
}

static esp_err_t emac_w5100_set_addr( esp_eth_mac_t *mac, uint8_t *addr )
{
	ESP_RETURN_ON_FALSE( addr, ESP_ERR_INVALID_ARG, TAG, "can't set mac addr to null" );
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	esp_err_t ret = ESP_OK;

	lock( emac );
	w5100_setMAC( addr );
	memcpy( emac->addr, addr, 6 );
	// The MAC filter compares against SHAR: reopen so it applies to the new address
	if ( emac->socket_open )
		ret = reopen_socket( emac );
	unlock( emac );

	return ret;
}

static esp_err_t emac_w5100_get_addr( esp_eth_mac_t *mac, uint8_t *addr )
{
	ESP_RETURN_ON_FALSE( addr, ESP_ERR_INVALID_ARG, TAG, "can't set mac addr to null" );
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	memcpy( addr, emac->addr, 6 );

	return ESP_OK;
}

static void emac_w5100_poll_rx( emac_w5100_t *emac )
{
	for ( int frames = 0; frames < W5100_RX_BURST; ++frames )
	{
		uint16_t frame_len = 0;
		uint8_t *frame = NULL;

		lock( emac );
		esp_err_t ret = w5100_socket_rx_peek( &frame_len );
		if ( ret == ESP_OK )
		{
			// On allocation failure the frame is still consumed, so the RX ring does not stall
			frame = malloc( frame_len );
			ret = w5100_socket_rx_consume( frame, frame_len );
		}
		else if ( ret == ESP_ERR_INVALID_SIZE )
			reopen_socket( emac );
		unlock( emac );

		if ( ret != ESP_OK )
		{
			free( frame );
			return;
		}

		if ( !frame )
		{
			ESP_LOGE( TAG, "no memory for a %u byte frame, dropped", frame_len );
			continue;
		}

#if CONFIG_W5100_DEBUG_RX
		ESP_LOGD( TAG, "rx length = %u", frame_len );
		ESP_LOG_BUFFER_HEXDUMP( TAG, frame, frame_len, ESP_LOG_DEBUG );
#endif
		// The stack takes ownership of the buffer and frees it, also on error
		emac->eth->stack_input( emac->eth, frame, frame_len );
	}
}

static void emac_w5100_task( void *arg )
{
	emac_w5100_t *emac = ( emac_w5100_t * )arg;
	TickType_t notif_wait_time = portMAX_DELAY;

	while ( 1 )
	{
		const uint32_t notification_value = ulTaskNotifyTake( pdTRUE, notif_wait_time );

		if ( notification_value == W5100_TSK_RUN )
			emac_w5100_poll_rx( emac );
		else if ( notification_value == W5100_TSK_HOLD_ON )
		{
			ESP_LOGD( TAG, "rx polling paused" );
			notif_wait_time = portMAX_DELAY;
		}
		else if ( notification_value == W5100_TSK_GO_ON )
		{
			ESP_LOGD( TAG, "rx polling started" );
			notif_wait_time = CONFIG_W5100_RX_POLL_TICKS;
		}
		else if ( notification_value == W5100_TSK_DELETE )
			break;
	}

	xSemaphoreGive( emac->rx_task_exited );
	vTaskDelete( NULL );
}

static esp_err_t emac_w5100_set_link( esp_eth_mac_t *mac, eth_link_t link )
{
	return link == ETH_LINK_UP ? mac->start( mac ) : mac->stop( mac );
}

static esp_err_t emac_w5100_set_speed( esp_eth_mac_t *mac, eth_speed_t speed )
{
	return ESP_OK;
}

static esp_err_t emac_w5100_set_duplex( esp_eth_mac_t *mac, eth_duplex_t duplex )
{
	return ESP_OK;
}

static esp_err_t emac_w5100_set_promiscuous( esp_eth_mac_t *mac, bool enable )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	esp_err_t ret = ESP_OK;

	lock( emac );
	if ( emac->promiscuous != enable )
	{
		emac->promiscuous = enable;
		if ( emac->socket_open )
			ret = reopen_socket( emac );
	}
	unlock( emac );

	return ret;
}

// MACRAW's filter (S0_MR.MF) passes only our unicast MAC and broadcast: there is no multicast table.
// Accept the request so lwIP does not log an error on every IGMP/MLD join, and say once what it means.
static esp_err_t emac_w5100_add_mac_filter( esp_eth_mac_t *mac, uint8_t *addr )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );

	if ( !emac->promiscuous && !emac->multicast_warned )
	{
		ESP_LOGW( TAG, "W5100 has no multicast filter: multicast frames (mDNS, IPv6 ND) are dropped unless promiscuous mode is on" );
		emac->multicast_warned = true;
	}

	return ESP_OK;
}

static esp_err_t emac_w5100_rm_mac_filter( esp_eth_mac_t *mac, uint8_t *addr )
{
	return ESP_OK;
}

static esp_err_t emac_w5100_send( emac_w5100_t *emac, const struct w5100_tx_part *const parts, const size_t count )
{
#if CONFIG_W5100_DEBUG_TX
	for ( size_t i = 0; i < count; ++i )
	{
		ESP_LOGD( TAG, "tx part %u length = %" PRIu32, ( unsigned )i, parts[ i ].len );
		ESP_LOG_BUFFER_HEXDUMP( TAG, parts[ i ].buf, parts[ i ].len, ESP_LOG_DEBUG );
	}
#endif
	lock( emac );
	esp_err_t ret = emac->socket_open ? w5100_socket_send( parts, count ) : ESP_ERR_INVALID_STATE;
	unlock( emac );

	return ret;
}

static esp_err_t emac_w5100_transmit( esp_eth_mac_t *mac, uint8_t *buf, uint32_t length )
{
	return emac_w5100_send( __containerof( mac, emac_w5100_t, parent ), &( const struct w5100_tx_part ) { buf, length }, 1 );
}

// Frame built from (uint8_t *buf, uint32_t len) pairs. ctrl (TX timestamping) is not supported by the chip.
static esp_err_t emac_w5100_transmit_ctrl_vargs( esp_eth_mac_t *mac, void *ctrl, uint32_t argc, va_list args )
{
	const uint32_t count = argc / 2;
	ESP_RETURN_ON_FALSE( count && count <= W5100_TX_MAX_PARTS, ESP_ERR_INVALID_ARG, TAG, "invalid number of buffers" );

	struct w5100_tx_part parts[ W5100_TX_MAX_PARTS ];

	for ( uint32_t i = 0; i < count; ++i )
	{
		parts[ i ].buf = va_arg( args, uint8_t * );
		parts[ i ].len = va_arg( args, uint32_t );
	}

	return emac_w5100_send( __containerof( mac, emac_w5100_t, parent ), parts, count );
}

static esp_err_t emac_w5100_receive( esp_eth_mac_t *mac, uint8_t *buf, uint32_t *length )
{
	ESP_RETURN_ON_FALSE( buf && length, ESP_ERR_INVALID_ARG, TAG, "can't set buf and length to null" );
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	uint16_t frame_len = 0;

	lock( emac );
	esp_err_t ret = w5100_socket_rx_peek( &frame_len );
	if ( ret == ESP_OK && frame_len > *length )
		ret = ESP_ERR_INVALID_SIZE;	 // frame left in the chip, *length says how big it is
	else if ( ret == ESP_OK )
		ret = w5100_socket_rx_consume( buf, frame_len );
	else if ( ret == ESP_ERR_NOT_FOUND )
		ret = ESP_OK;
	else
		reopen_socket( emac );
	unlock( emac );

	*length = frame_len;

	return ret;
}

static esp_err_t emac_w5100_init( esp_eth_mac_t *mac )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	esp_eth_mediator_t *eth = emac->eth;
	esp_err_t ret = ESP_OK;

	ESP_RETURN_ON_ERROR( eth->on_state_changed( eth, ETH_STATE_LLINIT, NULL ), TAG, "lowlevel init failed" );

	lock( emac );
	ret = w5100_init();
	if ( ret == ESP_OK )
		ret = reopen_socket( emac );
	unlock( emac );
	ESP_GOTO_ON_ERROR( ret, err, TAG, "W5100 init failed" );

	ESP_GOTO_ON_FALSE(
		pdPASS == xTaskCreatePinnedToCore(
					  emac_w5100_task,
					  "w5100_tsk",
					  emac->rx_task_stack_size +
#if CONFIG_W5100_DEBUG_RX
						  1024,
#else
						  0,
#endif
					  emac,
					  emac->rx_task_prio,
					  &emac->rx_task_hdl,
					  W5100_RX_TASK_CORE ),
		ESP_ERR_NO_MEM,
		err,
		TAG,
		"create rx task failed" );

	return ESP_OK;

err:
	lock( emac );
	w5100_socket_close();
	emac->socket_open = false;
	unlock( emac );
	eth->on_state_changed( eth, ETH_STATE_DEINIT, NULL );

	return ret;
}

static esp_err_t emac_w5100_deinit( esp_eth_mac_t *mac )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );

	// Wait for the task to leave its loop: after deinit the driver may free this instance
	xTaskNotify( emac->rx_task_hdl, W5100_TSK_DELETE, eSetValueWithOverwrite );
	xSemaphoreTake( emac->rx_task_exited, portMAX_DELAY );
	emac->rx_task_hdl = NULL;

	lock( emac );
	w5100_socket_close();
	emac->socket_open = false;
	unlock( emac );

	return emac->eth->on_state_changed( emac->eth, ETH_STATE_DEINIT, NULL );
}

static esp_err_t emac_w5100_del( esp_eth_mac_t *mac )
{
	emac_w5100_t *emac = __containerof( mac, emac_w5100_t, parent );
	vSemaphoreDelete( emac->rx_task_exited );
	vSemaphoreDelete( emac->lock );
	free( emac );

	return ESP_OK;
}

static esp_err_t emac_w5100_enable_flow_ctrl( esp_eth_mac_t *mac, bool enable )
{
	return ESP_OK;
}

static esp_err_t emac_w5100_set_peer_pause_ability( esp_eth_mac_t *mac, uint32_t ability )
{
	return ESP_OK;
}

esp_eth_mac_t *esp_eth_mac_new_w5100( const eth_mac_config_t *const mac_config )
{
	ESP_RETURN_ON_FALSE( mac_config, NULL, TAG, "mac config can't be null" );
	emac_w5100_t *emac = calloc( 1, sizeof( emac_w5100_t ) );
	ESP_RETURN_ON_FALSE( emac, NULL, TAG, "no mem for MAC instance" );

	emac->lock = xSemaphoreCreateMutex();
	emac->rx_task_exited = xSemaphoreCreateBinary();
	if ( !emac->lock || !emac->rx_task_exited )
	{
		if ( emac->lock )
			vSemaphoreDelete( emac->lock );
		if ( emac->rx_task_exited )
			vSemaphoreDelete( emac->rx_task_exited );
		free( emac );
		ESP_LOGE( TAG, "no mem for MAC semaphores" );
		return NULL;
	}

	emac->rx_task_stack_size = mac_config->rx_task_stack_size;
	emac->rx_task_prio = mac_config->rx_task_prio;

	emac->parent.set_mediator = emac_w5100_set_mediator;
	emac->parent.init = emac_w5100_init;
	emac->parent.deinit = emac_w5100_deinit;
	emac->parent.start = emac_w5100_start;
	emac->parent.stop = emac_w5100_stop;
	emac->parent.transmit = emac_w5100_transmit;
	emac->parent.transmit_ctrl_vargs = emac_w5100_transmit_ctrl_vargs;
	emac->parent.receive = emac_w5100_receive;
	emac->parent.read_phy_reg = emac_w5100_read_phy_reg;
	emac->parent.write_phy_reg = emac_w5100_write_phy_reg;
	emac->parent.set_addr = emac_w5100_set_addr;
	emac->parent.get_addr = emac_w5100_get_addr;
	emac->parent.add_mac_filter = emac_w5100_add_mac_filter;
	emac->parent.rm_mac_filter = emac_w5100_rm_mac_filter;
	emac->parent.set_speed = emac_w5100_set_speed;
	emac->parent.set_duplex = emac_w5100_set_duplex;
	emac->parent.set_link = emac_w5100_set_link;
	emac->parent.set_promiscuous = emac_w5100_set_promiscuous;
	emac->parent.enable_flow_ctrl = emac_w5100_enable_flow_ctrl;
	emac->parent.set_peer_pause_ability = emac_w5100_set_peer_pause_ability;
	emac->parent.del = emac_w5100_del;

	return &( emac->parent );
}
