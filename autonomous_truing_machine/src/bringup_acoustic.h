/* Bring-up section for the acoustic subsystem (Phase 1f): the real I2S front end, the ported
 * layers 2-4 on an embedded recorded excerpt (host/firmware parity ON TARGET), the measured
 * analysis cost, and drain integrity under a competing task on the audio core. */
#ifndef TRUING_BRINGUP_ACOUSTIC_H
#define TRUING_BRINGUP_ACOUSTIC_H

#include <stdbool.h>

void truing_bringup_acoustic_section(int *pass, int *fail, bool *ok_out);

#endif /* TRUING_BRINGUP_ACOUSTIC_H */
