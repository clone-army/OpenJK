#pragma once

// Native kill/death/suicide/playtime stats for OpenJK / MB2 - replaces the
// old !stats implementation, which round-tripped through mbiiez's Python
// log-watcher and three unindexed SQL queries (two of them wildcard LIKE
// scans) against a shared SQLite DB that only grew slower as it grew
// bigger, plus four separate individual RCON round-trips just to reply.
// This does it all in-process: tracked every frame from the same stable
// playerState_t fields the economy/gungame/killstreak systems already rely
// on, and answered instantly from an in-memory table, same architecture as
// the economy system it's modeled on. See stats.cpp for the shared-file
// format and cross-process locking.

// Called every server frame (from sv_main.cpp). No-ops immediately when
// g_statsEnable is 0.
void SV_StatsFrame(void);

// Called from SV_DropClient (before the client_t slot's fields are
// cleared) so a departing client's final partial playtime segment isn't
// lost, and so a new connection into that slot starts its own tracking
// clean.
void SV_StatsClientDisconnect(client_t *cl);

// Handles the "!stats" chat command - called from the chat-command
// dispatcher in sv_client.cpp, independently of whether the economy system
// is enabled (stats has its own g_statsEnable toggle).
void SV_StatsShowCommand(client_t *cl);
