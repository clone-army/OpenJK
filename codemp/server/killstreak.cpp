/*
===========================================================================
killstreak.cpp — Kill streak announcements for OpenJK / MB2

Counter-Strike/UT-style escalating broadcasts: every player who racks up
kills without their streak being reset gets a server-wide callout at a
handful of thresholds, picked at random from a small pool per tier so it
doesn't read the same way twice, getting more dramatic as the number climbs.
Streaks reset once per round (or map change), not on the player's own
death - killed enemies interrupt nothing except their own streak.

Detection history - three iterations, the last one actually verified
against the real MBII game source (moviebattles-2, not compiled into this
build, but available to read for exactly this kind of question):

1. First version detected kills via stats[STAT_HEALTH] crossing zero,
   cross-referenced against persistant[PERS_ATTACKER]. Live-tested bug:
   fired falsely at every round restart - MBII's round transition can
   produce a health readout that crosses zero on its own, with
   PERS_ATTACKER still holding a stale value from the *previous* round.

2. Second version switched to watching each client's own persistant[PERS_SCORE]
   in isolation (naturally immune to a stale PERS_ATTACKER, since it never
   looks at another client's state), gated an assist/kill distinction on
   persistant[PERS_ASSIST_COUNT]. Live-tested bug: assists were still
   inflating streaks. Reading g_combat.c confirmed why - PERS_ASSIST_COUNT
   is never written anywhere in MBII's actual code; real assist tracking
   (client->pers.iAssists) lives entirely in a private extended struct this
   layer can't see, and AddScore() is called with the identical +1 for both
   a kill (the "attacker" parameter) and an assist (a separately-tracked
   "entAssister" entity) - so a plain PERS_SCORE increase genuinely cannot
   distinguish the two on its own.

3. This version: g_combat.c's G_Damage() sets persistant[PERS_ATTACKER] on
   *every* damage instance to whoever dealt it, and player_die() sets
   pm_type to PM_DEAD in the same call chain, synchronously (no frame lag
   between health dropping and pm_type updating) - and never to an
   assister, only the entity that actually landed the killing blow. So
   PERS_ATTACKER was the right signal all along for excluding assists; what
   iteration 1 actually needed was a better *round-transition* guard, not a
   different attacker-identification mechanism. pm_type == PM_DEAD confirms
   a genuine death rather than a round-transition artifact, the same design
   already used in stats.cpp.

   Caution on the moviebattles-2 source itself: it's a separate dump, not
   necessarily the exact revision actually compiled into the game module
   this server loads - its pmtype_t enum includes extra values (PM_CRYOFREEZE,
   PM_NOCLIP_DEAD, PM_BARGED) this project's own bg_public.h doesn't have,
   which would shift every later value's number if it were a true match.
   gungame.cpp's PM_NORMAL/PM_JETPACK/PM_FLOAT checks (this project's own
   enum values) are already live-tested and working, which they couldn't be
   if the real module used moviebattles-2's shifted numbering - so this
   file trusts this project's own PM_DEAD symbol (proven consistent with
   what's actually running) and deliberately does NOT import moviebattles-2's
   PM_NOCLIP_DEAD numeric value. The *behavioral* findings above (what
   AddScore/G_Damage/player_die actually do) are still sound regardless -
   they're about control flow, not about a specific enum's numbering.

Two further live-tested bugs, still fixed the same way as before:

- Streaks were observed surviving into the *next map*, not just across an
  in-map round restart. PERS_SCORE apparently doesn't reliably drop at
  every map transition the way it does at a round restart within the same
  map - so map changes are also caught directly, via sv.serverId (server.h:
  "cleared each map"), a hard engine-level fact rather than something
  inferred from the closed game module's scoring behavior.

- 2026-09-17, live on CloneArmy|NA|Legends with real players: a streak
  (confirmed via chat log - a kill straight after a round change announced
  as "7 kills", i.e. picking up exactly where a pre-change streak left off)
  survived what looked like a map change but wasn't one. Traced through
  sv_gameapi.cpp/sv_ccmds.cpp: Legends cycles rounds via "map_restart", which
  calls SV_InitGame(restart=qtrue) -> GVM_InitGame -> GAME_INIT, the *same*
  ShutdownGame/InitGame pair a genuine map load produces - but does NOT go
  through SV_SpawnServer, so sv.serverId (this file's map-change signal)
  never changes, and evidently PERS_SCORE doesn't reliably drop here either
  (same unreliability already noted above for genuine map changes, just
  never previously observed on this specific transition). Neither existing
  guard fires, so the streak just carries over.
  Fixed at the actual reliable choke point instead of another score/serverId
  proxy: SV_InitGame is the one place both a fresh map load and a
  map_restart both always pass through (confirmed unconditionally, not
  gated on the restart flag - see its own comment on why restart==qtrue
  is a MBII round-to-round restart, not a fresh load). Added
  SV_KillstreakMapChange(), called from there unconditionally, doing the
  exact same full-state wipe the sv.serverId branch below already does.
  That serverId check is left in place too, purely as a redundant second
  guard for the genuine-new-map case - harmless (memset of already-zeroed
  state) if SV_KillstreakMapChange already ran first, which it always will.

That source access doesn't change what's true means-of-death here, though:
it confirmed the *engine* (playerState_t) side, but the actual meansOfDeath_t
value passed into player_die() is a local parameter, never written back out
to anything this layer can read. "Weapon used" below is still the
attacker's *currently-equipped* weapon (ps->weapon) at the instant of the
kill - close enough for flavor text, not a guarantee it's literally what
landed the fatal hit.

All per-client state lives in a private static array here, like
gunGameClientState_t in gungame.cpp - nothing added to client_t/server.h.
===========================================================================
*/

#include "server.h"
#include "killstreak.h"
#include "game/bg_weapons.h"

#define SVSAY_PREFIX  "Server^7\x19: "

// g_killstreakEnable: defined in sv_main.cpp, declared extern in server.h,
// Cvar_Get'd in sv_init.cpp - same three-file pattern as g_chaosEnable/g_gungame.

struct killstreakTier_t {
	int          threshold;
	const char **phrases;
	int          phraseCount;
};

static const char *killstreakPhrases0[] = {
	"is on a killing spree!",
	"is heating up!",
	"is on fire!",
};
static const char *killstreakPhrases1[] = {
	"is dominating!",
	"is on a rampage!",
	"can't be stopped!",
};
static const char *killstreakPhrases2[] = {
	"is unstoppable!",
	"is wrecking everyone!",
	"is a one-person army!",
};
static const char *killstreakPhrases3[] = {
	"is GODLIKE!",
	"owns this server right now!",
};
static const char *killstreakPhrases4[] = {
	"is a LEGEND!",
	"has become a myth!",
};

// Ordered ascending by threshold - Killstreak_MaybeAnnounce below relies on it.
static const killstreakTier_t killstreakTiers[] = {
	{ 3,  killstreakPhrases0, (int)ARRAY_LEN( killstreakPhrases0 ) },
	{ 5,  killstreakPhrases1, (int)ARRAY_LEN( killstreakPhrases1 ) },
	{ 7,  killstreakPhrases2, (int)ARRAY_LEN( killstreakPhrases2 ) },
	{ 10, killstreakPhrases3, (int)ARRAY_LEN( killstreakPhrases3 ) },
	{ 15, killstreakPhrases4, (int)ARRAY_LEN( killstreakPhrases4 ) },
};
static const int killstreakTierCount = (int)ARRAY_LEN( killstreakTiers );

// Once a streak passes the last tier's threshold, keep re-announcing from
// that same phrase pool every this-many further kills, so an absurd streak
// doesn't just go silent.
#define KILLSTREAK_TOP_TIER_REPEAT_EVERY	5

struct killstreakClientState_t {
	qboolean	active;
	int			lastHealth;	// last-seen stats[STAT_HEALTH] - this client as a potential VICTIM
	int			lastScore;	// last-seen persistant[PERS_SCORE] - for round-reset detection
	int			streak;		// this client's current streak - as a potential ATTACKER
};

// Indexed by client slot number, same lifetime/reset convention as
// gGunGameState in gungame.cpp. Every client is tracked in both roles at
// once (potential victim via lastHealth, potential attacker via streak) -
// which one applies on a given frame depends only on which side of a kill
// they're on.
static killstreakClientState_t gKillstreakState[MAX_CLIENTS];
static qboolean gKillstreakWasEnabled = qfalse;

// sv.serverId (server.h: "cleared each map") changes on every map load -
// a hard engine-level fact, unlike PERS_SCORE dropping, which live-testing
// showed does NOT reliably happen at every map transition the way it does
// at an in-map round restart (streaks were observed surviving into the next
// map). Used below to force a full wipe independent of the score-based
// round-reset check.
static int gKillstreakLastServerId = 0;

// Not literal means-of-death (see file header) - the attacker's currently
// held weapon at the instant of the kill. Returns "" for anything not worth
// calling out specially (most guns).
static const char *Killstreak_WeaponFlavor( client_t *attacker ) {
	if ( !attacker->gentity || !attacker->gentity->playerState ) {
		return "";
	}

	switch ( attacker->gentity->playerState->weapon ) {
		case WP_SABER:
			return "with a saber";
		case WP_MELEE:
		case WP_STUN_BATON:
			return "bare-handed";
		case WP_ROCKET_LAUNCHER:
		case WP_THERMAL:
		case WP_FRAG_NADE:
		case WP_DET_PACK:
		case WP_TRIP_MINE:
		case WP_CONCUSSION:
		case WP_PULSE_NADE:
			return "with explosives";
		case WP_DISRUPTOR:
			return "with a sniper shot";
		default:
			return "";
	}
}

static void Killstreak_MaybeAnnounce( client_t *attacker, int clientNum ) {
	const int streak = gKillstreakState[clientNum].streak;
	const int lastThreshold = killstreakTiers[killstreakTierCount - 1].threshold;
	const killstreakTier_t *tier = NULL;

	if ( streak >= lastThreshold ) {
		if ( (streak - lastThreshold) % KILLSTREAK_TOP_TIER_REPEAT_EVERY != 0 ) {
			return;
		}
		tier = &killstreakTiers[killstreakTierCount - 1];
	} else {
		int t;
		for ( t = 0; t < killstreakTierCount; t++ ) {
			if ( streak == killstreakTiers[t].threshold ) {
				tier = &killstreakTiers[t];
				break;
			}
		}
		if ( !tier ) {
			return;	// not a threshold kill count
		}
	}

	const char *phrase = tier->phrases[rand() % tier->phraseCount];
	const char *weaponFlavor = Killstreak_WeaponFlavor( attacker );

	if ( weaponFlavor[0] ) {
		SV_SendServerCommand( NULL, "chat \"" SVSAY_PREFIX "^1%s^7 %s ^7(%s^7, %d kills)\"\n",
			attacker->name, phrase, weaponFlavor, streak );
	} else {
		SV_SendServerCommand( NULL, "chat \"" SVSAY_PREFIX "^1%s^7 %s ^7(%d kills)\"\n",
			attacker->name, phrase, streak );
	}
}

void SV_KillstreakClientDisconnect( int clientNum ) {
	if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
		return;
	}
	Com_Memset( &gKillstreakState[clientNum], 0, sizeof( gKillstreakState[clientNum] ) );
}

// Called unconditionally from SV_InitGame - see file header ("2026-09-17")
// for why this exists alongside the sv.serverId check in SV_KillstreakFrame
// rather than replacing it: this is the one that actually fires for a
// map_restart (Legends' round-to-round transition), which never touches
// sv.serverId. Also resyncs gKillstreakLastServerId so SV_KillstreakFrame's
// own check doesn't immediately fire a second, redundant memset next frame.
void SV_KillstreakMapChange( void ) {
	Com_Memset( gKillstreakState, 0, sizeof( gKillstreakState ) );
	gKillstreakLastServerId = sv.serverId;
}

// Called every server frame. Loops over every active client as a potential
// VICTIM (health-crossing-zero + pm_type confirming a genuine death, not a
// round-transition artifact - see file header), and on a real kill, credits
// the streak of whoever persistant[PERS_ATTACKER] identifies - which, per
// G_Damage()/player_die() in the real MBII source, is set on every damage
// instance to whoever dealt it and never to a mere assister.
void SV_KillstreakFrame( void ) {
	int i;

	if ( !g_killstreakEnable || !g_killstreakEnable->integer ) {
		if ( gKillstreakWasEnabled ) {
			Com_Memset( gKillstreakState, 0, sizeof( gKillstreakState ) );
			gKillstreakWasEnabled = qfalse;
		}
		return;
	}
	gKillstreakWasEnabled = qtrue;

	if ( sv.serverId != gKillstreakLastServerId ) {
		Com_Memset( gKillstreakState, 0, sizeof( gKillstreakState ) );
		gKillstreakLastServerId = sv.serverId;
	}

	for ( i = 0; i < sv_maxclients->integer; i++ ) {
		client_t *victim = &svs.clients[i];
		playerState_t *vps;
		int health, score;

		if ( victim->state < CS_ACTIVE || !victim->gentity || !victim->gentity->playerState ) {
			continue;
		}
		vps = victim->gentity->playerState;

		if ( !gKillstreakState[i].active ) {
			gKillstreakState[i].active = qtrue;
			gKillstreakState[i].lastHealth = vps->stats[STAT_HEALTH];
			gKillstreakState[i].lastScore = vps->persistant[PERS_SCORE];
			gKillstreakState[i].streak = 0;
			continue;
		}

		// Round reset within the same map - MBII zeroed everyone's score.
		score = vps->persistant[PERS_SCORE];
		if ( score < gKillstreakState[i].lastScore ) {
			gKillstreakState[i].streak = 0;
		}
		gKillstreakState[i].lastScore = score;

		health = vps->stats[STAT_HEALTH];
		if ( gKillstreakState[i].lastHealth > 0 && health <= 0 &&
			vps->pm_type == MB2_PM_DEAD ) {
			const int attackerNum = vps->persistant[PERS_ATTACKER];

			if ( attackerNum >= 0 && attackerNum < sv_maxclients->integer && attackerNum != i ) {
				client_t *attacker = &svs.clients[attackerNum];

				if ( attacker->state >= CS_ACTIVE && attacker->gentity && attacker->gentity->playerState &&
					gKillstreakState[attackerNum].active ) {
					gKillstreakState[attackerNum].streak++;
					Killstreak_MaybeAnnounce( attacker, attackerNum );
				}
			}
		}
		gKillstreakState[i].lastHealth = health;
	}
}
