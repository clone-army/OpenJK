/*
===========================================================================
spin.cpp — Spin mode logic for OpenJK / MB2

Responsible for: prize table generation, win checking, and the SV_Spin
entry point that is called by sv_client.cpp when a player types /spin.
===========================================================================
*/

#include <cstdint>
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

// When >= 0, SV_Spin will force this win index instead of picking randomly.
// Set by SV_SpinWin_f (rcon spinwin command); reset to -1 after each use.
static int gSpinForceWin = -1;

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers: delayed command execution and timed powerups
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Deferred command queue — avoids blocking the server thread
// ─────────────────────────────────────────────────────────────────────────────
struct DeferredCmd {
	int         clientNum;
	std::string cmd;
	int         fireTime; // svs.time when to execute
};
static std::vector<DeferredCmd> gDeferredCmds;

static void SV_DrainDeferredCmds(void)
{
	for (int i = (int)gDeferredCmds.size() - 1; i >= 0; i--) {
		DeferredCmd& dc = gDeferredCmds[i];
		if (svs.time < dc.fireTime)
			continue;
		client_t* cl = &svs.clients[dc.clientNum];
		if (cl->state == CS_ACTIVE && cl->gentity) {
			Cvar_Set("sv_cheats", "1");
			SV_ExecuteClientCommand(cl, dc.cmd.c_str(), qtrue);
			Cvar_Set("sv_cheats", "0");
		}
		gDeferredCmds.erase(gDeferredCmds.begin() + i);
	}
}

void SV_ExecuteClientCommandDelayed_h(client_t* cl, std::string cmd, int delay)
{
	DeferredCmd dc;
	dc.clientNum = (int)(cl - svs.clients);
	dc.cmd       = cmd;
	dc.fireTime  = svs.time + delay * 1000;
	gDeferredCmds.push_back(dc);
}

// ─────────────────────────────────────────────────────────────────────────────
// Spin_GiveWeaponAmmo
// Directly sets ps->ammo[] for the specific ammo type used by the given
// weapon, capped to its in-game maximum.  Avoids "give ammo N" which floods
// all 70+ ammo slots (including grenades capped at 1, rockets capped at 6).
//
// Ammo indices and maxes mirror weaponData[].ammoIndex / ammoData[].max
// from moviebattles-master/game/bg_weapons.c.
// ─────────────────────────────────────────────────────────────────────────────
static void Spin_GiveWeaponAmmo(client_t* cl, weapon_t weapon)
{
	playerState_t* ps = cl->gentity->playerState;
	if (!ps) return;

	// Struct layout mismatch: MB2 (GCJ_AMMO_NETCODE) stores ammo as
	// int16_t ammo[70] at the same byte offset where the engine's
	// playerState_t has int powerups[16].  The engine's ps->ammo[] field
	// sits 64 bytes too deep (past powerups[16]).  We must cast ps->powerups
	// to int16_t* so that AMMO_* enum indices address the correct 2-byte
	// slots in MB2's in-memory playerState_t layout.
	int16_t* mb2ammo = (int16_t*)ps->powerups;

	switch (weapon) {
		// AMMO_PISTOL (max 60) — Bryar Pistol, Heavy Pistol
		case WP_BRYAR_PISTOL:
		case WP_HEAVY_PISTOL:
			mb2ammo[AMMO_PISTOL] = 60; break;

		// AMMO_WESTAR (max 175) — Mando Pistol, Classic Bryar
		case WP_MANDO_PISTOL:
		case WP_BRYAR_OLD:
			mb2ammo[AMMO_WESTAR] = 175; break;

		// AMMO_CLONEPISTOL (max 175)
		case WP_CLONE_PISTOL:
			mb2ammo[AMMO_CLONEPISTOL] = 175; break;

		// AMMO_EE3 (max 150)
		case WP_EE3:
			mb2ammo[AMMO_EE3] = 150; break;

		// AMMO_BLASTER (max 120) — E-11, DC-15 Carbine
		case WP_BLASTER:
		case WP_DC_CARBINE:
			mb2ammo[AMMO_BLASTER] = 120; break;

		// AMMO_CR2 (max 150)
		case WP_CR2:
			mb2ammo[AMMO_CR2] = 150; break;

		// AMMO_E22 (max 150)
		case WP_E_22:
			mb2ammo[AMMO_E22] = 150; break;

		// AMMO_METAL_BOLTS (max 150) — DLT-19, Repeater, A280, DLT-20A, M5, SBD
		case WP_DLT19:
		case WP_REPEATER:
		case WP_A280:
		case WP_DLT20A:
		case WP_M5:
		case WP_SBD:
			mb2ammo[AMMO_METAL_BOLTS] = 150; break;

		// AMMO_TRAD_CASTER (max 150)
		case WP_TRAD_BOWCASTER:
			mb2ammo[AMMO_TRAD_CASTER] = 150; break;

		// AMMO_POWERCELL (max 150) — Disruptor
		case WP_DISRUPTOR:
			mb2ammo[AMMO_POWERCELL] = 150; break;

		// AMMO_BOWCASTER (max 175)
		case WP_BOWCASTER:
			mb2ammo[AMMO_BOWCASTER] = 175; break;

		// AMMO_CLONERIFLE (max 150)
		case WP_CLONE_RIFLE:
			mb2ammo[AMMO_CLONERIFLE] = 150; break;

		// AMMO_THROWER (max 100)
		case WP_THROWER:
			mb2ammo[AMMO_THROWER] = 100; break;

		// AMMO_MINIGUN (max 150)
		case WP_MINIGUN:
			mb2ammo[AMMO_MINIGUN] = 150; break;

		// AMMO_DEMP2 (max 100)
		case WP_DEMP2:
			mb2ammo[AMMO_DEMP2] = 100; break;

		// AMMO_SHOTGUN (max 100)
		case WP_SHOTGUN:
			mb2ammo[AMMO_SHOTGUN] = 100; break;

		// AMMO_FLECHETTE (max 100)
		case WP_FLECHETTE:
			mb2ammo[AMMO_FLECHETTE] = 100; break;

		// AMMO_T21 (max 150)
		case WP_T21:
			mb2ammo[AMMO_T21] = 150; break;

		// AMMO_HOMING (max 100) — Rocket Launcher
		case WP_ROCKET_LAUNCHER:
			mb2ammo[AMMO_HOMING] = 3; break;

		// AMMO_ROCKETS (max 6) — PLX-1
		case WP_PLX1:
			mb2ammo[AMMO_ROCKETS] = 3; break;

		// AMMO_EE4 (max 150)
		case WP_EE4:
			mb2ammo[AMMO_EE4] = 150; break;

		// AMMO_AMBAN (max 12)
		case WP_AMBAN:
			mb2ammo[AMMO_AMBAN] = 12; break;

		// AMMO_PROJECTILE (max 10)
		case WP_PROJ:
			mb2ammo[AMMO_PROJECTILE] = 10; break;

		// AMMO_CONCUSSION (max 100)
		case WP_CONCUSSION:
			mb2ammo[AMMO_CONCUSSION] = 100; break;

		// ── Grenades / Explosives ────────────────────────────────────────────
		// AMMO_THERMAL (max 3)
		case WP_THERMAL:
			mb2ammo[AMMO_THERMAL] = 3; break;

		// AMMO_FRAG_NADE (max 1)
		case WP_FRAG_NADE:
			mb2ammo[AMMO_FRAG_NADE] = 1; break;

		// AMMO_REAL_TD (max 1)
		case WP_REAL_TD:
			mb2ammo[AMMO_REAL_TD] = 1; break;

		// AMMO_TRIPMINE (max 3)
		case WP_TRIP_MINE:
			mb2ammo[AMMO_TRIPMINE] = 3; break;

		// AMMO_PULSE_NADE (max 1)
		case WP_PULSE_NADE:
			mb2ammo[AMMO_PULSE_NADE] = 1; break;

		// AMMO_FIRE_NADE (max 1)
		case WP_FIRE_NADE:
			mb2ammo[AMMO_FIRE_NADE] = 1; break;

		// AMMO_SONIC_NADE (max 1)
		case WP_SONIC_NADE:
			mb2ammo[AMMO_SONIC_NADE] = 1; break;

		// AMMO_CRYO_NADE (max 1)
		case WP_CRYO_NADE:
			mb2ammo[AMMO_CRYO_NADE] = 1; break;

		// AMMO_CONC_NADE (max 1)
		case WP_CONC_NADE:
			mb2ammo[AMMO_CONC_NADE] = 1; break;

		// AMMO_DETPACK (max 3)
		case WP_DET_PACK:
			mb2ammo[AMMO_DETPACK] = 3; break;

		// AMMO_STICKY_BOMBS (max 15) — MGL
		case WP_MGL:
			mb2ammo[AMMO_STICKY_BOMBS] = 15; break;

		// WP_UGL draws from carried grenades — give one of every compatible type
		// (uglGrenadeTypes[]: FRAG, REAL_TD, PULSE, FIRE, SONIC, CRYO, CONC)
		case WP_UGL:
			mb2ammo[AMMO_FRAG_NADE]  = 1;
			mb2ammo[AMMO_REAL_TD]    = 1;
			mb2ammo[AMMO_PULSE_NADE] = 1;
			mb2ammo[AMMO_FIRE_NADE]  = 1;
			mb2ammo[AMMO_SONIC_NADE] = 1;
			mb2ammo[AMMO_CRYO_NADE]  = 1;
			mb2ammo[AMMO_CONC_NADE]  = 1;
			break;

		default:
			break;
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

	// Player is spectating — skip silently, timer was not advanced so it retries next frame
	if (cl->gentity->playerState->persistant[PERS_TEAM] == TEAM_SPECTATOR) {
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

	// spinwin rcon: force a specific win by flooding cprizes with it
	if (gSpinForceWin >= 0 && gSpinForceWin < WIN_NUM_WINS) {
		cprizes.assign(1000, gSpinForceWin);
	}

	do {
		// Pick a random index into the weighted prize vector
		rando = rand() % cprizes.size() + 1;

		// ── Pistols & Light Sidearms ────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_BRYAR)) {
			SV_WannaGiveWeapon(cl, WP_BRYAR_PISTOL);
			Spin_GiveWeaponAmmo(cl, WP_BRYAR_PISTOL);
			Com_Printf("Giving %s^7 a Bryar Pistol\n", playername);
			response = "You win a Bryar Pistol";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CLONE_PISTOL)) {
			SV_WannaGiveWeapon(cl, WP_CLONE_PISTOL);
			Spin_GiveWeaponAmmo(cl, WP_CLONE_PISTOL);
			Com_Printf("Giving %s^7 a DC-17 Pistol\n", playername);
			response = "You win a DC-17 Pistol";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_MANDO_PISTOL)) {
			SV_WannaGiveWeapon(cl, WP_MANDO_PISTOL);
			Spin_GiveWeaponAmmo(cl, WP_MANDO_PISTOL);
			Com_Printf("Giving %s^7 a Westar 34\n", playername);
			response = "You win a Westar 34";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_HEAVY_PISTOL)) {
			SV_WannaGiveWeapon(cl, WP_HEAVY_PISTOL);
			Spin_GiveWeaponAmmo(cl, WP_HEAVY_PISTOL);
			Com_Printf("Giving %s^7 a Heavy Pistol\n", playername);
			response = "You win a Heavy Pistol";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_BRYAR_OLD)) {
			SV_WannaGiveWeapon(cl, WP_BRYAR_OLD);
			Spin_GiveWeaponAmmo(cl, WP_BRYAR_OLD);
			Com_Printf("Giving %s^7 a Classic Bryar Pistol\n", playername);
			response = "You win a Classic Bryar Pistol";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_EE3)) {
			SV_WannaGiveWeapon(cl, WP_EE3);
			Spin_GiveWeaponAmmo(cl, WP_EE3);
			Com_Printf("Giving %s^7 an EE-3\n", playername);
			response = "You win an EE-3";
			valid_spin = qtrue; break;
		}

		// ── Blasters & Carbines ─────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_BLASTER)) {
			SV_WannaGiveWeapon(cl, WP_BLASTER);
			Spin_GiveWeaponAmmo(cl, WP_BLASTER);
			Com_Printf("Giving %s^7 an E-11 Blaster\n", playername);
			response = "You win an E-11 Blaster";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_DC_CARBINE)) {
			SV_WannaGiveWeapon(cl, WP_DC_CARBINE);
			Spin_GiveWeaponAmmo(cl, WP_DC_CARBINE);
			Com_Printf("Giving %s^7 a DC-15 Carbine\n", playername);
			response = "You win a DC-15 Carbine";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CR2)) {
			SV_WannaGiveWeapon(cl, WP_CR2);
			Spin_GiveWeaponAmmo(cl, WP_CR2);
			Com_Printf("Giving %s^7 a CR-2\n", playername);
			response = "You win a CR-2";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_E22)) {
			SV_WannaGiveWeapon(cl, WP_E_22);
			Spin_GiveWeaponAmmo(cl, WP_E_22);
			Com_Printf("Giving %s^7 an E-22\n", playername);
			response = "You win an E-22";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_DLT19)) {
			SV_WannaGiveWeapon(cl, WP_DLT19);
			Spin_GiveWeaponAmmo(cl, WP_DLT19);
			Com_Printf("Giving %s^7 a DLT-19\n", playername);
			response = "You win a DLT-19";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_TRAD_BOWCASTER)) {
			SV_WannaGiveWeapon(cl, WP_TRAD_BOWCASTER);
			Spin_GiveWeaponAmmo(cl, WP_TRAD_BOWCASTER);
			Com_Printf("Giving %s^7 a Traditional Bowcaster\n", playername);
			response = "You win a Traditional Bowcaster";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_DISRUPTOR)) {
			SV_WannaGiveWeapon(cl, WP_DISRUPTOR);
			Spin_GiveWeaponAmmo(cl, WP_DISRUPTOR);
			Com_Printf("Giving %s^7 a Disruptor Rifle\n", playername);
			response = "You win a Disruptor Rifle";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_BOWCASTER)) {
			SV_WannaGiveWeapon(cl, WP_BOWCASTER);
			Spin_GiveWeaponAmmo(cl, WP_BOWCASTER);
			Com_Printf("Giving %s^7 a Bowcaster\n", playername);
			response = "You win a Bowcaster";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_REPEATER)) {
			SV_WannaGiveWeapon(cl, WP_REPEATER);
			Spin_GiveWeaponAmmo(cl, WP_REPEATER);
			Com_Printf("Giving %s^7 an Imperial Repeater\n", playername);
			response = "You win an Imperial Repeater";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CLONE_RIFLE)) {
			SV_WannaGiveWeapon(cl, WP_CLONE_RIFLE);
			Spin_GiveWeaponAmmo(cl, WP_CLONE_RIFLE);
			Com_Printf("Giving %s^7 a DC-15A Clone Rifle\n", playername);
			response = "You win a DC-15A Clone Rifle";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_A280)) {
			SV_WannaGiveWeapon(cl, WP_A280);
			Spin_GiveWeaponAmmo(cl, WP_A280);
			Com_Printf("Giving %s^7 an A280\n", playername);
			response = "You win an A280";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_DLT20A)) {
			SV_WannaGiveWeapon(cl, WP_DLT20A);
			Spin_GiveWeaponAmmo(cl, WP_DLT20A);
			Com_Printf("Giving %s^7 a DLT-20A\n", playername);
			response = "You win a DLT-20A";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_M5)) {
			SV_WannaGiveWeapon(cl, WP_M5);
			Spin_GiveWeaponAmmo(cl, WP_M5);
			Com_Printf("Giving %s^7 an M5\n", playername);
			response = "You win an M5";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_T21)) {
			SV_WannaGiveWeapon(cl, WP_T21);
			Spin_GiveWeaponAmmo(cl, WP_T21);
			Com_Printf("Giving %s^7 a T-21\n", playername);
			response = "You win a T-21";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_EE4)) {
			SV_WannaGiveWeapon(cl, WP_EE4);
			Spin_GiveWeaponAmmo(cl, WP_EE4);
			Com_Printf("Giving %s^7 an EE-4\n", playername);
			response = "You win an EE-4";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_AMBAN)) {
			SV_WannaGiveWeapon(cl, WP_AMBAN);
			Spin_GiveWeaponAmmo(cl, WP_AMBAN);
			Com_Printf("Giving %s^7 an Amban Phase-Pulse Rifle\n", playername);
			response = "You win an Amban Phase-Pulse Rifle";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_PROJ)) {
			SV_WannaGiveWeapon(cl, WP_PROJ);
			Spin_GiveWeaponAmmo(cl, WP_PROJ);
			Com_Printf("Giving %s^7 a Projectile Rifle\n", playername);
			response = "You win a Projectile Rifle";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SBD)) {
			SV_WannaGiveWeapon(cl, WP_SBD);
			Spin_GiveWeaponAmmo(cl, WP_SBD);
			Com_Printf("Giving %s^7 an SBD Wrist Blaster\n", playername);
			response = "You win an SBD Wrist Blaster";
			valid_spin = qtrue; break;
		}

		// ── Special Weapons ─────────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_DEMP2)) {
			SV_WannaGiveWeapon(cl, WP_DEMP2);
			Spin_GiveWeaponAmmo(cl, WP_DEMP2);
			Com_Printf("Giving %s^7 a DEMP-2\n", playername);
			response = "You win a DEMP-2";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FLECHETTE)) {
			SV_WannaGiveWeapon(cl, WP_FLECHETTE);
			Spin_GiveWeaponAmmo(cl, WP_FLECHETTE);
			Com_Printf("Giving %s^7 a Flechette Cannon\n", playername);
			response = "You win a Flechette Cannon";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CONCUSSION)) {
			SV_WannaGiveWeapon(cl, WP_CONCUSSION);
			Spin_GiveWeaponAmmo(cl, WP_CONCUSSION);
			Com_Printf("Giving %s^7 a Concussion Rifle\n", playername);
			response = "You win a Concussion Rifle";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_THROWER)) {
			SV_WannaGiveWeapon(cl, WP_THROWER);
			Spin_GiveWeaponAmmo(cl, WP_THROWER);
			Com_Printf("Giving %s^7 a Flamethrower\n", playername);
			response = "You win a Flamethrower";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_MINIGUN)) {
			SV_WannaGiveWeapon(cl, WP_MINIGUN);
			Spin_GiveWeaponAmmo(cl, WP_MINIGUN);
			Com_Printf("Giving %s^7 a Minigun\n", playername);
			response = "You win a Minigun";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SHOTGUN)) {
			SV_WannaGiveWeapon(cl, WP_SHOTGUN);
			Spin_GiveWeaponAmmo(cl, WP_SHOTGUN);
			Com_Printf("Giving %s^7 a Shotgun\n", playername);
			response = "You win a Shotgun";
			valid_spin = qtrue; break;
		}

		// ── Heavy Launchers ─────────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_ROCKET_LAUNCHER)) {
			SV_WannaGiveWeapon(cl, WP_ROCKET_LAUNCHER);
			Spin_GiveWeaponAmmo(cl, WP_ROCKET_LAUNCHER);
			Com_Printf("Giving %s^7 a Rocket Launcher\n", playername);
			response = "You win a Rocket Launcher";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_PLX1)) {
			SV_WannaGiveWeapon(cl, WP_PLX1);
			Spin_GiveWeaponAmmo(cl, WP_PLX1);
			Com_Printf("Giving %s^7 a PLX-1 Missile Launcher\n", playername);
			response = "You win a PLX-1 Missile Launcher";
			valid_spin = qtrue; break;
		}

		// ── Grenades & Explosives ────────────────────────────────────────────

		if (Spin_HasWon(cprizes, rando, WIN_FRAG_NADE)) {
			SV_WannaGiveWeapon(cl, WP_FRAG_NADE);
			Spin_GiveWeaponAmmo(cl, WP_FRAG_NADE);
			Com_Printf("Giving %s^7 Frag Grenades\n", playername);
			response = "You win Frag Grenades";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_PULSE_NADE)) {
			SV_WannaGiveWeapon(cl, WP_PULSE_NADE);
			Spin_GiveWeaponAmmo(cl, WP_PULSE_NADE);
			Com_Printf("Giving %s^7 Pulse Grenades\n", playername);
			response = "You win Pulse Grenades";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_THERMAL)) {
			SV_WannaGiveWeapon(cl, WP_THERMAL);
			Spin_GiveWeaponAmmo(cl, WP_THERMAL);
			Com_Printf("Giving %s^7 a Thermal Detonator\n", playername);
			response = "You win a Thermal Detonator";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_REAL_TD)) {
			SV_WannaGiveWeapon(cl, WP_REAL_TD);
			Spin_GiveWeaponAmmo(cl, WP_REAL_TD);
			Com_Printf("Giving %s^7 a Proximity Detonator\n", playername);
			response = "You win a Proximity Detonator";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_FIRE_NADE)) {
			SV_WannaGiveWeapon(cl, WP_FIRE_NADE);
			Spin_GiveWeaponAmmo(cl, WP_FIRE_NADE);
			Com_Printf("Giving %s^7 an Incendiary Grenade\n", playername);
			response = "You win an Incendiary Grenade";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SONIC_NADE)) {
			SV_WannaGiveWeapon(cl, WP_SONIC_NADE);
			Spin_GiveWeaponAmmo(cl, WP_SONIC_NADE);
			Com_Printf("Giving %s^7 a Sonic Grenade\n", playername);
			response = "You win a Sonic Grenade";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CRYO_NADE)) {
			SV_WannaGiveWeapon(cl, WP_CRYO_NADE);
			Spin_GiveWeaponAmmo(cl, WP_CRYO_NADE);
			Com_Printf("Giving %s^7 a Cryo Grenade\n", playername);
			response = "You win a Cryo Grenade";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_CONC_NADE)) {
			SV_WannaGiveWeapon(cl, WP_CONC_NADE);
			Spin_GiveWeaponAmmo(cl, WP_CONC_NADE);
			Com_Printf("Giving %s^7 a Concussion Grenade\n", playername);
			response = "You win a Concussion Grenade";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_TRIP_MINE)) {
			SV_WannaGiveWeapon(cl, WP_TRIP_MINE);
			Spin_GiveWeaponAmmo(cl, WP_TRIP_MINE);
			Com_Printf("Giving %s^7 a Trip Mine\n", playername);
			response = "You win a Trip Mine";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_DET_PACK)) {
			SV_WannaGiveWeapon(cl, WP_DET_PACK);
			Spin_GiveWeaponAmmo(cl, WP_DET_PACK);
			Com_Printf("Giving %s^7 a Det Pack\n", playername);
			response = "You win a Det Pack";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_UGL)) {
			SV_WannaGiveWeapon(cl, WP_UGL);
			Spin_GiveWeaponAmmo(cl, WP_UGL);
			Com_Printf("Giving %s^7 a Universal Grenade Launcher\n", playername);
			response = "You win a Universal Grenade Launcher";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_MGL)) {
			SV_WannaGiveWeapon(cl, WP_MGL);
			Spin_GiveWeaponAmmo(cl, WP_MGL);
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
			int newArm = cl->gentity->playerState->stats[STAT_ARMOR] + 100;
			if (newArm > 999) newArm = 999;
			cl->gentity->playerState->stats[STAT_ARMOR] = newArm;
			Com_Printf("Giving %s^7 100 Armor\n", playername);
			response = "You win 100 extra Armor";
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_250_ARMOR)) {
			int newArm = cl->gentity->playerState->stats[STAT_ARMOR] + 250;
			if (newArm > 999) newArm = 999;
			cl->gentity->playerState->stats[STAT_ARMOR] = newArm;
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
			SV_ExecuteClientCommandDelayed_h(cl, "npc spawn vehicle tauntaun", 5);
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
			SV_ExecuteClientCommandDelayed_h(cl, swoop_cmd, 5);
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_SPEEDER)) {
			Com_Printf("Giving %s^7 a Sith Speeder\n", playername);
			SV_ExecuteClientCommandDelayed_h(cl, "npc spawn vehicle sithspeeder_mst", 5);
			response = "You win a Sith Speeder " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_DEWBACK)) {
			Com_Printf("Giving %s^7 a Dewback\n", playername);
			SV_ExecuteClientCommandDelayed_h(cl, "npc spawn vehicle dewback", 5);
			response = "You win a Dewback " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_MECH)) {
			Com_Printf("Giving %s^7 a Shinrar Mech\n", playername);
			SV_ExecuteClientCommandDelayed_h(cl, "npc spawn vehicle shinraR", 5);
			SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "%s won a Shinrar Mech! We're in the end-game now...\"\n", playername);
			response = "You win a Mech " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_AWING_MINI)) {
			Com_Printf("Giving %s^7 an A-Wing (mini)\n", playername);
			SV_ExecuteClientCommandDelayed_h(cl, "npc spawn vehicle a-wing_mini", 5);
			SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "%s won an A-Wing!\"\n", playername);
			response = "You win an A-Wing " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_TIE_BOMBER_MINI)) {
			Com_Printf("Giving %s^7 a TIE Bomber (mini)\n", playername);
			SV_ExecuteClientCommandDelayed_h(cl, "npc spawn vehicle tie-bomber_mini", 5);
			SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "%s won a TIE Bomber!\"\n", playername);
			response = "You win a TIE Bomber " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_TIE_FIGHTER_MINI)) {
			Com_Printf("Giving %s^7 a TIE Fighter (mini)\n", playername);
			SV_ExecuteClientCommandDelayed_h(cl, "npc spawn vehicle tie-fighter_mini", 5);
			SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "%s won a TIE Fighter!\"\n", playername);
			response = "You win a TIE Fighter " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_YWING_MINI)) {
			Com_Printf("Giving %s^7 a Y-Wing (mini)\n", playername);
			SV_ExecuteClientCommandDelayed_h(cl, "npc spawn vehicle y-wing_mini", 5);
			SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "%s won a Y-Wing!\"\n", playername);
			response = "You win a Y-Wing " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue; break;
		}

		if (Spin_HasWon(cprizes, rando, WIN_BANTHA)) {
			Com_Printf("Giving %s^7 a Bantha\n", playername);
			SV_ExecuteClientCommandDelayed_h(cl, "npc spawn vehicle bantha", 5);
			SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "%s won a Bantha!\"\n", playername);
			response = "You win a Bantha " SPAWN_VEHICLE_SUFFIX;
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

		// ── Health ───────────────────────────────────────────────────────────


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

// ─────────────────────────────────────────────────────────────────────────────
// Spin_LookupWinByName
// Maps a friendly string (or raw integer) to a spin_wins_t index.
// Returns -1 if not found.
// ─────────────────────────────────────────────────────────────────────────────
static int Spin_LookupWinByName(const char* name)
{
	if (*name >= '0' && *name <= '9')
		return atoi(name);

	static const struct { const char* name; int win; } table[] = {
		// Pistols & Light Sidearms
		{"bryar",              WIN_BRYAR},
		{"clone_pistol",       WIN_CLONE_PISTOL},
		{"mando_pistol",       WIN_MANDO_PISTOL},
		{"heavy_pistol",       WIN_HEAVY_PISTOL},
		{"bryar_old",          WIN_BRYAR_OLD},
		{"ee3",                WIN_EE3},
		// Blasters & Carbines
		{"blaster",            WIN_BLASTER},
		{"dc_carbine",         WIN_DC_CARBINE},
		{"cr2",                WIN_CR2},
		{"e22",                WIN_E22},
		{"dlt19",              WIN_DLT19},
		{"trad_bowcaster",     WIN_TRAD_BOWCASTER},
		{"disruptor",          WIN_DISRUPTOR},
		{"bowcaster",          WIN_BOWCASTER},
		{"repeater",           WIN_REPEATER},
		{"clone_rifle",        WIN_CLONE_RIFLE},
		{"a280",               WIN_A280},
		{"dlt20a",             WIN_DLT20A},
		{"m5",                 WIN_M5},
		{"t21",                WIN_T21},
		{"ee4",                WIN_EE4},
		{"amban",              WIN_AMBAN},
		{"proj",               WIN_PROJ},
		{"sbd",                WIN_SBD},
		// Special Weapons
		{"demp2",              WIN_DEMP2},
		{"flechette",          WIN_FLECHETTE},
		{"concussion",         WIN_CONCUSSION},
		{"thrower",            WIN_THROWER},
		{"minigun",            WIN_MINIGUN},
		{"shotgun",            WIN_SHOTGUN},
		// Heavy Launchers
		{"rocket_launcher",    WIN_ROCKET_LAUNCHER},
		{"plx1",               WIN_PLX1},
		// Grenades & Explosives
		{"frag_nade",          WIN_FRAG_NADE},
		{"pulse_nade",         WIN_PULSE_NADE},
		{"thermal",            WIN_THERMAL},
		{"real_td",            WIN_REAL_TD},
		{"fire_nade",          WIN_FIRE_NADE},
		{"sonic_nade",         WIN_SONIC_NADE},
		{"cryo_nade",          WIN_CRYO_NADE},
		{"conc_nade",          WIN_CONC_NADE},
		{"trip_mine",          WIN_TRIP_MINE},
		{"det_pack",           WIN_DET_PACK},
		{"ugl",                WIN_UGL},
		{"mgl",                WIN_MGL},
		// Melee
		{"saber",              WIN_SABER},
		// Equipment
		{"100_armor",          WIN_100_ARMOR},
		{"250_armor",          WIN_250_ARMOR},
		{"jetpack",            WIN_JETPACK},
		{"cloak",              WIN_CLOAK},
		{"eweb",               WIN_EWEB},
		{"sentry",             WIN_SENTRY},
		{"seeker",             WIN_SEEKER},
		{"bacta",              WIN_BACTA},
		{"forcefield",         WIN_FORCEFIELD},
		// Vehicles
		{"taun_taun",          WIN_TAUN_TAUN},
		{"swoop",              WIN_SWOOP},
		{"speeder",            WIN_SPEEDER},
		{"dewback",            WIN_DEWBACK},
		{"mech",               WIN_MECH},
		{"awing_mini",         WIN_AWING_MINI},
		{"tie_bomber_mini",    WIN_TIE_BOMBER_MINI},
		{"tie_fighter_mini",   WIN_TIE_FIGHTER_MINI},
		{"ywing_mini",         WIN_YWING_MINI},
		{"bantha",             WIN_BANTHA},
		// Fun / Size
		{"size_xs",            WIN_SIZE_XS},
		{"size_s",             WIN_SIZE_S},
		{"size_l",             WIN_SIZE_L},
		{"size_xl",            WIN_SIZE_XL},
		{"size_huge",          WIN_SIZE_HUGE},
		// Health
		{nullptr, -1}
	};

	for (int i = 0; table[i].name; i++)
		if (Q_stricmp(name, table[i].name) == 0)
			return table[i].win;

	return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// SV_SpinWin_f  (rcon: spinwin <clientNum> <winName|winIndex>)
// Force-gives a specific spin prize to a player without cooldown or RNG.
// Examples:
//   spinwin 0 force_master
//   spinwin 2 jetpack
//   spinwin 1 42          (raw win index)
// ─────────────────────────────────────────────────────────────────────────────
void SV_SpinWin_f(void)
{
	char      clientArg[32], winArg[64];
	int       clientNum, winIndex;
	client_t* cl;

	if (Cmd_Argc() < 3) {
		Com_Printf("Usage: spinwin <clientNum> <winName|winIndex>\n");
		Com_Printf("Win names: bryar, blaster, saber, jetpack, cloak, force_master, swap_position, random_model ...\n");
		Com_Printf("Win index range: 0-%d (see spin.h enum)\n", WIN_NUM_WINS - 1);
		return;
	}

	Q_strncpyz(clientArg, Cmd_Argv(1), sizeof(clientArg));
	Q_strncpyz(winArg,    Cmd_Argv(2), sizeof(winArg));

	clientNum = atoi(clientArg);
	if (clientNum < 0 || clientNum >= sv_maxclients->integer) {
		Com_Printf("spinwin: invalid client number %d\n", clientNum);
		return;
	}

	cl = &svs.clients[clientNum];
	if (cl->state != CS_ACTIVE || !cl->gentity) {
		Com_Printf("spinwin: client %d is not active\n", clientNum);
		return;
	}

	winIndex = Spin_LookupWinByName(winArg);
	if (winIndex < 0 || winIndex >= WIN_NUM_WINS) {
		Com_Printf("spinwin: unknown win '%s'\n", winArg);
		Com_Printf("Use a name (bryar, saber, jetpack, force_master...) or an integer 0-%d.\n", WIN_NUM_WINS - 1);
		return;
	}

	// Force the win and bypass the cooldown timer
	gSpinForceWin = winIndex;
	cl->gentity->playerState->userInt1 = 0;
	SV_Spin(cl);
	gSpinForceWin = -1;
	cl->gentity->playerState->userInt1 = 0; // clear cooldown set by SV_Spin

	Com_Printf("spinwin: forced win '%s' for %s^7\n", winArg, cl->name);
}

// ─────────────────────────────────────────────────────────────────────────────
// SV_SpinFrame — called every server frame to auto-spin players whose timer
// has expired. Replaces the need for players to type !spin.
// ─────────────────────────────────────────────────────────────────────────────
void SV_SpinFrame(void)
{
	SV_DrainDeferredCmds();

	if (!sv_spin->integer)
		return;

	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		if (cl->state != CS_ACTIVE || !cl->gentity)
			continue;

		int* spinTimer = &cl->gentity->playerState->userInt1;

		// First time this client has been seen — set their initial countdown
		if (*spinTimer == 0) {
			*spinTimer = svs.time + sv_spinCooldown->integer * 1000;
			continue;
		}

		// Timer expired — spin; SV_Spin will silently skip spectators
		if (svs.time >= *spinTimer) {
			SV_Spin(cl);
		}
	}
}
