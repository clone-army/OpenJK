/*
===========================================================================
social.cpp — Social Mode for OpenJK / MB2

A hang-out mode layered on top of whatever MBII mode the instance already
runs (Legends, Open, ...): players can't be hurt, and anyone can spawn in
at any time - dying (or joining mid-round) just puts you on a short
respawn timer instead of sitting out until the next round.

Neither of those needs new MBII behaviour, only MBII's own existing
behaviour switched on in a mode that normally doesn't use it:

  - Mid-round spawning is MBII's RESPAWN_MODE, the same system CTF /
    Conquest maps and Duel mode use. It's driven by one global,
    bNewRespawnMode, which MBII computes from the map name in G_InitGame
    (and on every map_restart, i.e. every round). The client side needs
    nothing - cgame reads the "RespawnMode" serverinfo cvar the game module
    sets alongside it. We force both back on after every G_InitGame.
    Outside CTF/CQ/Duel, SetNewRespawnTimers uses
    ctfcqRespawnTimers.changePenalty (50s in release builds, and
    g_respawnTimers is ignored outside beta builds), so we overwrite the
    whole struct with g_socialRespawnTime.

  - No damage is a detour on MBII's G_Damage: every source of player harm
    (saber, blasters, explosives, Force, fall damage, poison/fire ticks)
    goes through it. Damage to non-players (breakables, vehicles, NPCs) and
    a small allow-list of environmental/admin deaths still go through, so
    death pits, out-of-bounds, /kill and team changes keep working. NPCs
    (map NPCs, anything players summon, and vehicles, which are NPCs too)
    are protected the same way.

  - Duels (g_socialDuels) are MBII's own private duels, which
    it only allows in Duel mode: bowing at someone (GENCMD_BOW) calls
    Cmd_EngageDuel_f only when MB_DUEL_MODE, and G_CheckPrivateDuel - which
    ends a duel when someone dies - is likewise only run in Duel mode.
    Neither function checks the mode anywhere else, so the engine calls
    both itself: Cmd_EngageDuel_f when a player bows
    (with g_Authenticity.integer briefly reading as Duel, for that one
    call only), and G_CheckPrivateDuel every frame for anyone in a duel.
    MBII's G_Damage already stops duelists and bystanders hurting each
    other in any mode; our hook just lets duel partners through. Any class
    can duel with any weapon, as in MBII's own Duel mode.

  - Round length (g_socialRoundTime) overrides the per-map round time MBII
    parses from the .siege file into rebel/imperial_time_limit on every
    G_InitGame. Clients draw the round clock themselves from their own copy
    of the .siege file plus the "TimeAdd" serverinfo cvar (normally MBII's
    target_extend_time bonus), so TimeAdd is set to make up the difference.

  - Stuck joiners: in Legends (FA) mode a first-time join mid-round can
    miss RESPAWN_MODE's spawn timer entirely - RefreshNewRespawnTimers only
    starts it if the player's OldSessionTeam still reads as spectator when
    their "siegeclass" command lands, and the join path can overwrite that
    first (a new connection sits on TEAM_FREE, not spectator, until it's
    been to spectator once). Live, those players sat unspawned for minutes until they went to
    spectator and picked their class again, which always worked. So the
    engine does exactly that for them: it remembers each player's last
    accepted siegeclass command, and if they're still not spawned well
    after the respawn timer should have run, replays "team spectator" +
    that siegeclass on their behalf (a few tries at most).

  - Bots (g_socialBots) never pick a Legends class at all, since that's
    done by the human class menu, so they'd sit in spectator. The same
    rescue path gives them a random Legends class on the emptier team.
    This works with or without g_socialMode, on any Legends server.

Unlike spin.cpp's hasSkill[] offset guessing, nothing here depends on
MBII's private struct layout: the module ships with its dynamic symbol table
intact, so everything above is looked up by name (dlsym) on every game load.
The only layout-ish assumption is G_Damage's first 10 bytes (see
kGDamagePrologue); if an MBII update changes them, the hook refuses to
install and says so, rather than patching blind.
===========================================================================
*/

#include "server.h"
#include "social.h"
#include "sv_gameapi.h"
#include "sys/sys_loadlib.h"

#include <sys/mman.h>
#include <unistd.h>

// Must match MBII's meansOfDeath_t (bg_public.h). MBII keeps this enum
// append-only because its game stats database is keyed on it - verified
// against the MOD_* name table in the live jampgamei386.so.
enum {
	SOCIAL_MOD_WATER = 1,
	SOCIAL_MOD_SLIME = 2,
	SOCIAL_MOD_LAVA = 3,
	SOCIAL_MOD_CRUSH = 4,
	SOCIAL_MOD_TELEFRAG = 5,
	SOCIAL_MOD_FALLING = 6,
	SOCIAL_MOD_SUICIDE = 7,
	SOCIAL_MOD_CHANGEDCLASSES = 8,
	SOCIAL_MOD_WENTSPECTATOR = 9,
	SOCIAL_MOD_CHANGEDTEAMS = 10,
	SOCIAL_MOD_TOOMANYTKS = 11,
	SOCIAL_MOD_SPACE = 13,
	SOCIAL_MOD_TRIGGER_HURT = 16,
	SOCIAL_MOD_TRIGGER_HURT_POISON = 19,
	SOCIAL_MOD_TEAM_CHANGE = 21,
};

// MBII's DAMAGE_NO_TKPOINTS (g_local.h): no TK points for the damage, and a
// kill by a teammate counts as a suicide - so no punish/forgive prompt or
// TK respawn penalty (g_combat.c player_die). Added to all damage in social
// mode, where teammates duelling each other is the only way to TK at all.
#define SOCIAL_DAMAGE_NO_TKPOINTS 0x01000000

// Death pits (trigger_hurt with the falling flag, g_trigger.c) deal
// Q3_INFINITE / 9999+ as MOD_FALLING; ordinary landing damage is small.
#define SOCIAL_PIT_DAMAGE 9999

// MBII's respawnTimers_t, all in seconds. Same field order as g_main.c's
// initializer (verified in the live module's .data: 42 24 10 50).
struct socialRespawnTimers_t {
	int oneLife;
	int twoLife;
	int threeLife;
	int changePenalty;
};

// MBII's g_Authenticity value for Duel mode (MB_DUEL_MODE, g_local.h).
#define SOCIAL_AUTHENTICITY_DUEL 3

// MBII's SIEGE_TIMER_MIN / SIEGE_TIMER_RESPAWN_MAX (mb_defines.h) - the
// clamp cgame applies to a .siege "Timed" value when RespawnMode is on.
#define SOCIAL_CLIENT_TIMER_MIN 180
#define SOCIAL_CLIENT_TIMER_MAX 1200

typedef void (*GDamageFn)(void* targ, void* inflictor, void* attacker, float* dir, float* point, int damage, int dflags, int mod);

// push ebp; push edi; push esi; push ebx; sub esp, imm32
// All position-independent, so they can be copied into the trampoline as-is.
static const byte kGDamagePrologue[] = { 0x55, 0x57, 0x56, 0x53, 0x81, 0xEC };
#define SOCIAL_PROLOGUE_LEN 10

static void* gSocialDll = NULL;
static qboolean* gBNewRespawnMode = NULL;
static socialRespawnTimers_t* gRespawnTimers = NULL;
static socialRespawnTimers_t gOriginalTimers;
static qboolean gHaveOriginalTimers = qfalse;
static qboolean gTimersOverridden = qfalse;
static byte* gGDamage = NULL;
static byte* gTrampoline = NULL;
static qboolean gHookInstalled = qfalse;
static qboolean gHookAttempted = qfalse; // this game load; reset every G_InitGame
static void (*gEngageDuel)(void* ent) = NULL;
static void (*gCheckPrivateDuel)(void* ent) = NULL;
static vmCvar_t* gAuthenticity = NULL;
static int* gRebelTimeLimit = NULL;
static int* gImperialTimeLimit = NULL;
static int* gRebelCountdown = NULL;
static int* gImperialCountdown = NULL;
static qboolean* gSiegeRoundBegun = NULL;

// Stuck-joiner rescue, per client slot. Cleared whenever the slot isn't
// active, so a new player in a reused slot starts clean.
#define SOCIAL_STUCK_GRACE_MS 5000
#define SOCIAL_STUCK_RETRY_MS 10000
#define SOCIAL_STUCK_MAX_TRIES 3
struct socialJoinState_t {
	char siegeClassCmd[MAX_STRING_CHARS]; // last accepted "siegeclass ..." line
	int pickedAt;                         // svs.time it was sent
	int lastRescueAt;
	int rescues;
	int lastSpawnedAt;                    // last frame seen on a team
	int botSide;                          // bots: pending side, TEAM_RED/TEAM_BLUE, 0 = none
};

// Bots never pick a Legends class themselves (that comes from the human
// class menu), so they'd sit in spectator forever. With g_socialBots on, a
// bot that isn't spawned gets one of these, on the emptier side: MBII's
// Legends hero (h*) classes are the red team, villains (v*) blue. Names are
// ones players have actually picked on our Legends servers. Nute Gunray is
// deliberately absent (see SV_GunrayClassBlockCheck).
static const char* const kBotHeroClasses[] = {
	"h9_JKnight", "h9_JSage", "h9_Anakin", "h10_Obi", "h10_LukeSky", "h9_Windu",
	"h4_Kyle", "h5_BoKatan", "h3_Clone", "h3_Arc", "h3_CloneLead", "h3_Rex",
	"h8_Chewie", "h2_Han",
};
static const char* const kBotVillainClasses[] = {
	"v9_Vader", "v9_Dooku", "v9_Maul", "v9_Grievous", "v9_SWarr", "v10_Palp",
	"v4_Mara", "v5_Jango", "v5_Boba", "v1_B1", "v8_SBD", "v1_ImpComm",
	"v6_Aurra", "v2_Greedo",
};
static socialJoinState_t gJoinState[MAX_CLIENTS];

static qboolean Social_Enabled(void)
{
	return (g_socialMode && g_socialMode->integer) ? qtrue : qfalse;
}

static qboolean Social_IsPlayerEntity(void* ent)
{
	if (!ent || !sv.gentities || sv.gentitySize <= 0) {
		return qfalse;
	}
	const intptr_t delta = (byte*)ent - (byte*)sv.gentities;
	if (delta < 0 || delta % sv.gentitySize) {
		return qfalse;
	}
	const intptr_t num = delta / sv.gentitySize;
	return (num < sv_maxclients->integer) ? qtrue : qfalse;
}

// Stock entity type, networked to cgame so it can't differ between MBII and
// the engine: ET_NPC is 13 in both bg_public.h's entityType_t.
static qboolean Social_IsNPCEntity(void* ent)
{
	if (!ent || !sv.gentities || sv.gentitySize <= 0) {
		return qfalse;
	}
	const intptr_t delta = (byte*)ent - (byte*)sv.gentities;
	if (delta < 0 || delta % sv.gentitySize || delta / sv.gentitySize >= MAX_GENTITIES) {
		return qfalse;
	}
	return (((sharedEntity_t*)ent)->s.eType == ET_NPC) ? qtrue : qfalse;
}

static playerState_t* Social_PlayerState(void* ent)
{
	const int num = (int)(((byte*)ent - (byte*)sv.gentities) / sv.gentitySize);
	client_t* cl = &svs.clients[num];
	if (cl->state < CS_CONNECTED || !cl->gentity) {
		return NULL;
	}
	return cl->gentity->playerState;
}

// Duel partners fighting each other - MBII's own G_Damage checks keep
// everyone else out of the duel in both directions.
static qboolean Social_IsDuelDamage(void* targ, void* attacker)
{
	if (!g_socialDuels || !g_socialDuels->integer || !Social_IsPlayerEntity(attacker)) {
		return qfalse;
	}
	playerState_t* tps = Social_PlayerState(targ);
	playerState_t* aps = Social_PlayerState(attacker);
	if (!tps || !aps || !tps->duelInProgress || !aps->duelInProgress) {
		return qfalse;
	}
	return (tps->duelIndex == aps->clientNum && aps->duelIndex == tps->clientNum) ? qtrue : qfalse;
}

static qboolean Social_AllowPlayerDamage(int damage, int mod)
{
	if (damage <= 0) {
		return qtrue; // MBII heals by passing negative damage (w_force.c)
	}

	switch (mod) {
		case SOCIAL_MOD_WATER:
		case SOCIAL_MOD_SLIME:          // also MBII's out-of-bounds damage
		case SOCIAL_MOD_LAVA:
		case SOCIAL_MOD_CRUSH:          // doors/movers, or players get wedged
		case SOCIAL_MOD_TELEFRAG:
		case SOCIAL_MOD_SUICIDE:        // /kill
		case SOCIAL_MOD_CHANGEDCLASSES:
		case SOCIAL_MOD_WENTSPECTATOR:
		case SOCIAL_MOD_CHANGEDTEAMS:
		case SOCIAL_MOD_TOOMANYTKS:
		case SOCIAL_MOD_SPACE:
		case SOCIAL_MOD_TEAM_CHANGE:
			return qtrue;
		case SOCIAL_MOD_FALLING:
			return (damage >= SOCIAL_PIT_DAMAGE) ? qtrue : qfalse;
		default:
			return (mod >= SOCIAL_MOD_TRIGGER_HURT && mod <= SOCIAL_MOD_TRIGGER_HURT_POISON) ? qtrue : qfalse;
	}
}

static void Social_GDamageHook(void* targ, void* inflictor, void* attacker, float* dir, float* point, int damage, int dflags, int mod)
{
	if (Social_Enabled()) {
		if (Social_IsPlayerEntity(targ) && !Social_AllowPlayerDamage(damage, mod) && !Social_IsDuelDamage(targ, attacker)) {
			return;
		}
		if (Social_IsNPCEntity(targ) && !Social_AllowPlayerDamage(damage, mod)) {
			return;
		}
		dflags |= SOCIAL_DAMAGE_NO_TKPOINTS;
	}
	((GDamageFn)gTrampoline)(targ, inflictor, attacker, dir, point, damage, dflags, mod);
}

static qboolean Social_SetProtection(void* addr, size_t len, int prot)
{
	const uintptr_t pageSize = (uintptr_t)sysconf(_SC_PAGESIZE);
	const uintptr_t start = (uintptr_t)addr & ~(pageSize - 1);
	const uintptr_t end = ((uintptr_t)addr + len + pageSize - 1) & ~(pageSize - 1);
	return (mprotect((void*)start, end - start, prot) == 0) ? qtrue : qfalse;
}

static void Social_WriteJump(byte* from, const void* to)
{
	from[0] = 0xE9; // jmp rel32
	const int32_t rel = (int32_t)((intptr_t)to - ((intptr_t)from + 5));
	memcpy(from + 1, &rel, sizeof(rel));
}

static qboolean Social_HookPresent(void)
{
	if (!gGDamage || gGDamage[0] != 0xE9) {
		return qfalse;
	}
	int32_t rel;
	memcpy(&rel, gGDamage + 1, sizeof(rel));
	return ((intptr_t)gGDamage + 5 + rel == (intptr_t)&Social_GDamageHook) ? qtrue : qfalse;
}

static void Social_InstallHook(void)
{
	gHookAttempted = qtrue;
	if (Social_HookPresent()) {
		gHookInstalled = qtrue;
		return;
	}
	gHookInstalled = qfalse;

	if (memcmp(gGDamage, kGDamagePrologue, sizeof(kGDamagePrologue)) != 0) {
		Com_Printf(S_COLOR_YELLOW "Social mode: G_Damage prologue doesn't match (MBII updated?) - damage blocking disabled\n");
		return;
	}

	if (!gTrampoline) {
		void* mem = mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mem == MAP_FAILED) {
			Com_Printf(S_COLOR_YELLOW "Social mode: couldn't allocate G_Damage trampoline - damage blocking disabled\n");
			return;
		}
		gTrampoline = (byte*)mem;
	}

	// Rebuilt on every install: the module can be reloaded at a new address.
	memcpy(gTrampoline, gGDamage, SOCIAL_PROLOGUE_LEN);
	Social_WriteJump(gTrampoline + SOCIAL_PROLOGUE_LEN, gGDamage + SOCIAL_PROLOGUE_LEN);

	if (!Social_SetProtection(gGDamage, 5, PROT_READ | PROT_WRITE | PROT_EXEC)) {
		Com_Printf(S_COLOR_YELLOW "Social mode: mprotect on G_Damage failed - damage blocking disabled\n");
		return;
	}
	Social_WriteJump(gGDamage, (const void*)&Social_GDamageHook);
	Social_SetProtection(gGDamage, 5, PROT_READ | PROT_EXEC);

	gHookInstalled = qtrue;
	Com_Printf("Social mode: G_Damage hook installed\n");
}

static void Social_ApplyTimers(void)
{
	if (!gRespawnTimers) {
		return;
	}

	if (Social_Enabled()) {
		int secs = g_socialRespawnTime ? g_socialRespawnTime->integer : 3;
		if (secs < 1) {
			secs = 1;
		}
		gRespawnTimers->oneLife = secs;
		gRespawnTimers->twoLife = secs;
		gRespawnTimers->threeLife = secs;
		gRespawnTimers->changePenalty = secs;
		gTimersOverridden = qtrue;
	}
	else if (gTimersOverridden && gHaveOriginalTimers) {
		*gRespawnTimers = gOriginalTimers;
		gTimersOverridden = qfalse;
	}
}

static void Social_ForceRespawnMode(void)
{
	if (!gBNewRespawnMode || !Social_Enabled()) {
		return;
	}
	if (!*gBNewRespawnMode) {
		*gBNewRespawnMode = qtrue;
	}
	if (Cvar_VariableIntegerValue("RespawnMode") != 1) {
		Cvar_Set("RespawnMode", "1");
	}
}

// The round clock a client draws is its own clamp of the map's first .siege
// "Timed" value (cg_saga.c), so work that out the same way.
static int Social_ClientRoundTimeMs(int serverLimitMs)
{
	char path[MAX_QPATH];
	void* buf = NULL;
	int secs = serverLimitMs / 1000;

	Com_sprintf(path, sizeof(path), "maps/%s.siege", sv_mapname->string);
	if (FS_ReadFile(path, &buf) > 0 && buf) {
		const char* p = (const char*)buf;
		char* token;
		while (*(token = COM_ParseExt(&p, qtrue))) {
			if (!Q_stricmp(token, "Timed")) {
				secs = atoi(COM_ParseExt(&p, qfalse));
				break;
			}
		}
		FS_FreeFile(buf);
	}

	if (secs < SOCIAL_CLIENT_TIMER_MIN) {
		secs = SOCIAL_CLIENT_TIMER_MIN;
	}
	if (secs > SOCIAL_CLIENT_TIMER_MAX) {
		secs = SOCIAL_CLIENT_TIMER_MAX;
	}
	return secs * 1000;
}

static void Social_ApplyRoundTime(void)
{
	if (!Social_Enabled() || !g_socialRoundTime || g_socialRoundTime->integer <= 0) {
		return;
	}
	if (!gRebelTimeLimit || !gImperialTimeLimit || !gRebelCountdown || !gImperialCountdown) {
		return;
	}

	// Whichever team the map puts on the clock; neither means an untimed
	// map (MBII's tutorial maps), which stays untimed.
	int* limit = *gRebelTimeLimit ? gRebelTimeLimit : (*gImperialTimeLimit ? gImperialTimeLimit : NULL);
	int* countdown = (limit == gRebelTimeLimit) ? gRebelCountdown : gImperialCountdown;
	if (!limit) {
		return;
	}

	const int wantMs = g_socialRoundTime->integer * 1000;
	const int clientMs = Social_ClientRoundTimeMs(*limit);

	if (*countdown) {
		*countdown += wantMs - *limit;
	}
	*limit = wantMs;

	Cvar_Set("TimeAdd", va("%i", wantMs > clientMs ? wantMs - clientMs : 0));
}

void SV_SocialClientThink(client_t* cl)
{
	if (!Social_Enabled() || !g_socialDuels || !g_socialDuels->integer) {
		return;
	}
	if (!gEngageDuel || !gAuthenticity || !cl->gentity || !cl->gentity->playerState) {
		return;
	}
	if (cl->lastUsercmd.generic_cmd != GENCMD_BOW) {
		return;
	}

	// Challenging and accepting are the same call: bow at someone to
	// challenge them, they bow back at you to accept.
	const int realMode = gAuthenticity->integer;
	gAuthenticity->integer = SOCIAL_AUTHENTICITY_DUEL;
	GVM_CallNative(gEngageDuel, cl->gentity);
	gAuthenticity->integer = realMode;
}

static void Social_CheckDuels(void)
{
	if (!gCheckPrivateDuel || !g_socialDuels || !g_socialDuels->integer) {
		return;
	}
	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
			continue;
		}
		if (cl->gentity->playerState->duelInProgress) {
			GVM_CallNative(gCheckPrivateDuel, cl->gentity);
		}
	}
}

void SV_SocialClientCommand(client_t* cl)
{
	if (!Social_Enabled()) {
		return;
	}
	socialJoinState_t* js = &gJoinState[cl - svs.clients];
	const char* cmd = Cmd_Argv(0);

	if (!Q_stricmp(cmd, "siegeclass")) {
		Q_strncpyz(js->siegeClassCmd, Cmd_Cmd(), sizeof(js->siegeClassCmd));
		js->pickedAt = svs.time;
		js->rescues = 0;
	}
	else if (!Q_stricmp(cmd, "team")) {
		// Choosing to spectate means they don't want pulling back in.
		const char* team = Cmd_Argv(1);
		if (!Q_stricmp(team, "spectator") || !Q_stricmp(team, "s") || !Q_stricmp(team, "follow1") || !Q_stricmp(team, "follow2")) {
			js->siegeClassCmd[0] = '\0';
		}
	}
}

// Actually in the game: on red or blue, and moving about. A freshly connected
// player sits on TEAM_FREE rather than TEAM_SPECTATOR until they've been to
// spectator once - which is exactly the state RESPAWN_MODE never gives a
// spawn timer to - so "not spectator" isn't enough. pm_type is the same
// stock alive test gungame.cpp uses.
static qboolean Social_IsSpawned(const playerState_t* ps)
{
	const int team = ps->persistant[PERS_TEAM];
	if (team != TEAM_RED && team != TEAM_BLUE) {
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

static void Social_ReplayClientCommand(client_t* cl, const char* line)
{
	Cmd_TokenizeString(line);
	GVM_ClientCommand(cl - svs.clients);
}

static void Social_PickBotClass(int clientNum)
{
	int red = 0, blue = 0;
	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		if (i == clientNum || cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
			continue;
		}
		int team = cl->gentity->playerState->persistant[PERS_TEAM];
		if (team == TEAM_SPECTATOR) {
			team = gJoinState[i].botSide; // another bot still on its way in
		}
		red += (team == TEAM_RED);
		blue += (team == TEAM_BLUE);
	}

	socialJoinState_t* js = &gJoinState[clientNum];
	const qboolean hero = (red <= blue) ? qtrue : qfalse;
	const char* const* list = hero ? kBotHeroClasses : kBotVillainClasses;
	const int count = hero ? ARRAY_LEN(kBotHeroClasses) : ARRAY_LEN(kBotVillainClasses);

	// Same shape as the class menu sends: class, model/saber variant, RGB.
	Com_sprintf(js->siegeClassCmd, sizeof(js->siegeClassCmd), "siegeclass %s 0 0 255 255 255",
		list[Q_irand(0, count - 1)]);
	js->botSide = hero ? TEAM_RED : TEAM_BLUE;
}

static void Social_RescueStuckJoiners(void)
{
	if (!gSiegeRoundBegun) {
		return;
	}
	// Before the round starts everyone's waiting for it anyway - except
	// bots, whose first class pick is what starts a bots-only round.
	const qboolean roundBegun = *gSiegeRoundBegun;
	const int timerMs = (g_socialRespawnTime ? g_socialRespawnTime->integer : 3) * 1000;

	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		socialJoinState_t* js = &gJoinState[i];

		if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
			memset(js, 0, sizeof(*js));
			continue;
		}
		playerState_t* ps = cl->gentity->playerState;
		if (Social_IsSpawned(ps)) {
			js->siegeClassCmd[0] = '\0'; // spawned - job done
			js->lastSpawnedAt = svs.time;
			js->botSide = 0;
			continue;
		}

		if (cl->netchan.remoteAddress.type == NA_BOT) {
			if (!g_socialBots || !g_socialBots->integer) {
				continue;
			}
			// Without respawn mode a mid-round pick just queues for the next
			// round, like a player's would; that round's start spawns it.
			const qboolean respawnMode = (gBNewRespawnMode && *gBNewRespawnMode) ? qtrue : qfalse;
			if (roundBegun && !respawnMode && js->botSide) {
				continue;
			}
			// Not if just dead and waiting out the normal respawn timer; a
			// fresh class (and side) on every try, in case one is refused -
			// e.g. by MBII's team balance - or doesn't exist.
			if (svs.time - js->lastSpawnedAt < timerMs + SOCIAL_STUCK_GRACE_MS ||
				(js->lastRescueAt && svs.time - js->lastRescueAt < SOCIAL_STUCK_RETRY_MS)) {
				continue;
			}
			// Already queued for a round that hasn't started: that start is
			// what spawns it, so don't keep re-picking through the countdown.
			if (!roundBegun && js->botSide) {
				continue;
			}
			Social_PickBotClass(i);
			js->lastRescueAt = svs.time;
			Com_Printf("Social mode: bot %d (%s) not spawned - joining as %s\n",
				i, cl->name, js->siegeClassCmd + strlen("siegeclass "));
			Social_ReplayClientCommand(cl, "team spectator");
			Social_ReplayClientCommand(cl, js->siegeClassCmd);
			continue;
		}

		if (!Social_Enabled() || !js->siegeClassCmd[0]) {
			continue;
		}
		if (!roundBegun) {
			continue;
		}
		if (svs.time - js->pickedAt < timerMs + SOCIAL_STUCK_GRACE_MS) {
			continue;
		}
		if (js->rescues >= SOCIAL_STUCK_MAX_TRIES || (js->rescues && svs.time - js->lastRescueAt < SOCIAL_STUCK_RETRY_MS)) {
			continue;
		}

		js->rescues++;
		js->lastRescueAt = svs.time;
		Com_Printf("Social mode: client %d (%s) picked a class %ds ago but hasn't spawned - re-joining them (try %d/%d)\n",
			i, cl->name, (svs.time - js->pickedAt) / 1000, js->rescues, SOCIAL_STUCK_MAX_TRIES);

		char line[MAX_STRING_CHARS];
		Q_strncpyz(line, js->siegeClassCmd, sizeof(line)); // the replay below re-records it
		Social_ReplayClientCommand(cl, "team spectator");
		Social_ReplayClientCommand(cl, line);
	}
}

void SV_SocialGameInit(void)
{
	void* dll = GVM_GetDllHandle();
	if (!dll) {
		return; // legacy/QVM game, nothing to look up
	}

	if (dll != gSocialDll) {
		gSocialDll = dll;
		gBNewRespawnMode = (qboolean*)Sys_LoadFunction(dll, "bNewRespawnMode");
		gRespawnTimers = (socialRespawnTimers_t*)Sys_LoadFunction(dll, "ctfcqRespawnTimers");
		gGDamage = (byte*)Sys_LoadFunction(dll, "G_Damage");
		gEngageDuel = (void (*)(void*))Sys_LoadFunction(dll, "Cmd_EngageDuel_f");
		gCheckPrivateDuel = (void (*)(void*))Sys_LoadFunction(dll, "G_CheckPrivateDuel");
		gAuthenticity = (vmCvar_t*)Sys_LoadFunction(dll, "g_Authenticity");
		gRebelTimeLimit = (int*)Sys_LoadFunction(dll, "rebel_time_limit");
		gImperialTimeLimit = (int*)Sys_LoadFunction(dll, "imperial_time_limit");
		gRebelCountdown = (int*)Sys_LoadFunction(dll, "gRebelCountdown");
		gImperialCountdown = (int*)Sys_LoadFunction(dll, "gImperialCountdown");
		gSiegeRoundBegun = (qboolean*)Sys_LoadFunction(dll, "gSiegeRoundBegun");
		gHookInstalled = qfalse;

		// Fresh module image = fresh .data, so re-capture the stock values.
		gHaveOriginalTimers = qfalse;
		gTimersOverridden = qfalse;
		if (gRespawnTimers) {
			gOriginalTimers = *gRespawnTimers;
			gHaveOriginalTimers = qtrue;

			// Belt and braces on the struct layout: stock values are seconds
			// in MBII's own 1-119 accepted range.
			const int* t = (const int*)&gOriginalTimers;
			for (int i = 0; i < 4; i++) {
				if (t[i] < 1 || t[i] > 119) {
					Com_Printf(S_COLOR_YELLOW "Social mode: ctfcqRespawnTimers doesn't look like MBII's respawn timers - respawn override disabled\n");
					gRespawnTimers = NULL;
					break;
				}
			}
		}

		if (!gBNewRespawnMode || !gRespawnTimers || !gGDamage) {
			Com_Printf(S_COLOR_YELLOW "Social mode: missing MBII symbols (bNewRespawnMode=%p ctfcqRespawnTimers=%p G_Damage=%p)\n",
				(void*)gBNewRespawnMode, (void*)gRespawnTimers, (void*)gGDamage);
		}
		if (!gEngageDuel || !gCheckPrivateDuel || !gAuthenticity) {
			Com_Printf(S_COLOR_YELLOW "Social mode: missing MBII duel symbols - duels disabled\n");
		}
		if (!gRebelTimeLimit || !gImperialTimeLimit || !gRebelCountdown || !gImperialCountdown) {
			Com_Printf(S_COLOR_YELLOW "Social mode: missing MBII round timer symbols - round time override disabled\n");
		}
	}

	// Only patched on servers actually using social mode, so every other
	// caded.i386 instance runs MBII's G_Damage untouched. Switching the cvar
	// on mid-map installs it from SV_SocialFrame; once in, the hook checks
	// the cvar per call, so switching it back off takes effect immediately.
	gHookAttempted = qfalse;
	if (gGDamage && Social_Enabled()) {
		Social_InstallHook();
	}

	Social_ApplyTimers();
	Social_ForceRespawnMode();
	Social_ApplyRoundTime();

	// New round: any bot still unspawned picks again for it.
	for (int i = 0; i < MAX_CLIENTS; i++) {
		gJoinState[i].botSide = 0;
		gJoinState[i].lastRescueAt = 0;
	}
}

void SV_SocialFrame(void)
{
	// MBII's respawn timer struct and respawn flag aren't cvars, so there's
	// no modification callback - just keep them in line each frame.
	Social_ApplyTimers();
	Social_ForceRespawnMode();

	if (Social_Enabled()) {
		if (gGDamage && !gHookAttempted) {
			Social_InstallHook();
		}
		Social_CheckDuels();
	}
	if (Social_Enabled() || (g_socialBots && g_socialBots->integer)) {
		Social_RescueStuckJoiners();
	}
}
