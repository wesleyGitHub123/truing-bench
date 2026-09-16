# Compiles the REAL driver under test (../../../../src/pluck_gpio.c) alongside this harness's own
# main.c -- proving the literal committed file, not a reimplementation of its behaviour.
idf_component_register(
    SRCS "main.c" "../../../../src/pluck_gpio.c"
    INCLUDE_DIRS "."
                 "../../../../src"
                 "../../../../lib/truing_hal/include"
                 "../../../../lib/truing_core/include"
    REQUIRES esp_driver_gpio esp_driver_gptimer esp_timer log
)
