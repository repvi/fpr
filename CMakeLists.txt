set(SPROUT_SOURCES
    "core/init.c"
    "net/send.c"
    "net/peer.c"
    "net/state.c"
    "net/receive.c"
    "mode/client.c"
    "mode/host.c"
    "mode/extender.c"
    "security/security.c"
    "security/handshake.c"
    "version/handle.c"
    "version/lts.c"
    "version/legacy.c"
    "version/new.c"
    "lib/helpers.c"
    "lib/hashmap.c"
)

idf_component_register(
    SRCS ${SPROUT_SOURCES}
    INCLUDE_DIRS
        "include"
        "lib"
    PRIV_INCLUDE_DIRS
        "internal_include"

    REQUIRES
        esp_wifi
        nvs_flash
        freertos
        esp_timer
        driver
        esp_event
        esp_system
        log
)