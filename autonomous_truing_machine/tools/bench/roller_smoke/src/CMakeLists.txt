# Compiles the REAL driver under test (../../../../src/wheel_drive_tmc2209.c) and the pure HAL pieces
# it is built on, alongside this harness's own main.c -- the literal committed files, not copies.
idf_component_register(
    SRCS "main.c"
         "../../../../src/wheel_drive_tmc2209.c"
         "../../../../lib/truing_hal/src/step_ramp.c"
         "../../../../lib/truing_hal/src/tmc2209_frame.c"
         "../../../../lib/truing_hal/src/wheel_drive_fake.c"
    INCLUDE_DIRS "."
                 "../../../../src"
                 "../../../../lib/truing_hal/include"
                 "../../../../lib/truing_core/include"
                 "../../../../lib/truing_board/include"
    REQUIRES esp_driver_gpio esp_driver_gptimer esp_driver_uart esp_timer log vfs
)
