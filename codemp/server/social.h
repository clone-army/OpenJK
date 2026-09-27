#pragma once

// Social Mode for OpenJK / MB2 - no player damage, and anyone can spawn in
// mid-round on a short timer, on top of whatever MBII mode the instance
// runs (Legends, Open, ...). See social.cpp for how: it switches on MBII's
// own RESPAWN_MODE and detours G_Damage, both found by symbol name in the
// loaded game module rather than by guessed struct offsets.

// Called from SV_InitGame (sv_gameapi.cpp) right after GVM_InitGame - i.e.
// on every map load AND every round's map_restart, since G_InitGame is what
// recomputes MBII's respawn-mode flag from the map name. Resolves symbols,
// (re)installs the G_Damage hook, and re-applies the respawn overrides.
void SV_SocialGameInit(void);

// Called every server frame (from sv_main.cpp, alongside SV_GunGameFrame).
// Keeps the respawn overrides applied, or restores MBII's stock timers once
// g_socialMode is switched off.
void SV_SocialFrame(void);

// Called from SV_ClientThink (sv_client.cpp) after the game module has
// processed the command: a bow challenges / accepts a duel when
// g_socialDuels is on.
void SV_SocialClientThink(client_t* cl);

// Called from SV_ExecuteClientCommand (sv_client.cpp) just before a command
// is passed to the game module - i.e. after the Gunray class block, so only
// accepted class picks are remembered for the stuck-joiner rescue.
void SV_SocialClientCommand(client_t* cl);
