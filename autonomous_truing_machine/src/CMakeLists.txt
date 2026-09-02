# Application ("main") component. Framework-free core libraries live under lib/
# and are built by PlatformIO's library builder (SPEC §3 P3 boundary: this
# component may call core logic; core logic never calls ESP-IDF).
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)
idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_psram esp_hw_support esp_system spi_flash esp_timer log esp_driver_i2s esp_driver_gpio
)

# The recorded campaign excerpts used by the on-target acoustic parity check (SPEC §14.2) are
# compiled in as C arrays by lib/truing_fixtures/src/fixture_acoustic_pcm.c, not embedded as
# binaries. None of the binary-embedding routes works under this toolchain: PlatformIO parses
# CMake for sources and flags but builds with SCons, so the assembly file that `EMBED_FILES` and
# `target_add_binary_data` generate through a CMake custom command is never produced, and
# `board_build.embed_files` is not injected at all when the project supplies this file.
