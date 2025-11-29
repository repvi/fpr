#pragma once

#include "fpr/internal/helpers.h"
#include "fpr/fpr_def.h"

void _handle_host_receive(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);

void _fpr_host_reconnect_task(void *arg);