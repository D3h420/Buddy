#ifndef BLACKOUT_H
#define BLACKOUT_H

#include <stdbool.h>

int cmd_start_blackout(int argc, char **argv);
void blackout_stop(void);
bool blackout_attack_is_active(void);
bool blackout_attack_is_running(void);

#endif // BLACKOUT_H
