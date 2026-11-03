
#pragma once

#include "esp_err.h"

#include <stdint.h>

esp_err_t w5100_init( void );
void w5100_setMAC( const uint8_t *const mac );
