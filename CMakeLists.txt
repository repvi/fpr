set(FPR_SOURCES
    "fpr_client.c"
    "fpr_extender.c"
    "fpr_handle.c"
    "fpr_host.c"
    "fpr_legacy.c"
    "fpr_lts.c"
    "fpr_new.c"
    "fpr_security.c"
    "fpr_security_handshake.c"
    "fpr.c"
    "fpr_peers.c"
    "fpr_send.c"
    "fpr_diag.c"

    "internal_src/helpers.c"
    "internal_src/rx.c"
)

# SIL test-support probe: lets the sim component's virtual ESP-NOW
# radio build/parse wire-format packets without exporting the private
# fpr_package_t layout. See include/fpr/fpr_sil.h.
if(CONFIG_RAMINOX_SIL)
    list(APPEND FPR_SOURCES "sil_probe.c")
endif()

if(CONFIG_FPR_TEST_MODE)
    # Test helpers (built into fpr component so main can call them)
    list(APPEND FPR_SOURCES
        "test/test_fpr_host.c"
        "test/test_fpr_client.c"
        "test/test_fpr_extender.c"
    )
endif()

if(CONFIG_FPR_TEST_DATA_SIZES)
    list(APPEND FPR_SOURCES "test/test_fpr_data_sizes.c")
endif()

if(CONFIG_FPR_TEST_MODE OR CONFIG_FPR_TEST_DATA_SIZES)
    set(FPR_TEST_INCLUDE_DIRS "test")
else()
    set(FPR_TEST_INCLUDE_DIRS "")
endif()

idf_component_register(
    SRCS ${FPR_SOURCES}
    INCLUDE_DIRS
        "include"
        ${FPR_TEST_INCLUDE_DIRS}
    PRIV_INCLUDE_DIRS
        "internal_include"

    REQUIRES
        esp_wifi
        nvs_flash
        freertos
        esp_timer
        driver
        common-utils
)