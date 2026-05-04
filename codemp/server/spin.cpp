/*
===========================================================================
spin.cpp — Spin mode logic for OpenJK / MB2

Responsible for: prize table generation, win checking, and the SV_Spin
entry point that is called by sv_client.cpp when a player types /spin.
===========================================================================
*/

#include <thread>
#include <array>
#include <vector>

#include "server.h"
#include "spin.h"
#include "game/bg_mb2.h"
#include "qcommon/stringed_ingame.h"
#include "server/sv_gameapi.h"
#include "qcommon/game_version.h"
#include "game/bg_weapons.h"

#define SVTELL_PREFIX "\x19[Server^7\x19]\x19: "
#define SVSAY_PREFIX  "Server^7\x19: "
#define SPAWN_VEHICLE_SUFFIX "(Spawns in 5 seconds)"

// Forward-declare weapon helper that lives in sv_ccmds.cpp
void SV_WannaGiveWeapon(client_t* cl, int wnum);

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers: delayed command execution and timed powerups
// ─────────────────────────────────────────────────────────────────────────────

void SV_ExecuteClientCommandDelayed_h(client_t* cl, char* cmd, int delay)
{
	std::this_thread::sleep_for(std::chrono::seconds(delay));
	Cvar_Set("sv_cheats", "1");
	GVM_RunFrame(sv.time);
	SV_ExecuteClientCommand(cl, cmd, qtrue);
	Cvar_Set("sv_cheats", "0");
	GVM_RunFrame(sv.time);
}

void SV_ClientTimedPowerup_h(client_t* cl, int pu, int duration)
{
	cl->gentity->playerState->powerups[pu] |= (1 << 21);
	std::this_thread::sleep_for(std::chrono::seconds(duration));
	cl->gentity->playerState->powerups[pu] = -1;
}

void SV_ExecuteClientCommandDelayed(client_t* cl, char* cmd, int delay)
{
	std::thread(SV_ExecuteClientCommandDelayed_h, cl, cmd, delay).detach();
}

void SV_ClientTimedPowerup(client_t* cl, int pu, int duration)
{
	std::thread(SV_ClientTimedPowerup_h, cl, pu, duration).detach();
}

// ─────────────────────────────────────────────────────────────────────────────
// Spin_EnsureForcePool
// If the player has no force pool yet, give them 100 points so they can
// actually use any force power they just won.
// ─────────────────────────────────────────────────────────────────────────────
static void Spin_EnsureForcePool(client_t* cl)
{
	if (cl->gentity->playerState->fd.forcePowerMax == 0) {
		cl->gentity->playerState->fd.forcePowerMax = 100;
		cl->gentity->playerState->fd.forcePower    = 100;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Spin_GeneratePrices
// Builds a weighted vector of WIN_* values, zeroing-out prizes that are
// already owned or otherwise not applicable to this player.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<int> Spin_GeneratePrices(client_t* cl) {

	int cweights[WIN_NUM_WINS] = {};
	std::vector<int> cprizes;

	// Copy default weights
	for (int i = 0; i < WIN_NUM_WINS; i++)
		cweights[i] = weights[i];

	// Weapon ownership checks removed — MB2 stores weapons in weaponMask[]
	// (NEW_WEAPON_NETWORKING) and ammo in a 70-slot int16_t array
	// (GCJ_AMMO_NETCODE); old MB_* constants no longer exist.
	// Players may spin for weapons they already own — they will simply
	// receive more ammo.

	// Armor exclusion: already at or above 500 armor
	if (cl->gentity->playerState->stats[STAT_ARMOR] >= 500) {
		cweights[WIN_100_ARMOR] = 0;
		cweights[WIN_250_ARMOR] = 0;
	}

	// Health exclusion: already at or above 500 health
	if (cl->gentity->health >= 500) {
		cweights[WIN_100_HEALTH] = 0;
		cweights[WIN_250_HEALTH] = 0;
	}

	// Equipment exclusions: already owned
	if (cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_JETPACK))
		cweights[WIN_JETPACK] = 0;

	if (cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_CLOAK))
		cweights[WIN_CLOAK] = 0;

	if (cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_EWEB))
		cweights[WIN_EWEB] = 0;

	if (cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_SENTRY_GUN))
		cweights[WIN_SENTRY] = 0;

	if (cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_SEEKER))
		cweights[WIN_SEEKER] = 0;

	if (cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_MEDPAC_BIG))
		cweights[WIN_BACTA] = 0;

	if (cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_SHIELD))
		cweights[WIN_FORCEFIELD] = 0;

	// Force power exclusions: require at least jump level 2 for offensive powers
	if (cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_JUMP] == FORCE_LEVEL_0 ||
	    cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_JUMP] == FORCE_LEVEL_1) {
		cweights[WIN_FORCE_GRIP]      = 0;
		cweights[WIN_FORCE_LIGHTNING] = 0;
		cweights[WIN_FORCE_MINDTRICK] = 0;
		cweights[WIN_FORCE_PULL]      = 0;
		cweights[WIN_FORCE_PUSH]      = 0;
		cweights[WIN_FORCE_SPEED]     = 0;
	}

	// Exclude individual powers the player already has
	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_GRIP))
		cweights[WIN_FORCE_GRIP] = 0;

	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_LIGHTNING))
		cweights[WIN_FORCE_LIGHTNING] = 0;

	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_MIND_TRICK))
		cweights[WIN_FORCE_MINDTRICK] = 0;

	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_PULL))
		cweights[WIN_FORCE_PULL] = 0;

	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_PUSH))
		cweights[WIN_FORCE_PUSH] = 0;

	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_SPEED))
		cweights[WIN_FORCE_SPEED] = 0;

	// Force Sensitivity requires the player not already have Force Jump
	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_JUMP))
		cweights[WIN_FORCE_SENSITIVITY] = 0;

	// Build weighted prize vector
	for (int i = 0; i < WIN_NUM_WINS; i++)
		for (int y = 0; y < cweights[i]; y++)
			cprizes.push_back(i);

	return cprizes;
}

// ─────────────────────────────────────────────────────────────────────────────
// Spin_HasWon — returns qtrue if cprizes[rando] == prize
// ─────────────────────────────────────────────────────────────────────────────
qboolean Spin_HasWon(std::vector<int> cprizes, int rando, int prize) {

	if (rando >= (int)cprizes.size())
		return qfalse;

	return (cprizes[rando] == prize) ? qtrue : qfalse;
}

// ─────────────────────────────────────────────────────────────────────────────
// SV_Spin — called when a player runs the /spin client command
// ─────────────────────────────────────────────────────────────────────────────
void SV_Spin(client_t* cl) {

	int              cooldown;
	char             tmp[50];
	char*            playername;
	int              mb_class;
	char*            response;
	int              rando;
	int              spins;
	std::vector<int> cprizes;
	qboolean         valid_spin;

	response = "";

	if (!com_sv_running->integer) {
		Com_Printf("Server is not running.\n");
		return;
	}

	playername = cl->name;
	SV_UserinfoChanged(cl);

	// Player is dead / spectating
	if (cl->gentity->playerState->persistant[PERS_TEAM] == TEAM_SPECTATOR) {
		SV_SendServerCommand(cl, "chat \"" SVTELL_PREFIX S_COLOR_RED "%s" S_COLOR_WHITE "\"\n",
		                     "You must be alive to spin");
		return;
	}

	// Testing if we get less crashes
	mb_class = 99;

	// Dekas cannot spin
	if (mb_class == MB_CLASS_DEKA) {
		SV_SendServerCommand(cl, "chat \"" SVTELL_PREFIX S_COLOR_RED "%s" S_COLOR_WHITE "\"\n",
		                     "Dekas cant spin");
		return;
	}

	// Cooldown check (stored in userInt1)
	if (svs.time < cl->gentity->playerState->userInt1) {
		cooldown = (cl->gentity->playerState->userInt1 - svs.time) / 1000;

		if (cooldown > 1)
			Com_sprintf(tmp, sizeof(tmp), "Spin CoolDown: %d seconds", cooldown);
		else
			Com_sprintf(tmp, sizeof(tmp), "Spin CoolDown: %d second", cooldown);

		SV_SendServerCommand(cl, "chat \"" S_COLOR_RED "%s" S_COLOR_WHITE "\"\n", tmp);
		return;
	}

	Cvar_Set("sv_cheats", "1");
	Cvar_SetValue("sv_cheats", 1);
	Cvar_SetCheatState();
	GVM_RunFrame(sv.time);

	valid_spin = qfalse;
	spins      = 0;

	cprizes = Spin_GeneratePrices(cl);

	srand(sv.time + cl->gentity->s.clientNum + cl->lastPacketTime);

	do {
		// Pick a random index into the weighted prize vector
		rando = rand() % cprizes.size() + 1;

		// ── Pistols & Light Sidearms ────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_BRYAR)) {
			SV_WannaGiveWeapon(cl, WP_BRYAR_PISTOL);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Bryar Pistol\n", playername);
			response = "You win a Bryar Pistol";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CLONE_PISTOL)) {
			SV_WannaGiveWeapon(cl, WP_CLONE_PISTOL);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a DC-17 Pistol\n", playername);
			response = "You win a DC-17 Pistol";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_MANDO_PISTOL)) {
			SV_WannaGiveWeapon(cl, WP_MANDO_PISTOL);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Westar 34\n", playername);
			response = "You win a Westar 34";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_HEAVY_PISTOL)) {
			SV_WannaGiveWeapon(cl, WP_HEAVY_PISTOL);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Heavy Pistol\n", playername);
			response = "You win a Heavy Pistol";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_BRYAR_OLD)) {
			SV_WannaGiveWeapon(cl, WP_BRYAR_OLD);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Classic Bryar Pistol\n", playername);
			response = "You win a Classic Bryar Pistol";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_EE3)) {
			SV_WannaGiveWeapon(cl, WP_EE3);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 an EE-3\n", playername);
			response = "You win an EE-3";
			valid_spin = qtrue; break;
		}

		// ── Blasters & Carbines ─────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_BLASTER)) {
			SV_WannaGiveWeapon(cl, WP_BLASTER);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 an E-11 Blaster\n", playername);
			response = "You win an E-11 Blaster";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_DC_CARBINE)) {
			SV_WannaGiveWeapon(cl, WP_DC_CARBINE);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a DC-15 Carbine\n", playername);
			response = "You win a DC-15 Carbine";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CR2)) {
			SV_WannaGiveWeapon(cl, WP_CR2);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a CR-2\n", playername);
			response = "You win a CR-2";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_E22)) {
			SV_WannaGiveWeapon(cl, WP_E_22);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 an E-22\n", playername);
			response = "You win an E-22";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_DLT19)) {
			SV_WannaGiveWeapon(cl, WP_DLT19);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a DLT-19\n", playername);
			response = "You win a DLT-19";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_TRAD_BOWCASTER)) {
			SV_WannaGiveWeapon(cl, WP_TRAD_BOWCASTER);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Traditional Bowcaster\n", playername);
			response = "You win a Traditional Bowcaster";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_DISRUPTOR)) {
			SV_WannaGiveWeapon(cl, WP_DISRUPTOR);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Disruptor Rifle\n", playername);
			response = "You win a Disruptor Rifle";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_BOWCASTER)) {
			SV_WannaGiveWeapon(cl, WP_BOWCASTER);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Bowcaster\n", playername);
			response = "You win a Bowcaster";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_REPEATER)) {
			SV_WannaGiveWeapon(cl, WP_REPEATER);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 an Imperial Repeater\n", playername);
			response = "You win an Imperial Repeater";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CLONE_RIFLE)) {
			SV_WannaGiveWeapon(cl, WP_CLONE_RIFLE);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a DC-15A Clone Rifle\n", playername);
			response = "You win a DC-15A Clone Rifle";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_A280)) {
			SV_WannaGiveWeapon(cl, WP_A280);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 an A280\n", playername);
			response = "You win an A280";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_DLT20A)) {
			SV_WannaGiveWeapon(cl, WP_DLT20A);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a DLT-20A\n", playername);
			response = "You win a DLT-20A";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_M5)) {
			SV_WannaGiveWeapon(cl, WP_M5);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 an M5\n", playername);
			response = "You win an M5";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_T21)) {
			SV_WannaGiveWeapon(cl, WP_T21);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a T-21\n", playername);
			response = "You win a T-21";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_EE4)) {
			SV_WannaGiveWeapon(cl, WP_EE4);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 an EE-4\n", playername);
			response = "You win an EE-4";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_AMBAN)) {
			SV_WannaGiveWeapon(cl, WP_AMBAN);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 an Amban Phase-Pulse Rifle\n", playername);
			response = "You win an Amban Phase-Pulse Rifle";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_PROJ)) {
			SV_WannaGiveWeapon(cl, WP_PROJ);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Projectile Rifle\n", playername);
			response = "You win a Projectile Rifle";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SBD)) {
			SV_WannaGiveWeapon(cl, WP_SBD);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 an SBD Wrist Blaster\n", playername);
			response = "You win an SBD Wrist Blaster";
			valid_spin = qtrue; break;
		}

		// ── Special Weapons ─────────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_DEMP2)) {
			SV_WannaGiveWeapon(cl, WP_DEMP2);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a DEMP-2\n", playername);
			response = "You win a DEMP-2";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FLECHETTE)) {
			SV_WannaGiveWeapon(cl, WP_FLECHETTE);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Flechette Cannon\n", playername);
			response = "You win a Flechette Cannon";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CONCUSSION)) {
			SV_WannaGiveWeapon(cl, WP_CONCUSSION);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Concussion Rifle\n", playername);
			response = "You win a Concussion Rifle";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_THROWER)) {
			SV_WannaGiveWeapon(cl, WP_THROWER);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Flamethrower\n", playername);
			response = "You win a Flamethrower";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_MINIGUN)) {
			SV_WannaGiveWeapon(cl, WP_MINIGUN);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Minigun\n", playername);
			response = "You win a Minigun";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SHOTGUN)) {
			SV_WannaGiveWeapon(cl, WP_SHOTGUN);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Shotgun\n", playername);
			response = "You win a Shotgun";
			valid_spin = qtrue; break;
		}

		// ── Heavy Launchers ─────────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_ROCKET_LAUNCHER)) {
			SV_WannaGiveWeapon(cl, WP_ROCKET_LAUNCHER);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Rocket Launcher\n", playername);
			response = "You win a Rocket Launcher";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_PLX1)) {
			SV_WannaGiveWeapon(cl, WP_PLX1);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a PLX-1 Missile Launcher\n", playername);
			response = "You win a PLX-1 Missile Launcher";
			valid_spin = qtrue; break;
		}

		// ── Grenades & Explosives ────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_FRAG_NADE)) {
			SV_WannaGiveWeapon(cl, WP_FRAG_NADE);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 Frag Grenades\n", playername);
			response = "You win Frag Grenades";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_PULSE_NADE)) {
			SV_WannaGiveWeapon(cl, WP_PULSE_NADE);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 Pulse Grenades\n", playername);
			response = "You win Pulse Grenades";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_THERMAL)) {
			SV_WannaGiveWeapon(cl, WP_THERMAL);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Thermal Detonator\n", playername);
			response = "You win a Thermal Detonator";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_REAL_TD)) {
			SV_WannaGiveWeapon(cl, WP_REAL_TD);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Proximity Detonator\n", playername);
			response = "You win a Proximity Detonator";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FIRE_NADE)) {
			SV_WannaGiveWeapon(cl, WP_FIRE_NADE);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 an Incendiary Grenade\n", playername);
			response = "You win an Incendiary Grenade";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SONIC_NADE)) {
			SV_WannaGiveWeapon(cl, WP_SONIC_NADE);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Sonic Grenade\n", playername);
			response = "You win a Sonic Grenade";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CRYO_NADE)) {
			SV_WannaGiveWeapon(cl, WP_CRYO_NADE);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Cryo Grenade\n", playername);
			response = "You win a Cryo Grenade";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CONC_NADE)) {
			SV_WannaGiveWeapon(cl, WP_CONC_NADE);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Concussion Grenade\n", playername);
			response = "You win a Concussion Grenade";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_TRIP_MINE)) {
			SV_WannaGiveWeapon(cl, WP_TRIP_MINE);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Trip Mine\n", playername);
			response = "You win a Trip Mine";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_DET_PACK)) {
			SV_WannaGiveWeapon(cl, WP_DET_PACK);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Det Pack\n", playername);
			response = "You win a Det Pack";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_UGL)) {
			SV_WannaGiveWeapon(cl, WP_UGL);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Universal Grenade Launcher\n", playername);
			response = "You win a Universal Grenade Launcher";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_MGL)) {
			SV_WannaGiveWeapon(cl, WP_MGL);
			SV_ExecuteClientCommand(cl, "give ammo 500", qtrue);
			Com_Printf("Giving %s^7 a Micro Grenade Launcher\n", playername);
			response = "You win a Micro Grenade Launcher";
			valid_spin = qtrue; break;
		}

		// ── Lightsaber (random style) ────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_SABER)) {
			char* saberstyle_name = "";
			int saberstyles[] = { MB_SS_BLUE, MB_SS_YELLOW, MB_SS_RED, MB_SS_PURPLE, MB_SS_CYAN, MB_SS_STAFF };
			int rand_saberstyle = saberstyles[rand() % 6];

			cl->gentity->playerState->fd.saberAnimLevel = rand_saberstyle;
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SABER_DEFENCE] = 1;
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SABER_OFFENCE] = 1;
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SABER_THROW]   = 1;
			SV_WannaGiveWeapon(cl, WP_SABER);

			if (rand_saberstyle == MB_SS_BLUE)   saberstyle_name = "Blue";
			if (rand_saberstyle == MB_SS_YELLOW) saberstyle_name = "Yellow";
			if (rand_saberstyle == MB_SS_RED)    saberstyle_name = "Red";
			if (rand_saberstyle == MB_SS_PURPLE) saberstyle_name = "Purple";
			if (rand_saberstyle == MB_SS_CYAN)   saberstyle_name = "Cyan";
			if (rand_saberstyle == MB_SS_STAFF)  saberstyle_name = "Staff";

			Com_Printf("Giving %s^7 a Lightsaber with %s style\n", playername, saberstyle_name);
			Com_sprintf(tmp, sizeof(tmp), "You win a Lightsaber with %s style", saberstyle_name);
			response = tmp;
			valid_spin = qtrue; break;
		}

		// ── Equipment ───────────────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_100_ARMOR)) {
			cl->gentity->playerState->stats[STAT_ARMOR] += 100;
			Com_Printf("Giving %s^7 100 Armor\n", playername);
			response = "You win 100 extra Armor";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_250_ARMOR)) {
			cl->gentity->playerState->stats[STAT_ARMOR] += 250;
			Com_Printf("Giving %s^7 250 Armor\n", playername);
			response = "You win 250 extra Armor";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_JETPACK)) {
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_JETPACK);
			cl->gentity->playerState->jetpackFuel = 100;
			Com_Printf("Giving %s^7 a Jetpack\n", playername);
			response = "You win a Jetpack";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CLOAK)) {
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_CLOAK);
			cl->gentity->playerState->cloakFuel = 100;
			Com_Printf("Giving %s^7 a Cloak Generator\n", playername);
			response = "You win a Cloak Generator";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_EWEB)) {
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_EWEB);
			Com_Printf("Giving %s^7 an EWEB Gun Emplacement\n", playername);
			response = "You win an EWEB Gun Emplacement";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SENTRY)) {
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_SENTRY_GUN);
			Com_Printf("Giving %s^7 an Automated Sentry Gun\n", playername);
			response = "You win an Automated Sentry Gun";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SEEKER)) {
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_SEEKER);
			Com_Printf("Giving %s^7 a Seeker Droid\n", playername);
			response = "You win a Seeker Droid";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_BACTA)) {
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_MEDPAC_BIG);
			Com_Printf("Giving %s^7 a Tank of Bacta\n", playername);
			response = "You win a Tank of Bacta";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCEFIELD)) {
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_SHIELD);
			Com_Printf("Giving %s^7 a Forcefield Generator\n", playername);
			response = "You win a Forcefield Generator";
			valid_spin = qtrue; break;
		}

		// ── Vehicles ────────────────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_TAUN_TAUN)) {
			SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle tauntaun", 5);
			Com_Printf("Giving %s^7 a TaunTaun\n", playername);
			response = "You win a TaunTaun " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SWOOP)) {
			int rand_swoop = rand() % 6;
			Com_Printf("Giving %s^7 a Swoop Bike\n", playername);
			response = "You win a Swoop Bike " SPAWN_VEHICLE_SUFFIX;
			const char* swoop_types[] = {
				"swoop_mp2", "swoop_mp", "swoop_battle_cunning",
				"swoop_race_b", "yavin_swoop", "swoop_mp2"
			};
			char swoop_cmd[64];
			Com_sprintf(swoop_cmd, sizeof(swoop_cmd), "npc spawn vehicle %s", swoop_types[rand_swoop]);
			SV_ExecuteClientCommandDelayed(cl, swoop_cmd, 5);
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SPEEDER)) {
			Com_Printf("Giving %s^7 a Sith Speeder\n", playername);
			SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle sithspeeder_mst", 5);
			response = "You win a Sith Speeder " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_DEWBACK)) {
			Com_Printf("Giving %s^7 a Dewback\n", playername);
			SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle dewback", 5);
			response = "You win a Dewback " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_MECH)) {
			Com_Printf("Giving %s^7 a Shinrar Mech\n", playername);
			SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle shinraR", 5);
			SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "%s won a Shinrar Mech! We're in the end-game now...\"\n", playername);
			response = "You win a Mech " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue; break;
		}

		// ── Fun / Size ───────────────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_SIZE_XS)) {
			cl->gentity->playerState->iModelScale = 50;
			Com_Printf("Making %s^7 Extra-Small\n", playername);
			response = "You are Extra Small!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SIZE_S)) {
			cl->gentity->playerState->iModelScale = 80;
			Com_Printf("Making %s^7 Small\n", playername);
			response = "You are Small!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SIZE_L)) {
			cl->gentity->playerState->iModelScale = 130;
			Com_Printf("Making %s^7 Large\n", playername);
			response = "You are Large!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SIZE_XL)) {
			cl->gentity->playerState->iModelScale = 175;
			Com_Printf("Making %s^7 Extra-Large\n", playername);
			response = "You are Extra-Large!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SIZE_HUGE)) {
			cl->gentity->playerState->iModelScale = 250;
			Com_Printf("Making %s^7 Huge\n", playername);
			response = "You are HUGE!";
			valid_spin = qtrue; break;
		}

		// ── Force Powers ─────────────────────────────────────────────────────
		// NOTE: Individual power wins call Spin_EnsureForcePool() so that
		// non-force-users actually have force energy to use the awarded power.

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_SENSITIVITY)) {
			Com_Printf("Making %s^7 Force Sensitive\n", playername);
			cl->gentity->playerState->fd.forcePower    = 300;
			cl->gentity->playerState->fd.forcePowerMax = 300;
			cl->gentity->playerState->jetpackFuel      = 100;
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_JUMP);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_JUMP]           = FORCE_LEVEL_1;
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_SABER_OFFENCE);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SABER_OFFENCE]  = FORCE_LEVEL_2;
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_SABER_DEFENCE);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SABER_DEFENCE]  = FORCE_LEVEL_2;
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_PULL);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_PULL]           = FORCE_LEVEL_2;
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_PUSH);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_PUSH]           = FORCE_LEVEL_2;
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_SPEED);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SPEED]          = FORCE_LEVEL_2;
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_SENSE);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SENSE]          = FORCE_LEVEL_2;
			response = "You Win Force Sensitivity!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_SPEED)) {
			Com_Printf("Giving %s^7 Force Speed Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_SPEED);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SPEED] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Speed Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_PUSH)) {
			Com_Printf("Giving %s^7 Force Push Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_PUSH);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_PUSH] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Push Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_PULL)) {
			Com_Printf("Giving %s^7 Force Pull Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_PULL);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_PULL] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Pull Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_LIGHTNING)) {
			Com_Printf("Giving %s^7 Force Lightning Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_LIGHTNING);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_LIGHTNING] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Lightning Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_GRIP)) {
			Com_Printf("Giving %s^7 Force Grip Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_GRIP);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_GRIP] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Grip Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_MINDTRICK)) {
			Com_Printf("Giving %s^7 Force Mind Trick Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_MIND_TRICK);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_MIND_TRICK] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Mind Trick Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_HEAL)) {
			Com_Printf("Giving %s^7 Force Heal Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_HEAL);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_HEAL] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Heal Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_JUMP)) {
			Com_Printf("Giving %s^7 Force Jump Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_JUMP);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_JUMP] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Jump Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_DESTRUCTION)) {
			Com_Printf("Giving %s^7 Force Destruction Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_DESTRUCTION);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_DESTRUCTION] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Destruction Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_PROTECT)) {
			Com_Printf("Giving %s^7 Force Protect Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_PROTECT);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_PROTECT] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Protect Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_ABSORB)) {
			Com_Printf("Giving %s^7 Force Absorb Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_ABSORB);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_ABSORB] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Absorb Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_DRAIN)) {
			Com_Printf("Giving %s^7 Force Drain Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_DRAIN);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_DRAIN] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Drain Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_SENSE)) {
			Com_Printf("Giving %s^7 Force Sense Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_SENSE);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SENSE] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Sense Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_SABER_THROW)) {
			Com_Printf("Giving %s^7 Force Saber Throw Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_SABER_THROW);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SABER_THROW] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Saber Throw Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_TEAM_HEAL)) {
			Com_Printf("Giving %s^7 Force Team Heal Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_TEAM_HEAL);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_TEAM_HEAL] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Team Heal Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_TEAM_ENERGIZE)) {
			Com_Printf("Giving %s^7 Force Team Energize Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_TEAM_ENERGISE);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_TEAM_ENERGISE] = 3;
			Spin_EnsureForcePool(cl);
			response = "You Win Force Team Energize Level 3!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FORCE_MASTER)) {
			Com_Printf("Giving %s^7 ALL Force Powers Level 3\n", playername);
			cl->gentity->playerState->fd.forcePower    = 300;
			cl->gentity->playerState->fd.forcePowerMax = 300;
			for (int fp = 0; fp < NUM_FORCE_POWERS; ++fp) {
				cl->gentity->playerState->fd.forcePowersKnown |= (1 << fp);
				cl->gentity->playerState->fd.forcePowerLevel[fp] = 3;
			}
			SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "%s won Force Mastery! The Force is strong with this one...\"\n", playername);
			response = "You Win Force Mastery — all powers at Level 3!";
			valid_spin = qtrue; break;
		}

		// ── Special Effects ──────────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_PHASING)) {
			Com_Printf("Giving %s^7 30s Phasing\n", playername);
			SV_ClientTimedPowerup(cl, MB_PW_PHASING, 30);
			response = "30 Seconds of Phasing";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_INVINCIBLE)) {
			Com_Printf("Giving %s^7 Invincibility\n", playername);
			SV_ExecuteClientCommand(cl, "god", qtrue);
			GVM_RunFrame(sv.time);
			SV_ExecuteClientCommandDelayed(cl, "god", 30);
			SV_ClientTimedPowerup(cl, MB_PW_INVINSIBLE, 30);
			response = "30 Seconds of Invincibility";
			valid_spin = qtrue; break;
		}

		// ── Health ───────────────────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_100_HEALTH)) {
			cl->gentity->health += 100;
			cl->gentity->playerState->stats[STAT_MAX_HEALTH] = cl->gentity->health;
			Com_Printf("Giving %s^7 100 Health\n", playername);
			response = "You win a 100 Health Boost!";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_250_HEALTH)) {
			cl->gentity->health += 250;
			cl->gentity->playerState->stats[STAT_MAX_HEALTH] = cl->gentity->health;
			Com_Printf("Giving %s^7 250 Health\n", playername);
			response = "You win a 250 Health Boost!";
			valid_spin = qtrue; break;
		}

		spins++;

	} while (valid_spin == qfalse || spins < 20);

	if (spins == 20) {
		response = "Something went wrong with your spin. We did 20 spins and you won nothing — report to admin";
	}

	Cvar_Set("sv_cheats", "0");

	// Record when the cooldown expires
	cl->gentity->playerState->userInt1 = svs.time + sv_spinCooldown->integer * 1000;

	SV_SendServerCommand(cl, "chat \"" S_COLOR_MAGENTA "%s" S_COLOR_WHITE "\"\n", response);
}
