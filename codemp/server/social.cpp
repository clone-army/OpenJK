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
static void (*gSetTeam)(void* ent, char* team) = NULL;
static void* (*gNPCSpawnType)(void* ent, char* type, char* targetname, int isVehicle, int asIfPlayer, int siegeTeam) = NULL;
static void (*gFreeEntity)(void* ent) = NULL;
static void (*gSetEnemy)(void* self, void* enemy) = NULL;
static void (*gSetMoveGoal)(void* ent, float* point, int radius, int isNavGoal, int combatPoint, void* targetEnt) = NULL;
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
	int lastSpawnCmdAt;                   // !spawn cooldown
	int spawnCmdTries;                    // !spawn presses since last spawned
	int enteredAt;                        // first seen in the game this connection/map
	qboolean choseSpectator;              // went to spectator on purpose - leave them be
	qboolean autoSpawned;                 // g_socialAutoSpawn has put them in once
	int lastKillAt;                       // !kill cooldown
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

static qboolean Social_BarFightHit(void* targ, void* attacker);

static void Social_GDamageHook(void* targ, void* inflictor, void* attacker, float* dir, float* point, int damage, int dflags, int mod)
{
	if (Social_Enabled() && Social_BarFightHit(targ, attacker)) {
		((GDamageFn)gTrampoline)(targ, inflictor, attacker, dir, point, damage, dflags | SOCIAL_DAMAGE_NO_TKPOINTS, mod);
		return;
	}
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
		js->choseSpectator = qfalse;
	}
	else if (!Q_stricmp(cmd, "team")) {
		// Choosing to spectate means they don't want pulling back in.
		const char* team = Cmd_Argv(1);
		if (!Q_stricmp(team, "spectator") || !Q_stricmp(team, "s") || !Q_stricmp(team, "follow1") || !Q_stricmp(team, "follow2")) {
			js->siegeClassCmd[0] = '\0';
			js->choseSpectator = qtrue;
		}
	}
}

// Actually in the game: on red or blue, and moving about. A freshly connected
// player sits on TEAM_FREE rather than TEAM_SPECTATOR until they've been to
// spectator once - which is exactly the state RESPAWN_MODE never gives a
// spawn timer to - so "not spectator" isn't enough. pm_type is the same
// stock alive test gungame.cpp uses. A spectator following someone carries a
// copy of *their* playerState - team, pm_type and all - so it only counts if
// the playerState is the player's own (clientNum).
static qboolean Social_IsSpawned(const playerState_t* ps, int clientNum);

// Whether a client is in the game on a team (not spectating, not dead).
qboolean SV_ClientIsSpawned(client_t* cl)
{
	if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
		return qfalse;
	}
	return Social_IsSpawned(cl->gentity->playerState, (int)(cl - svs.clients));
}

static qboolean Social_IsSpawned(const playerState_t* ps, int clientNum)
{
	if (ps->clientNum != clientNum) {
		return qfalse;
	}
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

// A Legends class on the emptier side, into js->siegeClassCmd (and
// js->botSide) - for bots, and for players whose own pick won't take.
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

// Moves a client to spectator, then replays a class pick. MBII only spawns
// someone from a class pick if they're a spectator at the time (anyone else
// just has it queued for their next respawn), and a fresh joiner sits on
// TEAM_FREE, which never respawns. Its "team spectator" command refuses to
// switch mid-round, so the rescue used to replay picks that only ever
// queued. SetTeam itself, called directly, has no such rule.
static void Social_JoinWithClass(client_t* cl, const char* classCmd)
{
	char line[MAX_STRING_CHARS];
	Q_strncpyz(line, classCmd, sizeof(line)); // the replay below re-records it

	if (gSetTeam && cl->gentity) {
		void* old = GVM_BeginNative();
		gSetTeam(cl->gentity, (char*)"spectator");
		GVM_EndNative(old);
	} else {
		Social_ReplayClientCommand(cl, "team spectator");
	}
	Social_ReplayClientCommand(cl, line);
}

// "h9_Anakin" -> "Anakin", for messages.
static const char* Social_ClassDisplayName(const char* classCmd)
{
	static char name[64];
	char cls[64] = "";
	sscanf(classCmd, "%*s %63s", cls);
	const char* underscore = strchr(cls, '_');
	Q_strncpyz(name, underscore ? underscore + 1 : cls, sizeof(name));
	return name;
}

static const char* Social_SpawnPlayer(client_t* cl);

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
		if (Social_IsSpawned(ps, i)) {
			js->siegeClassCmd[0] = '\0'; // spawned - job done
			js->lastSpawnedAt = svs.time;
			js->botSide = 0;
			js->spawnCmdTries = 0;
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
			Social_JoinWithClass(cl, js->siegeClassCmd);
			continue;
		}

		// Joined but not in the game after g_socialAutoSpawn seconds: put
		// them in, as !spawn would - once, and never anyone who chose to
		// spectate. The class menu is still there to change class.
		if (!js->enteredAt) {
			js->enteredAt = svs.time;
		}
		const int autoMs = (g_socialAutoSpawn ? g_socialAutoSpawn->integer : 0) * 1000;
		if (Social_Enabled() && autoMs > 0 && roundBegun && !js->autoSpawned && !js->choseSpectator &&
			!js->lastSpawnedAt && svs.time - js->enteredAt >= autoMs &&
			!(js->siegeClassCmd[0] && svs.time - js->pickedAt < timerMs + SOCIAL_STUCK_GRACE_MS)) {
			js->autoSpawned = qtrue;
			const char* cls = Social_SpawnPlayer(cl);
			Com_Printf("Social mode: client %d (%s) not in the game after %ds - auto-spawning as %s\n",
				i, cl->name, autoMs / 1000, cls);
			SV_SendServerCommand(cl, "chat \"^5[Social]^7 Putting you in as ^3%s^7 - pick your own class from the menu any time.\"\n", cls);
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
		// Their own class first; if that hasn't worked, it may be full.
		if (js->rescues > 1) {
			Social_PickBotClass(i);
		}
		Com_Printf("Social mode: client %d (%s) picked a class %ds ago but hasn't spawned - re-joining them as %s (try %d/%d)\n",
			i, cl->name, (svs.time - js->pickedAt) / 1000, Social_ClassDisplayName(js->siegeClassCmd),
			js->rescues, SOCIAL_STUCK_MAX_TRIES);
		const int rescues = js->rescues;
		Social_JoinWithClass(cl, js->siegeClassCmd);
		js->rescues = rescues; // the replayed pick resets it
	}
}

// Into the game as their own class pick, or (none, or it already failed)
// a class on the emptier side. Returns the class it used, for messages.
static const char* Social_SpawnPlayer(client_t* cl)
{
	socialJoinState_t* js = &gJoinState[cl - svs.clients];
	const qboolean ownPick = (js->siegeClassCmd[0] && js->spawnCmdTries == 0) ? qtrue : qfalse;
	if (!ownPick) {
		Social_PickBotClass(cl - svs.clients);
	}
	js->spawnCmdTries++;
	Social_JoinWithClass(cl, js->siegeClassCmd);
	return Social_ClassDisplayName(js->siegeClassCmd);
}

// "!spawn": gets a player stuck in spectator into the game - their own
// class pick the first time, a class on the emptier side after that (or if
// they never picked one). qfalse (pass through as chat) off social servers.
static qboolean Social_KillCommand(client_t* cl)
{
	socialJoinState_t* js = &gJoinState[cl - svs.clients];
	const playerState_t* ps = cl->gentity ? cl->gentity->playerState : NULL;

	if (!ps || !Social_IsSpawned(ps, cl - svs.clients)) {
		SV_SendServerCommand(cl, "chat \"^5[Social]^7 You're not in the game - ^5!spawn ^7gets you in.\"\n");
		return qtrue;
	}
	if (ps->duelInProgress) {
		SV_SendServerCommand(cl, "chat \"^5[Social]^7 Not in the middle of a duel - finish it first.\"\n");
		return qtrue;
	}
	if (js->lastKillAt && svs.time - js->lastKillAt < 5000) {
		return qtrue;
	}
	js->lastKillAt = svs.time;
	// MBII's own /kill: a normal death, then the usual respawn timer.
	Cmd_TokenizeString("kill");
	GVM_ClientCommand(cl - svs.clients);
	return qtrue;
}

qboolean SV_SocialSpawnCommand(client_t* cl, const char* command)
{
	if (!Social_Enabled()) {
		return qfalse;
	}
	if (!Q_stricmp(command, "kill")) {
		return Social_KillCommand(cl);
	}
	if (Q_stricmp(command, "spawn")) {
		return qfalse;
	}
	const int i = cl - svs.clients;
	socialJoinState_t* js = &gJoinState[i];

	if (!cl->gentity || !cl->gentity->playerState) {
		return qtrue;
	}
	if (Social_IsSpawned(cl->gentity->playerState, i)) {
		SV_SendServerCommand(cl, "chat \"^5[Social]^7 You're already in the game.\"\n");
		return qtrue;
	}
	if (js->lastSpawnCmdAt && svs.time - js->lastSpawnCmdAt < 10000) {
		SV_SendServerCommand(cl, "chat \"^5[Social]^7 Hang on - give it a few seconds to spawn you.\"\n");
		return qtrue;
	}
	js->lastSpawnCmdAt = svs.time;

	const qboolean ownPick = (js->siegeClassCmd[0] && js->spawnCmdTries == 0) ? qtrue : qfalse;
	const char* cls = Social_SpawnPlayer(cl);
	Com_Printf("Social mode: client %d (%s) used !spawn - joining as %s\n", i, cl->name, cls);
	if (ownPick) {
		SV_SendServerCommand(cl, "chat \"^5[Social]^7 Getting you in as ^3%s^7...\"\n", cls);
	} else {
		SV_SendServerCommand(cl, "chat \"^5[Social]^7 Getting you in as ^3%s^7 - pick your own class from the menu any time.\"\n", cls);
	}
	if (gSiegeRoundBegun && !*gSiegeRoundBegun) {
		SV_SendServerCommand(cl, "chat \"^5[Social]^7 The round's about to start - you'll spawn when it does.\"\n");
	}
	return qtrue;
}

// --- Cantina NPCs -----------------------------------------------------------
//
// g_socialNpcs (continued in g_socialNpcs2..4, since one cvar holds 255
// characters) lists NPCs to stand about the map, "type x y z yaw [pose]"
// each, separated by ';'. The pose: "sit" (sits there), "idle" (stands
// there, now and then looking about or gesturing), "bartend" (idle, more
// often, and gestures when !bartender answers), "roam" (wanders freely -
// MBII's droids have their own wander), or nothing (just stands). - e.g. "bartender 4008 -550 -1769 169" (MBII ships a
// neutral, unarmed "bartender", the JKO one). x y z is where a player's
// origin would be (a /viewpos reading, minus 36 for eye height).
//
// MBII's NPC_SpawnType puts a spawner 64 units in front of a player and
// returns the new NPC before it has begun (NPC_Begin runs next frame, from
// the NPC's playerState origin, facing the spawner's yaw - which comes from
// that player's view). So: any player on, their view turned to the NPC's
// yaw for the call, then the NPC's playerState origin set to its spot
// before it begins. Only engine-known entity fields are touched. Each
// round's G_InitGame clears every entity, so they're spawned again; and
// they're held on their spot, since their AI may wander. Damage to NPCs is
// already blocked on social servers (Social_GDamageHook).
#define SOCIAL_MAX_NPCS 16

typedef struct {
	char   type[32];
	vec3_t origin;
	float  yaw;
	int    ent;        // entity number once spawned, -1 before
	int    nextTry;
	int    deadAt;     // when it was seen dead (0 = alive)
	qboolean roam;     // free to wander, not held on its spot
	int    pose;       // SOCIAL_POSE_*
	int    nextAnim;   // idle/bartend: next gesture
	int    offSince;   // first seen off its spot (0 = on it)
	int    failures;   // spawns MBII refused, in a row
	char   route[32];  // patrol: the route it walks
	int    wp;         // ...the point it's heading for
	int    nextPatrol;
	int    goalAt;     // when its move goal was last given
	float  bestDist;   // closest it's got to that point
	int    stuckSince; // since when it's not got any closer
	int    stuckCount;
} socialNpc_t;

enum { SOCIAL_POSE_STAND, SOCIAL_POSE_SIT, SOCIAL_POSE_IDLE, SOCIAL_POSE_BARTEND, SOCIAL_POSE_ROAM, SOCIAL_POSE_PATROL };

static const char* const kNpcIdleAnims[] = {
	"BOTH_GUARD_LOOKAROUND1", "BOTH_HEADNOD", "BOTH_TALK1", "BOTH_HEADSHAKE", "BOTH_GUARD_LOOKAROUND1",
};
static const char* const kNpcBartendAnims[] = {
	"BOTH_TALK1", "BOTH_HEADNOD", "BOTH_GUARD_LOOKAROUND1", "BOTH_TALK1", "BOTH_HEADSHAKE",
};

static socialNpc_t gSocialNpcs[SOCIAL_MAX_NPCS];
static int gSocialNpcCount = 0;
static char gSocialNpcsParsed[MAX_CVAR_VALUE_STRING * 4 + 4] = "\x01"; // never a real value, so the first frame parses

static qboolean Social_NpcAlive(const socialNpc_t* n);

static void Social_ParseNpcs(void)
{
	char want[MAX_CVAR_VALUE_STRING * 4 + 4];
	Com_sprintf(want, sizeof(want), "%s;%s;%s;%s",
		g_socialNpcs ? g_socialNpcs->string : "", g_socialNpcs2 ? g_socialNpcs2->string : "",
		g_socialNpcs3 ? g_socialNpcs3->string : "", g_socialNpcs4 ? g_socialNpcs4->string : "");
	if (!strcmp(want, gSocialNpcsParsed)) {
		return;
	}
	Q_strncpyz(gSocialNpcsParsed, want, sizeof(gSocialNpcsParsed));

	// A new list: the old NPCs go, or they'd stay alongside the new ones.
	for (int i = 0; i < gSocialNpcCount; i++) {
		if (Social_NpcAlive(&gSocialNpcs[i]) && gFreeEntity) {
			void* old = GVM_BeginNative();
			gFreeEntity(SV_GentityNum(gSocialNpcs[i].ent));
			GVM_EndNative(old);
		}
	}
	gSocialNpcCount = 0;

	char buf[MAX_CVAR_VALUE_STRING * 4 + 4];
	Q_strncpyz(buf, want, sizeof(buf));
	for (char* entry = strtok(buf, ";"); entry && gSocialNpcCount < SOCIAL_MAX_NPCS; entry = strtok(NULL, ";")) {
		socialNpc_t* n = &gSocialNpcs[gSocialNpcCount];
		memset(n, 0, sizeof(*n));
		char flag[48] = "";
		if (sscanf(entry, "%31s %f %f %f %f %47s", n->type, &n->origin[0], &n->origin[1], &n->origin[2], &n->yaw, flag) >= 5) {
			n->ent = -1;
			n->roam = !Q_stricmp(flag, "roam") ? qtrue : qfalse;
			if (!Q_stricmpn(flag, "patrol:", 7) && flag[7]) {
				Q_strncpyz(n->route, flag + 7, sizeof(n->route));
				n->roam = qtrue; // never held on its spot
			}
			n->pose = n->route[0] ? SOCIAL_POSE_PATROL :
				n->roam ? SOCIAL_POSE_ROAM :
				!Q_stricmp(flag, "sit") ? SOCIAL_POSE_SIT :
				!Q_stricmp(flag, "idle") ? SOCIAL_POSE_IDLE :
				!Q_stricmp(flag, "bartend") ? SOCIAL_POSE_BARTEND : SOCIAL_POSE_STAND;
			gSocialNpcCount++;
		} else if (entry[strspn(entry, " ")]) {
			Com_Printf("Social mode: g_socialNpcs entry \"%s\" isn't \"type x y z yaw\" - skipped\n", entry);
		}
	}
}

static qboolean Social_NpcAlive(const socialNpc_t* n)
{
	if (n->ent < MAX_CLIENTS || n->ent >= sv.num_entities) {
		return qfalse;
	}
	const sharedEntity_t* e = SV_GentityNum(n->ent);
	return (e->r.linked && e->playerState && e->s.number == n->ent) ? qtrue : qfalse;
}

// Keeps a seated NPC seated, and gives idle ones something to do now and then.
static void Social_NpcAnimate(socialNpc_t* n, sharedEntity_t* e)
{
	const playerState_t* ps = e->playerState;

	if (n->pose == SOCIAL_POSE_SIT) {
		if (ps->legsTimer < 300 || ps->torsoTimer < 300) {
			SV_EntitySetAnim(e, "BOTH_SIT1", qfalse);
		}
		return;
	}
	if (n->pose != SOCIAL_POSE_IDLE && n->pose != SOCIAL_POSE_BARTEND) {
		return;
	}
	if (!n->nextAnim) {
		n->nextAnim = svs.time + Q_irand(2000, 8000); // don't all start at once
		return;
	}
	if (svs.time < n->nextAnim) {
		return;
	}
	if (n->pose == SOCIAL_POSE_BARTEND) {
		SV_EntitySetAnim(e, kNpcBartendAnims[Q_irand(0, ARRAY_LEN(kNpcBartendAnims) - 1)], qfalse);
		n->nextAnim = svs.time + Q_irand(6000, 12000);
	} else {
		SV_EntitySetAnim(e, kNpcIdleAnims[Q_irand(0, ARRAY_LEN(kNpcIdleAnims) - 1)], qfalse);
		n->nextAnim = svs.time + Q_irand(12000, 25000);
	}
}

// A gesture from every live NPC of a type (the bartender, as he answers).
void SV_SocialNpcGesture(const char* type, const char* anim)
{
	for (int i = 0; i < gSocialNpcCount; i++) {
		socialNpc_t* n = &gSocialNpcs[i];
		if (!Q_stricmp(n->type, type) && Social_NpcAlive(n)) {
			SV_EntitySetAnim(SV_GentityNum(n->ent), anim, qfalse);
			n->nextAnim = svs.time + Q_irand(6000, 12000);
		}
	}
}

// --- Patrol routes ---------------------------------------------------------
//
// An admin (an economy account listed in g_socialAdmins) walks a loop and
// types "!wp add <route>" at each point; they're kept in order, in
// social_routes.txt in the instance's own MBII folder, one "route x y z"
// line per point. An NPC entry with the pose "patrol:<route>" walks them
// round and round: MBII's NPC_SetMoveGoal gives it each point in turn (its
// default AI walks to a move goal while it has no enemy, facing where it's
// going). Every half second: a point within 40 units is reached; one it
// hasn't got any closer to in 8s is skipped, and three skips in a row put
// it straight on the point, so it can't stay stuck.
#define SOCIAL_MAX_ROUTES      8
#define SOCIAL_ROUTE_POINTS    32

typedef struct {
	char   name[32];
	vec3_t pts[SOCIAL_ROUTE_POINTS];
	int    count;
} socialRoute_t;

static socialRoute_t gRoutes[SOCIAL_MAX_ROUTES];
static int gRouteCount = 0;
static qboolean gRoutesLoaded = qfalse;

static const char* Social_RoutesPath(void)
{
	return va("%s/%s/social_routes.txt", Cvar_VariableString("fs_homepath"), Cvar_VariableString("fs_game"));
}

static void Social_LoadRoutes(void)
{
	gRoutesLoaded = qtrue;
	gRouteCount = 0;
	FILE* f = fopen(Social_RoutesPath(), "r");
	if (!f) {
		return;
	}
	char line[256], name[32];
	vec3_t p;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%31s %f %f %f", name, &p[0], &p[1], &p[2]) != 4) {
			continue;
		}
		socialRoute_t* r = NULL;
		for (int i = 0; i < gRouteCount && !r; i++) {
			if (!Q_stricmp(gRoutes[i].name, name)) {
				r = &gRoutes[i];
			}
		}
		if (!r && gRouteCount < SOCIAL_MAX_ROUTES) {
			r = &gRoutes[gRouteCount++];
			memset(r, 0, sizeof(*r));
			Q_strncpyz(r->name, name, sizeof(r->name));
		}
		if (r && r->count < SOCIAL_ROUTE_POINTS) {
			VectorCopy(p, r->pts[r->count++]);
		}
	}
	fclose(f);
}

static void Social_SaveRoutes(void)
{
	FILE* f = fopen(Social_RoutesPath(), "w");
	if (!f) {
		Com_Printf("Social mode: couldn't write %s\n", Social_RoutesPath());
		return;
	}
	for (int i = 0; i < gRouteCount; i++) {
		for (int p = 0; p < gRoutes[i].count; p++) {
			fprintf(f, "%s %.0f %.0f %.0f\n", gRoutes[i].name, gRoutes[i].pts[p][0], gRoutes[i].pts[p][1], gRoutes[i].pts[p][2]);
		}
	}
	fclose(f);
}

static socialRoute_t* Social_FindRoute(const char* name, qboolean create)
{
	if (!gRoutesLoaded) {
		Social_LoadRoutes();
	}
	for (int i = 0; i < gRouteCount; i++) {
		if (!Q_stricmp(gRoutes[i].name, name)) {
			return &gRoutes[i];
		}
	}
	if (!create || gRouteCount >= SOCIAL_MAX_ROUTES) {
		return NULL;
	}
	socialRoute_t* r = &gRoutes[gRouteCount++];
	memset(r, 0, sizeof(*r));
	Q_strncpyz(r->name, name, sizeof(r->name));
	return r;
}

static void Social_Tell(client_t* cl, const char* text)
{
	SV_SendServerCommand(cl, "chat \"^5[Social]^7 %s\"\n", text);
}

// Logged into an account listed in g_socialAdmins (space separated).
static qboolean Social_IsAdmin(client_t* cl)
{
	if (!cl->economyHandle[0] || !g_socialAdmins) {
		return qfalse;
	}
	char buf[MAX_CVAR_VALUE_STRING];
	Q_strncpyz(buf, g_socialAdmins->string, sizeof(buf));
	for (char* w = strtok(buf, " ,"); w; w = strtok(NULL, " ,")) {
		if (!Q_stricmp(w, cl->economyHandle)) {
			return qtrue;
		}
	}
	return qfalse;
}

// "x y z : yaw", origin (a standing player's, 24 above the floor - the
// height an NPC spot wants) rather than /viewpos's eye height.
static const char* Social_PositionText(const playerState_t* ps)
{
	return va("%.0f %.0f %.0f : %.0f", ps->origin[0], ps->origin[1], ps->origin[2], AngleNormalize360(ps->viewangles[YAW]));
}

qboolean SV_SocialWhereCommand(client_t* cl)
{
	if (!cl->gentity || !cl->gentity->playerState) {
		return qtrue;
	}
	const char* pos = Social_PositionText(cl->gentity->playerState);
	Social_Tell(cl, va("You're at ^3%s ^7(x y z : facing - use it for an NPC spot as x y z yaw).", pos));
	Com_Printf("Social mode: %s is at %s\n", cl->name, pos);
	return qtrue;
}

// rcon "where <player>"
void SV_SocialWhere_f(void)
{
	if (Cmd_Argc() < 2) {
		Com_Printf("Usage: where <player>\n");
		return;
	}
	client_t* cl = SV_BetterGetPlayerByHandle(Cmd_Argv(1));
	if (!cl || cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
		Com_Printf("No such player: %s\n", Cmd_Argv(1));
		return;
	}
	Com_Printf("%s^7: %s\n", cl->name, Social_PositionText(cl->gentity->playerState));
}

// "!wp add <route>", "!wp undo <route>", "!wp clear <route>", "!wp list"
qboolean SV_SocialWaypointCommand(client_t* cl, const char* args)
{
	char verb[16] = "", name[32] = "";
	sscanf(args, "%15s %31s", verb, name);

	if (!Social_IsAdmin(cl)) {
		Social_Tell(cl, "Only admins can edit NPC routes (log in with an admin account).");
		return qtrue;
	}
	if (!Q_stricmp(verb, "list") || !verb[0]) {
		if (!gRoutesLoaded) {
			Social_LoadRoutes();
		}
		if (!gRouteCount) {
			Social_Tell(cl, "No routes yet. Walk the loop and ^5!wp add <route> ^7at each point.");
		}
		for (int i = 0; i < gRouteCount; i++) {
			Social_Tell(cl, va("^3%s^7: %d points - use ^5patrol:%s ^7as an NPC's pose.", gRoutes[i].name, gRoutes[i].count, gRoutes[i].name));
		}
		return qtrue;
	}
	if (!name[0]) {
		Social_Tell(cl, "Usage: ^5!wp add <route>^7, ^5!wp undo <route>^7, ^5!wp clear <route>^7, ^5!wp list");
		return qtrue;
	}
	if (!Q_stricmp(verb, "add")) {
		socialRoute_t* r = Social_FindRoute(name, qtrue);
		if (!r || !cl->gentity || !cl->gentity->playerState) {
			Social_Tell(cl, "No room for another route.");
		} else if (r->count >= SOCIAL_ROUTE_POINTS) {
			Social_Tell(cl, va("^3%s ^7already has %d points.", r->name, SOCIAL_ROUTE_POINTS));
		} else {
			VectorCopy(cl->gentity->playerState->origin, r->pts[r->count++]);
			Social_SaveRoutes();
			Social_Tell(cl, va("Point %d added to ^3%s ^7at %s.", r->count, r->name, Social_PositionText(cl->gentity->playerState)));
		}
	} else if (!Q_stricmp(verb, "undo")) {
		socialRoute_t* r = Social_FindRoute(name, qfalse);
		if (r && r->count) {
			r->count--;
			Social_SaveRoutes();
			Social_Tell(cl, va("Removed the last point of ^3%s ^7(%d left).", r->name, r->count));
		} else {
			Social_Tell(cl, "Nothing to undo.");
		}
	} else if (!Q_stricmp(verb, "clear")) {
		socialRoute_t* r = Social_FindRoute(name, qfalse);
		if (r) {
			r->count = 0;
			Social_SaveRoutes();
			Social_Tell(cl, va("Cleared ^3%s^7.", name));
		}
	} else {
		Social_Tell(cl, "Usage: ^5!wp add <route>^7, ^5!wp undo <route>^7, ^5!wp clear <route>^7, ^5!wp list");
	}
	return qtrue;
}

// For the NPC navigation graph (sv_gameapi.cpp): the routes, freshly read.
int SV_SocialRouteCount(void)
{
	Social_LoadRoutes();
	return gRouteCount;
}

int SV_SocialRoutePoints(int route, vec3_t* out, int max)
{
	if (route < 0 || route >= gRouteCount) {
		return 0;
	}
	const int n = Q_min(gRoutes[route].count, max);
	for (int i = 0; i < n; i++) {
		VectorCopy(gRoutes[route].pts[i], out[i]);
	}
	return n;
}

static void Social_NpcPatrol(socialNpc_t* n, sharedEntity_t* e)
{
	socialRoute_t* r = Social_FindRoute(n->route, qfalse);
	if (!r || r->count < 1 || !gSetMoveGoal || svs.time < n->nextPatrol) {
		return;
	}
	n->nextPatrol = svs.time + 500;
	if (n->wp >= r->count) {
		n->wp = 0;
	}

	playerState_t* ps = e->playerState;
	const float* p = r->pts[n->wp];
	const float dx = ps->origin[0] - p[0], dy = ps->origin[1] - p[1];
	const float d = sqrtf(dx * dx + dy * dy);

	if (d < 40.0f) {
		n->wp = (n->wp + 1) % r->count;
		n->goalAt = 0;
		n->bestDist = 1e9f;
		n->stuckSince = svs.time;
		n->stuckCount = 0;
		return;
	}
	if (d < n->bestDist - 16.0f) {
		n->bestDist = d;
		n->stuckSince = svs.time;
	} else if (n->stuckSince && svs.time - n->stuckSince > 8000) {
		// Not getting any closer: on to the next point - and after three
		// of those, straight onto this one.
		if (++n->stuckCount >= 3) {
			VectorCopy(p, ps->origin);
			VectorClear(ps->velocity);
			n->stuckCount = 0;
		}
		n->wp = (n->wp + 1) % r->count;
		n->goalAt = 0;
		n->bestDist = 1e9f;
		n->stuckSince = svs.time;
		return;
	}
	if (!n->stuckSince) {
		n->stuckSince = svs.time;
		n->bestDist = d;
	}
	// Given again every few seconds, in case its AI let go of it.
	if (!n->goalAt || svs.time - n->goalAt > 4000) {
		vec3_t goal;
		VectorCopy(p, goal);
		void* old = GVM_BeginNative();
		gSetMoveGoal(e, goal, 24, 0, -1, NULL);
		GVM_EndNative(old);
		n->goalAt = svs.time;
	}
}

// Someone standing on (or next to) an NPC's spot.
static qboolean Social_SpotOccupied(const vec3_t spot, int npcEnt)
{
	for (int i = 0; i < sv_maxclients->integer; i++) {
		const client_t* cl = &svs.clients[i];
		if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
			continue;
		}
		const playerState_t* ps = cl->gentity->playerState;
		const float dx = ps->origin[0] - spot[0], dy = ps->origin[1] - spot[1];
		if (dx * dx + dy * dy < 48.0f * 48.0f && fabs(ps->origin[2] - spot[2]) < 96.0f) {
			return qtrue;
		}
	}
	return qfalse;
}

static qboolean gBarFightActive = qfalse;

static void Social_NpcFrame(void)
{
	Social_ParseNpcs();
	if (!gSocialNpcCount || !gNPCSpawnType) {
		return;
	}
	// A bar fight's on: the regulars keep out of it (a fight's FREE-team
	// NPCs would go for them too), and are back once it's over.
	if (gBarFightActive) {
		for (int i = 0; i < gSocialNpcCount; i++) {
			socialNpc_t* n = &gSocialNpcs[i];
			if (Social_NpcAlive(n) && gFreeEntity) {
				void* old = GVM_BeginNative();
				gFreeEntity(SV_GentityNum(n->ent));
				GVM_EndNative(old);
			}
			n->ent = -1;
			n->nextTry = 0;
			n->deadAt = 0;
		}
		return;
	}

	client_t* spawner = NULL;
	for (int i = 0; i < sv_maxclients->integer && !spawner; i++) {
		client_t* cl = &svs.clients[i];
		if (cl->state == CS_ACTIVE && cl->gentity && cl->gentity->playerState &&
			cl->netchan.remoteAddress.type != NA_BOT) {
			spawner = cl;
		}
	}

	for (int i = 0; i < gSocialNpcCount; i++) {
		socialNpc_t* n = &gSocialNpcs[i];

		if (Social_NpcAlive(n)) {
			playerState_t* body = SV_GentityNum(n->ent)->playerState;
			// Dead - players can still kill it in ways that never reach
			// G_Damage (a grapple throw did): clear the body away after a
			// moment, as MBII's own NPC_RemoveBody would, and a fresh one
			// spawns in its place.
			if (body->stats[STAT_HEALTH] <= 0 || body->pm_type == MB2_PM_DEAD) {
				if (!n->deadAt) {
					n->deadAt = svs.time;
				} else if (svs.time - n->deadAt > 3000 && gFreeEntity) {
					void* old = GVM_BeginNative();
					gFreeEntity(SV_GentityNum(n->ent));
					GVM_EndNative(old);
					Com_Printf("Social mode: NPC \"%s\" died - bringing it back\n", n->type);
					n->ent = -1;
					n->deadAt = 0;
					n->nextTry = 0;
				}
				continue;
			}
			n->deadAt = 0;
			if (n->pose == SOCIAL_POSE_PATROL) {
				Social_NpcPatrol(n, SV_GentityNum(n->ent));
				continue;
			}
			if (n->roam) {
				continue;
			}

			// Held on the spot across the floor only - height is left to
			// gravity (pinning that too bounced it, dropping to the floor
			// and being put back up) - and only once it's drifted a way and
			// nobody's standing there, or it's put back inside them.
			playerState_t* ps = SV_GentityNum(n->ent)->playerState;
			const float dx = ps->origin[0] - n->origin[0], dy = ps->origin[1] - n->origin[1];
			// Seated ones stay put: their AI shuffling them off the chair is
			// undone at once, not after the grace standing ones get.
			const float slack = (n->pose == SOCIAL_POSE_SIT) ? 4.0f : 32.0f;
			const int graceMs = (n->pose == SOCIAL_POSE_SIT) ? 0 : 2000;
			const qboolean off = (dx * dx + dy * dy > slack * slack) ? qtrue : qfalse;
			if (off && ps->groundEntityNum != ENTITYNUM_NONE && graceMs == 0 &&
				!Social_SpotOccupied(n->origin, n->ent)) {
				ps->origin[0] = n->origin[0];
				ps->origin[1] = n->origin[1];
				ps->velocity[0] = ps->velocity[1] = 0.0f;
				n->offSince = 0;
			} else if (!off || ps->groundEntityNum == ENTITYNUM_NONE) {
				n->offSince = 0;
			} else if (!n->offSince) {
				n->offSince = svs.time;
			} else if (svs.time - n->offSince > graceMs && !Social_SpotOccupied(n->origin, n->ent)) {
				ps->origin[0] = n->origin[0];
				ps->origin[1] = n->origin[1];
				ps->velocity[0] = ps->velocity[1] = 0.0f;
				n->offSince = 0;
			}
			if (n->pose == SOCIAL_POSE_SIT && ps->groundEntityNum != ENTITYNUM_NONE) {
				ps->velocity[0] = ps->velocity[1] = 0.0f;
			}
			Social_NpcAnimate(n, SV_GentityNum(n->ent));
			continue;
		}
		if (!spawner || svs.time < n->nextTry) {
			continue;
		}
		n->nextTry = svs.time + 10000;

		playerState_t* pps = spawner->gentity->playerState;
		const float savedYaw = pps->viewangles[YAW];
		pps->viewangles[YAW] = n->yaw; // the new NPC faces where its spawner does
		void* old = GVM_BeginNative();
		sharedEntity_t* e = (sharedEntity_t*)gNPCSpawnType(spawner->gentity, n->type, NULL, 0, 0, 0);
		GVM_EndNative(old);
		pps->viewangles[YAW] = savedYaw;

		if (!e) {
			if (n->failures++ < 3) {
				Com_Printf("Social mode: couldn't spawn NPC \"%s\" - retrying every 10s%s\n", n->type,
					n->failures == 3 ? " (no more of these messages)" : "");
			}
			continue;
		}
		n->failures = 0;
		n->ent = e->s.number;
		n->wp = 0;
		n->goalAt = 0;
		n->stuckSince = 0;
		n->stuckCount = 0;
		n->nextPatrol = svs.time + 1500; // let it begin first
		if (e->playerState) {
			VectorCopy(n->origin, e->playerState->origin); // NPC_Begin spawns it here
		}
		VectorCopy(n->origin, e->s.origin);
		VectorCopy(n->origin, e->s.pos.trBase);
		VectorCopy(n->origin, e->r.currentOrigin);
		Com_Printf("Social mode: NPC \"%s\" (entity %d) at %.0f %.0f %.0f facing %.0f\n",
			n->type, n->ent, n->origin[0], n->origin[1], n->origin[2], n->yaw);
	}
}

// --- Bar fights -----------------------------------------------------------
//
// "!barfight <n>" starts one: hostile NPCs arrive at g_barFightSpawn ("x y
// z yaw", a point on the floor - they're dropped in 24 units above it) and
// they and the players can hurt each other - players still can't hurt
// each other. MBII's NPC_ValidEnemy has an NPC on NPCTEAM_FREE attack anyone
// who isn't (and players are always on NPCTEAM_ENEMY or NPCTEAM_PLAYER,
// g_client.c), so the fights use NPC types whose .npc files put them on
// TEAM_FREE: they go for both teams alike. The regulars leave while it's on
// (Social_NpcFrame). It ends when they're all down, after g_barFightSeconds,
// or on "!barfight stop"; anything left is removed. g_barFightEnable.
// With g_barFightRally ("x y z yaw") set, they first walk there (MBII's
// NPC_SetMoveGoal) and start hunting once they've arrived; either way each
// is pointed at the nearest player (G_SetEnemy), since NPC AI only charges
// an enemy it has seen.
#define BARFIGHT_MAX 12
#define BARFIGHT_MUSIC "music/sailbargealternate" // Jabba's Palace (mb2_jabba's own music), for every fight

typedef struct {
	const char* name;
	const char* intro;
	const char* types[4];
	int base, perPlayer, max;     // how many: base + perPlayer * players, up to max
	const char* music;            // plays while it's on
	float spacing;                // how far apart they arrive - beasts are big
} barFightKind_t;

// Only types MBII spawns as NPCs: most armed TEAM_FREE ones (droideka,
// espo, dxun_g0t0, maxrebo...) are vehicles and are refused.
static const barFightKind_t kBarFights[] = {
	{ "Thugs",   "Noghri assassins storm the cantina!", { "noghri", NULL },                 2, 1, 8, BARFIGHT_MUSIC, 64.0f },
	{ "Beasts",  "Something's escaped from the cellar!", { "nexu", "howler", "BomaBeast", NULL }, 3, 0, 3, BARFIGHT_MUSIC, 140.0f },
	{ "Rancor",  "A rancor's got loose in the bar!",   { "rancor", NULL },                  1, 0, 1, BARFIGHT_MUSIC, 0.0f },
	{ "Wampas",  "Wampas want a drink!",               { "wampa", NULL },                   2, 0, 3, BARFIGHT_MUSIC, 110.0f },
	{ "Horrors", "Horrors crawl out of the swamp!",    { "selkath_zombie", "ice_spider", "acklaymb", NULL }, 3, 0, 3, BARFIGHT_MUSIC, 120.0f },
};

static struct {
	int    kind;
	int    ends;
	int    toSpawn;
	int    spawned;
	int    nextSpawn;
	int    ents[BARFIGHT_MAX];
	int    spawnedAt[BARFIGHT_MAX];
	qboolean goaled[BARFIGHT_MAX];   // sent towards the rally point
	qboolean arrived[BARFIGHT_MAX];  // there (or gave up walking): hunting now
	int    count;
	int    nextHunt;
	qboolean haveRally;
	vec3_t rally;
	vec3_t origin;
	float  yaw;
	int    cooldownUntil;
} gBarFight;

static qboolean Social_IsFightNpc(void* ent)
{
	if (!gBarFightActive || !ent || !sv.gentities || sv.gentitySize <= 0) {
		return qfalse;
	}
	const intptr_t delta = (byte*)ent - (byte*)sv.gentities;
	if (delta < 0 || delta % sv.gentitySize) {
		return qfalse;
	}
	const int num = (int)(delta / sv.gentitySize);
	for (int i = 0; i < gBarFight.count; i++) {
		if (gBarFight.ents[i] == num) {
			return qtrue;
		}
	}
	return qfalse;
}

static qboolean Social_BarFightHit(void* targ, void* attacker)
{
	if (!gBarFightActive) {
		return qfalse;
	}
	return ((Social_IsPlayerEntity(targ) && Social_IsFightNpc(attacker)) ||
		(Social_IsFightNpc(targ) && Social_IsPlayerEntity(attacker))) ? qtrue : qfalse;
}

static qboolean Social_FightNpcUp(int num)
{
	if (num < MAX_CLIENTS || num >= sv.num_entities) {
		return qfalse;
	}
	const sharedEntity_t* e = SV_GentityNum(num);
	return (e->r.linked && e->playerState && e->s.number == num && e->s.eType == ET_NPC &&
		e->playerState->stats[STAT_HEALTH] > 0 && e->playerState->pm_type != MB2_PM_DEAD) ? qtrue : qfalse;
}

static client_t* Social_AnyPlayer(void)
{
	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		if (cl->state == CS_ACTIVE && cl->gentity && cl->gentity->playerState && cl->netchan.remoteAddress.type != NA_BOT) {
			return cl;
		}
	}
	return NULL;
}

static void Social_BarFightEnd(const char* how)
{
	int down = 0;
	for (int i = 0; i < gBarFight.count; i++) {
		const int num = gBarFight.ents[i];
		if (!Social_FightNpcUp(num)) {
			down++;
		}
		// Whatever's left of it - the fighter, or its body - goes.
		if (num >= MAX_CLIENTS && num < sv.num_entities && gFreeEntity) {
			sharedEntity_t* e = SV_GentityNum(num);
			if (e->r.linked && e->s.eType == ET_NPC) {
				void* old = GVM_BeginNative();
				gFreeEntity(e);
				GVM_EndNative(old);
			}
		}
	}
	SV_SendServerCommand(NULL, "cp \"^3%s\n^7%d of %d down\"\n", how, down, gBarFight.spawned);
	SV_SendServerCommand(NULL, "chat \"^1[Bar fight] ^7%s - %d of %d down. The regulars are back.\"\n",
		how, down, gBarFight.spawned);
	Com_Printf("Social mode: bar fight over (%s), %d of %d down\n", how, down, gBarFight.spawned);
	gBarFightActive = qfalse;
	gBarFight.count = 0;
	gBarFight.cooldownUntil = svs.time + 15000;
	SV_JukeboxFightEnd();
}

static void Social_BarFightFrame(void)
{
	if (!gBarFightActive) {
		return;
	}
	if (!Social_Enabled()) {
		Social_BarFightEnd("The fight's called off");
		return;
	}

	if (gBarFight.toSpawn > 0 && svs.time >= gBarFight.nextSpawn && gNPCSpawnType) {
		client_t* spawner = Social_AnyPlayer();
		if (spawner && gBarFight.count < BARFIGHT_MAX) {
			const barFightKind_t* k = &kBarFights[gBarFight.kind];
			int types = 0;
			while (types < 4 && k->types[types]) {
				types++;
			}
			// In a line going into the room (along the spawn point's facing),
			// staggered side to side, spaced for their size.
			const int n = gBarFight.spawned;
			const float yawRad = DEG2RAD(gBarFight.yaw);
			const float fwd = k->spacing * (n / 2), side = (n % 2) ? k->spacing * 0.5f : -k->spacing * 0.5f * (n > 0);
			vec3_t org;
			VectorCopy(gBarFight.origin, org);
			org[0] += cosf(yawRad) * fwd - sinf(yawRad) * side;
			org[1] += sinf(yawRad) * fwd + cosf(yawRad) * side;

			playerState_t* pps = spawner->gentity->playerState;
			const float savedYaw = pps->viewangles[YAW];
			pps->viewangles[YAW] = gBarFight.yaw;
			void* old = GVM_BeginNative();
			sharedEntity_t* e = (sharedEntity_t*)gNPCSpawnType(spawner->gentity, (char*)k->types[n % types], NULL, 0, 0, 0);
			GVM_EndNative(old);
			pps->viewangles[YAW] = savedYaw;

			if (e) {
				if (e->playerState) {
					VectorCopy(org, e->playerState->origin);
				}
				VectorCopy(org, e->s.origin);
				VectorCopy(org, e->s.pos.trBase);
				VectorCopy(org, e->r.currentOrigin);
				gBarFight.spawnedAt[gBarFight.count] = svs.time;
				gBarFight.ents[gBarFight.count++] = e->s.number;
				Com_Printf("Social mode: bar fight - %s (entity %d)\n", k->types[n % types], e->s.number);
			} else {
				Com_Printf("Social mode: bar fight - couldn't spawn %s\n", k->types[n % types]);
			}
			gBarFight.spawned++;
			gBarFight.toSpawn--;
		}
		gBarFight.nextSpawn = svs.time + 700;
		return;
	}

	if (gSetEnemy && svs.time >= gBarFight.nextHunt) {
		gBarFight.nextHunt = svs.time + 1000;
		for (int i = 0; i < gBarFight.count; i++) {
			if (!Social_FightNpcUp(gBarFight.ents[i])) {
				continue;
			}
			sharedEntity_t* npc = SV_GentityNum(gBarFight.ents[i]);

			// First, into the room: walk to the rally point (its AI
			// follows a move goal while it has no enemy), a little apart
			// from each other; hunt once there, or after 10s regardless.
			if (gBarFight.haveRally && !gBarFight.arrived[i]) {
				const int age = svs.time - gBarFight.spawnedAt[i];
				vec3_t goal;
				VectorCopy(gBarFight.rally, goal);
				goal[0] += ((i % 3) - 1) * 56.0f;
				goal[1] += (((i / 3) % 3) - 1) * 56.0f;
				if (DistanceSquared(npc->playerState->origin, goal) < 160.0f * 160.0f || age > 10000) {
					gBarFight.arrived[i] = qtrue;
				} else {
					if (!gBarFight.goaled[i] && age > 800 && gSetMoveGoal) {
						void* old = GVM_BeginNative();
						gSetMoveGoal(npc, goal, 48, 0, -1, NULL);
						GVM_EndNative(old);
						gBarFight.goaled[i] = qtrue;
					}
					continue;
				}
			}
			client_t* nearest = NULL;
			float best = 0.0f;
			for (int c = 0; c < sv_maxclients->integer; c++) {
				client_t* cl = &svs.clients[c];
				if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState ||
					!Social_IsSpawned(cl->gentity->playerState, c) || cl->gentity->playerState->stats[STAT_HEALTH] <= 0 ||
					cl->gentity->playerState->duelInProgress) {
					continue;
				}
				const float d = DistanceSquared(cl->gentity->playerState->origin, npc->playerState->origin);
				if (!nearest || d < best) {
					nearest = cl;
					best = d;
				}
			}
			if (nearest) {
				// G_SetEnemy only takes it if the NPC has no enemy yet, so
				// one already in a fight carries on with it.
				void* old = GVM_BeginNative();
				gSetEnemy(npc, nearest->gentity);
				GVM_EndNative(old);
			}
		}
	}

	if (gBarFight.toSpawn <= 0) {
		qboolean anyUp = qfalse;
		for (int i = 0; i < gBarFight.count && !anyUp; i++) {
			anyUp = (svs.time - gBarFight.spawnedAt[i] < 4000 || Social_FightNpcUp(gBarFight.ents[i])) ? qtrue : qfalse;
		}
		if (!anyUp) {
			Social_BarFightEnd("^2The bar is cleared!");
			return;
		}
	}
	if (svs.time >= gBarFight.ends) {
		Social_BarFightEnd("Last orders - the fight's over");
	}
}

// "!barfight", "!barfight <n>", "!barfight stop".
qboolean SV_SocialBarFightCommand(client_t* cl, const char* args)
{
	char arg[32] = "";
	sscanf(args, "%31s", arg);

	if (!Social_Enabled() || !g_barFightEnable || !g_barFightEnable->integer) {
		SV_SendServerCommand(cl, "chat \"^1[Bar fight]^7 No bar fights on this server.\"\n");
		return qtrue;
	}
	if (!Q_stricmp(arg, "stop")) {
		if (gBarFightActive) {
			Social_BarFightEnd(va("%s ^7broke it up", cl->name));
		}
		return qtrue;
	}
	if (!arg[0]) {
		SV_SendServerCommand(cl, "chat \"^1[Bar fight]^7 %s\"\n", gBarFightActive ?
			"A fight's on! ^5!barfight stop ^7ends it." : "Start one - they can hurt you and you can hurt them:");
		for (int i = 0; i < (int)ARRAY_LEN(kBarFights); i++) {
			SV_SendServerCommand(cl, "chat \"^5!barfight %d ^7- %s\"\n", i + 1, kBarFights[i].name);
		}
		return qtrue;
	}

	const int pick = atoi(arg) - 1;
	if (pick < 0 || pick >= (int)ARRAY_LEN(kBarFights)) {
		SV_SendServerCommand(cl, "chat \"^1[Bar fight]^7 Pick 1 to %d - ^5!barfight ^7lists them.\"\n", (int)ARRAY_LEN(kBarFights));
		return qtrue;
	}
	if (gBarFightActive) {
		SV_SendServerCommand(cl, "chat \"^1[Bar fight]^7 One's already on!\"\n");
		return qtrue;
	}
	if (svs.time < gBarFight.cooldownUntil) {
		SV_SendServerCommand(cl, "chat \"^1[Bar fight]^7 Let the dust settle - try again in %ds.\"\n",
			(gBarFight.cooldownUntil - svs.time + 999) / 1000);
		return qtrue;
	}

	vec3_t org;
	float yaw = 0.0f;
	if (!g_barFightSpawn || sscanf(g_barFightSpawn->string, "%f %f %f %f", &org[0], &org[1], &org[2], &yaw) < 3) {
		SV_SendServerCommand(cl, "chat \"^1[Bar fight]^7 No spawn point set (g_barFightSpawn).\"\n");
		return qtrue;
	}
	org[2] += 24.0f; // a point on the floor: drop them in above it

	vec3_t rally;
	float rallyYaw = 0.0f;
	const qboolean haveRally = (g_barFightRally &&
		sscanf(g_barFightRally->string, "%f %f %f %f", &rally[0], &rally[1], &rally[2], &rallyYaw) >= 3) ? qtrue : qfalse;

	int players = 0;
	for (int i = 0; i < sv_maxclients->integer; i++) {
		players += (svs.clients[i].state == CS_ACTIVE && svs.clients[i].netchan.remoteAddress.type != NA_BOT);
	}
	const barFightKind_t* k = &kBarFights[pick];
	memset(&gBarFight, 0, sizeof(gBarFight));
	gBarFight.kind = pick;
	gBarFight.toSpawn = Q_min(k->max, k->base + k->perPlayer * players);
	gBarFight.toSpawn = Q_min(gBarFight.toSpawn, BARFIGHT_MAX);
	gBarFight.nextSpawn = svs.time + 1500; // the regulars clear out first
	gBarFight.ends = svs.time + 1000 * Q_max(30, g_barFightSeconds ? g_barFightSeconds->integer : 180);
	VectorCopy(org, gBarFight.origin);
	gBarFight.yaw = yaw;
	gBarFight.haveRally = haveRally;
	if (haveRally) {
		VectorCopy(rally, gBarFight.rally);
	}
	gBarFightActive = qtrue;

	SV_JukeboxFightStart(k->music);
	SV_SendServerCommand(NULL, "cp \"^1BAR FIGHT!\n^7%s\"\n", k->intro);
	SV_SendServerCommand(NULL, "chat \"^1[Bar fight] ^7%s ^7started a bar fight: ^1%s^7! They can hurt you and you can hurt them.\"\n",
		cl->name, k->name);
	Com_Printf("Social mode: %s started a bar fight (%s, %d)\n", cl->name, k->name, gBarFight.toSpawn);
	return qtrue;
}

void SV_SocialGameInit(void)
{
	// A new round or map frees every entity, fights included.
	gBarFightActive = qfalse;
	gBarFight.count = 0;
	SV_JukeboxFightEnd();

	// A new round or map frees every entity: spawn the NPCs again.
	for (int i = 0; i < SOCIAL_MAX_NPCS; i++) {
		gSocialNpcs[i].ent = -1;
		gSocialNpcs[i].nextTry = 0;
		gSocialNpcs[i].deadAt = 0;
	}

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
		gSetTeam = (void (*)(void*, char*))Sys_LoadFunction(dll, "SetTeam");
		gNPCSpawnType = (void* (*)(void*, char*, char*, int, int, int))Sys_LoadFunction(dll, "NPC_SpawnType");
		gFreeEntity = (void (*)(void*))Sys_LoadFunction(dll, "G_FreeEntity");
		gSetEnemy = (void (*)(void*, void*))Sys_LoadFunction(dll, "G_SetEnemy");
		gSetMoveGoal = (void (*)(void*, float*, int, int, int, void*))Sys_LoadFunction(dll, "NPC_SetMoveGoal");
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
		Social_NpcFrame();
		Social_BarFightFrame();
	}
	if (Social_Enabled() || (g_socialBots && g_socialBots->integer)) {
		Social_RescueStuckJoiners();
	}
}
