#pragma once

#include "exceptions.h"

int sched_pick_next_runnable(void);
void proc_switch_to(int idx, trap_frame_t *tf);
void sched_maybe_switch(trap_frame_t *tf);
