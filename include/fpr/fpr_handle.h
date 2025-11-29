#pragma once

#include "fpr/fpr_lts.h"

#ifdef __cplusplus
extern "C" {
#endif

bool fpr_version_handle_version(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len, code_version_t version);

#ifdef __cplusplus
}
#endif