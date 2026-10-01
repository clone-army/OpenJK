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
#include <sys/stat.h>
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
// Holotable actions. Argument counts checked against MBII's own code.
static int (*gEffectIndex)(const char* name) = NULL;
static void (*gPlayEffectID)(int fxID, float* org, float* ang) = NULL;
static void* (*gScreenShake)(float* org, void* target, float intensity, int duration, int global) = NULL;
static int (*gSoundIndex)(const char* name) = NULL;
static void (*gSoundAtLoc)(float* loc, int channel, int soundIndex) = NULL;
static void (*gTeleportPlayer)(void* player, float* origin, float* angles) = NULL;
static void (*gUseTargets2)(void* ent, void* activator, const char* target) = NULL;
// MBII ends a round with this: (winning team - 1, 2, or 0 for a draw,
// ENTITYNUM_NONE, 0), as its own calls do. gSiegeRoundEnded: already over.
static void (*gSiegeRoundComplete)(int team, int client, int unused) = NULL;
// More Holotable actions - argument counts checked against MBII's code, and
// CS_SHADERSTATE (24) against its own BuildShaderStateConfig caller.
static void (*gHoloKnockdown)(void* victim, int attacker, int addTime, int downVelocity, qboolean quickGetup) = NULL;
static void (*gSiegeSetObjectiveComplete)(int team, int objective, qboolean failIt) = NULL;
static void* (*gBGFindItem)(const char* classname) = NULL;
static void* (*gLaunchItem)(void* item, float* origin, float* velocity) = NULL;
static void (*gAddRemap)(const char* oldShader, const char* newShader, float timeOffset) = NULL;
static const char* (*gBuildShaderStateConfig)(void) = NULL;
#define HOLO_CS_SHADERSTATE 24

static int* gSiegeRoundEnded = NULL;
static void* (*gNPCSpawnType)(void* ent, char* type, char* targetname, int isVehicle, int asIfPlayer, int siegeTeam) = NULL;
static void (*gFreeEntity)(void* ent) = NULL;
static void (*gSetEnemy)(void* self, void* enemy) = NULL;
static void (*gClearEnemy)(void* self) = NULL;
static void (*gSetMoveGoal)(void* ent, float* point, int radius, int isNavGoal, int combatPoint, void* targetEnt) = NULL;
static void (*gSoundOnEnt)(void* ent, int channel, const char* path) = NULL;
static void (*gSaveNPCGlobals)(void) = NULL;
static void (*gRestoreNPCGlobals)(void) = NULL;
static void (*gSetNPCGlobals)(void* ent) = NULL;
static int (*gNPCFacePosition)(float* position, int doPitch) = NULL;
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
static qboolean Holo_IsNpc(void* ent);
// Both on a side (PERS_TEAM 1/2), and the same / different ones.
static int Holo_SideOf(void* ent)
{
	const sharedEntity_t* e = (const sharedEntity_t*)ent;
	if (!e || !e->playerState) {
		return 0;
	}
	const int t = e->playerState->persistant[PERS_TEAM];
	return (t == TEAM_RED || t == TEAM_BLUE) ? t : 0;
}
static qboolean Holo_SameSide(void* a, void* b)
{
	const int sa = Holo_SideOf(a), sb = Holo_SideOf(b);
	return (sa && sa == sb) ? qtrue : qfalse;
}
static qboolean Holo_OppositeSides(void* a, void* b)
{
	const int sa = Holo_SideOf(a), sb = Holo_SideOf(b);
	return (sa && sb && sa != sb) ? qtrue : qfalse;
}

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

// A running Holotable scenario with "anytime spawn" on (further down).
static qboolean Holo_AnytimeSpawn(void);
static int Holo_RespawnSeconds(void);

// Respawn mode wanted: always on social servers, and on any server while a
// Holotable scenario with anytime spawn runs.
static qboolean Social_AnytimeSpawnWanted(void)
{
	return (Social_Enabled() || Holo_AnytimeSpawn()) ? qtrue : qfalse;
}

static void Social_ApplyTimers(void)
{
	if (!gRespawnTimers) {
		return;
	}

	if (Social_AnytimeSpawnWanted()) {
		int secs = Social_Enabled() ? (g_socialRespawnTime ? g_socialRespawnTime->integer : 3) : Holo_RespawnSeconds();
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

// Switched on while wanted; what it was before is put back once it isn't
// (a scenario's anytime spawn ending, on a server that doesn't respawn).
static qboolean gRespawnForced = qfalse;
static qboolean gRespawnWasNew = qfalse;
static char gRespawnWasMode[16];

static void Social_ForceRespawnMode(void)
{
	if (!gBNewRespawnMode) {
		return;
	}
	if (!Social_AnytimeSpawnWanted()) {
		if (gRespawnForced) {
			*gBNewRespawnMode = gRespawnWasNew;
			Cvar_Set("RespawnMode", gRespawnWasMode);
			gRespawnForced = qfalse;
		}
		return;
	}
	if (!gRespawnForced) {
		gRespawnWasNew = *gBNewRespawnMode;
		Q_strncpyz(gRespawnWasMode, Cvar_VariableString("RespawnMode"), sizeof(gRespawnWasMode));
		gRespawnForced = qtrue;
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
	if (!Social_AnytimeSpawnWanted()) {
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

		if (!Social_AnytimeSpawnWanted() || !js->siegeClassCmd[0]) {
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
	vec3_t lastPos;    // where it was last frame (walking legs only if it really moved)
	float  lookYaw;    // patrol: the way it's looking while stood at a point
	int    nextLook;   // ...and when it looks somewhere else
	int    nextPatrol;
	int    goalAt;     // when its move goal was last given
	float  bestDist;   // closest it's got to that point
	int    stuckSince; // since when it's not got any closer
	int    stuckCount;
	int    dir;        // pace: +1 towards the last point, -1 back
	int    pauseUntil; // pace: standing at an end until then
} socialNpc_t;

enum { SOCIAL_POSE_STAND, SOCIAL_POSE_SIT, SOCIAL_POSE_IDLE, SOCIAL_POSE_BARTEND, SOCIAL_POSE_ROAM, SOCIAL_POSE_PATROL, SOCIAL_POSE_PACE };

static const char* const kNpcIdleAnims[] = {
	"BOTH_GUARD_LOOKAROUND1", "BOTH_HEADNOD", "BOTH_TALK1", "BOTH_HEADSHAKE", "BOTH_GUARD_LOOKAROUND1",
};
// Patrollers stood at a point: something every few seconds, so they look
// alive (the look-around twice, so it's the commonest).
static const char* const kNpcPatrolAnims[] = {
	"BOTH_GUARD_LOOKAROUND1", "BOTH_GUARD_LOOKAROUND1", "BOTH_HEADNOD", "BOTH_TALK1", "BOTH_TALK1",
	"BOTH_HEADSHAKE", "BOTH_HAN_TAUNT", "BOTH_ENGAGETAUNT",
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
			qboolean pace = qfalse;
			if (!Q_stricmpn(flag, "patrol:", 7) && flag[7]) {
				Q_strncpyz(n->route, flag + 7, sizeof(n->route));
				n->roam = qtrue; // never held on its spot
			} else if (!Q_stricmpn(flag, "pace:", 5) && flag[5]) {
				Q_strncpyz(n->route, flag + 5, sizeof(n->route));
				n->roam = qtrue;
				pace = qtrue;
			}
			n->pose = pace ? SOCIAL_POSE_PACE :
				n->route[0] ? SOCIAL_POSE_PATROL :
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

// economy_admins.dat: one account handle a line, kept beside the accounts
// file (fs_basepath/fs_game - shared by every instance) and edited from the
// web panel's Economy page. Re-read every few seconds, so a tick there
// counts straight away.
static qboolean Social_AdminFileHas(const char* handle)
{
	static char cache[4096];
	static int loadedAt = -1000000;
	if (svs.time - loadedAt > 5000 || svs.time < loadedAt) {
		loadedAt = svs.time;
		cache[0] = '\0';
		FILE* f = fopen(va("%s/%s/economy_admins.dat", Cvar_VariableString("fs_basepath"), Cvar_VariableString("fs_game")), "r");
		if (f) {
			const size_t n = fread(cache, 1, sizeof(cache) - 1, f);
			cache[n] = '\0';
			fclose(f);
		}
	}
	char buf[sizeof(cache)];
	Q_strncpyz(buf, cache, sizeof(buf));
	for (char* w = strtok(buf, " \r\n\t"); w; w = strtok(NULL, " \r\n\t")) {
		if (!Q_stricmp(w, handle)) {
			return qtrue;
		}
	}
	return qfalse;
}

// Logged into an account in economy_admins.dat, or listed in g_socialAdmins.
static qboolean Social_IsAdmin(client_t* cl)
{
	if (!cl->economyHandle[0]) {
		return qfalse;
	}
	if (Social_AdminFileHas(cl->economyHandle)) {
		return qtrue;
	}
	if (!g_socialAdmins) {
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

// "!playsound <path>" (admins): plays a game sound on yourself - for
// listening through candidates, e.g. sound/chars/cody/misc/anger1.mp3.
qboolean SV_SocialPlaySoundCommand(client_t* cl, const char* args)
{
	char path[MAX_QPATH] = "";
	sscanf(args, "%63s", path);
	if (!Social_IsAdmin(cl)) {
		Social_Tell(cl, "Only admins can do that.");
		return qtrue;
	}
	if (!path[0] || !cl->gentity || !gSoundOnEnt) {
		Social_Tell(cl, "Usage: ^5!playsound <path>^7, e.g. ^5!playsound sound/chars/cody/misc/anger1.mp3");
		return qtrue;
	}
	void* old = GVM_BeginNative();
	gSoundOnEnt(cl->gentity, 0 /* CHAN_AUTO */, path);
	GVM_EndNative(old);
	Social_Tell(cl, va("Playing ^3%s", path));
	return qtrue;
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
	if (n->wp >= r->count || n->wp < 0) {
		n->wp = 0;
	}
	if (!n->dir) {
		n->dir = 1;
	}

	playerState_t* ps = e->playerState;
	const qboolean pace = (n->pose == SOCIAL_POSE_PACE) ? qtrue : qfalse;

	// Patrolling and stood at a point: till it's time, it faces anyone
	// close by (else a way it picks now and then) and does something every
	// few seconds.
	if (!pace && svs.time < n->pauseUntil) {
		n->stuckSince = svs.time;
		vec3_t look;
		float nearest = 200.0f * 200.0f;
		qboolean someone = qfalse;
		for (int i = 0; i < sv_maxclients->integer; i++) {
			const client_t* cl = &svs.clients[i];
			if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
				continue;
			}
			const float d2 = DistanceSquared(cl->gentity->playerState->origin, ps->origin);
			if (d2 < nearest) {
				nearest = d2;
				VectorCopy(cl->gentity->playerState->origin, look);
				someone = qtrue;
			}
		}
		if (!someone) {
			if (svs.time >= n->nextLook) {
				n->lookYaw = (float)Q_irand(0, 359);
				n->nextLook = svs.time + Q_irand(4000, 9000);
			}
			VectorCopy(ps->origin, look);
			look[0] += cosf(DEG2RAD(n->lookYaw)) * 64.0f;
			look[1] += sinf(DEG2RAD(n->lookYaw)) * 64.0f;
		}
		if (gSaveNPCGlobals && gRestoreNPCGlobals && gSetNPCGlobals && gNPCFacePosition) {
			void* old = GVM_BeginNative();
			gSaveNPCGlobals();
			gSetNPCGlobals(e);
			gNPCFacePosition(look, 0);
			gRestoreNPCGlobals();
			GVM_EndNative(old);
		}
		if (svs.time >= n->nextAnim) {
			SV_EntitySetAnim(e, kNpcPatrolAnims[Q_irand(0, ARRAY_LEN(kNpcPatrolAnims) - 1)], qfalse);
			n->nextAnim = svs.time + Q_irand(3000, 7000);
		}
		return;
	}

	// Pacing and stood at an end: facing its own way (its entry's yaw -
	// otherwise it'd face wherever it walked from), now and then a gesture,
	// till it's time. NPC_FacePosition works on MBII's "current NPC", so
	// that's set for the call and put back.
	if (pace && svs.time < n->pauseUntil) {
		if (gSaveNPCGlobals && gRestoreNPCGlobals && gSetNPCGlobals && gNPCFacePosition) {
			vec3_t look;
			VectorCopy(ps->origin, look);
			look[0] += cosf(DEG2RAD(n->yaw)) * 64.0f;
			look[1] += sinf(DEG2RAD(n->yaw)) * 64.0f;
			void* old = GVM_BeginNative();
			gSaveNPCGlobals();
			gSetNPCGlobals(e);
			gNPCFacePosition(look, 0);
			gRestoreNPCGlobals();
			GVM_EndNative(old);
		}
		if (svs.time >= n->nextAnim) {
			SV_EntitySetAnim(e, kNpcBartendAnims[Q_irand(0, ARRAY_LEN(kNpcBartendAnims) - 1)], qfalse);
			n->nextAnim = svs.time + Q_irand(4000, 8000);
		}
		n->stuckSince = svs.time;
		return;
	}
	const float* p = r->pts[n->wp];
	const float dx = ps->origin[0] - p[0], dy = ps->origin[1] - p[1];
	const float d = sqrtf(dx * dx + dy * dy);

	if (d < 40.0f) {
		if (pace) {
			// At an end: stand a while, then head back the other way.
			if (n->wp == 0 || n->wp == r->count - 1) {
				n->pauseUntil = svs.time + Q_irand(10000, 20000);
				n->nextAnim = svs.time + Q_irand(1500, 4000);
				n->dir = (n->wp == 0) ? 1 : -1;
			}
			if (r->count > 1) {
				n->wp += n->dir;
			}
		} else {
			n->wp = (n->wp + 1) % r->count;
			n->pauseUntil = svs.time + Q_irand(5000, 20000); // a look round, then on
			n->nextAnim = svs.time + Q_irand(1000, 2500);
			n->lookYaw = ps->viewangles[YAW]; // carries on the way it came, at first
			n->nextLook = svs.time + Q_irand(2000, 5000);
		}
		SV_EntitySetLegsAnim(e, "BOTH_STAND1");
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
		if (pace) {
			if (n->wp + n->dir < 0 || n->wp + n->dir >= r->count) {
				n->dir = -n->dir;
			}
			n->wp = (r->count > 1) ? n->wp + n->dir : 0;
		} else {
			n->wp = (n->wp + 1) % r->count;
		}
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
static qboolean Social_HoloRunning(void);

static void Social_NpcFrame(void)
{
	Social_ParseNpcs();
	if (!gSocialNpcCount || !gNPCSpawnType) {
		return;
	}
	// A bar fight's on: the regulars keep out of it (a fight's FREE-team
	// NPCs would go for them too), and are back once it's over. Same for a
	// Holotable scenario.
	if (gBarFightActive || Social_HoloRunning()) {
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
			if (n->pose == SOCIAL_POSE_PATROL || n->pose == SOCIAL_POSE_PACE) {
				// Walking legs while it moves: left to itself it glided
				// along its route with its legs still.
				playerState_t* ps = SV_GentityNum(n->ent)->playerState;
				const float mdx = ps->origin[0] - n->lastPos[0], mdy = ps->origin[1] - n->lastPos[1];
				const qboolean moved = (mdx * mdx + mdy * mdy > 1.0f) ? qtrue : qfalse;
				VectorCopy(ps->origin, n->lastPos);
				if (svs.time < n->pauseUntil) {
					ps->velocity[0] = ps->velocity[1] = 0.0f;
				} else if (moved && ps->legsTimer < 200) {
					SV_EntitySetLegsAnim(SV_GentityNum(n->ent), "BOTH_WALK1");
				}
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
		n->dir = 1;
		// Patrolling: join the loop at the point nearest where it stands,
		// so several on one route start spread out, not all at point 0.
		if (n->pose == SOCIAL_POSE_PATROL) {
			socialRoute_t* r = Social_FindRoute(n->route, qfalse);
			float best = 0.0f;
			for (int p = 0; r && p < r->count; p++) {
				const float d = DistanceSquared(r->pts[p], n->origin);
				if (!p || d < best) {
					best = d;
					n->wp = p;
				}
			}
		}
		n->pauseUntil = 0;
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
// With g_barFightRoutes set ("main bar"), each walks one of those !wp routes
// (handed out in turn) and breaks off to fight when a player's close or the
// route's done. Otherwise, with g_barFightRally ("x y z yaw") set, they
// first walk there (MBII's
// NPC_SetMoveGoal) and start hunting once they've arrived; either way each
// is pointed at the nearest player (G_SetEnemy), since NPC AI only charges
// an enemy it has seen.
#define BARFIGHT_MAX 20
#define BARFIGHT_MUSIC "music/sailbargealternate" // Jabba's Palace (mb2_jabba's own music), for every fight

typedef struct {
	const char* name;
	const char* intro;
	const char* types[8];
	int base, perPlayer, max;     // how many: base + perPlayer * players, up to max
	const char* music;            // plays while it's on
	float spacing;                // how far apart they arrive - beasts are big
	const char* leader;           // arrives first, once (NULL = none)
	const char* quote;            // said in chat as it starts (NULL = none)
	const char* shouts[13];       // one played as it starts, at random (NULL-ended)
} barFightKind_t;

// Only types MBII spawns as NPCs: most armed TEAM_FREE ones (droideka,
// espo, dxun_g0t0, maxrebo...) are vehicles and are refused.
static const barFightKind_t kBarFights[] = {
	// Our own NPC types (ext_data/NPCs/ca_cantina.npc, written out by the
	// engine - SV_SocialEnsureNpcFiles), except the rancor.
	{ "Thugs", "Jabba's heavies kick the door in!",
		{ "CA_Trando", "CA_Weequay", "CA_Nikto", "CA_Klatoo", "CA_Rodian", NULL }, 3, 2, 20, BARFIGHT_MUSIC, 64.0f, "CA_Gamorrean",
		"^3[Gamorrean Enforcer]^7 *snort* Somebody in here owes Jabba. Everybody pays!",
		{ NULL } },
	{ "Rancor", "A rancor is on the loose in the cantina!",
		{ "rancor", NULL }, 1, 0, 1, BARFIGHT_MUSIC, 0.0f, NULL, NULL, { NULL } },
	{ "Droid Attack", "Roger roger - battle droids roll in!",
		{ "CA_B1", "CA_B1", "CA_B2", NULL }, 3, 2, 20, BARFIGHT_MUSIC, 64.0f, "CA_Magna",
		"^3[Tactical Droid]^7 Enemy combatants detected in this cantina. Leave no survivors.",
		{ "sound/canyon/roger_blowitup.wav", "sound/chars/battledroid/misc/anger1.mp3", "sound/chars/battledroid/misc/anger2.mp3",
		  "sound/chars/battledroid/misc/taunt.mp3", "sound/chars/battledroid/misc/taunt1.mp3", "sound/chars/battledroid/misc/taunt2.mp3",
		  "sound/chars/battledroid/misc/taunt3.mp3", "sound/chars/battledroid/misc/taunt4.mp3", "sound/chars/battledroid_cw/misc/combat1.mp3",
		  "sound/chars/battledroid_cw2/misc/combat1.mp3", "sound/chars/battledroid_cw2/misc/combat2.mp3", NULL } },
	{ "Clone Raid: 212th", "The 212th Attack Battalion storms the bar!",
		{ "CA_212", NULL }, 3, 2, 20, BARFIGHT_MUSIC, 64.0f, "CA_212ARC",
		"^3[ARC Trooper]^7 Commander Cody wants these separatists dealt with. You're all under arrest!",
		{ "sound/chars/cody/misc/anger1.mp3", "sound/chars/cody/misc/anger2.mp3", "sound/chars/cody/misc/taunt.mp3",
		  "sound/chars/cody/misc/taunt1.mp3", "sound/chars/cody/misc/taunt2.mp3", "sound/chars/cody/misc/taunt3.mp3",
		  "sound/chars/cody/misc/taunt4.mp3", "sound/chars/cody/misc/detected1.mp3", NULL } },
	{ "Clone Raid: 501st", "The 501st Legion kicks the door in!",
		{ "CA_501", NULL }, 3, 2, 20, BARFIGHT_MUSIC, 64.0f, "CA_Rex",
		"^3[Captain Rex]^7 General's orders - there's been a disturbance, and we're here to put it down!",
		{ "sound/chars/rex/misc/anger1.mp3", "sound/chars/rex/misc/taunt.mp3", "sound/chars/rex/misc/taunt1.mp3",
		  "sound/chars/rex/misc/taunt2.mp3", "sound/chars/rex/misc/taunt3.mp3", "sound/chars/rex/misc/taunt4.mp3",
		  "sound/chars/rex2/misc/anger1.mp3", "sound/chars/rex2/misc/anger2.mp3", "sound/chars/rex2/misc/combat1.mp3",
		  "sound/chars/rex2/misc/combat2.mp3", NULL } },
	{ "Death Watch", "Death Watch drops into the cantina!",
		{ "CA_DeathWatch", "CA_DeathWatchRed", NULL }, 3, 2, 16, BARFIGHT_MUSIC, 64.0f, "CA_Vizsla",
		"^3[Pre Vizsla]^7 This cantina answers to Death Watch now. Anyone who disagrees can take it up with Mandalore!",
		{ NULL } },
	{ "Pyke Syndicate", "The Pyke Syndicate raids the cantina!",
		{ "CA_Pyke", "CA_Pyke", "CA_PykeGunner", NULL }, 3, 2, 20, BARFIGHT_MUSIC, 64.0f, "CA_PykeBoss",
		"^3[Pyke Boss]^7 Someone in here has been skimming our spice. Nobody leaves until we get it back!",
		{ NULL } },
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
	char   route[BARFIGHT_MAX][32];  // attack route it walks first ("" = rally point)
	int    wp[BARFIGHT_MAX];         // ...the point it's heading for
	int    wpGoalAt[BARFIGHT_MAX];
	int    wpSince[BARFIGHT_MAX];    // heading for this point since
	float  wpBest[BARFIGHT_MAX];
	int    count;
	int    nextHunt;
	int    spawnStart;     // first of the recorded spawn points to use
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
	// A Holotable scenario's NPCs: the same rules as a bar fight's - except
	// that one on a side (it attacks only the other) doesn't hurt or get hurt
	// by players on its own side, and fights scenario NPCs on the other one.
	if (Holo_IsNpc(targ) || Holo_IsNpc(attacker)) {
		if (Holo_SameSide(targ, attacker)) {
			return qfalse;
		}
		if (Holo_IsNpc(targ) && Holo_IsNpc(attacker)) {
			return Holo_OppositeSides(targ, attacker);
		}
		return ((Social_IsPlayerEntity(targ) && Holo_IsNpc(attacker)) ||
			(Holo_IsNpc(targ) && Social_IsPlayerEntity(attacker))) ? qtrue : qfalse;
	}
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

static void Social_ShoutToAll(const char* path)
{
	if (!gSoundOnEnt || !path) {
		return;
	}
	int played[MAX_CLIENTS], count = 0;
	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState || cl->netchan.remoteAddress.type == NA_BOT) {
			continue;
		}
		qboolean near = qfalse;
		for (int j = 0; j < count && !near; j++) {
			near = (DistanceSquared(cl->gentity->playerState->origin,
				svs.clients[played[j]].gentity->playerState->origin) < 1000.0f * 1000.0f) ? qtrue : qfalse;
		}
		if (near) {
			continue;
		}
		void* old = GVM_BeginNative();
		gSoundOnEnt(cl->gentity, 0 /* CHAN_AUTO */, path);
		GVM_EndNative(old);
		played[count++] = i;
	}
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

static int gBarFightAutoNext = 0; // svs.time the next timed fight is due (0 = not counting yet)

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
	gBarFightAutoNext = 0; // the next timed one is a full interval from now
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
			while (types < 8 && k->types[types]) {
				types++;
			}
			const int n = gBarFight.spawned;
			const float yawRad = DEG2RAD(gBarFight.yaw);
			vec3_t org;
			socialRoute_t* spawns = (g_barFightSpawnRoute && g_barFightSpawnRoute->string[0]) ?
				Social_FindRoute(g_barFightSpawnRoute->string, qfalse) : NULL;
			if (spawns && spawns->count > 0) {
				// The recorded spawn points (!wp route g_barFightSpawnRoute),
				// in turn from a random start; once they've all been used,
				// the next round of them just beside the last.
				const int round = n / spawns->count;
				VectorCopy(spawns->pts[(gBarFight.spawnStart + n) % spawns->count], org);
				org[0] += cosf(yawRad + M_PI * 0.5f) * 40.0f * round;
				org[1] += sinf(yawRad + M_PI * 0.5f) * 40.0f * round;
				org[2] += 8.0f;
			} else {
				// In a line going into the room (along the spawn point's
				// facing), staggered side to side, spaced for their size.
				const float fwd = k->spacing * (n / 2), side = (n % 2) ? k->spacing * 0.5f : -k->spacing * 0.5f * (n > 0);
				VectorCopy(gBarFight.origin, org);
				org[0] += cosf(yawRad) * fwd - sinf(yawRad) * side;
				org[1] += sinf(yawRad) * fwd + cosf(yawRad) * side;
			}

			playerState_t* pps = spawner->gentity->playerState;
			const float savedYaw = pps->viewangles[YAW];
			pps->viewangles[YAW] = gBarFight.yaw;
			void* old = GVM_BeginNative();
			const char* type = (n == 0 && k->leader) ? k->leader : k->types[(k->leader ? n - 1 : n) % types];
			sharedEntity_t* e = (sharedEntity_t*)gNPCSpawnType(spawner->gentity, (char*)type, NULL, 0, 0, 0);
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
				// Attack routes (g_barFightRoutes), handed out in turn.
				gBarFight.route[gBarFight.count][0] = '\0';
				if (g_barFightRoutes && g_barFightRoutes->string[0]) {
					char names[MAX_CVAR_VALUE_STRING], *list[8];
					int routes = 0;
					Q_strncpyz(names, g_barFightRoutes->string, sizeof(names));
					for (char* w = strtok(names, " ,"); w && routes < 8; w = strtok(NULL, " ,")) {
						list[routes++] = w;
					}
					if (routes) {
						Q_strncpyz(gBarFight.route[gBarFight.count], list[gBarFight.count % routes], sizeof(gBarFight.route[0]));
						// Joined at its point nearest where this one arrives,
						// not its first - spawns are spread about the room.
						socialRoute_t* r = Social_FindRoute(gBarFight.route[gBarFight.count], qfalse);
						gBarFight.wp[gBarFight.count] = 0;
						if (r) {
							float best = 0.0f;
							for (int p = 0; p < r->count; p++) {
								const float d = DistanceSquared(r->pts[p], org);
								if (!p || d < best) {
									best = d;
									gBarFight.wp[gBarFight.count] = p;
								}
							}
						}
					}
				}
				gBarFight.ents[gBarFight.count++] = e->s.number;
				Com_Printf("Social mode: bar fight - %s (entity %d)\n", type, e->s.number);
			} else {
				Com_Printf("Social mode: bar fight - couldn't spawn %s\n", type);
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

			// An attack route (g_barFightRoutes): walk its points in order,
			// round and round, and break off to fight once a player's within
			// 350 units (back to it when nobody's within 500) - a point it
			// can't get any closer to in 6s is skipped.
			if (gBarFight.route[i][0] && !gBarFight.arrived[i]) {
				socialRoute_t* r = Social_FindRoute(gBarFight.route[i], qfalse);
				qboolean close = qfalse;
				for (int c = 0; c < sv_maxclients->integer && !close; c++) {
					const client_t* pc = &svs.clients[c];
					if (pc->state == CS_ACTIVE && pc->gentity && pc->gentity->playerState &&
						Social_IsSpawned(pc->gentity->playerState, c) && pc->gentity->playerState->stats[STAT_HEALTH] > 0 &&
						DistanceSquared(pc->gentity->playerState->origin, npc->playerState->origin) < 350.0f * 350.0f) {
						close = qtrue;
					}
				}
				if (r && gBarFight.wp[i] >= r->count) {
					gBarFight.wp[i] = 0; // round again
				}
				if (close || !r || !r->count || !gSetMoveGoal) {
					gBarFight.arrived[i] = qtrue;
				} else if (svs.time - gBarFight.spawnedAt[i] > 800) {
					const float* p = r->pts[gBarFight.wp[i]];
					const float dx = npc->playerState->origin[0] - p[0], dy = npc->playerState->origin[1] - p[1];
					const float d = sqrtf(dx * dx + dy * dy);
					if (d < 48.0f || (gBarFight.wpSince[i] && svs.time - gBarFight.wpSince[i] > 6000 && d > gBarFight.wpBest[i] - 16.0f)) {
						gBarFight.wp[i]++;
						gBarFight.wpGoalAt[i] = 0;
						gBarFight.wpSince[i] = 0;
					} else {
						if (!gBarFight.wpSince[i] || d < gBarFight.wpBest[i] - 16.0f) {
							gBarFight.wpSince[i] = svs.time;
							gBarFight.wpBest[i] = d;
						}
						if (!gBarFight.wpGoalAt[i] || svs.time - gBarFight.wpGoalAt[i] > 3000) {
							vec3_t goal;
							VectorCopy(p, goal);
							void* old = GVM_BeginNative();
							gSetMoveGoal(npc, goal, 24, 0, -1, NULL);
							GVM_EndNative(old);
							gBarFight.wpGoalAt[i] = svs.time;
						}
					}
					continue;
				}
			}

			// First, into the room: walk to the rally point (its AI
			// follows a move goal while it has no enemy), a little apart
			// from each other; hunt once there, or after 10s regardless.
			if (gBarFight.haveRally && !gBarFight.arrived[i] && !gBarFight.route[i][0]) {
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
			// Hunting with nobody within 500 units: back to its route,
			// from its nearest point, till someone comes close again.
			if (gBarFight.route[i][0] && (!nearest || best > 500.0f * 500.0f)) {
				socialRoute_t* r = Social_FindRoute(gBarFight.route[i], qfalse);
				if (r && r->count) {
					float closest = 0.0f;
					for (int p = 0; p < r->count; p++) {
						const float d = DistanceSquared(r->pts[p], npc->playerState->origin);
						if (!p || d < closest) {
							closest = d;
							gBarFight.wp[i] = p;
						}
					}
					gBarFight.arrived[i] = qfalse;
					gBarFight.wpGoalAt[i] = 0;
					gBarFight.wpSince[i] = 0;
					continue;
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

// Starts bar fight kBarFights[pick] - for !barfight (cl: who asked, told
// if it can't) or the hourly timer (cl NULL). Checks for one already on
// and the cooldown are the caller's.
static qboolean Social_BarFightBegin(int pick, client_t* cl)
{
	vec3_t org;
	float yaw = 0.0f;
	const qboolean haveSpawnRoute = (g_barFightSpawnRoute && g_barFightSpawnRoute->string[0] &&
		Social_FindRoute(g_barFightSpawnRoute->string, qfalse)) ? qtrue : qfalse;
	if (!g_barFightSpawn || sscanf(g_barFightSpawn->string, "%f %f %f %f", &org[0], &org[1], &org[2], &yaw) < 3) {
		if (!haveSpawnRoute) {
			if (cl) {
				SV_SendServerCommand(cl, "chat \"^1[Bar fight]^7 No spawn point set (g_barFightSpawn or g_barFightSpawnRoute).\"\n");
			}
			return qfalse;
		}
		VectorClear(org);
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
	// More players, more of them - with a little randomness for the ones
	// that scale (soldiers: 3 to 20); creatures keep their few.
	gBarFight.toSpawn = k->base + k->perPlayer * players + (k->perPlayer ? Q_irand(-1, 2) : 0);
	gBarFight.toSpawn = Q_max(k->perPlayer ? 3 : 1, Q_min(k->max, gBarFight.toSpawn));
	gBarFight.toSpawn = Q_min(gBarFight.toSpawn, BARFIGHT_MAX);
	gBarFight.nextSpawn = svs.time + 1500; // the regulars clear out first
	gBarFight.ends = svs.time + 1000 * Q_max(30, g_barFightSeconds ? g_barFightSeconds->integer : 180);
	VectorCopy(org, gBarFight.origin);
	gBarFight.yaw = yaw;
	gBarFight.spawnStart = Q_irand(0, 63);
	gBarFight.haveRally = haveRally;
	if (haveRally) {
		VectorCopy(rally, gBarFight.rally);
	}
	gBarFightActive = qtrue;

	SV_JukeboxFightStart(k->music);
	SV_SendServerCommand(NULL, "cp \"^1BAR FIGHT!\n^7%s\"\n", k->intro);
	if (cl) {
		SV_SendServerCommand(NULL, "chat \"^1[Bar fight] ^7%s ^7started a bar fight: ^1%s^7! They can hurt you and you can hurt them.\"\n",
			cl->name, k->name);
	} else {
		SV_SendServerCommand(NULL, "chat \"^1[Bar fight] ^7A bar fight breaks out: ^1%s^7! They can hurt you and you can hurt them.\"\n",
			k->name);
	}
	if (k->quote) {
		SV_SendServerCommand(NULL, "chat \"%s\"\n", k->quote);
	}
	int shouts = 0;
	while (shouts < (int)ARRAY_LEN(k->shouts) && k->shouts[shouts]) {
		shouts++;
	}
	if (shouts) {
		Social_ShoutToAll(k->shouts[Q_irand(0, shouts - 1)]);
	}
	Com_Printf("Social mode: %s started a bar fight (%s, %d)\n", cl ? cl->name : "the timer", k->name, gBarFight.toSpawn);
	gBarFightAutoNext = 0; // the timer counts from this one's end
	return qtrue;
}

// g_barFightAutoMinutes: a random bar fight that long after the last one
// ended (or the server started), once at least g_barFightAutoPlayers are in
// and have been for 2 minutes - so it doesn't go off in someone's face the
// moment they join.
static void Social_BarFightAutoFrame(void)
{
	static int enoughSince = 0;
	if (!g_barFightEnable || !g_barFightEnable->integer || !g_barFightAutoMinutes || g_barFightAutoMinutes->integer <= 0) {
		gBarFightAutoNext = 0;
		return;
	}
	if (gBarFightActive || Social_HoloRunning()) {
		return;
	}
	const int interval = 60000 * g_barFightAutoMinutes->integer;
	if (!gBarFightAutoNext || gBarFightAutoNext - svs.time > interval) {
		gBarFightAutoNext = svs.time + interval;
	}
	int players = 0;
	for (int i = 0; i < sv_maxclients->integer; i++) {
		players += (svs.clients[i].state == CS_ACTIVE && svs.clients[i].netchan.remoteAddress.type != NA_BOT);
	}
	const int need = Q_max(1, g_barFightAutoPlayers ? g_barFightAutoPlayers->integer : 2);
	if (players < need) {
		enoughSince = 0;
		return;
	}
	if (!enoughSince) {
		enoughSince = svs.time ? svs.time : 1;
	}
	if (svs.time < gBarFightAutoNext || svs.time - enoughSince < 120000 || svs.time < gBarFight.cooldownUntil) {
		return;
	}
	if (!Social_BarFightBegin(Q_irand(0, (int)ARRAY_LEN(kBarFights) - 1), NULL)) {
		gBarFightAutoNext = svs.time + interval; // no spawn point: try again next time round
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
	if (!Social_IsAdmin(cl)) {
		SV_SendServerCommand(cl, "chat \"^1[Bar fight]^7 Only admins can start bar fights - log in with an admin account.\"\n");
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
	if (Social_HoloRunning()) {
		SV_SendServerCommand(cl, "chat \"^1[Bar fight]^7 A Holotable scenario's running.\"\n");
		return qtrue;
	}
	if (svs.time < gBarFight.cooldownUntil) {
		SV_SendServerCommand(cl, "chat \"^1[Bar fight]^7 Let the dust settle - try again in %ds.\"\n",
			(gBarFight.cooldownUntil - svs.time + 999) / 1000);
		return qtrue;
	}

	Social_BarFightBegin(pick, cl);
	return qtrue;
}

// --- Holotable scenarios ------------------------------------------------------
//
// Scenarios built on the Holotable web app (github.com/clone-army/holotable)
// and saved as JSON in the game folder's holotable/ directory (any search
// path - fs_basepath/fs_game/holotable, the instance's homepath, or a pk3).
// On any server with g_holotable 1 (MBIIEZ's Holotable plugin sets it).
// Each names the map it's for; "!ht" lists the ones for the map that's on,
// "!ht <n> play" (admins, and "scenario runners" ticked on Holotable's
// Users page) runs one, "!ht restart" reloads it from its file
// (after an edit) and "!ht stop" ends it. rcon "ht ..."
// takes the same.
//
// A scenario is places (points, routes, areas), groups of NPCs (types, how
// many, where they spawn, how they behave) and triggers - when something
// happens (the start, a timer, a player entering an area, a group wiped
// out, everyone down, another trigger), do things (spawn a group, a chat or
// centre message, an NPC saying something (chat line and voice), a sound,
// music, end it). It runs like a bar fight - its
// NPCs and players can hurt each other, the regulars step out, and it's over
// on an "end" action, its time limit or "!ht stop". On a social server,
// only its NPCs and players hurt each other (and the regulars step out);
// elsewhere MBII's own damage rules apply as normal.
#include "cJSON.h"

#define HT_MAX_POINTS    64
#define HT_MAX_ROUTES    16
#define HT_MAX_ROUTE_PTS 64
#define HT_MAX_AREAS     32
#define HT_MAX_GROUPS    16
#define HT_MAX_TYPES     8
#define HT_MAX_TRIGGERS  32
#define HT_MAX_ACTIONS   16
#define HT_MAX_NPCS      32   // alive at once (slots of dead ones are reused)
#define HT_MAX_LIST      32
#define HT_DIR           "holotable"

enum { HT_BEHAVE_HUNT, HT_BEHAVE_ROUTE, HT_BEHAVE_GUARD, HT_BEHAVE_IDLE };
enum { HT_WHEN_START, HT_WHEN_TIMER, HT_WHEN_ENTER, HT_WHEN_GROUP_DEAD, HT_WHEN_ALL_DEAD, HT_WHEN_AFTER,
	HT_WHEN_ALL_IN_AREA, HT_WHEN_GROUP_LEFT, HT_WHEN_PLAYERS, HT_WHEN_PLAYER_DIED, HT_WHEN_NPC_KILLED,
	HT_WHEN_COUNTER, HT_WHEN_COUNTDOWN_END };
enum { HT_DO_SPAWN, HT_DO_MESSAGE, HT_DO_CENTER, HT_DO_SOUND, HT_DO_MUSIC, HT_DO_END, HT_DO_SAY,
	HT_DO_TELL, HT_DO_EXPLODE, HT_DO_EFFECT, HT_DO_SHAKE, HT_DO_TELEPORT, HT_DO_USE, HT_DO_DESPAWN, HT_DO_WIN,
	HT_DO_GIVE, HT_DO_KNOCKDOWN, HT_DO_KILL, HT_DO_HEAL, HT_DO_FREEZE, HT_DO_VEHICLE, HT_DO_ADDTIME, HT_DO_MOVE,
	HT_DO_TRIGGER_ON, HT_DO_TRIGGER_OFF, HT_DO_COUNTER, HT_DO_COUNTDOWN, HT_DO_OBJECTIVE, HT_DO_PICKUP,
	HT_DO_TEXTURE, HT_DO_GRAVITY, HT_DO_SPEED };
enum { HT_WHO_PLAYER, HT_WHO_ALL, HT_WHO_TEAM1, HT_WHO_TEAM2, HT_WHO_AREA }; // whom a player action is for
enum { HT_AT_NONE, HT_AT_POINT, HT_AT_AREA, HT_AT_PLAYER }; // where an action happens

typedef struct { char id[40]; vec3_t org; float yaw; } htPoint_t;
typedef struct { char id[40]; int count; vec3_t pts[HT_MAX_ROUTE_PTS]; } htRoute_t;
typedef struct { char id[40]; vec3_t org; float radius, height; } htArea_t;
typedef struct {
	char id[40];
	char name[48];
	char types[HT_MAX_TYPES][48];
	int  numTypes;
	char leader[48];
	int  count, perPlayer, max;
	int  spawnPoint, spawnRoute;  // where the next ones arrive (-1 = not that)
	int  homePoint, homeRoute;    // the group's own spawn place (a Spawn action can say elsewhere)
	qboolean spawnAtStart;        // spawned as the scenario starts, no trigger needed
	int  behaviour;
	int  route;                   // HT_BEHAVE_ROUTE: the route it walks
	float engage;                 // how close a player comes before it goes for them
	int  attacks;                 // 0 = everyone; TEAM_RED / TEAM_BLUE = only that side (it fights for the other)
	qboolean routeWalk;           // walks its route (else runs it); after someone, it runs either way
	// running
	int  queued;                  // still to spawn
	int  spawned;
	qboolean leaderDue;
} htGroup_t;
typedef struct {
	int  type;
	int  ref;                     // group it's about
	char text[200];
	char speaker[48];
	char sound[128];
	char effect[96];
	char target[64];              // map entity targetname (use)
	int  atKind, atRef;           // where (HT_AT_*)
	float damage, radius, intensity, seconds;
	qboolean center;              // tell: big centre message, not chat
	qboolean everyone;            // teleport: everyone, not just who set it off
	int  whoKind, whoRef;         // HT_WHO_*: whom a player action is for (whoRef: an area)
	int  value;                   // counter amount, objective number, behaviour...
	char extra[96];               // a second name: texture swap's new shader, group move's route...
	int  spawnPt, spawnRt;        // spawn: where this time (-1 = the group's own place)
	int  route;                   // move: the route to walk (not atRef - the "at" place below sets that)
} htAction_t;
typedef struct {
	char id[40];
	int  when;
	int  ref;                     // area / group / trigger it's about (-1 = none)
	float seconds;
	htAction_t actions[HT_MAX_ACTIONS];
	int  numActions;
	int  count;                   // group_left: N or fewer left; players: N or more in
	qboolean repeat;              // fires every time, not just once
	int  cooldownMs;              // ...at most this often
	qboolean fired;
	int  firedAt;
	int  lastFiredAt;
	qboolean wasTrue;             // last check's condition, so it fires as it becomes true
	int  nextAt;                  // timer: when it next goes off
	int  afterSeen;               // after: the other trigger's firing it's already followed
	byte inside[MAX_CLIENTS];     // enter_area: who was in it last check
	qboolean off;                 // turned off (starts off, or an action turned it off)
	int  compare;                 // counter: -1 at most, 0 exactly, 1 at least
} htTrigger_t;
typedef struct {
	int  ent;                     // -1 = free slot
	int  group;
	int  spawnedAt;
	vec3_t home;                  // where it arrived (guards go back there)
	qboolean onRoute;
	int  wp, wpGoalAt, wpSince;
	float wpBest;
	int  homeGoalAt;
	int  pace;                    // what it's been told: 0 not yet / its AI's own, 1 walk, 2 run
} htNpc_t;

static qboolean gHoloActive = qfalse;

// g_holotable: on any server (set by MBIIEZ's Holotable plugin).
static qboolean Holo_Enabled(void)
{
	return (g_holotable && g_holotable->integer) ? qtrue : qfalse;
}
static struct {
	char file[64];
	char name[64];
	int  timeLimit;               // seconds
	htPoint_t   points[HT_MAX_POINTS];     int numPoints;
	htRoute_t   routes[HT_MAX_ROUTES];     int numRoutes;
	htArea_t    areas[HT_MAX_AREAS];       int numAreas;
	htGroup_t   groups[HT_MAX_GROUPS];     int numGroups;
	htTrigger_t triggers[HT_MAX_TRIGGERS]; int numTriggers;
	htNpc_t     npcs[HT_MAX_NPCS];
	int  startedAt;
	int  nextSpawn, nextThink, nextTriggers;
	int  spawnTurn;
	qboolean music;
	int  down;                    // NPCs of it put down, for the end message
	// Players
	int  joinTeam;                // 0 = either team, else TEAM_RED (team1) / TEAM_BLUE (team2) only
	qboolean anytime;             // respawn mode while it runs
	int  respawnSecs;
	char teamNames[3][32];        // the map's names for its sides (.siege team1 / team2)
	qboolean balanceChanged;
	char balanceWas[16];          // g_balance before it was turned off
	int  roundExtendMs;           // added to the round clock at the start
	int  nextTeamCheck;
	char classes[256][40];        // the classes (.mbch names) that can be played; none = any
	int  numClasses;
	int  warnedAt[MAX_CLIENTS];
	qboolean playerUp[MAX_CLIENTS];   // for "a player dies"
	// Counters, the countdown, frozen players, and server settings changed
	// for the scenario (put back when it ends).
	char counterIds[16][40];
	int  counters[16];
	int  numCounters;
	int  countdownEnd;                // svs.time it reaches 0 (0 = none running)
	char countdownText[96];
	int  countdownShown;              // seconds last shown
	qboolean countdownDone;           // reached 0 since triggers last looked
	int  frozenUntil[MAX_CLIENTS];
	char gravityWas[16];
	qboolean gravitySet;
	int  gravityUntil;                // 0 = till the end
	int  speedPct;                    // players' ground speed, % of normal (0 = not changed)
	int  speedUntil;                  // 0 = till the end
	char remaps[8][2][96];            // texture swaps made (old, new)
	int  numRemaps;
	qboolean npcUp[HT_MAX_NPCS];      // for "an NPC is killed"
	int  pointSpawns[HT_MAX_POINTS];  // NPCs spawned at each point so far (where the next one goes)
} gHolo;

// A class the running scenario doesn't allow (by its "sc" class name). Not
// in Open mode: there players build their own classes.
qboolean SV_HoloClassRefused(const char* sc)
{
	if (!gHoloActive || !gHolo.numClasses || !sc || !sc[0] || Cvar_VariableIntegerValue("g_Authenticity") == 0) {
		return qfalse;
	}
	for (int i = 0; i < gHolo.numClasses; i++) {
		if (!Q_stricmp(sc, gHolo.classes[i])) {
			return qfalse;
		}
	}
	return qtrue;
}

static qboolean Holo_AnytimeSpawn(void)
{
	return (gHoloActive && gHolo.anytime) ? qtrue : qfalse;
}

static int Holo_RespawnSeconds(void)
{
	return gHolo.respawnSecs > 0 ? gHolo.respawnSecs : 5;
}

typedef struct { char file[64]; char name[64]; char desc[96]; } htListEntry_t;
static htListEntry_t gHoloList[HT_MAX_LIST];
static int gHoloListCount = 0;

static qboolean Holo_IsNpc(void* ent)
{
	if (!gHoloActive || !ent || !sv.gentities || sv.gentitySize <= 0) {
		return qfalse;
	}
	const intptr_t delta = (byte*)ent - (byte*)sv.gentities;
	if (delta < 0 || delta % sv.gentitySize) {
		return qfalse;
	}
	const int num = (int)(delta / sv.gentitySize);
	for (int i = 0; i < HT_MAX_NPCS; i++) {
		if (gHolo.npcs[i].ent == num) {
			return qtrue;
		}
	}
	return qfalse;
}

static void Holo_Reply(client_t* cl, const char* text)
{
	if (cl) {
		Social_Tell(cl, text);
	} else {
		Com_Printf("%s\n", text);
	}
}

// Text from a scenario, made safe to put inside a quoted server command.
static void Holo_Clean(char* out, const char* in, size_t size)
{
	size_t n = 0;
	for (; *in && n + 1 < size; in++) {
		out[n++] = (*in == '"') ? '\'' : (*in == '\r' ? ' ' : *in);
	}
	out[n] = '\0';
}

static const char* HtStr(const cJSON* o, const char* key, const char* def)
{
	const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
	return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : def;
}

static float HtNum(const cJSON* o, const char* key, float def)
{
	const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void HtVec(const cJSON* o, vec3_t out)
{
	out[0] = HtNum(o, "x", 0.0f);
	out[1] = HtNum(o, "y", 0.0f);
	out[2] = HtNum(o, "z", 0.0f);
}

static int HtFind(const char* id, const char* ids, int stride, int count)
{
	if (!id || !id[0]) {
		return -1;
	}
	for (int i = 0; i < count; i++) {
		if (!Q_stricmp(ids + i * stride, id)) {
			return i;
		}
	}
	return -1;
}
#define HT_FIND(arr, n, key) HtFind((key), (arr)[0].id, (int)sizeof((arr)[0]), (n))

// A scenario file's JSON (NULL if it isn't one), and its map.
static cJSON* Holo_ReadFile(const char* file)
{
	char* buf = NULL;
	if (FS_ReadFile(va("%s/%s", HT_DIR, file), (void**)&buf) <= 0 || !buf) {
		return NULL;
	}
	cJSON* root = cJSON_Parse(buf);
	FS_FreeFile(buf);
	if (root && !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		root = NULL;
	}
	return root;
}

// The scenarios for the map that's on, in file-name order.
static void Holo_RefreshList(void)
{
	gHoloListCount = 0;
	int num = 0;
	char** files = FS_ListFiles(HT_DIR, ".json", &num);
	if (!files) {
		return;
	}
	const char* map = Cvar_VariableString("mapname");
	for (int i = 0; i < num && gHoloListCount < HT_MAX_LIST; i++) {
		cJSON* root = Holo_ReadFile(files[i]);
		if (!root) {
			continue;
		}
		if (!Q_stricmp(HtStr(root, "map", ""), map)) {
			htListEntry_t* e = &gHoloList[gHoloListCount++];
			Q_strncpyz(e->file, files[i], sizeof(e->file));
			Holo_Clean(e->name, HtStr(root, "name", files[i]), sizeof(e->name));
			Holo_Clean(e->desc, HtStr(root, "description", ""), sizeof(e->desc));
		}
		cJSON_Delete(root);
	}
	FS_FreeFileList(files);
	// FS_ListFiles order isn't fixed: sort by name, so the numbers hold.
	for (int i = 1; i < gHoloListCount; i++) {
		for (int j = i; j > 0 && Q_stricmp(gHoloList[j - 1].name, gHoloList[j].name) > 0; j--) {
			htListEntry_t t = gHoloList[j];
			gHoloList[j] = gHoloList[j - 1];
			gHoloList[j - 1] = t;
		}
	}
}

static qboolean Holo_Load(const char* file, char* err, size_t errSize)
{
	cJSON* root = Holo_ReadFile(file);
	if (!root) {
		Q_strncpyz(err, "couldn't read it (is it valid JSON?)", errSize);
		return qfalse;
	}
	memset(&gHolo, 0, sizeof(gHolo));
	for (int i = 0; i < HT_MAX_NPCS; i++) {
		gHolo.npcs[i].ent = -1;
	}
	Q_strncpyz(gHolo.file, file, sizeof(gHolo.file));
	Holo_Clean(gHolo.name, HtStr(root, "name", file), sizeof(gHolo.name));
	gHolo.timeLimit = (int)HtNum(root, "timeLimit", 900.0f);
	gHolo.timeLimit = Q_max(30, Q_min(3600, gHolo.timeLimit));
	const char* jt = HtStr(root, "joinTeam", "any");
	gHolo.joinTeam = !Q_stricmp(jt, "team1") ? TEAM_RED : !Q_stricmp(jt, "team2") ? TEAM_BLUE : 0;
	gHolo.anytime = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "anytimeSpawn")) ? qtrue : qfalse;
	// Classes players can pick, when the scenario limits them.
	const cJSON* cls;
	const cJSON* classes = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "limitClasses")) ? cJSON_GetObjectItemCaseSensitive(root, "classes") : NULL;
	cJSON_ArrayForEach(cls, classes) {
		if (cJSON_IsString(cls) && cls->valuestring[0] && gHolo.numClasses < (int)ARRAY_LEN(gHolo.classes)) {
			Q_strncpyz(gHolo.classes[gHolo.numClasses++], cls->valuestring, sizeof(gHolo.classes[0]));
		}
	}
	gHolo.respawnSecs = Q_max(1, Q_min(60, (int)HtNum(root, "respawnSeconds", 5.0f)));

	const cJSON* it;
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "points")) {
		if (gHolo.numPoints >= HT_MAX_POINTS) break;
		htPoint_t* p = &gHolo.points[gHolo.numPoints++];
		Q_strncpyz(p->id, HtStr(it, "id", ""), sizeof(p->id));
		HtVec(it, p->org);
		p->yaw = HtNum(it, "yaw", 0.0f);
	}
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "routes")) {
		if (gHolo.numRoutes >= HT_MAX_ROUTES) break;
		htRoute_t* r = &gHolo.routes[gHolo.numRoutes++];
		Q_strncpyz(r->id, HtStr(it, "id", ""), sizeof(r->id));
		const cJSON* pt;
		cJSON_ArrayForEach(pt, cJSON_GetObjectItemCaseSensitive(it, "points")) {
			if (r->count >= HT_MAX_ROUTE_PTS) break;
			HtVec(pt, r->pts[r->count++]);
		}
	}
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "counters")) {
		if (gHolo.numCounters >= 16) break;
		Q_strncpyz(gHolo.counterIds[gHolo.numCounters], HtStr(it, "id", ""), sizeof(gHolo.counterIds[0]));
		gHolo.counters[gHolo.numCounters++] = (int)HtNum(it, "start", 0.0f);
	}
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "areas")) {
		if (gHolo.numAreas >= HT_MAX_AREAS) break;
		htArea_t* a = &gHolo.areas[gHolo.numAreas++];
		Q_strncpyz(a->id, HtStr(it, "id", ""), sizeof(a->id));
		HtVec(it, a->org);
		a->radius = Q_max(16.0f, HtNum(it, "radius", 128.0f));
		a->height = Q_max(32.0f, HtNum(it, "height", 128.0f));
	}
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "groups")) {
		if (gHolo.numGroups >= HT_MAX_GROUPS) break;
		htGroup_t* g = &gHolo.groups[gHolo.numGroups++];
		Q_strncpyz(g->id, HtStr(it, "id", ""), sizeof(g->id));
		Holo_Clean(g->name, HtStr(it, "name", g->id), sizeof(g->name));
		const cJSON* t;
		cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(it, "npcs")) {
			if (g->numTypes < HT_MAX_TYPES && cJSON_IsString(t) && t->valuestring[0]) {
				Q_strncpyz(g->types[g->numTypes++], t->valuestring, sizeof(g->types[0]));
			}
		}
		Q_strncpyz(g->leader, HtStr(it, "leader", ""), sizeof(g->leader));
		g->count = Q_max(0, (int)HtNum(it, "count", 1.0f));
		g->perPlayer = Q_max(0, (int)HtNum(it, "perPlayer", 0.0f));
		g->max = Q_max(1, Q_min(HT_MAX_NPCS, (int)HtNum(it, "max", 20.0f)));
		g->engage = HtNum(it, "engage", 0.0f);
		g->routeWalk = !Q_stricmp(HtStr(it, "routePace", "walk"), "run") ? qfalse : qtrue;
		const char* at = HtStr(it, "attacks", "all");
		g->attacks = !Q_stricmp(at, "team1") ? TEAM_RED : !Q_stricmp(at, "team2") ? TEAM_BLUE : 0;
		const char* b = HtStr(it, "behaviour", "hunt");
		g->behaviour = !Q_stricmp(b, "route") ? HT_BEHAVE_ROUTE : !Q_stricmp(b, "guard") ? HT_BEHAVE_GUARD :
			!Q_stricmp(b, "idle") ? HT_BEHAVE_IDLE : HT_BEHAVE_HUNT;
		if (g->engage <= 0.0f) {
			g->engage = (g->behaviour == HT_BEHAVE_GUARD) ? 600.0f : 350.0f;
		}
	}
	// Triggers' ids first: one can be "after" another further down.
	const cJSON* trigs = cJSON_GetObjectItemCaseSensitive(root, "triggers");
	cJSON_ArrayForEach(it, trigs) {
		if (gHolo.numTriggers >= HT_MAX_TRIGGERS) break;
		Q_strncpyz(gHolo.triggers[gHolo.numTriggers++].id, HtStr(it, "id", ""), sizeof(gHolo.triggers[0].id));
	}

	// References, now everything has its id.
	int gi = 0;
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "groups")) {
		if (gi >= gHolo.numGroups) break;
		htGroup_t* g = &gHolo.groups[gi++];
		const char* spawn = HtStr(it, "spawn", "");
		g->spawnPoint = HT_FIND(gHolo.points, gHolo.numPoints, spawn);
		g->spawnRoute = (g->spawnPoint < 0) ? HT_FIND(gHolo.routes, gHolo.numRoutes, spawn) : -1;
		g->route = HT_FIND(gHolo.routes, gHolo.numRoutes, HtStr(it, "route", ""));
		g->homePoint = g->spawnPoint;
		g->homeRoute = g->spawnRoute;
		g->spawnAtStart = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "spawnAtStart")) ? qtrue : qfalse;
		// No place of its own is fine, if whatever spawns it says where.
		if (!g->numTypes && !g->leader[0]) {
			Com_sprintf(err, errSize, "group \"%s\" has no NPC types", g->name);
			cJSON_Delete(root);
			return qfalse;
		}
		if (g->behaviour == HT_BEHAVE_ROUTE && (g->route < 0 || !gHolo.routes[g->route].count)) {
			g->behaviour = HT_BEHAVE_HUNT; // no route to walk: straight at them
		}
	}
	int ti = 0;
	cJSON_ArrayForEach(it, trigs) {
		if (ti >= gHolo.numTriggers) break;
		htTrigger_t* t = &gHolo.triggers[ti++];
		const char* w = HtStr(it, "when", "start");
		t->when = !Q_stricmp(w, "timer") ? HT_WHEN_TIMER : !Q_stricmp(w, "enter_area") ? HT_WHEN_ENTER :
			!Q_stricmp(w, "group_dead") ? HT_WHEN_GROUP_DEAD : !Q_stricmp(w, "all_dead") ? HT_WHEN_ALL_DEAD :
			!Q_stricmp(w, "after") ? HT_WHEN_AFTER : !Q_stricmp(w, "all_in_area") ? HT_WHEN_ALL_IN_AREA :
			!Q_stricmp(w, "group_left") ? HT_WHEN_GROUP_LEFT : !Q_stricmp(w, "players") ? HT_WHEN_PLAYERS :
			!Q_stricmp(w, "player_died") ? HT_WHEN_PLAYER_DIED : !Q_stricmp(w, "npc_killed") ? HT_WHEN_NPC_KILLED :
			!Q_stricmp(w, "counter") ? HT_WHEN_COUNTER : !Q_stricmp(w, "countdown_end") ? HT_WHEN_COUNTDOWN_END : HT_WHEN_START;
		t->off = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "startOff")) ? qtrue : qfalse;
		const char* cmp = HtStr(it, "compare", ">=");
		t->compare = !Q_stricmp(cmp, "<=") ? -1 : !Q_stricmp(cmp, "==") ? 0 : 1;
		t->seconds = Q_max(0.0f, HtNum(it, "seconds", 0.0f));
		t->count = (int)HtNum(it, "count", 0.0f);
		t->repeat = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "repeat")) ? qtrue : qfalse;
		t->cooldownMs = (int)(Q_max(1.0f, HtNum(it, "cooldown", 5.0f)) * 1000.0f);
		t->ref = (t->when == HT_WHEN_ENTER || t->when == HT_WHEN_ALL_IN_AREA) ? HT_FIND(gHolo.areas, gHolo.numAreas, HtStr(it, "area", "")) :
			(t->when == HT_WHEN_GROUP_DEAD || t->when == HT_WHEN_GROUP_LEFT) ? HT_FIND(gHolo.groups, gHolo.numGroups, HtStr(it, "group", "")) :
			(t->when == HT_WHEN_AFTER) ? HT_FIND(gHolo.triggers, gHolo.numTriggers, HtStr(it, "trigger", "")) :
			(t->when == HT_WHEN_COUNTER) ? HtFind(HtStr(it, "counter", ""), gHolo.counterIds[0], sizeof(gHolo.counterIds[0]), gHolo.numCounters) : -1;
		const cJSON* a;
		cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(it, "actions")) {
			if (t->numActions >= HT_MAX_ACTIONS) break;
			htAction_t* act = &t->actions[t->numActions];
			const char* d = HtStr(a, "do", "");
			act->ref = -1;
			if (!Q_stricmp(d, "spawn")) {
				act->type = HT_DO_SPAWN;
				act->ref = HT_FIND(gHolo.groups, gHolo.numGroups, HtStr(a, "group", ""));
				if (act->ref < 0) continue;
				const char* where = HtStr(a, "at", "");
				act->spawnPt = HT_FIND(gHolo.points, gHolo.numPoints, where);
				act->spawnRt = (act->spawnPt < 0) ? HT_FIND(gHolo.routes, gHolo.numRoutes, where) : -1;
			} else if (!Q_stricmp(d, "message") || !Q_stricmp(d, "center")) {
				act->type = !Q_stricmp(d, "center") ? HT_DO_CENTER : HT_DO_MESSAGE;
				Holo_Clean(act->text, HtStr(a, "text", ""), sizeof(act->text));
			} else if (!Q_stricmp(d, "sound") || !Q_stricmp(d, "music")) {
				act->type = !Q_stricmp(d, "music") ? HT_DO_MUSIC : HT_DO_SOUND;
				Holo_Clean(act->text, HtStr(a, "path", ""), sizeof(act->text));
				if (!act->text[0]) continue;
			} else if (!Q_stricmp(d, "say")) {
				// A line in chat as if an NPC said it, and its voice.
				act->type = HT_DO_SAY;
				Holo_Clean(act->speaker, HtStr(a, "speaker", ""), sizeof(act->speaker));
				Holo_Clean(act->text, HtStr(a, "text", ""), sizeof(act->text));
				Holo_Clean(act->sound, HtStr(a, "path", ""), sizeof(act->sound));
				if (!act->text[0] && !act->sound[0]) continue;
			} else if (!Q_stricmp(d, "tell")) {
				// Only the player who set it off sees it.
				act->type = HT_DO_TELL;
				Holo_Clean(act->text, HtStr(a, "text", ""), sizeof(act->text));
				act->center = !Q_stricmp(HtStr(a, "style", "chat"), "center") ? qtrue : qfalse;
				if (!act->text[0]) continue;
			} else if (!Q_stricmp(d, "explode") || !Q_stricmp(d, "effect") || !Q_stricmp(d, "shake")) {
				act->type = !Q_stricmp(d, "explode") ? HT_DO_EXPLODE : !Q_stricmp(d, "effect") ? HT_DO_EFFECT : HT_DO_SHAKE;
				Holo_Clean(act->effect, HtStr(a, "effect", act->type == HT_DO_EXPLODE ? "Grenades/EXP_BaseThermal" : ""), sizeof(act->effect));
				Holo_Clean(act->sound, HtStr(a, "path", act->type == HT_DO_EXPLODE ? "sound/weapons/thermal/explode.mp3" : ""), sizeof(act->sound));
				act->damage = Q_max(0.0f, Q_min(1000.0f, HtNum(a, "damage", 60.0f)));
				act->radius = Q_max(16.0f, Q_min(2048.0f, HtNum(a, "radius", 250.0f)));
				act->intensity = Q_max(0.5f, Q_min(20.0f, HtNum(a, "intensity", 4.0f)));
				act->seconds = Q_max(0.1f, Q_min(10.0f, HtNum(a, "seconds", 1.0f)));
				if (act->type == HT_DO_EFFECT && !act->effect[0]) continue;
			} else if (!Q_stricmp(d, "teleport")) {
				act->type = HT_DO_TELEPORT;
				act->everyone = !Q_stricmp(HtStr(a, "who", "player"), "all") ? qtrue : qfalse;
			} else if (!Q_stricmp(d, "use")) {
				// A map entity's targetname: doors, lifts, buttons, triggers.
				act->type = HT_DO_USE;
				Q_strncpyz(act->target, HtStr(a, "target", ""), sizeof(act->target));
				if (!act->target[0]) continue;
			} else if (!Q_stricmp(d, "win")) {
				// Ends the round: that side wins (or a draw).
				act->type = HT_DO_WIN;
				const char* team = HtStr(a, "team", "team1");
				act->ref = !Q_stricmp(team, "team2") ? TEAM_BLUE : !Q_stricmp(team, "draw") ? 0 : TEAM_RED;
				Holo_Clean(act->text, HtStr(a, "text", ""), sizeof(act->text));
			} else if (!Q_stricmp(d, "give") || !Q_stricmp(d, "knockdown") || !Q_stricmp(d, "kill") ||
				!Q_stricmp(d, "heal") || !Q_stricmp(d, "freeze")) {
				// Player actions: for whom (who set it off, everyone, a side,
				// everyone in an area).
				act->type = !Q_stricmp(d, "give") ? HT_DO_GIVE : !Q_stricmp(d, "knockdown") ? HT_DO_KNOCKDOWN :
					!Q_stricmp(d, "kill") ? HT_DO_KILL : !Q_stricmp(d, "heal") ? HT_DO_HEAL : HT_DO_FREEZE;
				Q_strncpyz(act->extra, HtStr(a, "item", ""), sizeof(act->extra));
				act->seconds = Q_max(0.5f, Q_min(60.0f, HtNum(a, "seconds", 3.0f)));
				if (act->type == HT_DO_GIVE && !act->extra[0]) continue;
			} else if (!Q_stricmp(d, "vehicle") || !Q_stricmp(d, "pickup")) {
				act->type = !Q_stricmp(d, "vehicle") ? HT_DO_VEHICLE : HT_DO_PICKUP;
				Q_strncpyz(act->extra, HtStr(a, act->type == HT_DO_VEHICLE ? "vehicle" : "item", ""), sizeof(act->extra));
				if (!act->extra[0]) continue;
			} else if (!Q_stricmp(d, "addtime")) {
				act->type = HT_DO_ADDTIME;
				act->seconds = Q_max(-3600.0f, Q_min(3600.0f, HtNum(a, "seconds", 60.0f)));
			} else if (!Q_stricmp(d, "move")) {
				// A group's new orders: hunt, walk a route, guard a point, idle.
				act->type = HT_DO_MOVE;
				act->ref = HT_FIND(gHolo.groups, gHolo.numGroups, HtStr(a, "group", ""));
				const char* b = HtStr(a, "behaviour", "hunt");
				act->value = !Q_stricmp(b, "route") ? HT_BEHAVE_ROUTE : !Q_stricmp(b, "guard") ? HT_BEHAVE_GUARD :
					!Q_stricmp(b, "idle") ? HT_BEHAVE_IDLE : HT_BEHAVE_HUNT;
				act->route = HT_FIND(gHolo.routes, gHolo.numRoutes, HtStr(a, "route", ""));
				act->center = !Q_stricmp(HtStr(a, "pace", "walk"), "run") ? qfalse : qtrue; // walk the route
				if (act->ref < 0 || (act->value == HT_BEHAVE_ROUTE && (act->route < 0 || !gHolo.routes[act->route].count))) continue;
			} else if (!Q_stricmp(d, "trigger_on") || !Q_stricmp(d, "trigger_off")) {
				act->type = !Q_stricmp(d, "trigger_on") ? HT_DO_TRIGGER_ON : HT_DO_TRIGGER_OFF;
				act->ref = HT_FIND(gHolo.triggers, gHolo.numTriggers, HtStr(a, "trigger", ""));
				if (act->ref < 0) continue;
			} else if (!Q_stricmp(d, "counter")) {
				act->type = HT_DO_COUNTER;
				act->ref = HtFind(HtStr(a, "counter", ""), gHolo.counterIds[0], sizeof(gHolo.counterIds[0]), gHolo.numCounters);
				act->value = (int)HtNum(a, "value", 1.0f);
				act->center = !Q_stricmp(HtStr(a, "op", "add"), "set") ? qtrue : qfalse; // set, not add
				if (act->ref < 0) continue;
			} else if (!Q_stricmp(d, "countdown")) {
				act->type = HT_DO_COUNTDOWN;
				act->seconds = Q_max(1.0f, Q_min(3600.0f, HtNum(a, "seconds", 30.0f)));
				Holo_Clean(act->text, HtStr(a, "text", ""), sizeof(act->text));
			} else if (!Q_stricmp(d, "objective")) {
				act->type = HT_DO_OBJECTIVE;
				act->ref = !Q_stricmp(HtStr(a, "team", "team1"), "team2") ? TEAM_BLUE : TEAM_RED;
				act->value = Q_max(1, (int)HtNum(a, "objective", 1.0f));
			} else if (!Q_stricmp(d, "texture")) {
				act->type = HT_DO_TEXTURE;
				Q_strncpyz(act->effect, HtStr(a, "from", ""), sizeof(act->effect));
				Q_strncpyz(act->extra, HtStr(a, "to", ""), sizeof(act->extra));
				if (!act->effect[0] || !act->extra[0]) continue;
			} else if (!Q_stricmp(d, "gravity") || !Q_stricmp(d, "speed")) {
				act->type = !Q_stricmp(d, "gravity") ? HT_DO_GRAVITY : HT_DO_SPEED;
				act->value = act->type == HT_DO_GRAVITY ? (int)Q_max(0.0f, Q_min(5000.0f, HtNum(a, "value", 800.0f)))
					: (int)Q_max(10.0f, Q_min(400.0f, HtNum(a, "value", 200.0f)));
				act->seconds = Q_max(0.0f, Q_min(3600.0f, HtNum(a, "seconds", 0.0f)));
			} else if (!Q_stricmp(d, "despawn")) {
				act->type = HT_DO_DESPAWN;
				act->ref = HT_FIND(gHolo.groups, gHolo.numGroups, HtStr(a, "group", ""));
				if (act->ref < 0) continue;
			} else if (!Q_stricmp(d, "end")) {
				act->type = HT_DO_END;
				Holo_Clean(act->text, HtStr(a, "text", ""), sizeof(act->text));
			} else {
				continue;
			}
			const char* at = HtStr(a, "at", "");
			if (!Q_stricmp(at, "player")) {
				act->atKind = HT_AT_PLAYER;
			} else if ((act->atRef = HT_FIND(gHolo.points, gHolo.numPoints, at)) >= 0) {
				act->atKind = HT_AT_POINT;
			} else if ((act->atRef = HT_FIND(gHolo.areas, gHolo.numAreas, at)) >= 0) {
				act->atKind = HT_AT_AREA;
			} else {
				act->atKind = HT_AT_NONE;
			}
			if ((act->type == HT_DO_EXPLODE || act->type == HT_DO_EFFECT || act->type == HT_DO_TELEPORT) && act->atKind == HT_AT_NONE) {
				continue; // needs somewhere
			}
			if (act->type == HT_DO_TELEPORT && act->atKind == HT_AT_PLAYER) {
				continue; // to a point or area, not to themselves
			}
			if ((act->type == HT_DO_VEHICLE || act->type == HT_DO_PICKUP) && act->atKind == HT_AT_NONE) {
				continue; // needs somewhere
			}
			{
				const char* who = HtStr(a, "who", "player");
				act->whoKind = !Q_stricmp(who, "all") ? HT_WHO_ALL : !Q_stricmp(who, "team1") ? HT_WHO_TEAM1 :
					!Q_stricmp(who, "team2") ? HT_WHO_TEAM2 : HT_WHO_PLAYER;
				if (act->whoKind == HT_WHO_PLAYER && Q_stricmp(who, "player")) {
					act->whoRef = HT_FIND(gHolo.areas, gHolo.numAreas, who);
					act->whoKind = (act->whoRef >= 0) ? HT_WHO_AREA : HT_WHO_PLAYER;
				}
			}
			t->numActions++;
		}
	}
	cJSON_Delete(root);
	qboolean startsSpawned = qfalse;
	for (int gi = 0; gi < gHolo.numGroups; gi++) {
		startsSpawned = (startsSpawned || gHolo.groups[gi].spawnAtStart) ? qtrue : qfalse;
	}
	if (!gHolo.numTriggers && !startsSpawned) {
		Q_strncpyz(err, "nothing happens in it - no triggers, and no group spawns at the start", errSize);
		return qfalse;
	}
	return qtrue;
}

static int Holo_Players(void)
{
	int players = 0;
	for (int i = 0; i < sv_maxclients->integer; i++) {
		players += (svs.clients[i].state == CS_ACTIVE && svs.clients[i].netchan.remoteAddress.type != NA_BOT);
	}
	return players;
}

static qboolean Holo_NpcUp(const htNpc_t* n)
{
	return (n->ent >= 0 && Social_FightNpcUp(n->ent)) ? qtrue : qfalse;
}

// Alive (or only just spawned - its health isn't set the moment it's made).
static qboolean Holo_NpcCounts(const htNpc_t* n)
{
	return (n->ent >= 0 && (svs.time - n->spawnedAt < 4000 || Social_FightNpcUp(n->ent))) ? qtrue : qfalse;
}

static qboolean Holo_GroupDone(int g)
{
	const htGroup_t* grp = &gHolo.groups[g];
	if (!grp->spawned || grp->queued > 0 || grp->leaderDue) {
		return qfalse;
	}
	for (int i = 0; i < HT_MAX_NPCS; i++) {
		if (gHolo.npcs[i].group == g && Holo_NpcCounts(&gHolo.npcs[i])) {
			return qfalse;
		}
	}
	return qtrue;
}

// The map's names for its two sides: team1 / team2 in maps/<map>.siege
// (TEAM_RED / TEAM_BLUE), e.g. "Jedi" and "Sith".
static void Holo_ReadTeamNames(void)
{
	Q_strncpyz(gHolo.teamNames[TEAM_RED], "Team 1", sizeof(gHolo.teamNames[0]));
	Q_strncpyz(gHolo.teamNames[TEAM_BLUE], "Team 2", sizeof(gHolo.teamNames[0]));
	void* buf = NULL;
	if (FS_ReadFile(va("maps/%s.siege", sv_mapname->string), &buf) <= 0 || !buf) {
		return;
	}
	const char* p = (const char*)buf;
	qboolean got1 = qfalse, got2 = qfalse;
	char* token;
	while (*(token = COM_ParseExt(&p, qtrue)) && !(got1 && got2)) {
		const int which = !Q_stricmp(token, "team1") ? TEAM_RED : !Q_stricmp(token, "team2") ? TEAM_BLUE : 0;
		if (!which || (which == TEAM_RED ? got1 : got2)) {
			continue;
		}
		token = COM_ParseExt(&p, qfalse);
		if (token[0] && token[0] != '{') {
			Q_strncpyz(gHolo.teamNames[which], token, sizeof(gHolo.teamNames[0]));
			if (which == TEAM_RED) got1 = qtrue; else got2 = qtrue;
		}
	}
	FS_FreeFile(buf);
}

// Moves the round's end by deltaMs (as Social_ApplyRoundTime does), so a
// scenario isn't cut off by the round ending under it. Untimed rounds stay so.
static qboolean Holo_ShiftRound(int deltaMs)
{
	if (!gRebelTimeLimit || !gImperialTimeLimit || !gRebelCountdown || !gImperialCountdown || !deltaMs) {
		return qfalse;
	}
	int* limit = *gRebelTimeLimit ? gRebelTimeLimit : (*gImperialTimeLimit ? gImperialTimeLimit : NULL);
	int* countdown = (limit == gRebelTimeLimit) ? gRebelCountdown : gImperialCountdown;
	if (!limit || *limit + deltaMs <= 0) {
		return qfalse;
	}
	if (*countdown) {
		*countdown += deltaMs;
	}
	*limit += deltaMs;
	Cvar_Set("TimeAdd", va("%i", Q_max(0, Cvar_VariableIntegerValue("TimeAdd") + deltaMs)));
	return qtrue;
}

// Puts back what a scenario changed for its players.
static void Holo_RestorePlayers(qboolean mapChanging)
{
	if (gHolo.gravitySet) {
		Cvar_Set("g_gravity", gHolo.gravityWas);
		gHolo.gravitySet = qfalse;
	}
	gHolo.speedPct = 0;
	memset(gHolo.frozenUntil, 0, sizeof(gHolo.frozenUntil));
	gHolo.countdownEnd = 0;
	if (!mapChanging && gHolo.numRemaps && gAddRemap && gBuildShaderStateConfig) {
		// Every swapped texture back to itself.
		void* old = GVM_BeginNative();
		for (int i = 0; i < gHolo.numRemaps; i++) {
			gAddRemap(gHolo.remaps[i][0], gHolo.remaps[i][0], sv.time * 0.001f);
		}
		const char* state = gBuildShaderStateConfig();
		GVM_EndNative(old);
		SV_SetConfigstring(HOLO_CS_SHADERSTATE, state);
	}
	gHolo.numRemaps = 0;
	if (gHolo.balanceChanged) {
		Cvar_Set("g_balance", gHolo.balanceWas);
		gHolo.balanceChanged = qfalse;
	}
	if (!mapChanging && gHolo.roundExtendMs) {
		const int unused = gHolo.roundExtendMs - (svs.time - gHolo.startedAt);
		if (unused > 0) {
			Holo_ShiftRound(-unused); // back to when the round was going to end
		}
	}
	gHolo.roundExtendMs = 0;
}

// One team only: anyone on the other one goes back to spectator, told which
// side to pick. Checked twice a second - MBII has no team command (your
// class picks your side), so this catches a pick as it lands.
static void Holo_TeamFrame(void)
{
	if (!gHolo.joinTeam || svs.time < gHolo.nextTeamCheck || !gSetTeam) {
		return;
	}
	gHolo.nextTeamCheck = svs.time + 500;
	const int other = (gHolo.joinTeam == TEAM_RED) ? TEAM_BLUE : TEAM_RED;
	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState ||
			cl->netchan.remoteAddress.type == NA_BOT) {
			continue;
		}
		if (cl->gentity->playerState->persistant[PERS_TEAM] != other) {
			continue;
		}
		void* old = GVM_BeginNative();
		gSetTeam(cl->gentity, (char*)"spectator");
		GVM_EndNative(old);
		if (svs.time - gHolo.warnedAt[i] > 3000 || !gHolo.warnedAt[i]) {
			gHolo.warnedAt[i] = svs.time;
			SV_SendServerCommand(cl, "cp \"^5Co-op scenario\n^7Pick a class on the ^3%s^7 side\"\n", gHolo.teamNames[gHolo.joinTeam]);
			SV_SendServerCommand(cl, "chat \"^5[Holotable]^7 %s is co-op - everyone plays on the ^3%s^7 side.\"\n",
				gHolo.name, gHolo.teamNames[gHolo.joinTeam]);
		}
	}
}

// Which side an NPC is on lives in MBII's gclient_t (which starts with its
// playerState): playerTeam and enemyTeam, NPC teams where 1 is team1's side
// and 2 team2's (0 = TEAM_FREE, hostile to all). Found by comparing NPCs of
// known teams and players on each side - players carry the same fields, so
// before writing, the layout is checked against one; if MBII ever moves
// them, sides are switched off rather than guessed.
#define HOLO_OFS_PLAYERTEAM 9888
#define HOLO_OFS_ENEMYTEAM  9892
static int gHoloSidesOk = -1;     // -1 not checked yet this map, 0 no, 1 yes

static int* Holo_ClientInt(playerState_t* ps, int ofs)
{
	return (int*)((byte*)ps + ofs);
}

static qboolean Holo_SidesUsable(void)
{
	if (gHoloSidesOk >= 0) {
		return gHoloSidesOk ? qtrue : qfalse;
	}
	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
			continue;
		}
		playerState_t* ps = cl->gentity->playerState;
		// Only someone in the game as themselves: a spectator following a
		// player has that player's side in its playerState, but its own
		// team fields.
		if (!Social_IsSpawned(ps, i)) {
			continue;
		}
		const int team = ps->persistant[PERS_TEAM];
		const int other = (team == TEAM_RED) ? TEAM_BLUE : TEAM_RED;
		gHoloSidesOk = (*Holo_ClientInt(ps, HOLO_OFS_PLAYERTEAM) == team && *Holo_ClientInt(ps, HOLO_OFS_ENEMYTEAM) == other) ? 1 : 0;
		if (!gHoloSidesOk) {
			Com_Printf(S_COLOR_YELLOW "Holotable: MBII's NPC team fields aren't where expected (MBII updated?) - groups attack everyone\n");
		}
		return gHoloSidesOk ? qtrue : qfalse;
	}
	return qfalse; // nobody on a side to check against yet: try again later
}

// Puts a spawned NPC on the side that fights `attacks` (TEAM_RED/BLUE).
static void Holo_SetSide(sharedEntity_t* e, int attacks)
{
	if (!attacks || !e || !e->playerState || !Holo_SidesUsable()) {
		return;
	}
	playerState_t* ps = e->playerState;
	const int* pt = Holo_ClientInt(ps, HOLO_OFS_PLAYERTEAM);
	const int* et = Holo_ClientInt(ps, HOLO_OFS_ENEMYTEAM);
	if (*pt < 0 || *pt > 3 || *et < 0 || *et > 3) {
		return; // not what an NPC's team looks like: leave it alone
	}
	const int side = (attacks == TEAM_RED) ? TEAM_BLUE : TEAM_RED;
	*Holo_ClientInt(ps, HOLO_OFS_PLAYERTEAM) = side;
	*Holo_ClientInt(ps, HOLO_OFS_ENEMYTEAM) = attacks;
	ps->persistant[PERS_TEAM] = side;
}

static void Holo_End(const char* how)
{
	if (!gHoloActive) {
		return;
	}
	for (int i = 0; i < HT_MAX_NPCS; i++) {
		const int num = gHolo.npcs[i].ent;
		if (num >= MAX_CLIENTS && num < sv.num_entities && gFreeEntity) {
			sharedEntity_t* e = SV_GentityNum(num);
			if (e->r.linked && e->s.eType == ET_NPC) {
				void* old = GVM_BeginNative();
				gFreeEntity(e);
				GVM_EndNative(old);
			}
		}
		gHolo.npcs[i].ent = -1;
	}
	if (gHolo.music) {
		SV_JukeboxFightEnd();
	}
	Holo_RestorePlayers(qfalse);
	gHoloActive = qfalse; // anytime spawn off with it: respawn mode goes back next frame
	SV_SendServerCommand(NULL, "chat \"^5[Holotable] ^7%s ^7- %s. The regulars are back.\"\n", gHolo.name, how);
	Com_Printf("Holotable: %s over (%s)\n", gHolo.name, how);
}

// Damage from a scenario (an explosion): straight to MBII's G_Damage - past
// the social hook, which would stop it on a social server.
static void Holo_Damage(sharedEntity_t* targ, const vec3_t from, int damage)
{
	if (!gGDamage || damage <= 0) {
		return;
	}
	vec3_t dir, point;
	VectorSubtract(targ->r.currentOrigin, from, dir);
	VectorNormalize(dir);
	VectorCopy(targ->r.currentOrigin, point);
	GDamageFn fn = (gHookInstalled && gTrampoline) ? (GDamageFn)gTrampoline : (GDamageFn)gGDamage;
	sharedEntity_t* world = SV_GentityNum(ENTITYNUM_WORLD);
	void* old = GVM_BeginNative();
	fn(targ, world, world, dir, point, damage, 0x00000001 /* DAMAGE_RADIUS */ | (Social_Enabled() ? SOCIAL_DAMAGE_NO_TKPOINTS : 0), 0 /* MOD_UNKNOWN */);
	GVM_EndNative(old);
}

// Where an action happens: its point, its area's middle, or the player who
// set it off. qfalse if that's nowhere (no such player, say).
static qboolean Holo_ActionAt(const htAction_t* act, client_t* who, vec3_t out)
{
	switch (act->atKind) {
	case HT_AT_POINT:
		VectorCopy(gHolo.points[act->atRef].org, out);
		out[2] += 16.0f;
		return qtrue;
	case HT_AT_AREA:
		VectorCopy(gHolo.areas[act->atRef].org, out);
		out[2] += 16.0f;
		return qtrue;
	case HT_AT_PLAYER:
		if (who && who->gentity && who->gentity->playerState) {
			VectorCopy(who->gentity->playerState->origin, out);
			return qtrue;
		}
		return qfalse;
	default:
		return qfalse;
	}
}

static qboolean Holo_PlayerIn(int c)
{
	const client_t* cl = &svs.clients[c];
	return (cl->state == CS_ACTIVE && cl->gentity && cl->gentity->playerState &&
		Social_IsSpawned(cl->gentity->playerState, c) && cl->gentity->playerState->stats[STAT_HEALTH] > 0) ? qtrue : qfalse;
}

static qboolean Holo_InArea(const htArea_t* a, int c);

static void Holo_Teleport(client_t* cl, const vec3_t spot, float yaw, int n)
{
	if (!gTeleportPlayer || !cl || !cl->gentity) {
		return;
	}
	// Several at once: round the spot, so nobody lands in anyone else.
	vec3_t org, ang = { 0.0f, yaw, 0.0f };
	VectorCopy(spot, org);
	if (n > 0) {
		const float a = n * 0.9f, r = 48.0f + 16.0f * (n / 7);
		org[0] += cosf(a) * r;
		org[1] += sinf(a) * r;
	}
	org[2] += 25.0f;
	void* old = GVM_BeginNative();
	gTeleportPlayer(cl->gentity, org, ang);
	GVM_EndNative(old);
}

// MBII's own cheat commands (give ...) run as a player for a moment: the
// same way the engine's other features give things.
static void Holo_CheatCommand(client_t* cl, const char* cmd)
{
	const qboolean cheats = Cvar_VariableIntegerValue("sv_cheats") ? qtrue : qfalse;
	if (!cheats) {
		Cvar_Set("sv_cheats", "1");
		GVM_RunFrame(sv.time);
	}
	SV_ExecuteClientCommand(cl, cmd, qtrue);
	if (!cheats) {
		Cvar_Set("sv_cheats", "0");
		GVM_RunFrame(sv.time);
	}
}

// MBII's weapon numbers, by name (game/bg_weapons.h's weapon_t).
static const struct { const char* name; int wp; } kHoloWeapons[] = {
	{ "WP_STUN_BATON", WP_STUN_BATON }, { "WP_MELEE", WP_MELEE }, { "WP_SABER", WP_SABER },
	{ "WP_BRYAR_PISTOL", WP_BRYAR_PISTOL }, { "WP_CLONE_PISTOL", WP_CLONE_PISTOL }, { "WP_MANDO_PISTOL", WP_MANDO_PISTOL },
	{ "WP_BLASTER", WP_BLASTER }, { "WP_DC_CARBINE", WP_DC_CARBINE }, { "WP_CR2", WP_CR2 }, { "WP_E_22", WP_E_22 },
	{ "WP_HEAVY_PISTOL", WP_HEAVY_PISTOL }, { "WP_DLT19", WP_DLT19 }, { "WP_TRAD_BOWCASTER", WP_TRAD_BOWCASTER },
	{ "WP_DISRUPTOR", WP_DISRUPTOR }, { "WP_BOWCASTER", WP_BOWCASTER }, { "WP_REPEATER", WP_REPEATER },
	{ "WP_CLONE_RIFLE", WP_CLONE_RIFLE }, { "WP_THROWER", WP_THROWER }, { "WP_MINIGUN", WP_MINIGUN }, { "WP_DEMP2", WP_DEMP2 },
	{ "WP_SHOTGUN", WP_SHOTGUN }, { "WP_FLECHETTE", WP_FLECHETTE }, { "WP_A280", WP_A280 }, { "WP_DLT20A", WP_DLT20A },
	{ "WP_M5", WP_M5 }, { "WP_T21", WP_T21 }, { "WP_ROCKET_LAUNCHER", WP_ROCKET_LAUNCHER }, { "WP_PLX1", WP_PLX1 },
	{ "WP_THERMAL", WP_THERMAL }, { "WP_FRAG_NADE", WP_FRAG_NADE }, { "WP_REAL_TD", WP_REAL_TD }, { "WP_TRIP_MINE", WP_TRIP_MINE },
	{ "WP_PULSE_NADE", WP_PULSE_NADE }, { "WP_FIRE_NADE", WP_FIRE_NADE }, { "WP_SONIC_NADE", WP_SONIC_NADE },
	{ "WP_CRYO_NADE", WP_CRYO_NADE }, { "WP_CONC_NADE", WP_CONC_NADE }, { "WP_DET_PACK", WP_DET_PACK },
	{ "WP_CONCUSSION", WP_CONCUSSION }, { "WP_SBD", WP_SBD }, { "WP_BRYAR_OLD", WP_BRYAR_OLD }, { "WP_EE3", WP_EE3 },
	{ "WP_EE4", WP_EE4 }, { "WP_AMBAN", WP_AMBAN }, { "WP_PROJ", WP_PROJ },
};

static void Holo_Give(client_t* cl, const char* what)
{
	if (!Q_stricmpn(what, "WP_", 3)) {
		for (size_t i = 0; i < ARRAY_LEN(kHoloWeapons); i++) {
			if (!Q_stricmp(kHoloWeapons[i].name, what)) {
				Holo_CheatCommand(cl, va("give weaponnum %d", kHoloWeapons[i].wp));
				Holo_CheatCommand(cl, "give ammo_all");
				return;
			}
		}
		return;
	}
	if (!Q_stricmp(what, "health") || !Q_stricmp(what, "armor")) {
		Holo_CheatCommand(cl, va("give %s", what));
	} else if (!Q_stricmp(what, "ammo")) {
		Holo_CheatCommand(cl, "give ammo_all");
	} else {
		// An item by its classname (item_jetpack, item_shockfield...).
		char item[64];
		Q_strncpyz(item, what, sizeof(item));
		for (char* c = item; *c; c++) {
			if (!isalnum((unsigned char)*c) && *c != '_') return; // only names, nothing else on the line
		}
		Holo_CheatCommand(cl, va("give %s", item));
		if (!Q_stricmp(item, "item_jetpack")) {
			Holo_CheatCommand(cl, "give fuel 100");
		}
	}
}

// Whether a player is one of an action's "who".
static qboolean Holo_IsTarget(const htAction_t* act, client_t* who, int c)
{
	if (!Holo_PlayerIn(c)) {
		return qfalse;
	}
	const int team = svs.clients[c].gentity->playerState->persistant[PERS_TEAM];
	switch (act->whoKind) {
	case HT_WHO_ALL: return qtrue;
	case HT_WHO_TEAM1: return team == TEAM_RED ? qtrue : qfalse;
	case HT_WHO_TEAM2: return team == TEAM_BLUE ? qtrue : qfalse;
	case HT_WHO_AREA: return Holo_InArea(&gHolo.areas[act->whoRef], c);
	default: return (who && who == &svs.clients[c]) ? qtrue : qfalse;
	}
}

static void Holo_SetCvarFor(const char* cvar, int value, char* was, size_t wasSize, qboolean* set, int* until, float seconds)
{
	if (!*set) {
		Q_strncpyz(was, Cvar_VariableString(cvar), wasSize);
		*set = qtrue;
	}
	Cvar_Set(cvar, va("%i", value));
	*until = seconds > 0.0f ? svs.time + (int)(seconds * 1000.0f) : 0;
}

// Queues a group to spawn - at a point or along a route if given, else at
// its own place. The NPCs then arrive one at a time (Holo_SpawnOne).
// A route that can be walked: one of the scenario's, with points.
static qboolean Holo_RouteOk(int route)
{
	return (route >= 0 && route < gHolo.numRoutes && gHolo.routes[route].count > 0) ? qtrue : qfalse;
}

static void Holo_QueueGroup(int gi, int point, int route)
{
	htGroup_t* g = &gHolo.groups[gi];
	g->spawnPoint = (point >= 0 || route >= 0) ? point : g->homePoint;
	g->spawnRoute = (point >= 0 || route >= 0) ? route : g->homeRoute;
	if (g->spawnPoint < 0 && g->spawnRoute < 0) {
		Com_Printf("Holotable: group %s has nowhere to spawn\n", g->name);
		return;
	}
	int n = g->count + g->perPlayer * Holo_Players();
	n = Q_max(g->numTypes ? 1 : 0, Q_min(g->max, n));
	g->queued += g->numTypes ? n : 0;
	if (g->leader[0]) {
		g->leaderDue = qtrue;
	}
}

static void Holo_RunActions(htTrigger_t* t, client_t* who)
{
	for (int a = 0; a < t->numActions && gHoloActive; a++) {
		const htAction_t* act = &t->actions[a];
		vec3_t at;
		switch (act->type) {
		case HT_DO_SPAWN:
			Holo_QueueGroup(act->ref, act->spawnPt, act->spawnRt);
			break;
		case HT_DO_MESSAGE:
			SV_SendServerCommand(NULL, "chat \"%s\"\n", act->text);
			break;
		case HT_DO_CENTER:
			SV_SendServerCommand(NULL, "cp \"%s\"\n", act->text);
			break;
		case HT_DO_TELL:
			if (who) {
				SV_SendServerCommand(who, act->center ? "cp \"%s\"\n" : "chat \"%s\"\n", act->text);
			}
			break;
		case HT_DO_SOUND:
			if (act->atKind != HT_AT_NONE && gSoundAtLoc && gSoundIndex && Holo_ActionAt(act, who, at)) {
				void* old = GVM_BeginNative();
				gSoundAtLoc(at, 0 /* CHAN_AUTO */, gSoundIndex(act->text));
				GVM_EndNative(old);
			} else {
				Social_ShoutToAll(act->text);
			}
			break;
		case HT_DO_MUSIC:
			SV_JukeboxFightStart(act->text);
			gHolo.music = qtrue;
			break;
		case HT_DO_SAY:
			if (act->text[0]) {
				if (act->speaker[0]) {
					SV_SendServerCommand(NULL, "chat \"^3[%s]^7 %s\"\n", act->speaker, act->text);
				} else {
					SV_SendServerCommand(NULL, "chat \"%s\"\n", act->text);
				}
			}
			if (act->sound[0]) {
				Social_ShoutToAll(act->sound);
			}
			break;
		case HT_DO_EFFECT:
		case HT_DO_EXPLODE:
		case HT_DO_SHAKE: {
			const qboolean placed = Holo_ActionAt(act, who, at);
			if (!placed && act->type != HT_DO_SHAKE) {
				break;
			}
			void* old = GVM_BeginNative();
			if (placed && act->effect[0] && gEffectIndex && gPlayEffectID) {
				vec3_t up = { 0.0f, 0.0f, 1.0f };
				gPlayEffectID(gEffectIndex(act->effect), at, up);
			}
			if (placed && act->sound[0] && gSoundAtLoc && gSoundIndex) {
				gSoundAtLoc(at, 0 /* CHAN_AUTO */, gSoundIndex(act->sound));
			}
			if ((act->type == HT_DO_SHAKE || act->type == HT_DO_EXPLODE) && gScreenShake) {
				// A shake with nowhere to be is felt everywhere.
				vec3_t org;
				VectorCopy(placed ? at : vec3_origin, org);
				const float strength = (act->type == HT_DO_EXPLODE) ? 3.0f : act->intensity;
				const int ms = (act->type == HT_DO_EXPLODE) ? 700 : (int)(act->seconds * 1000.0f);
				gScreenShake(org, NULL, strength, ms, placed ? 0 : 1);
			}
			GVM_EndNative(old);
			if (act->type == HT_DO_EXPLODE && act->damage > 0.0f) {
				// Everyone and every NPC in range, less the further out.
				for (int e = 0; e < sv.num_entities; e++) {
					sharedEntity_t* ent = SV_GentityNum(e);
					if (!ent->r.linked || !ent->playerState || ent->playerState->stats[STAT_HEALTH] <= 0) {
						continue;
					}
					if (e < MAX_CLIENTS ? !Holo_PlayerIn(e) : ent->s.eType != ET_NPC) {
						continue;
					}
					const float d = Distance(ent->r.currentOrigin, at);
					if (d < act->radius) {
						Holo_Damage(ent, at, (int)Q_max(1.0f, act->damage * (1.0f - d / act->radius)));
					}
				}
			}
			break;
		}
		case HT_DO_TELEPORT: {
			if (!Holo_ActionAt(act, who, at)) {
				break;
			}
			at[2] -= 16.0f;
			const float yaw = (act->atKind == HT_AT_POINT) ? gHolo.points[act->atRef].yaw : 0.0f;
			if (act->everyone) {
				int n = 0;
				for (int c = 0; c < sv_maxclients->integer; c++) {
					if (Holo_PlayerIn(c)) {
						Holo_Teleport(&svs.clients[c], at, yaw, n++);
					}
				}
			} else if (who && Holo_PlayerIn(who - svs.clients)) {
				Holo_Teleport(who, at, yaw, 0);
			}
			break;
		}
		case HT_DO_USE:
			if (gUseTargets2) {
				// Used by who set it off (doors check their activator), else anyone.
				client_t* by = (who && who->gentity) ? who : Social_AnyPlayer();
				if (by && by->gentity) {
					void* old = GVM_BeginNative();
					gUseTargets2(by->gentity, by->gentity, act->target);
					GVM_EndNative(old);
				}
			}
			break;
		case HT_DO_DESPAWN:
			gHolo.groups[act->ref].queued = 0;
			gHolo.groups[act->ref].leaderDue = qfalse;
			for (int k = 0; k < HT_MAX_NPCS; k++) {
				htNpc_t* h = &gHolo.npcs[k];
				if (h->group == act->ref && h->ent >= MAX_CLIENTS && h->ent < sv.num_entities && gFreeEntity) {
					sharedEntity_t* e = SV_GentityNum(h->ent);
					if (e->r.linked && e->s.eType == ET_NPC) {
						void* old = GVM_BeginNative();
						gFreeEntity(e);
						GVM_EndNative(old);
					}
					h->ent = -1;
					gHolo.npcUp[k] = qfalse;
				}
			}
			break;
		case HT_DO_END:
			if (act->text[0]) {
				SV_SendServerCommand(NULL, "cp \"%s\"\n", act->text);
			}
			Holo_End("finished");
			break;
		case HT_DO_GIVE:
		case HT_DO_KNOCKDOWN:
		case HT_DO_KILL:
		case HT_DO_HEAL:
		case HT_DO_FREEZE:
			for (int c = 0; c < sv_maxclients->integer; c++) {
				if (!Holo_IsTarget(act, who, c)) {
					continue;
				}
				client_t* cl = &svs.clients[c];
				if (act->type == HT_DO_GIVE) {
					Holo_Give(cl, act->extra);
				} else if (act->type == HT_DO_HEAL) {
					Holo_CheatCommand(cl, "give health");
				} else if (act->type == HT_DO_KILL) {
					Holo_Damage(cl->gentity, cl->gentity->playerState->origin, 100000);
				} else if (act->type == HT_DO_KNOCKDOWN && gHoloKnockdown) {
					void* old = GVM_BeginNative();
					gHoloKnockdown(cl->gentity, c, (int)(act->seconds * 1000.0f), 0, qfalse);
					GVM_EndNative(old);
				} else if (act->type == HT_DO_FREEZE) {
					gHolo.frozenUntil[c] = svs.time + (int)(act->seconds * 1000.0f);
				}
			}
			break;
		case HT_DO_VEHICLE: {
			// A vehicle (swoop, speeder...) at the spot, for players to take.
			// Not removed when the scenario ends: someone may be riding it.
			client_t* spawner = Social_AnyPlayer();
			for (int i = 0; i < sv_maxclients->integer && !spawner; i++) {
				if (svs.clients[i].state == CS_ACTIVE && svs.clients[i].gentity && svs.clients[i].gentity->playerState) {
					spawner = &svs.clients[i];
				}
			}
			if (!spawner || !gNPCSpawnType || !Holo_ActionAt(act, who, at)) {
				break;
			}
			at[2] += 24.0f;
			playerState_t* pps = spawner->gentity->playerState;
			const float savedYaw = pps->viewangles[YAW];
			if (act->atKind == HT_AT_POINT) {
				pps->viewangles[YAW] = gHolo.points[act->atRef].yaw;
			}
			void* old = GVM_BeginNative();
			sharedEntity_t* e = (sharedEntity_t*)gNPCSpawnType(spawner->gentity, (char*)act->extra, NULL, 1 /* vehicle */, 0, 0);
			GVM_EndNative(old);
			pps->viewangles[YAW] = savedYaw;
			if (e) {
				if (e->playerState) {
					VectorCopy(at, e->playerState->origin);
				}
				VectorCopy(at, e->s.origin);
				VectorCopy(at, e->s.pos.trBase);
				VectorCopy(at, e->r.currentOrigin);
				Com_Printf("Holotable: vehicle %s (entity %d)\n", act->extra, e->s.number);
			} else {
				Com_Printf("Holotable: couldn't spawn vehicle \"%s\"\n", act->extra);
			}
			break;
		}
		case HT_DO_PICKUP:
			// An item on the floor: MBII's LaunchItem, as a dropped one.
			if (gBGFindItem && gLaunchItem && Holo_ActionAt(act, who, at)) {
				void* old = GVM_BeginNative();
				void* item = gBGFindItem(act->extra);
				if (item) {
					vec3_t vel = { 0.0f, 0.0f, 80.0f };
					gLaunchItem(item, at, vel);
				}
				GVM_EndNative(old);
				if (!item) {
					Com_Printf("Holotable: no item called \"%s\"\n", act->extra);
				}
			}
			break;
		case HT_DO_ADDTIME:
			if (Holo_ShiftRound((int)(act->seconds * 1000.0f))) {
				SV_SendServerCommand(NULL, "chat \"^5[Holotable]^7 %s%d seconds %s the round clock.\"\n",
					act->seconds > 0 ? "+" : "", (int)act->seconds, act->seconds > 0 ? "on" : "off");
			}
			break;
		case HT_DO_MOVE: {
			// New orders for a group, and every one of it that's up.
			htGroup_t* g = &gHolo.groups[act->ref];
			g->behaviour = act->value;
			if (act->value == HT_BEHAVE_ROUTE) {
				g->route = act->route;
				g->routeWalk = act->center;
			}
			const qboolean guardAt = (act->value == HT_BEHAVE_GUARD && Holo_ActionAt(act, who, at)) ? qtrue : qfalse;
			for (int k = 0; k < HT_MAX_NPCS; k++) {
				htNpc_t* h = &gHolo.npcs[k];
				if (h->group != act->ref || !Holo_NpcUp(h)) {
					continue;
				}
				if (guardAt) {
					VectorCopy(at, h->home);
					h->home[2] -= 16.0f;
					h->homeGoalAt = 0;
				}
				if (act->value == HT_BEHAVE_ROUTE) {
					const htRoute_t* r = &gHolo.routes[g->route];
					float best = 0.0f;
					for (int p = 0; p < r->count; p++) {
						const float d = DistanceSquared(r->pts[p], SV_GentityNum(h->ent)->playerState->origin);
						if (!p || d < best) {
							best = d;
							h->wp = p;
						}
					}
					h->onRoute = qtrue;
					h->wpGoalAt = 0;
					h->wpSince = 0;
				}
			}
			break;
		}
		case HT_DO_TRIGGER_ON: {
			// On, and ready to go again (even if it's fired before).
			htTrigger_t* o = &gHolo.triggers[act->ref];
			o->off = qfalse;
			o->fired = qfalse;
			o->wasTrue = qfalse;
			o->nextAt = svs.time + (int)(o->seconds * 1000.0f);
			memset(o->inside, 0, sizeof(o->inside));
			break;
		}
		case HT_DO_TRIGGER_OFF:
			gHolo.triggers[act->ref].off = qtrue;
			break;
		case HT_DO_COUNTER:
			gHolo.counters[act->ref] = act->center ? act->value : gHolo.counters[act->ref] + act->value;
			Com_Printf("Holotable: counter %s = %d\n", gHolo.counterIds[act->ref], gHolo.counters[act->ref]);
			break;
		case HT_DO_COUNTDOWN:
			gHolo.countdownEnd = svs.time + (int)(act->seconds * 1000.0f);
			Q_strncpyz(gHolo.countdownText, act->text, sizeof(gHolo.countdownText));
			gHolo.countdownShown = -1;
			gHolo.countdownDone = qfalse;
			break;
		case HT_DO_OBJECTIVE:
			// The map's own objective, as if its team had done it.
			if (gSiegeSetObjectiveComplete) {
				void* old = GVM_BeginNative();
				gSiegeSetObjectiveComplete(act->ref, act->value, qfalse);
				GVM_EndNative(old);
				Com_Printf("Holotable: %s objective %d complete\n", gHolo.teamNames[act->ref], act->value);
			}
			break;
		case HT_DO_TEXTURE:
			// Swaps a surface's texture for everyone (put back at the end).
			if (gAddRemap && gBuildShaderStateConfig) {
				void* old = GVM_BeginNative();
				gAddRemap(act->effect, act->extra, sv.time * 0.001f);
				const char* state = gBuildShaderStateConfig();
				GVM_EndNative(old);
				SV_SetConfigstring(HOLO_CS_SHADERSTATE, state);
				if (gHolo.numRemaps < 8) {
					Q_strncpyz(gHolo.remaps[gHolo.numRemaps][0], act->effect, sizeof(gHolo.remaps[0][0]));
					Q_strncpyz(gHolo.remaps[gHolo.numRemaps][1], act->extra, sizeof(gHolo.remaps[0][1]));
					gHolo.numRemaps++;
				}
			}
			break;
		case HT_DO_GRAVITY:
			Holo_SetCvarFor("g_gravity", act->value, gHolo.gravityWas, sizeof(gHolo.gravityWas), &gHolo.gravitySet, &gHolo.gravityUntil, act->seconds);
			break;
		case HT_DO_SPEED:
			// MBII has no speed cvar (each class has its own): applied to
			// movement each frame instead, in SV_HoloClientThink.
			gHolo.speedPct = (act->value == 100) ? 0 : act->value;
			gHolo.speedUntil = act->seconds > 0.0f ? svs.time + (int)(act->seconds * 1000.0f) : 0;
			break;
		case HT_DO_WIN: {
			// The scenario's over (NPCs gone, settings back), then MBII ends
			// the round as if that side had won it - scores, its round-over
			// message, the next round.
			const int team = act->ref;
			char how[64];
			Com_sprintf(how, sizeof(how), team ? "%s win the round" : "the round's a draw", team ? gHolo.teamNames[team] : "");
			if (act->text[0]) {
				SV_SendServerCommand(NULL, "cp \"%s\"\n", act->text);
			}
			Holo_End(how);
			if (!gSiegeRoundComplete || (gSiegeRoundEnded && *gSiegeRoundEnded)) {
				Com_Printf("Holotable: can't end the round (%s)\n", gSiegeRoundComplete ? "it's already over" : "MBII's SiegeRoundComplete not found");
				break;
			}
			void* old = GVM_BeginNative();
			gSiegeRoundComplete(team, ENTITYNUM_NONE, 0);
			GVM_EndNative(old);
			Com_Printf("Holotable: round ended - %s\n", how);
			break;
		}
		}
	}
}

// One NPC from the next group with any waiting, in turn.
// Spawns the next queued NPC (groups taking turns): HT_SPAWN_NONE when
// there's nothing to spawn or no room, HT_SPAWN_FAILED when the game
// couldn't (an unknown type - the next one can go straight away).
enum { HT_SPAWN_NONE, HT_SPAWN_DONE, HT_SPAWN_FAILED };
static int Holo_SpawnOne(void)
{
	// Anyone in the game to spawn from - MBII's NPC spawn takes a client;
	// the NPC is moved to its spot straight after. Bots will do.
	client_t* spawner = Social_AnyPlayer();
	for (int i = 0; i < sv_maxclients->integer && !spawner; i++) {
		if (svs.clients[i].state == CS_ACTIVE && svs.clients[i].gentity && svs.clients[i].gentity->playerState) {
			spawner = &svs.clients[i];
		}
	}
	if (!spawner || !gNPCSpawnType) {
		return HT_SPAWN_NONE;
	}
	int slot = -1;
	for (int i = 0; i < HT_MAX_NPCS && slot < 0; i++) {
		if (gHolo.npcs[i].ent < 0 || !Holo_NpcCounts(&gHolo.npcs[i])) {
			slot = i;
		}
	}
	if (slot < 0) {
		return HT_SPAWN_NONE; // full: wait for some to go down
	}
	for (int k = 0; k < gHolo.numGroups; k++) {
		const int gi = (gHolo.spawnTurn + k) % gHolo.numGroups;
		htGroup_t* g = &gHolo.groups[gi];
		if (!g->leaderDue && g->queued <= 0) {
			continue;
		}
		gHolo.spawnTurn = gi + 1;
		if (g->spawnPoint < 0 && g->spawnRoute < 0) {
			g->queued = 0;
			g->leaderDue = qfalse;
			continue;
		}

		const char* type = g->leaderDue ? g->leader : g->types[g->spawned % g->numTypes];
		const int n = g->spawned;
		vec3_t org;
		float yaw = 0.0f;
		if (g->spawnPoint >= 0) {
			// Around the point: a line going its way, staggered side to side,
			// far enough apart that nobody lands in anyone (MBII kills an NPC
			// spawned inside another). Counted per point, so groups sharing
			// one lay out around each other too.
			const htPoint_t* p = &gHolo.points[g->spawnPoint];
			const int k = gHolo.pointSpawns[g->spawnPoint]++;
			yaw = p->yaw;
			const float yr = DEG2RAD(yaw), fwd = 64.0f * ((k + 1) / 2), side = (k == 0) ? 0.0f : ((k % 2) ? 48.0f : -48.0f);
			VectorCopy(p->org, org);
			org[0] += cosf(yr) * fwd - sinf(yr) * side;
			org[1] += sinf(yr) * fwd + cosf(yr) * side;
		} else {
			// The route's points in turn; once used, the next round beside them.
			const htRoute_t* r = &gHolo.routes[g->spawnRoute];
			if (!r->count) {
				g->queued = 0;
				g->leaderDue = qfalse;
				continue;
			}
			VectorCopy(r->pts[n % r->count], org);
			const vec3_t& nxt = r->pts[(n + 1) % r->count];
			yaw = RAD2DEG(atan2f(nxt[1] - org[1], nxt[0] - org[0]));
			org[0] += 40.0f * (n / r->count);
		}
		org[2] += 24.0f; // a point on the floor: drop them in just above it

		playerState_t* pps = spawner->gentity->playerState;
		const float savedYaw = pps->viewangles[YAW];
		pps->viewangles[YAW] = yaw;
		void* old = GVM_BeginNative();
		sharedEntity_t* e = (sharedEntity_t*)gNPCSpawnType(spawner->gentity, (char*)type, NULL, 0, 0, 0);
		GVM_EndNative(old);
		pps->viewangles[YAW] = savedYaw;

		if (g->leaderDue) {
			g->leaderDue = qfalse;
		} else {
			g->queued--;
		}
		g->spawned++;
		if (!e) {
			Com_Printf("Holotable: couldn't spawn NPC \"%s\"\n", type);
			return HT_SPAWN_FAILED;
		}
		if (e->playerState) {
			VectorCopy(org, e->playerState->origin);
		}
		VectorCopy(org, e->s.origin);
		VectorCopy(org, e->s.pos.trBase);
		VectorCopy(org, e->r.currentOrigin);
		Holo_SetSide(e, g->attacks);
		htNpc_t* h = &gHolo.npcs[slot];
		memset(h, 0, sizeof(*h));
		gHolo.npcUp[slot] = qtrue;
		h->ent = e->s.number;
		h->group = gi;
		h->spawnedAt = svs.time;
		VectorCopy(org, h->home);
		if (g->behaviour == HT_BEHAVE_ROUTE && Holo_RouteOk(g->route)) {
			// Joins its route at the point nearest where it arrived.
			const htRoute_t* r = &gHolo.routes[g->route];
			float best = 0.0f;
			for (int p = 0; p < r->count; p++) {
				const float d = DistanceSquared(r->pts[p], org);
				if (!p || d < best) {
					best = d;
					h->wp = p;
				}
			}
			h->onRoute = (r->count > 0) ? qtrue : qfalse;
		}
		Com_Printf("Holotable: %s - %s (entity %d)\n", g->name, type, e->s.number);
		return HT_SPAWN_DONE;
	}
	return HT_SPAWN_NONE;
}

static client_t* Holo_NearestPlayer(const vec3_t from, float* distSq, int onlyTeam)
{
	client_t* nearest = NULL;
	float best = 0.0f;
	for (int c = 0; c < sv_maxclients->integer; c++) {
		client_t* cl = &svs.clients[c];
		if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState ||
			!Social_IsSpawned(cl->gentity->playerState, c) || cl->gentity->playerState->stats[STAT_HEALTH] <= 0 ||
			cl->gentity->playerState->duelInProgress) {
			continue;
		}
		if (onlyTeam && cl->gentity->playerState->persistant[PERS_TEAM] != onlyTeam) {
			continue; // not a side this group attacks
		}
		const float d = DistanceSquared(cl->gentity->playerState->origin, from);
		if (!nearest || d < best) {
			nearest = cl;
			best = d;
		}
	}
	*distSq = best;
	return nearest;
}

// Who a scenario NPC goes for: the nearest player it attacks and - for a
// group on a side - the nearest scenario NPC fighting for the other side
// (MBII's AI doesn't go looking for enemy NPCs by itself).
static sharedEntity_t* Holo_NearestTarget(const vec3_t from, float* distSq, int onlyTeam)
{
	float best = 0.0f;
	client_t* cl = Holo_NearestPlayer(from, &best, onlyTeam);
	sharedEntity_t* target = cl ? cl->gentity : NULL;
	if (onlyTeam) {
		for (int i = 0; i < HT_MAX_NPCS; i++) {
			const htNpc_t* o = &gHolo.npcs[i];
			if (!Holo_NpcUp(o)) {
				continue;
			}
			sharedEntity_t* e = SV_GentityNum(o->ent);
			if (Holo_SideOf(e) != onlyTeam) {
				continue;
			}
			const float d = DistanceSquared(e->playerState->origin, from);
			if (!target || d < best) {
				target = e;
				best = d;
			}
		}
	}
	*distSq = best;
	return target;
}

static void Holo_MoveTo(sharedEntity_t* npc, const vec3_t goal)
{
	if (!gSetMoveGoal) {
		return;
	}
	vec3_t g;
	VectorCopy(goal, g);
	void* old = GVM_BeginNative();
	gSetMoveGoal(npc, g, 24, 0, -1, NULL);
	GVM_EndNative(old);
}

// Every frame, for NPCs on their route: MBII's own walk / run script flags
// (what an ICARUS "set SET_RUNNING" sets) - so they really run, and turn at
// their points as they should. Legs to match, in case the AI's anim lags.
#define HOLO_NPC_OFS      0x364 // gentity_t's NPC (gNPC_t*), from SetNPCGlobals
#define HOLO_SCRIPTFLAGS  0x1d8 // gNPC_t's scriptFlags, from NPC_ApplyScriptFlags
#define HOLO_SCF_WALKING  0x02
#define HOLO_SCF_RUNNING  0x20
static int* Holo_ScriptFlags(int ent)
{
	byte* npc = *(byte**)((byte*)SV_GentityNum(ent) + HOLO_NPC_OFS);
	return npc ? (int*)(npc + HOLO_SCRIPTFLAGS) : NULL;
}

// Reached its route point: on to the next one now, not at the next think -
// else it stops at every point (a runner most of all).
static qboolean Holo_RouteArrive(htNpc_t* h, sharedEntity_t* e)
{
	const htGroup_t* g = &gHolo.groups[h->group];
	if (!h->onRoute || g->behaviour != HT_BEHAVE_ROUTE || !Holo_RouteOk(g->route)) {
		return qfalse;
	}
	const htRoute_t* r = &gHolo.routes[g->route];
	if (r->count < 1 || h->wp >= r->count) {
		return qfalse;
	}
	const float* p = r->pts[h->wp];
	const float dx = e->r.currentOrigin[0] - p[0], dy = e->r.currentOrigin[1] - p[1];
	if (dx * dx + dy * dy >= 48.0f * 48.0f) {
		return qfalse;
	}
	h->wp = (h->wp + 1) % r->count;
	h->wpSince = 0;
	Holo_MoveTo(e, r->pts[h->wp]);
	h->wpGoalAt = svs.time;
	return qtrue;
}

static void Holo_PaceFrame(void)
{
	for (int i = 0; i < HT_MAX_NPCS; i++) {
		htNpc_t* h = &gHolo.npcs[i];
		if (!h->pace || !Holo_NpcUp(h)) {
			continue;
		}
		int* flags = Holo_ScriptFlags(h->ent);
		if (!flags) {
			continue;
		}
		*flags = (*flags & ~(HOLO_SCF_WALKING | HOLO_SCF_RUNNING)) | (h->pace == 2 ? HOLO_SCF_RUNNING : HOLO_SCF_WALKING);
		sharedEntity_t* e = SV_GentityNum(h->ent);
		Holo_RouteArrive(h, e);
		playerState_t* ps = e->playerState;
		const float speed = sqrtf(ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1]);
		if (ps->groundEntityNum != ENTITYNUM_NONE && speed >= 15.0f && ps->legsTimer < 200) {
			SV_EntitySetLegsAnim(e, h->pace == 2 ? "BOTH_RUN1" : "BOTH_WALK1");
		}
	}
}

// Leaving its route (after someone): flags cleared, its AI picks its pace.
static void Holo_Pace(htNpc_t* h, int pace)
{
	if (h->pace && !pace && Holo_NpcUp(h)) {
		int* flags = Holo_ScriptFlags(h->ent);
		if (flags) {
			*flags &= ~(HOLO_SCF_WALKING | HOLO_SCF_RUNNING);
		}
	}
	h->pace = pace;
}

// MBII's AI can hang on to an enemy that's died - an NPC then stands over
// the body attacking it till it's gone. Drop a dead (or gone) enemy so it
// picks a live one. gentity_t's enemy is at 0x584 (from G_SetEnemy).
#define HOLO_ENEMY_OFS 0x584
static void Holo_DropDeadEnemy(sharedEntity_t* npc)
{
	if (!gClearEnemy || !sv.gentities || sv.gentitySize <= 0) {
		return;
	}
	void* enemy = *(void**)((byte*)npc + HOLO_ENEMY_OFS);
	if (!enemy) {
		return;
	}
	const intptr_t delta = (byte*)enemy - (byte*)sv.gentities;
	if (delta < 0 || delta % sv.gentitySize || delta / sv.gentitySize >= sv.num_entities) {
		return;
	}
	const int num = (int)(delta / sv.gentitySize);
	sharedEntity_t* e = SV_GentityNum(num);
	qboolean alive;
	if (num < MAX_CLIENTS) {
		alive = (e->r.linked && e->playerState && e->playerState->stats[STAT_HEALTH] > 0 &&
			Social_IsSpawned(e->playerState, num)) ? qtrue : qfalse;
	} else if (e->s.eType == ET_NPC) {
		alive = Social_FightNpcUp(num);
	} else {
		alive = e->r.linked ? qtrue : qfalse; // something else it's after (a turret...): its own business
	}
	if (!alive) {
		void* old = GVM_BeginNative();
		gClearEnemy(npc);
		GVM_EndNative(old);
	}
}

static void Holo_Think(void)
{
	for (int i = 0; i < HT_MAX_NPCS; i++) {
		htNpc_t* h = &gHolo.npcs[i];
		if (!Holo_NpcUp(h) || svs.time - h->spawnedAt < 800) {
			continue;
		}
		const htGroup_t* g = &gHolo.groups[h->group];
		sharedEntity_t* npc = SV_GentityNum(h->ent);
		Holo_DropDeadEnemy(npc);
		const float* at = npc->playerState->origin;
		float d2 = 0.0f;
		sharedEntity_t* nearest = Holo_NearestTarget(at, &d2, g->attacks);
		const float engage2 = g->engage * g->engage;

		switch (g->behaviour) {
		case HT_BEHAVE_IDLE:
			break;
		case HT_BEHAVE_GUARD:
			if (nearest && d2 < engage2) {
				void* old = GVM_BeginNative();
				gSetEnemy(npc, nearest);
				GVM_EndNative(old);
			} else if (DistanceSquared(at, h->home) > 96.0f * 96.0f && (!h->homeGoalAt || svs.time - h->homeGoalAt > 3000)) {
				Holo_MoveTo(npc, h->home); // nobody near: back to its post
				h->homeGoalAt = svs.time;
			}
			break;
		case HT_BEHAVE_ROUTE: {
			// Round its route till a player's within engage, then after them;
			// back to the route when nobody's within engage + 150.
			if (!Holo_RouteOk(g->route)) {
				gHolo.groups[h->group].behaviour = HT_BEHAVE_HUNT; // nothing to walk: hunt instead
				break;
			}
			const htRoute_t* r = &gHolo.routes[g->route];
			if (h->onRoute) {
				if (nearest && d2 < engage2) {
					h->onRoute = qfalse;
					Holo_Pace(h, 0); // after them: its own pace (a run)
				} else {
					Holo_Pace(h, g->routeWalk ? 1 : 2);
					if (h->wp >= r->count) {
						h->wp = 0;
					}
					const float* p = r->pts[h->wp];
					const float dx = at[0] - p[0], dy = at[1] - p[1];
					const float d = sqrtf(dx * dx + dy * dy);
					if (d < 48.0f || (h->wpSince && svs.time - h->wpSince > 6000 && d > h->wpBest - 16.0f)) {
						h->wp = (h->wp + 1) % r->count;
						h->wpSince = 0;
						Holo_MoveTo(npc, r->pts[h->wp]);
						h->wpGoalAt = svs.time;
					} else {
						if (!h->wpSince || d < h->wpBest - 16.0f) {
							h->wpSince = svs.time;
							h->wpBest = d;
						}
						if (!h->wpGoalAt || svs.time - h->wpGoalAt > 3000) {
							Holo_MoveTo(npc, p);
							h->wpGoalAt = svs.time;
						}
					}
					break;
				}
			}
			const float leave = g->engage + 150.0f;
			if (!nearest || d2 > leave * leave) {
				float best = 0.0f;
				for (int p = 0; p < r->count; p++) {
					const float d = DistanceSquared(r->pts[p], at);
					if (!p || d < best) {
						best = d;
						h->wp = p;
					}
				}
				h->onRoute = qtrue;
				h->wpGoalAt = 0;
				h->wpSince = 0;
				break;
			}
		}
			// fall through: after the nearest player
		case HT_BEHAVE_HUNT:
		default:
			if (nearest && gSetEnemy) {
				// G_SetEnemy only takes if it has no enemy yet.
				void* old = GVM_BeginNative();
				gSetEnemy(npc, nearest);
				GVM_EndNative(old);
			}
			break;
		}
	}
}

static qboolean Holo_InArea(const htArea_t* a, int c)
{
	const float* o = svs.clients[c].gentity->playerState->origin;
	const float dx = o[0] - a->org[0], dy = o[1] - a->org[1];
	return (dx * dx + dy * dy <= a->radius * a->radius && o[2] >= a->org[2] - 64.0f && o[2] <= a->org[2] + a->height) ? qtrue : qfalse;
}

static int Holo_GroupAlive(int g)
{
	int n = 0;
	for (int i = 0; i < HT_MAX_NPCS; i++) {
		if (gHolo.npcs[i].group == g && Holo_NpcCounts(&gHolo.npcs[i])) {
			n++;
		}
	}
	return n;
}

// Each trigger: once (the first time its "when" happens) or every time
// (repeat - no more often than its cooldown). Conditions (a group down,
// everyone in an area...) fire as they become true, not all the time
// they stay true. Player triggers remember who set them off, for "tell",
// "teleport" and actions "at the player".
static void Holo_CheckTriggers(void)
{
	qboolean anySpawned = qfalse, allDone = qtrue;
	for (int g = 0; g < gHolo.numGroups; g++) {
		if (gHolo.groups[g].spawned || gHolo.groups[g].queued > 0) {
			anySpawned = qtrue;
			allDone = (allDone && Holo_GroupDone(g)) ? qtrue : qfalse;
		}
	}
	// Who died, and how many NPCs went down, since the last check.
	int died = -1, players = 0, downs = 0;
	for (int c = 0; c < sv_maxclients->integer; c++) {
		const qboolean up = Holo_PlayerIn(c);
		if (gHolo.playerUp[c] && !up && svs.clients[c].state == CS_ACTIVE && died < 0) {
			died = c;
		}
		gHolo.playerUp[c] = up;
		players += up;
	}
	for (int i = 0; i < HT_MAX_NPCS; i++) {
		const qboolean up = Holo_NpcCounts(&gHolo.npcs[i]);
		if (gHolo.npcUp[i] && !up) {
			downs++;
		}
		gHolo.npcUp[i] = up;
	}
	gHolo.down += downs;

	for (int i = 0; i < gHolo.numTriggers && gHoloActive; i++) {
		htTrigger_t* t = &gHolo.triggers[i];
		if (t->off || (t->fired && !t->repeat)) {
			continue;
		}
		qboolean go = qfalse, level = qfalse, edge = qfalse;
		client_t* who = NULL;
		switch (t->when) {
		case HT_WHEN_START:
			go = t->fired ? qfalse : qtrue;
			break;
		case HT_WHEN_TIMER:
			go = (svs.time >= t->nextAt) ? qtrue : qfalse;
			break;
		case HT_WHEN_ENTER:
			// Whoever's just walked in.
			for (int c = 0; c < sv_maxclients->integer && t->ref >= 0; c++) {
				const qboolean in = (Holo_PlayerIn(c) && Holo_InArea(&gHolo.areas[t->ref], c)) ? qtrue : qfalse;
				if (in && !t->inside[c] && !who) {
					who = &svs.clients[c];
				}
				t->inside[c] = in;
			}
			go = who ? qtrue : qfalse;
			break;
		case HT_WHEN_ALL_IN_AREA: {
			int in = 0;
			for (int c = 0; c < sv_maxclients->integer && t->ref >= 0; c++) {
				in += (Holo_PlayerIn(c) && Holo_InArea(&gHolo.areas[t->ref], c));
			}
			level = (players > 0 && in == players) ? qtrue : qfalse;
			edge = qtrue;
			break;
		}
		case HT_WHEN_GROUP_DEAD:
			level = (t->ref >= 0 && Holo_GroupDone(t->ref)) ? qtrue : qfalse;
			edge = qtrue;
			break;
		case HT_WHEN_GROUP_LEFT: {
			const htGroup_t* g = (t->ref >= 0) ? &gHolo.groups[t->ref] : NULL;
			level = (g && g->spawned && g->queued <= 0 && !g->leaderDue && Holo_GroupAlive(t->ref) <= t->count) ? qtrue : qfalse;
			edge = qtrue;
			break;
		}
		case HT_WHEN_ALL_DEAD:
			level = (anySpawned && allDone) ? qtrue : qfalse;
			edge = qtrue;
			break;
		case HT_WHEN_PLAYERS:
			level = (players >= Q_max(1, t->count)) ? qtrue : qfalse;
			edge = qtrue;
			break;
		case HT_WHEN_AFTER:
			if (t->ref >= 0) {
				const htTrigger_t* o = &gHolo.triggers[t->ref];
				go = (o->fired && o->firedAt != t->afterSeen && svs.time - o->firedAt >= (int)(t->seconds * 1000.0f)) ? qtrue : qfalse;
				if (go) {
					t->afterSeen = o->firedAt;
				}
			}
			break;
		case HT_WHEN_PLAYER_DIED:
			if (died >= 0) {
				who = &svs.clients[died];
				go = qtrue;
			}
			break;
		case HT_WHEN_NPC_KILLED:
			go = downs ? qtrue : qfalse;
			break;
		case HT_WHEN_COUNTER:
			if (t->ref >= 0) {
				const int v = gHolo.counters[t->ref];
				level = (t->compare > 0 ? v >= t->count : t->compare < 0 ? v <= t->count : v == t->count) ? qtrue : qfalse;
			}
			edge = qtrue;
			break;
		case HT_WHEN_COUNTDOWN_END:
			go = gHolo.countdownDone;
			break;
		}
		if (edge) {
			go = (level && !t->wasTrue) ? qtrue : qfalse;
			t->wasTrue = level;
		}
		if (!go) {
			continue;
		}
		if (t->repeat && t->fired && svs.time - t->lastFiredAt < t->cooldownMs) {
			continue; // too soon since the last time
		}
		if (t->when == HT_WHEN_TIMER) {
			t->nextAt = t->repeat ? svs.time + Q_max(1000, (int)(t->seconds * 1000.0f)) : 0x7fffffff;
		}
		t->fired = qtrue;
		t->firedAt = svs.time;
		t->lastFiredAt = svs.time;
		Com_Printf("Holotable: trigger %s%s%s\n", t->id, who ? " by " : "", who ? who->name : "");
		Holo_RunActions(t, who);
	}
	gHolo.countdownDone = qfalse; // every trigger waiting for it has had it
}

static void Holo_Frame(void)
{
	if (!gHoloActive) {
		return;
	}
	if (!Holo_Enabled()) {
		Holo_End("called off");
		return;
	}
	if (svs.time - gHolo.startedAt > gHolo.timeLimit * 1000) {
		Holo_End("time's up");
		return;
	}
	Holo_TeamFrame();
	// The countdown, on everyone's screen each second.
	if (gHolo.countdownEnd) {
		const int left = (gHolo.countdownEnd - svs.time + 999) / 1000;
		if (left <= 0) {
			gHolo.countdownEnd = 0;
			gHolo.countdownDone = qtrue;
			SV_SendServerCommand(NULL, "cp \"%s%s^10\"\n", gHolo.countdownText, gHolo.countdownText[0] ? "\n" : "");
		} else if (left != gHolo.countdownShown) {
			gHolo.countdownShown = left;
			SV_SendServerCommand(NULL, "cp \"%s%s^3%d:%02d\"\n", gHolo.countdownText, gHolo.countdownText[0] ? "\n" : "", left / 60, left % 60);
		}
	}
	// Gravity / speed changed for a while: back when it's up.
	if (gHolo.gravitySet && gHolo.gravityUntil && svs.time >= gHolo.gravityUntil) {
		Cvar_Set("g_gravity", gHolo.gravityWas);
		gHolo.gravitySet = qfalse;
	}
	if (gHolo.speedPct && gHolo.speedUntil && svs.time >= gHolo.speedUntil) {
		gHolo.speedPct = 0;
	}
	if (svs.time >= gHolo.nextTriggers) {
		gHolo.nextTriggers = svs.time + 250;
		Holo_CheckTriggers();
		if (!gHoloActive) {
			return;
		}
	}
	if (svs.time >= gHolo.nextSpawn) {
		// One NPC a quarter second; one the game won't spawn doesn't hold
		// up the rest.
		gHolo.nextSpawn = svs.time + 250;
		for (int tries = 0; tries < 8 && Holo_SpawnOne() == HT_SPAWN_FAILED; tries++) {
		}
	}
	if (svs.time >= gHolo.nextThink) {
		gHolo.nextThink = svs.time + 1000;
		Holo_Think();
	}
	Holo_PaceFrame();
}

// Loads a scenario file and starts it ("started", or "restarted" by
// !ht restart). qfalse, told to cl, if it won't load.
static qboolean Holo_Start(client_t* cl, const char* file, const char* label, const char* verb)
{
	char err[128] = "";
	if (!Holo_Load(file, err, sizeof(err))) {
		Holo_Reply(cl, va("^1Can't run %s^7: %s", label, err));
		return qfalse;
	}
	gHoloActive = qtrue;
	gHolo.startedAt = svs.time;
	gHolo.nextSpawn = svs.time + 1500; // the regulars clear out first
	for (int i = 0; i < gHolo.numTriggers; i++) {
		gHolo.triggers[i].nextAt = svs.time + (int)(gHolo.triggers[i].seconds * 1000.0f);
	}
	for (int c = 0; c < sv_maxclients->integer; c++) {
		gHolo.playerUp[c] = Holo_PlayerIn(c);
	}
	for (int gi = 0; gi < gHolo.numGroups; gi++) {
		if (gHolo.groups[gi].spawnAtStart) {
			Holo_QueueGroup(gi, -1, -1);
		}
	}
	Holo_ReadTeamNames();
	if (gHolo.joinTeam) {
		// One side only: MBII's team balance would refuse to stack it.
		Q_strncpyz(gHolo.balanceWas, Cvar_VariableString("g_balance"), sizeof(gHolo.balanceWas));
		Cvar_Set("g_balance", "0");
		gHolo.balanceChanged = qtrue;
	}
	// The round clock runs on at least as long as the scenario can.
	if (Holo_ShiftRound(gHolo.timeLimit * 1000)) {
		gHolo.roundExtendMs = gHolo.timeLimit * 1000;
	}
	SV_SendServerCommand(NULL, "chat \"^5[Holotable] ^7%s ^7%s ^5%s^7!\"\n", cl ? cl->name : "An admin", verb, gHolo.name);
	if (gHolo.joinTeam) {
		SV_SendServerCommand(NULL, "chat \"^5[Holotable]^7 Co-op: everyone on the ^3%s^7 side%s.\"\n",
			gHolo.teamNames[gHolo.joinTeam], gHolo.anytime ? va(", respawning after %ds", gHolo.respawnSecs) : "");
	} else if (gHolo.anytime) {
		SV_SendServerCommand(NULL, "chat \"^5[Holotable]^7 Anytime spawn: back in %ds after dying, and join any time.\"\n", gHolo.respawnSecs);
	}
	if (gHolo.numClasses && Cvar_VariableIntegerValue("g_Authenticity") != 0) {
		SV_SendServerCommand(NULL, "chat \"^5[Holotable]^7 Only the scenario's classes can be played (%d of them).\"\n", gHolo.numClasses);
	}
	Com_Printf("Holotable: %s %s %s (%s)\n", cl ? cl->name : "rcon", verb, gHolo.name, gHolo.file);
	return qtrue;
}

// Scenario runners: accounts allowed to play, restart and stop scenarios
// without being admins - holotable_runners.dat beside the accounts file,
// one handle a line, kept by the Holotable web app (its Users page) and
// re-read every few seconds.
static qboolean Holo_RunnerFileHas(const char* handle)
{
	static char cache[4096];
	static int loadedAt = -1000000;
	if (svs.time - loadedAt > 5000 || svs.time < loadedAt) {
		loadedAt = svs.time;
		cache[0] = '\0';
		FILE* f = fopen(va("%s/%s/holotable_runners.dat", Cvar_VariableString("fs_basepath"), Cvar_VariableString("fs_game")), "r");
		if (f) {
			const size_t n = fread(cache, 1, sizeof(cache) - 1, f);
			cache[n] = '\0';
			fclose(f);
		}
	}
	char buf[sizeof(cache)];
	Q_strncpyz(buf, cache, sizeof(buf));
	for (char* w = strtok(buf, " \r\n\t"); w; w = strtok(NULL, " \r\n\t")) {
		if (!Q_stricmp(w, handle)) {
			return qtrue;
		}
	}
	return qfalse;
}

// Who can play / restart / stop scenarios: rcon, admins, and runners.
static qboolean Holo_CanRun(client_t* cl)
{
	if (!cl) {
		return qtrue;
	}
	return (cl->economyHandle[0] && (Social_IsAdmin(cl) || Holo_RunnerFileHas(cl->economyHandle))) ? qtrue : qfalse;
}

// "!ht", "!ht <n>", "!ht <n> play" (or "!ht play <n>"), "!ht stop",
// "!ht restart" (the running one again, from its file as it is now) - and
// rcon "ht ..." (cl NULL: no login needed).
qboolean SV_SocialHoloCommand(client_t* cl, const char* args)
{
	char a1[64] = "", a2[64] = "";
	sscanf(args, "%63s %63s", a1, a2);
	if (!Holo_Enabled()) {
		Holo_Reply(cl, "Holotable scenarios aren't on for this server.");
		return qtrue;
	}
	if (cl && !cl->economyHandle[0]) {
		Holo_Reply(cl, "Log in first - ^5!login <name> <password>");
		return qtrue;
	}
	if (!Q_stricmp(a1, "stop")) {
		if (!Holo_CanRun(cl)) {
			Holo_Reply(cl, "Only admins and scenario runners can stop a scenario.");
		} else if (!gHoloActive) {
			Holo_Reply(cl, "No scenario is running.");
		} else {
			Holo_End(cl ? va("stopped by %s", cl->name) : "stopped");
		}
		return qtrue;
	}

	if (!Q_stricmp(a1, "restart") || !Q_stricmp(a1, "reload")) {
		// The running scenario, read from its file again - so a change saved
		// in Holotable shows straight away. Its NPCs go and it starts over.
		if (!Holo_CanRun(cl)) {
			Holo_Reply(cl, "Only admins and scenario runners can restart a scenario.");
			return qtrue;
		}
		if (!gHoloActive) {
			Holo_Reply(cl, "No scenario is running - ^5!ht <n> play^7 starts one.");
			return qtrue;
		}
		char file[64], label[64];
		Q_strncpyz(file, gHolo.file, sizeof(file));
		Q_strncpyz(label, gHolo.name, sizeof(label));
		// Only once the file's readable: a half-saved or broken file leaves
		// the running one as it is.
		cJSON* check = Holo_ReadFile(file);
		if (!check) {
			Holo_Reply(cl, va("^1Can't reload %s^7: its file isn't readable JSON right now - still running the old one.", label));
			return qtrue;
		}
		const qboolean sameMap = !Q_stricmp(HtStr(check, "map", ""), Cvar_VariableString("mapname")) ? qtrue : qfalse;
		cJSON_Delete(check);
		if (!sameMap) {
			Holo_Reply(cl, va("^1Can't reload %s^7: it's for another map now - still running the old one.", label));
			return qtrue;
		}
		Holo_End(cl ? va("reloading (%s)", cl->name) : "reloading");
		Holo_Start(cl, file, label, "restarted");
		return qtrue;
	}

	Holo_RefreshList();
	const char* map = Cvar_VariableString("mapname");
	if (!a1[0]) {
		if (gHoloActive) {
			Holo_Reply(cl, va("Running: ^5%s^7 - ^5!ht stop^7 ends it, ^5!ht restart^7 reloads it.", gHolo.name));
		}
		if (!gHoloListCount) {
			Holo_Reply(cl, va("No Holotable scenarios for ^3%s^7 yet.", map));
			return qtrue;
		}
		Holo_Reply(cl, va("Scenarios for ^3%s^7 (^5!ht <n> play^7):", map));
		for (int i = 0; i < gHoloListCount; i++) {
			Holo_Reply(cl, va("^5%d^7. %s%s%s", i + 1, gHoloList[i].name, gHoloList[i].desc[0] ? " ^9- " : "", gHoloList[i].desc));
		}
		return qtrue;
	}

	const char* which = !Q_stricmp(a1, "play") ? a2 : a1;
	const qboolean play = (!Q_stricmp(a1, "play") || !Q_stricmp(a2, "play")) ? qtrue : qfalse;
	int pick = atoi(which) - 1;
	if (pick < 0 || pick >= gHoloListCount) {
		pick = -1;
		for (int i = 0; i < gHoloListCount && pick < 0; i++) {
			if (!Q_stricmp(gHoloList[i].file, which) || !Q_stricmp(gHoloList[i].name, which)) {
				pick = i;
			}
		}
	}
	if (pick < 0) {
		Holo_Reply(cl, va("No scenario ^3%s^7 for this map - ^5!ht^7 lists them.", which));
		return qtrue;
	}
	if (!play) {
		Holo_Reply(cl, va("^5%d^7. %s%s%s - ^5!ht %d play^7 runs it.", pick + 1, gHoloList[pick].name,
			gHoloList[pick].desc[0] ? " ^9- " : "", gHoloList[pick].desc, pick + 1));
		return qtrue;
	}
	if (!Holo_CanRun(cl)) {
		Holo_Reply(cl, "Only admins and scenario runners can run scenarios.");
		return qtrue;
	}
	if (gHoloActive || gBarFightActive) {
		Holo_Reply(cl, gHoloActive ? "A scenario's already running - ^5!ht stop^7 first." : "A bar fight's on - wait for it to end.");
		return qtrue;
	}
	Holo_Start(cl, gHoloList[pick].file, gHoloList[pick].name, "started");
	return qtrue;
}

// rcon debugging aids (finding where MBII keeps an NPC's team):
// "htdbgspawn <type>" spawns an NPC at the first client (bots too), and
// "htdbgdump <entity>" writes its game-side client memory (from its
// playerState, the start of MBII's gclient_t) to /tmp/htdump_<entity>.bin.
void SV_HoloDebugSpawn(const char* type)
{
	client_t* by = NULL;
	for (int i = 0; i < sv_maxclients->integer && !by; i++) {
		if (svs.clients[i].state == CS_ACTIVE && svs.clients[i].gentity && svs.clients[i].gentity->playerState) {
			by = &svs.clients[i];
		}
	}
	if (!by || !gNPCSpawnType || !type || !type[0]) {
		Com_Printf("htdbgspawn: needs a client in the game and a type\n");
		return;
	}
	void* old = GVM_BeginNative();
	sharedEntity_t* e = (sharedEntity_t*)gNPCSpawnType(by->gentity, (char*)type, NULL, 0, 0, 0);
	GVM_EndNative(old);
	Com_Printf("htdbgspawn: %s -> entity %d\n", type, e ? e->s.number : -1);
}

void SV_HoloDebugDump(int num)
{
	if (num < 0 || num >= sv.num_entities) {
		Com_Printf("htdbgdump: no entity %d\n", num);
		return;
	}
	sharedEntity_t* e = SV_GentityNum(num);
	if (!e->playerState) {
		Com_Printf("htdbgdump: entity %d has no client\n", num);
		return;
	}
	FILE* f = fopen(va("/tmp/htdump_%d.bin", num), "wb");
	if (!f) {
		return;
	}
	fwrite(e->playerState, 1, 32768, f);
	fclose(f);
	Com_Printf("htdbgdump: entity %d (eType %d, PERS_TEAM %d, clientNum %d, health %d) -> /tmp/htdump_%d.bin\n",
		num, e->s.eType, e->playerState->persistant[PERS_TEAM], e->playerState->clientNum, e->playerState->stats[STAT_HEALTH], num);
}

// Called with every client's movement command: a frozen player can look
// about, but not move, jump or shoot.
static void Holo_SpeedThink(client_t* cl);

void SV_HoloClientThink(client_t* cl, usercmd_t* cmd)
{
	Holo_SpeedThink(cl);
	const int c = cl - svs.clients;
	if (!gHoloActive || c < 0 || c >= MAX_CLIENTS || svs.time >= gHolo.frozenUntil[c]) {
		return;
	}
	cmd->forwardmove = cmd->rightmove = cmd->upmove = 0;
	cmd->buttons = 0;
}

// Players' speed changed by the scenario: on the ground, pushed up towards it
// (as the bar's Sugar Rush does) or held down to it. Normal running is ~250.
static void Holo_SpeedThink(client_t* cl)
{
	if (!gHoloActive || !gHolo.speedPct || !cl->gentity || !cl->gentity->playerState) {
		return;
	}
	playerState_t* ps = cl->gentity->playerState;
	if (ps->groundEntityNum == ENTITYNUM_NONE || ps->pm_type != PM_NORMAL) {
		return;
	}
	const float target = 250.0f * gHolo.speedPct / 100.0f;
	const float speed = sqrtf(ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1]);
	float scale = 1.0f;
	if (gHolo.speedPct > 100 && speed > 50.0f && speed < target) {
		scale = Q_min(1.12f, target / speed);
	} else if (gHolo.speedPct < 100 && speed > target) {
		scale = target / speed;
	}
	ps->velocity[0] *= scale;
	ps->velocity[1] *= scale;
}

static qboolean Social_HoloRunning(void)
{
	return gHoloActive;
}

// --- Our NPC types, kept with the engine --------------------------------------
//
// The bar fights' own NPC types (droids, 212th and 501st clones) are MBII
// .npc data, which its game reads at start-up from every search path. So a
// fresh or moved server needs no copying by hand, the engine writes them
// into the instance's own MBII folder (fs_homepath/fs_game/ext_data/NPCs)
// before each game starts, if missing or out of date. Called from
// GVM_InitGame, ahead of MBII's NPC_LoadParms.
#include "social_npcs/ca_cantina_npc.h"

void SV_SocialEnsureNpcFiles(void)
{
	if ((!g_socialMode || !g_socialMode->integer) && !Holo_Enabled()) {
		return;
	}
	const char* dir = va("%s/%s/ext_data/NPCs", Cvar_VariableString("fs_homepath"), Cvar_VariableString("fs_game"));
	char path[MAX_OSPATH];
	Q_strncpyz(path, va("%s/ca_cantina.npc", dir), sizeof(path));

	FILE* f = fopen(path, "rb");
	if (f) {
		char have[sizeof(kCaCantinaNpc) + 1];
		const size_t n = fread(have, 1, sizeof(have), f);
		fclose(f);
		if (n == sizeof(kCaCantinaNpc) - 1 && !memcmp(have, kCaCantinaNpc, n)) {
			return; // already there, and current
		}
	}
	// mkdir -p, a level at a time.
	char build[MAX_OSPATH];
	Q_strncpyz(build, dir, sizeof(build));
	for (char* c = build + 1; *c; c++) {
		if (*c == '/') {
			*c = '\0';
			mkdir(build, 0755);
			*c = '/';
		}
	}
	mkdir(build, 0755);
	f = fopen(path, "wb");
	if (!f) {
		Com_Printf("Social mode: couldn't write %s\n", path);
		return;
	}
	fwrite(kCaCantinaNpc, 1, sizeof(kCaCantinaNpc) - 1, f);
	fclose(f);
	Com_Printf("Social mode: wrote %s\n", path);
}

void SV_SocialGameInit(void)
{
	// A new round or map frees every entity, fights included.
	gHoloSidesOk = -1;
	if (gHoloActive) {
		Holo_RestorePlayers(qtrue);
		gHoloActive = qfalse;
		for (int i = 0; i < HT_MAX_NPCS; i++) {
			gHolo.npcs[i].ent = -1;
		}
	}
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
		gEffectIndex = (int (*)(const char*))Sys_LoadFunction(dll, "G_EffectIndex");
		gPlayEffectID = (void (*)(int, float*, float*))Sys_LoadFunction(dll, "G_PlayEffectID");
		gScreenShake = (void* (*)(float*, void*, float, int, int))Sys_LoadFunction(dll, "G_ScreenShake");
		gSoundIndex = (int (*)(const char*))Sys_LoadFunction(dll, "G_SoundIndex");
		gSoundAtLoc = (void (*)(float*, int, int))Sys_LoadFunction(dll, "G_SoundAtLoc");
		gTeleportPlayer = (void (*)(void*, float*, float*))Sys_LoadFunction(dll, "TeleportPlayer");
		gUseTargets2 = (void (*)(void*, void*, const char*))Sys_LoadFunction(dll, "G_UseTargets2");
		gSiegeRoundComplete = (void (*)(int, int, int))Sys_LoadFunction(dll, "SiegeRoundComplete");
		gSiegeRoundEnded = (int*)Sys_LoadFunction(dll, "gSiegeRoundEnded");
		gHoloKnockdown = (void (*)(void*, int, int, int, qboolean))Sys_LoadFunction(dll, "G_Knockdown");
		gSiegeSetObjectiveComplete = (void (*)(int, int, qboolean))Sys_LoadFunction(dll, "G_SiegeSetObjectiveComplete");
		gBGFindItem = (void* (*)(const char*))Sys_LoadFunction(dll, "BG_FindItem");
		gLaunchItem = (void* (*)(void*, float*, float*))Sys_LoadFunction(dll, "LaunchItem");
		gAddRemap = (void (*)(const char*, const char*, float))Sys_LoadFunction(dll, "AddRemap");
		gBuildShaderStateConfig = (const char* (*)(void))Sys_LoadFunction(dll, "BuildShaderStateConfig");

		gNPCSpawnType = (void* (*)(void*, char*, char*, int, int, int))Sys_LoadFunction(dll, "NPC_SpawnType");
		gFreeEntity = (void (*)(void*))Sys_LoadFunction(dll, "G_FreeEntity");
		gSetEnemy = (void (*)(void*, void*))Sys_LoadFunction(dll, "G_SetEnemy");
		gClearEnemy = (void (*)(void*))Sys_LoadFunction(dll, "G_ClearEnemy");
		gSetMoveGoal = (void (*)(void*, float*, int, int, int, void*))Sys_LoadFunction(dll, "NPC_SetMoveGoal");
		gSoundOnEnt = (void (*)(void*, int, const char*))Sys_LoadFunction(dll, "G_SoundOnEnt");
		gSaveNPCGlobals = (void (*)(void))Sys_LoadFunction(dll, "SaveNPCGlobals");
		gRestoreNPCGlobals = (void (*)(void))Sys_LoadFunction(dll, "RestoreNPCGlobals");
		gSetNPCGlobals = (void (*)(void*))Sys_LoadFunction(dll, "SetNPCGlobals");
		gNPCFacePosition = (int (*)(float*, int))Sys_LoadFunction(dll, "NPC_FacePosition");
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
		Social_BarFightAutoFrame();
	}
	Holo_Frame(); // any server with g_holotable, not only social ones
	if (Social_AnytimeSpawnWanted() || (g_socialBots && g_socialBots->integer)) {
		Social_RescueStuckJoiners();
	}
}
