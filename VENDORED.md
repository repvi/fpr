# `fpr` component provenance and dependencies

This component is developed as an independent library (upstream repo
`https://github.com/repvi/fpr.git`, branch `raminox-patches`) and
consumed by Raminox as a git submodule. See `doc/FPR_PATCHES.md` in the
Raminox tree for the full local-patch history, and
`firmware/archive/fpr-vendored/` for a frozen snapshot of the last
in-tree vendored state.

## External dependencies

- **`lib/hashmap.h`, `lib/hashmap_presets.h`, `lib/base_macros.h`**
  (Raminox `common-utils` component) — peer/route tables. This is the
  only remaining raminox-side dependency; the public headers are
  otherwise self-contained.
- ESP-IDF: `esp_now`, `esp_wifi`, `esp_timer`, `esp_heap_caps`,
  `esp_log`, `esp_check`, `esp_mac`, FreeRTOS (task/queue/semphr).

Version macros live locally in `include/fpr/fpr_version.h` (vendored
from `lib/version_control.h`); the library no longer includes
`standard/time.h` or any other host-app header.

## Test mode

`test/` sources build only under `CONFIG_FPR_TEST_MODE` (demo
host/client/extender helpers) or `CONFIG_FPR_TEST_DATA_SIZES`
(`test_fpr_data_sizes.c`). The directory is added to the public include
path only when `CONFIG_FPR_TEST_MODE` is set.
