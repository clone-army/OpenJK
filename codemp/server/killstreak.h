#pragma once

// Kill streak announcements for OpenJK / MB2 - Counter-Strike/UT-style
// escalating "on a killing spree" broadcasts as a player racks up kills
// without dying, with a little variety (a random phrase per tier) and a
// weapon-flavored clause when the kill was with something distinctive
// (saber, explosives, a sniper shot, bare hands). See killstreak.cpp for
// why "weapon used" means the attacker's currently-equipped weapon at the
// moment of the kill rather than true means-of-death.
//
// Resets once per round (or map change) - not on the streaking player's own
// death - via the same persistant[PERS_SCORE]-went-down signal gungame.cpp
// already uses for the same purpose, PLUS an explicit SV_KillstreakMapChange
// call from SV_InitGame (see killstreak.cpp for why the score/serverId
// signals alone weren't enough).

// Called every server frame (from sv_main.cpp, alongside SV_GunGameFrame /
// SV_EconomyFrame). No-ops immediately when g_killstreakEnable is 0.
void SV_KillstreakFrame(void);

// Called from SV_DropClient so a departing client's slot doesn't hand its
// old streak to whoever connects into that slot next.
void SV_KillstreakClientDisconnect(int clientNum);

// Called unconditionally from SV_InitGame (sv_gameapi.cpp) - covers both a
// fresh map load AND a map_restart, unlike the sv.serverId check in
// SV_KillstreakFrame, which only fires for the former. See killstreak.cpp
// for the live-tested reasoning.
void SV_KillstreakMapChange(void);
