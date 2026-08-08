#pragma once

#include <stddef.h>
#include "esp_err.h"

typedef esp_err_t (*baby_command_handler_t)(
    const char *command, char *response, size_t response_size);

esp_err_t baby_network_start(baby_command_handler_t handler);

