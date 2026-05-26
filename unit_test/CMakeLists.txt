# Sprout Unit Tests
# Pure logic unit tests that don't require hardware

set(UNIT_TEST_SOURCES
    "test_helpers.c"
    "test_peer_management.c"
    "test_packet_handling.c"
    "test_state_management.c"
    "mocks/esp_mock.c"
)

idf_component_register(
    SRCS ${UNIT_TEST_SOURCES}
    INCLUDE_DIRS "." "mocks"
    REQUIRES
        unity
        fpr
)

message(STATUS "Sprout Unit Test component registered")
