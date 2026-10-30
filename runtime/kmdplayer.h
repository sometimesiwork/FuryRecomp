#ifndef KMDPPLAYER_H
#define KMDPPLAYER_H

#include "opl3.h"

void kmd_player_init(opl3_chip *chip);
int  kmd_player_load(const char *path);
void kmd_player_tick(void);
int  kmd_player_active(void);

#endif
