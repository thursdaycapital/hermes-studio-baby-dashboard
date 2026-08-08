#pragma once

#include <stddef.h>
#include "esp_err.h"

typedef esp_err_t (*baby_command_handler_t)(
    const char *command, char *response, size_t response_size);
typedef esp_err_t (*baby_backup_export_handler_t)(
    char *response, size_t response_size);
typedef esp_err_t (*baby_backup_import_handler_t)(
    const char *backup, size_t backup_size, char *response,
    size_t response_size);

esp_err_t baby_network_start(baby_command_handler_t handler,
                             baby_backup_export_handler_t export_handler,
                             baby_backup_import_handler_t import_handler);
