# Application ("main") component. Framework-free core libraries live under lib/
# and are built by PlatformIO's library builder (SPEC §3 P3 boundary: this
# component may call core logic; core logic never calls ESP-IDF).
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)
idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_psram esp_hw_support esp_system spi_flash esp_timer log esp_driver_i2s
)
