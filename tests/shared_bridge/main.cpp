#include "shared_ledger.h"
#include <cassert>
qboolean SV_EconomyStandaloneDebit(client_t *,int){return qfalse;}
qboolean SV_EconomyStandalonePair(client_t *,client_t *,int){return qfalse;}
int main(){client_t player;assert(SV_SharedEnabled());assert(SV_SharedLogin(&player,"login","Player","1234",0));assert(player.economyCredits==100);assert(SV_SharedSpend(&player,30));assert(player.economyCredits==70);assert(SV_SharedCredits("Player",20));assert(SV_SharedKill("h:Player","h:Other"));int balance=0;assert(SV_SharedBalance("Player",&balance));assert(balance==90);puts("Native bridge: login, reserved spend, durable earnings, atomic kill and pending balance passed.");}
