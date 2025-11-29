#pragma once

#include "fpr/internal/helpers.h"
#include "fpr/fpr_def.h"

void _handle_client_discovery(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);

void _find_host_callback(void *key, void *value, void *user_data);

void _fpr_client_reconnect_task(void *arg);