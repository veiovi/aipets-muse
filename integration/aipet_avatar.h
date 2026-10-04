#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "frame_player.h"

/* Caller keeps the complete, unpadded pack alive until shutdown/reinitialization.
 * Initialization validates with the canonical player and selects natural idle. */
bool aipet_avatar_init(const void *pack, uint32_t bytes);
void aipet_avatar_shutdown(void);

/* Operational state used only while Muse is idle. Active voice modes take
 * precedence; unsupported/voice states reset this override to FP_SYS_IDLE. */
void aipet_avatar_set_idle_state(fp_sys_state_t state);

/* Changes on initialization/failure or a changed canonical framebuffer.
 * All calls run on Muse's UI thread; strip readers must finish before render. */
uint32_t muse_pixel_revision(void);
