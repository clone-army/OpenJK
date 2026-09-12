/*
===========================================================================
gungame.cpp — Gun Game mode for OpenJK / MB2

Every player, regardless of chosen class, is stripped to a single weapon
that advances one step up a fixed ladder on every kill they get. Tier
progress persists across round restarts - it only resets when a client
disconnects (or gun game is turned off and back on) - and only ever moves
forward; dying doesn't cost you your tier, only failing to get kills does.

Design note - why this doesn't touch MBII's private per-client state the
way the spin spawner hack has to:

BuildMPGame is off in this fork's build, so codemp/game/*.c (g_combat.c,
g_client.c, etc.) are dead source - not compiled into the running server at
all. MBII's actual gameplay logic (classes, skills, its own extended
per-client struct) is a closed module the engine loads at runtime and this
repo has no header for. Anything that reaches into that private extended
state (like spin.cpp's hasSkill[] hack) has to guess a byte offset and can
break on any MBII patch.

Gun game avoids that category of fragility entirely by only ever touching
fields playerState_t is contractually guaranteed to have, per bg_public.h's
own comments:
  - persistant[PERS_SCORE]  ("!!! MUST NOT CHANGE, SERVER AND GAME BOTH
    REFERENCE !!!") - polled every frame to detect kills.
  - stats[STAT_WEAPONS]     ("MAKE SURE STAT_WEAPONS REMAINS 4!!!!") - the
    weapon-ownership bitmask; strips everything by replacing it outright.
  - ps->weapon              - the currently-equipped weapon, in q_shared.h
    since the very first Quake3 release.
plus the already-proven SV_WannaGiveWeapon / Spin_GiveWeaponAmmo give-paths
(sv_ccmds.cpp / spin.cpp) that the spin system already relies on across
every class in production. None of this depends on MBII's own struct
layout, so none of it needs re-discovering when MBII updates.

The same principle covers the two other things live-testing on a real
production instance turned up: SV_WannaGiveWeapon's underlying "give
weaponnum" is cheat-gated (fixed via the same sv_cheats-toggle trick
spin.cpp already uses elsewhere - see GunGame_GiveWeapon), and
stats[STAT_WEAPONS] isn't the whole story for weapon *selection* since
MBII has on the order of 90 weapons - past what one 32-bit bitmask can
represent - so real ownership tracking almost certainly lives in its own
private state too (fixed by intercepting the stock, stable usercmd_t.weapon
field before it reaches the game module at all - see
SV_GunGameClampWeaponSelect - rather than fighting whatever MBII's private
state allows after the fact). And GunGame_ApplyTier - the one function that
actually issues a give-weapon command - is now gated on ps->pm_type (see
GunGame_IsAlive), another stock field: attempting it on a player who isn't
actually alive (dead, spectating, or in the between-round intermission
state) hit "you must be alive to use this command", which is exactly what
was happening at every round restart, and left that window's mismatch
uncorrected until the player's next kill forced a fix.
===========================================================================
*/

#include "server.h"
#include "gungame.h"
#include "spin.h"
#include "game/bg_weapons.h"
#include "sv_gameapi.h"

extern void SV_WannaGiveWeapon(client_t* cl, int wnum);

#define SVTELL_PREFIX "\x19[Server^7\x19]\x19: "
#define SVSAY_PREFIX  "Server^7\x19: "

// g_gungame / g_gungameAnnounce cvar pointers: defined in sv_main.cpp,
// declared extern in server.h (already included above), Cvar_Get'd in
// sv_init.cpp - same three-file pattern as g_chaosEnable/g_spin*.

// Starting weapon first, "win" weapon (saber) last - classic gun game
// convention. Every entry here is a weapon spin.cpp already hands out
// successfully to any class via this same SV_WannaGiveWeapon path, so
// there's no new class-compatibility risk being introduced.
static const weapon_t gunGameLadder[] = {
	WP_BRYAR_PISTOL,
	WP_BLASTER,
	WP_DC_CARBINE,
	WP_CR2,
	WP_E_22,
	WP_CLONE_RIFLE,
	WP_A280,
	WP_DLT19,
	WP_REPEATER,
	WP_BOWCASTER,
	WP_DISRUPTOR,
	WP_SHOTGUN,
	WP_FLECHETTE,
	WP_DEMP2,
	WP_CONCUSSION,
	WP_ROCKET_LAUNCHER,
	WP_SABER,
};
static const int gunGameLadderSize = sizeof(gunGameLadder) / sizeof(gunGameLadder[0]);

struct gunGameClientState_t {
	qboolean active;    // has this slot been initialized into gun game
	int tier;           // index into gunGameLadder
	int lastScore;      // last-seen ps->persistant[PERS_SCORE]
};

// Indexed by client slot number. Persists for as long as that slot stays
// connected - which is exactly "across round restarts", since MBII doesn't
// reassign slots on a round change, only on disconnect (handled by
// SV_GunGameClientDisconnect below).
static gunGameClientState_t gGunGameState[MAX_CLIENTS];
static qboolean gGunGameWasEnabled = qfalse;

// "give weaponnum N" (what SV_WannaGiveWeapon sends as a simulated client
// command) is one of id's original cheat commands, gated behind sv_cheats -
// live-tested on a real production instance (sv_cheats 0, the normal state)
// and confirmed the give was being rejected outright ("cheats not enabled
// on this server", visible to the player), while the direct playerState_t
// writes above it landed fine regardless. That mismatch - stats/weapon
// force-set correctly, but the game module's own give-weapon bookkeeping
// never actually ran - is what looked like "weapons going weird": every
// frame's drift-check saw the give never took effect, so it kept retrying
// forever, spamming the rejection and repeatedly interrupting whatever
// state the player was actually in.
//
// Same fix spin.cpp already uses for other cheat-gated gives (item_jetpack,
// item_shockfield) via Spin_ExecCheatClientCommand: flip sv_cheats on for
// just this one call, then back off, running a game frame on each side so
// the toggle actually takes effect before/after.
static void GunGame_GiveWeapon(client_t* cl, int wnum)
{
	const qboolean cheatsWereEnabled = Cvar_VariableIntegerValue("sv_cheats") ? qtrue : qfalse;

	if (!cheatsWereEnabled) {
		Cvar_Set("sv_cheats", "1");
		GVM_RunFrame(sv.time);
	}

	SV_WannaGiveWeapon(cl, wnum);

	if (!cheatsWereEnabled) {
		Cvar_Set("sv_cheats", "0");
		GVM_RunFrame(sv.time);
	}
}

// Live-tested: applying a tier while the player isn't actually spawned in
// (dead, spectating, or in the between-round intermission state) hit
// SV_WannaGiveWeapon's "give weaponnum" with "you must be alive to use
// this command" - a rejection that showed up specifically at round
// restarts, exactly when MBII transitions everyone through
// PM_INTERMISSION/PM_SPINTERMISSION before the next round's spawns.
// Worse, that's also the moment MBII hands out its own fresh default
// class loadout for the new round - so failing to re-strip right then
// left players holding a real multi-weapon loadout until their next kill
// forced a correction, which is what "can still select other weapons"
// after a round restart actually was.
//
// pm_type is the stock, stable field this checks - not a private MBII
// extension - so "alive enough to hold a weapon" here means the normal
// ambulatory/jetpack/float movement states, excluding dead, spectator,
// noclip, frozen, and both intermission states.
static qboolean GunGame_IsAlive(playerState_t* ps)
{
	if (!ps) {
		return qfalse;
	}
	switch (ps->pm_type) {
		case PM_NORMAL:
		case PM_JETPACK:
		case PM_FLOAT:
			return qtrue;
		default:
			return qfalse;
	}
}

static void GunGame_ApplyTier(client_t* cl, int tier)
{
	if (!cl || !cl->gentity || !cl->gentity->playerState) {
		return;
	}
	if (tier < 0) {
		tier = 0;
	}
	if (tier >= gunGameLadderSize) {
		tier = gunGameLadderSize - 1;
	}

	const weapon_t weapon = gunGameLadder[tier];
	playerState_t* ps = cl->gentity->playerState;

	// Strip every weapon then own exactly this one, plus melee (WP_MELEE,
	// index 2 - safely inside the 32-bit stats[STAT_WEAPONS] range, unlike
	// most of MBII's weapon_t values) so punching stays available at every
	// tier regardless of what's currently on the ladder.
	ps->stats[STAT_WEAPONS] = (1 << weapon) | (1 << WP_MELEE);
	ps->weapon = weapon;
	ps->weaponstate = WEAPON_READY;

	// Run it through MBII's own real give-weapon handling too (same path
	// spin.cpp/wannagiveweapon already use across every class) so whatever
	// view-model/animation/valid-fire bookkeeping the game module itself
	// wants to do for this weapon happens for real.
	GunGame_GiveWeapon(cl, (int)weapon);
	Spin_GiveWeaponAmmo(cl, weapon);
}

static void GunGame_Announce(const char* fmt, ...)
{
	if (!g_gungameAnnounce || !g_gungameAnnounce->integer) {
		return;
	}
	va_list argptr;
	char msg[256];
	va_start(argptr, fmt);
	Q_vsnprintf(msg, sizeof(msg), fmt, argptr);
	va_end(argptr);

	SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "^3%s\"\n", msg);
}

static void GunGame_Init(client_t* cl, int clientNum)
{
	gGunGameState[clientNum].active = qtrue;
	gGunGameState[clientNum].tier = 0;
	gGunGameState[clientNum].lastScore =
		(cl->gentity && cl->gentity->playerState) ? cl->gentity->playerState->persistant[PERS_SCORE] : 0;

	// Only apply immediately if actually alive right now - see
	// GunGame_IsAlive. If not, SV_GunGameFrame's per-tick check applies it
	// the moment pm_type says they actually can hold a weapon; there's no
	// window where this gets missed, since that loop runs unconditionally
	// every frame gun game is on.
	if (cl->gentity && GunGame_IsAlive(cl->gentity->playerState)) {
		GunGame_ApplyTier(cl, 0);
	}
}

void SV_GunGameClientBegin(client_t* cl)
{
	if (!g_gungame || !g_gungame->integer || !cl) {
		return;
	}

	const int clientNum = (int)(cl - svs.clients);
	if (clientNum < 0 || clientNum >= MAX_CLIENTS) {
		return;
	}

	if (!gGunGameState[clientNum].active) {
		GunGame_Init(cl, clientNum);
		return;
	}

	// Re-apply the CURRENT tier, not tier 0 - dying and respawning (or a
	// round restart) doesn't cost progress, only failing to get kills
	// does. Only if actually alive right now - ClientBegin can fire during
	// a round transition before the player is substantively spawned into
	// a body; SV_GunGameFrame's per-tick check is what actually catches
	// that moment.
	if (cl->gentity && GunGame_IsAlive(cl->gentity->playerState)) {
		GunGame_ApplyTier(cl, gGunGameState[clientNum].tier);
	}
}

void SV_GunGameClientDisconnect(int clientNum)
{
	if (clientNum < 0 || clientNum >= MAX_CLIENTS) {
		return;
	}
	gGunGameState[clientNum].active = qfalse;
	gGunGameState[clientNum].tier = 0;
	gGunGameState[clientNum].lastScore = 0;
}

// Live-tested: stats[STAT_WEAPONS] being forced to a single bit in
// GunGame_ApplyTier did NOT stop a player from selecting another weapon
// they still (per MBII's own real, private tracking) own - visibly, the
// weapon would draw for an instant then get yanked back by the next
// frame's drift-check in SV_GunGameFrame, over and over, every time the
// player tried to switch to anything but their tier weapon. MBII almost
// certainly has to track ownership across its own extended per-client
// state rather than that one stock stat - it has on the order of 90
// weapon_t values (see bg_weapons.h's WP_EMPLACED_GUN), far more than a
// single 32-bit bitmask can represent, so stats[STAT_WEAPONS] can't be
// the whole story for anything beyond the first 32.
//
// Rather than chase that private state down too, this blocks the request
// at its source: usercmd_t.weapon (byte, q_shared.h - stock, stable, same
// category of field as everything else this file relies on) is what the
// client sets when the player presses a weapon-select key, and is 0 on
// every other tick. Redirecting it here, before SV_ClientThink hands the
// command to the game module, means the game module never even sees an
// attempt to switch to anything but the assigned tier weapon - nothing
// to yank back after the fact, no flicker.
void SV_GunGameClampWeaponSelect(client_t* cl, usercmd_t* cmd)
{
	if (!g_gungame || !g_gungame->integer || !cl || !cmd) {
		return;
	}

	const int clientNum = (int)(cl - svs.clients);
	if (clientNum < 0 || clientNum >= MAX_CLIENTS || !gGunGameState[clientNum].active) {
		return;
	}

	if (cmd->weapon == 0) {
		return; // no weapon-select requested this tick
	}

	if (cmd->weapon == (byte)WP_MELEE) {
		return; // melee is always allowed, regardless of tier
	}

	const weapon_t tierWeapon = gunGameLadder[gGunGameState[clientNum].tier];
	if (cmd->weapon != (byte)tierWeapon) {
		cmd->weapon = (byte)tierWeapon;
	}
}

// Called every server frame. Polls each active client's PERS_SCORE for
// kills and advances their tier when it goes up, and continuously
// re-enforces the current tier's weapon in case MBII's own class/loadout
// logic tries to reassert a stock loadout mid-life (the same "re-check,
// only act on drift" idiom the credit/spin systems already rely on) -
// guarded so it only ever writes when something's actually wrong, not
// every single frame unconditionally.
void SV_GunGameFrame(void)
{
	if (!g_gungame || !g_gungame->integer) {
		if (gGunGameWasEnabled) {
			// Just turned off - clear all state so a future re-enable
			// starts everyone fresh rather than resuming old tiers.
			Com_Memset(gGunGameState, 0, sizeof(gGunGameState));
			gGunGameWasEnabled = qfalse;
		}
		return;
	}
	gGunGameWasEnabled = qtrue;

	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
			continue;
		}

		if (!gGunGameState[i].active) {
			// Covers gun game being enabled after this client already
			// connected (SV_GunGameClientBegin only fires on spawn).
			GunGame_Init(cl, i);
			continue;
		}

		playerState_t* ps = cl->gentity->playerState;
		const int score = ps->persistant[PERS_SCORE];

		if (score < gGunGameState[i].lastScore) {
			// PERS_SCORE went DOWN - not a kill. MBII resets it to 0 at
			// round restarts and map changes (confirmed against stock
			// ExitLevel()'s "reset all the scores so we don't enter the
			// intermission again"), and a suicide/team-kill penalty can
			// also lower it. Either way, resync the baseline to the new
			// value now rather than leaving the OLD (now stale) high-water
			// mark in place - live-tested bug: a player who'd already
			// climbed the ladder, then hit a round restart / map change,
			// silently stopped advancing on real kills afterward, because
			// their post-reset score had to climb all the way back past
			// their pre-reset score before "score > lastScore" was true
			// again, swallowing every kill in between.
			gGunGameState[i].lastScore = score;
		} else if (score > gGunGameState[i].lastScore) {
			gGunGameState[i].lastScore = score;

			// Tier bookkeeping and the announcement are pure state/chat -
			// safe unconditionally. GunGame_ApplyTier is the one that
			// issues a give-weapon command, so it's the only part gated
			// on actually being alive right now (see GunGame_IsAlive).
			if (gGunGameState[i].tier < gunGameLadderSize - 1) {
				gGunGameState[i].tier++;
				GunGame_Announce("%s^7 advanced to weapon %d/%d!", cl->name,
					gGunGameState[i].tier + 1, gunGameLadderSize);
			} else {
				// Already at the last tier and got another kill.
				GunGame_Announce("%s^7 WINS Gun Game!", cl->name);
			}
		}

		// Make sure the current tier's weapon is still what they're
		// actually holding - catches both post-kill tier-ups (above) and
		// MBII reasserting its own loadout (e.g. the fresh default gear
		// it hands out at a round restart). Only while actually alive:
		// this is also what applies a pending tier-up (or the initial
		// tier 0 from GunGame_Init) the moment a player who was dead/in
		// intermission actually spawns back in, since this loop runs
		// unconditionally every frame regardless of what triggered the
		// mismatch.
		//
		// Live-tested: reloading got permanently stuck - the moment a
		// player's reload put ps->weapon/stats[STAT_WEAPONS] into whatever
		// transient state MBII's own (private, unseen) reload logic uses
		// mid-animation, this drift-check read it as corruption and
		// force-reapplied the tier weapon on top of it, over and over,
		// which is indistinguishable from cancelling the reload every time
		// it was attempted. ps->weaponTime is the stock, stable "busy for
		// N more ms doing a weapon action" timer (q_shared.h/bg_pmove.c -
		// used for switch delays, saber charge states, and almost
		// certainly whatever MBII's reload is timed against too, since
		// this codebase's own weapon code already reuses it rather than
		// adding a new field for every new busy-state). Waiting for it to
		// clear before correcting drift means a genuine in-progress action
		// - reload included - gets left alone to finish, rather than
		// getting reset mid-way every single frame.
		// A player who's switched to melee (now allowed - see
		// SV_GunGameClampWeaponSelect) is holding WP_MELEE, not their tier
		// weapon; that's expected, not drift, so don't yank them back to
		// the ranged weapon just for punching. stats[STAT_WEAPONS] always
		// carries both bits together (see GunGame_ApplyTier) regardless of
		// which one is currently active.
		const int expectedStats = (1 << gunGameLadder[gGunGameState[i].tier]) | (1 << WP_MELEE);
		const qboolean holdingAllowedWeapon =
			(ps->weapon == gunGameLadder[gGunGameState[i].tier] || ps->weapon == WP_MELEE) ? qtrue : qfalse;

		if (GunGame_IsAlive(ps) && ps->weaponTime <= 0 &&
			(!holdingAllowedWeapon || ps->stats[STAT_WEAPONS] != expectedStats)) {
			GunGame_ApplyTier(cl, gGunGameState[i].tier);
		}
	}
}
