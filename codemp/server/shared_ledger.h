#pragma once
#include "server.h"
#include "cJSON.h"
qboolean SV_SharedEnabled();
qboolean SV_EconomyStandaloneDebit(client_t *cl, int amount);
qboolean SV_EconomyStandalonePair(client_t *a, client_t *b, int amount);
qboolean SV_SharedEvent(cJSON *event);
qboolean SV_SharedCredits(const char *handle, int amount);
qboolean SV_SharedBalance(const char *handle, int *balance);
qboolean SV_SharedSpend(client_t *cl, int amount, const char *recipient = nullptr);
qboolean SV_SharedSpendPair(client_t *first, client_t *second, int amount);
qboolean SV_SharedLogin(client_t *cl, const char *kind, const char *handle, const char *pin, int bonus);
qboolean SV_SharedKill(const char *killer, const char *victim);
qboolean SV_SharedStats(const char *key, int kills, int deaths, int suicides, int seconds);
qboolean SV_SharedBan(const char *kind, const char *key, cJSON *row, int revision);
