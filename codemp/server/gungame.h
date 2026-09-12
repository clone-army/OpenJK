#pragma once

// Gun Game mode for OpenJK / MB2. See gungame.cpp for the design notes on
// why this only ever touches fields the server and game are contractually
// guaranteed to agree on (persistant[PERS_SCORE], stats[STAT_WEAPONS],
// ps->weapon), never MBII's own private per-client state the way the spin
// spawner hack has to.

#define GUNGAME_VERSION "1.000"

// Called every server frame (from sv_main.cpp, alongside SV_SpinFrame).
// No-ops immediately when g_gungame is 0.
void SV_GunGameFrame(void);

// Called from SV_ClientEnterWorld right after GVM_ClientBegin, i.e. every
// time a client spawns (including respawns after death and round
// restarts) - not just on initial connect.
void SV_GunGameClientBegin(client_t* cl);

// Called from SV_DropClient so a departing client's slot doesn't hand its
// old tier to whoever connects into that slot next.
void SV_GunGameClientDisconnect(int clientNum);

// Called from SV_ClientThink, before the usercmd reaches the game module.
// Redirects any weapon-select request that isn't this client's current
// tier weapon back to it, so switching to something you're not meant to
// have never actually reaches MBII's own weapon logic in the first place -
// stats[STAT_WEAPONS] being wrong for MBII's real (private, >32-weapon)
// ownership tracking meant the player could still select other weapons
// even with that stat forced to a single bit; blocking the request at the
// input stage doesn't depend on that private state being right at all.
void SV_GunGameClampWeaponSelect(client_t* cl, usercmd_t* cmd);
