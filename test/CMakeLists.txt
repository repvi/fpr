# FPR Test Component
# 
# This component provides test functions for the FPR library.
# Tests can be called from main.cpp by including the appropriate header.

set(FPR_TEST_SOURCES 
    "test_fpr_host.c"
    "test_fpr_client.c"
    "test_fpr_extender.c"
)

if (CONFIG_FPR_TEST_EXTENDER)
    list(APPEND FPR_TEST_SOURCES "test_fpr_extender.c")
endif()

idf_component_register(
    SRCS ${FPR_TEST_SOURCES}
    INCLUDE_DIRS "."
    REQUIRES 
        fpr
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
)

message(STATUS "FPR Test component registered - all test libraries available")