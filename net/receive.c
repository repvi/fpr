/**
 * @file receive.c
 * @brief Sprout Data Reception API
 *
 * Data reception from peers with fragmentation support.
 */

#include "sprout/internal/helpers.h"
#include "sprout/sprout.h"
#include "sprout/internal/private_defs.h"
#include "sprout/sprout_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "sprout_receive";

extern sprout_network_t sprout_net;

bool sprout_get_data_from_peer(uint8_t *peer_mac, void *data, int data_size, TickType_t timeout)
{
    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    if (peer && data && data_size > 0) {
        const size_t CHUNK_CAP = sizeof(((sprout_package_t *)0)->protocol);
        sprout_package_t pkg;
        size_t offset = 0;
        bool expecting_more = false;

        while (xQueueReceive(peer->response_queue, &pkg, timeout) == pdPASS) {
            size_t actual_payload = (pkg.payload_size > 0 && pkg.payload_size <= CHUNK_CAP)
                                    ? pkg.payload_size : CHUNK_CAP;

            size_t remaining_space = (size_t)data_size - offset;
            size_t copy_size = (remaining_space < actual_payload) ? remaining_space : actual_payload;

            switch (pkg.package_type) {
                case SPROUT_PACKAGE_TYPE_SINGLE:
                    copy_size = ((size_t)data_size < actual_payload) ? (size_t)data_size : actual_payload;
                    memcpy(data, &pkg.protocol, copy_size);
                    if (peer->queued_packets > 0) {
                        peer->queued_packets--;
                    }
                    return true;

                case SPROUT_PACKAGE_TYPE_START:
                    offset = 0;
                    expecting_more = true;
                    remaining_space = (size_t)data_size;
                    copy_size = (remaining_space < actual_payload) ? remaining_space : actual_payload;
                    memcpy((uint8_t*)data + offset, &pkg.protocol, copy_size);
                    offset += copy_size;
                    break;

                case SPROUT_PACKAGE_TYPE_CONTINUED:
                    if (!expecting_more) {
                        continue;
                    }
                    memcpy((uint8_t*)data + offset, &pkg.protocol, copy_size);
                    offset += copy_size;
                    break;

                case SPROUT_PACKAGE_TYPE_END:
                    if (!expecting_more) {
                        continue;
                    }
                    memcpy((uint8_t*)data + offset, &pkg.protocol, copy_size);
                    offset += copy_size;
                    if (peer->queued_packets > 0) {
                        peer->queued_packets--;
                    }
                    return true;

                default:
                    continue;
            }

            if (offset >= (size_t)data_size) {
                return true;
            }
        }
    }
    return false;
}
