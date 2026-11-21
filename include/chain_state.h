#ifndef BLACKOUT_CHAIN_STATE_H
#define BLACKOUT_CHAIN_STATE_H
#include <stdint.h>
typedef struct { int fd, offline, frozen; uint64_t version; } chain_state;
int chain_state_open(chain_state *state, const char *directory, int fresh);
int chain_state_save(chain_state *state, int offline, int frozen);
void chain_state_close(chain_state *state);
#endif
