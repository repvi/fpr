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

    "internal_src/helpers.c"

    # Test helpers (built into fpr component so main can call them)
    "test/test_fpr_host.c"
    "test/test_fpr_client.c"
    "test/test_fpr_extender.c"
)

if(CONFIG_FPR_TEST_DATA_SIZES)
    list(APPEND FPR_SOURCES "test/test_fpr_data_sizes.c")
endif()

idf_component_register(
    SRCS ${FPR_SOURCES}
    INCLUDE_DIRS
        "include"
        "test"
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