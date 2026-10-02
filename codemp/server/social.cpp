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
static void* gSocialProbe = NULL; // vmMain's address in that module
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
static void (*gNPCLoadParms)(void) = NULL; // reads every ext_data/NPCs/*.npc into MBII's NPC type buffer
static void (*gFreeEntity)(void* ent) = NULL;
static void (*gSetEnemy)(void* self, void* enemy) = NULL;
static void (*gClearEnemy)(void* self) = NULL;
// MBII's GlobalUse(self, other, activator): an entity's own use - as a
// button wired to it would.
static void (*gGlobalUse)(void* self, void* other, void* activator) = NULL;
// Props: a blank entity (G_Spawn) with a model registered for the clients
// (G_ModelIndex). MBII's misc_model_breakable is an empty stub, so they're
// made here.
static void* (*gGSpawn)(void) = NULL;
static int (*gModelIndex)(const char* name) = NULL;
enum { HT_PL_PROP, HT_PL_ITEM, HT_PL_VEHICLE, HT_PL_EFFECT, HT_PL_SOUND };

// Items: MBII's own map loading - G_ParseSpawnVars reads one entity's keys
// (through the engine's entity parser), G_SpawnGEntityFromSpawnVars spawns
// it, as if the map had it.
static qboolean (*gParseSpawnVars)(qboolean inSubBSP) = NULL;
static void (*gSpawnFromSpawnVars)(qboolean inSubBSP) = NULL;
static void (*gSetMoveGoal)(void* ent, float* point, int radius, int isNavGoal, int combatPoint, void* targetEnt) = NULL;
static void (*gSoundOnEnt)(void* ent, int channel, const char* path) = NULL;
static void (*gSaveNPCGlobals)(void) = NULL;
static void (*gRestoreNPCGlobals)(void) = NULL;
static void (*gSetNPCGlobals)(void* ent) = NULL;
static void (*gNPCChangeWeapon)(int weapon) = NULL;               // NPC_ChangeWeapon: the NPC globals' one
static int (*gGetIDForString)(void* table, const char* s) = NULL; // GetIDForString
static void* gWPTable = NULL;                                     // WPTable: MBII's weapon names
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

static qboolean Social_ScenarioHit(void* targ, void* attacker);
static qboolean Holo_IsNpc(void* ent);
static qboolean Holo_Running(void);
static int Holo_NpcSide(int num);
static qboolean Holo_Peaceful(void* ent);
// Both on a side (PERS_TEAM 1/2), and the same / different ones. An NPC's
// side is only what its scenario group gave it: MBII keeps an NPC's own team
// in PERS_TEAM too, and its numbers overlap the players' (an "enemy" NPC,
// like DarthVader, is 2 - the blue team's), which made every player on blue
// that NPC's ally and unhurtable by it.
static int Holo_SideOf(void* ent)
{
	const sharedEntity_t* e = (const sharedEntity_t*)ent;
	if (!e || !e->playerState) {
		return 0;
	}
	if (e->s.eType == ET_NPC) {
		return Holo_NpcSide(e->s.number);
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
	if (Holo_Peaceful(targ)) {
		return; // a scenario's peaceful NPCs can't be hurt, on any server
	}
	if (Social_Enabled() && Social_ScenarioHit(targ, attacker)) {
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
	} else if (Holo_Running() && (Holo_IsNpc(targ) || Holo_IsNpc(attacker))) {
		// A Holotable scenario on any other server: an NPC on a side and the
		// players / NPCs on that same side can't hurt each other (crossfire,
		// or MBII's AI picking a friend); everything else as MBII has it.
		if (Holo_SameSide(targ, attacker)) {
			return;
		}
		if (Holo_IsNpc(targ) && Holo_IsNpc(attacker)) {
			dflags |= SOCIAL_DAMAGE_NO_TKPOINTS;
		}
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

static qboolean Social_HoloRunning(void);

static void Social_NpcFrame(void)
{
	Social_ParseNpcs();
	if (!gSocialNpcCount || !gNPCSpawnType) {
		return;
	}
	// A Holotable scenario's on (bar fights are scenarios too): the regulars
	// keep out of it (its FREE-team NPCs would go for them too), and are back
	// once it's over.
	if (Social_HoloRunning()) {
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

// Who a Holotable scenario's NPCs can hurt (bar fights are scenarios too):
// players and NPCs, both ways - players still can't hurt each other - except
// that one on a side (it attacks only the other) doesn't hurt or get hurt by
// players on its own side, and fights scenario NPCs on the other one.
static qboolean Social_ScenarioHit(void* targ, void* attacker)
{
	if (!Holo_IsNpc(targ) && !Holo_IsNpc(attacker)) {
		return qfalse;
	}
	if (Holo_SameSide(targ, attacker)) {
		return qfalse;
	}
	if (Holo_IsNpc(targ) && Holo_IsNpc(attacker)) {
		return Holo_OppositeSides(targ, attacker);
	}
	return ((Social_IsPlayerEntity(targ) && Holo_IsNpc(attacker)) ||
		(Holo_IsNpc(targ) && Social_IsPlayerEntity(attacker))) ? qtrue : qfalse;
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

enum { HT_BEHAVE_HUNT, HT_BEHAVE_ROUTE, HT_BEHAVE_GUARD, HT_BEHAVE_IDLE, HT_BEHAVE_FOLLOW };
#define HT_FOLLOW_GAP   140.0f // follow: how close behind its player it stops
#define HT_FOLLOW_RUN   400.0f // ...and further than this, it runs to catch up
#define HT_ATTACKS_NONE -1 // a group's attacks: nobody - peaceful, and can't be hurt
enum { HT_WHEN_START, HT_WHEN_TIMER, HT_WHEN_ENTER, HT_WHEN_GROUP_DEAD, HT_WHEN_ALL_DEAD, HT_WHEN_AFTER,
	HT_WHEN_ALL_IN_AREA, HT_WHEN_GROUP_LEFT, HT_WHEN_PLAYERS, HT_WHEN_PLAYER_DIED, HT_WHEN_NPC_KILLED,
	HT_WHEN_COUNTER, HT_WHEN_COUNTDOWN_END, HT_WHEN_GROUP_IN_AREA, HT_WHEN_USE };
enum { HT_DO_SPAWN, HT_DO_MESSAGE, HT_DO_CENTER, HT_DO_SOUND, HT_DO_MUSIC, HT_DO_END, HT_DO_SAY,
	HT_DO_TELL, HT_DO_EXPLODE, HT_DO_EFFECT, HT_DO_SHAKE, HT_DO_TELEPORT, HT_DO_USE, HT_DO_DESPAWN, HT_DO_WIN,
	HT_DO_RESPAWN, HT_DO_BREAK, HT_DO_PROP,
	HT_DO_GIVE, HT_DO_KNOCKDOWN, HT_DO_KILL, HT_DO_HEAL, HT_DO_FREEZE, HT_DO_VEHICLE, HT_DO_ADDTIME, HT_DO_MOVE, HT_DO_SIDE, HT_DO_ARM,
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
	int  attacks;                 // 0 = everyone; TEAM_RED / TEAM_BLUE = only that side (it fights for the other); HT_ATTACKS_NONE = nobody
	qboolean routeWalk;           // walks its route (else runs it); after someone, it runs either way
	int  followClient;            // HT_BEHAVE_FOLLOW: the player it follows (-1 = nobody)
	char followClass[40];         // ...or (set) whoever's nearest playing this class ("sc" token)
	int  weapon;                  // given by an "arm" action: its NPCs carry this (0 = their type's own)
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
	vec3_t propMins, propMaxs;    // prop: its box (from the model), round its origin
} htAction_t;
typedef struct {
	char id[40];
	int  when;
	int  ref;                     // area / group / trigger it's about (-1 = none)
	int  ref2;                    // group_in_area: the group (ref is the area)
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
	// use: a player holds use at a point (within useRadius) or in an area
	int  usePt, useArea;          // where (-1 = not that)
	float useRadius;
	int  holdMs;                  // how long to hold use (0 = just press it)
	qboolean useBar;              // a progress bar on their screen while they hold it
	char useLabel[64];            // ...and what it says
	char useSound[128];           // a sound while they hold it ("" = none)...
	int  useSoundEveryMs;         // ...this often
	int  useTeam;                 // 0 = anyone; TEAM_RED / TEAM_BLUE = only that side
	int  useHeld[MAX_CLIENTS];    // how long each has held it so far
	int  useSoundAt[MAX_CLIENTS]; // when each next hears the sound
	int  useShownAt[MAX_CLIENTS]; // when each's bar was last sent
	qboolean usePressed[MAX_CLIENTS]; // use held at their last command (for "just press it")
	int  useDoneBy;               // who's just finished it (client + 1), for the trigger check
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
	int  sideDue;                 // the side it attacks, not yet set on it (nobody in the game to check against)
	int  weaponSet;               // the group weapon it's been given (0 = none yet)
} htNpc_t;

static qboolean gHoloActive = qfalse;
// The running scenario is the map's background (Holo_BackgroundFrame): it
// plays quietly whenever nothing else does, ignores its time limit, and is
// put away (quietly) for any other scenario.
static qboolean gHoloBackground = qfalse;
static qboolean gHoloStartingBackground = qfalse; // Holo_Start: this one's the background
static int gHoloBgNextTry = 0;                    // svs.time to (re)start the background
static qboolean gHoloBgOff = qfalse;              // stopped with !ht stop: not till the next round
static qboolean Holo_HasBackground(void);

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
	// Where each side respawns (by TEAM_RED / TEAM_BLUE): a point or a route
	// (-1 both = the map's own spawns), and who's been seen in the game.
	int  respawnPt[3], respawnRt[3], respawnTurn[3];
	int  props[128];                  // props and items placed (entity numbers), taken away at the end
	int  numProps;
	// What the scenario has from its start (placed in the editor): props,
	// items, vehicles, looping effects and looping sounds.
	struct { int kind; char name[96]; vec3_t org; float yaw; vec3_t mins, maxs; int every, nextAt; } placed[96];
	int  numPlaced;
	int  vehicles[32];                // vehicles placed (removed at the end if nobody's riding)
	int  numVehicles;
	qboolean respawnSeen[MAX_CLIENTS];
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

static qboolean Holo_Running(void)
{
	return gHoloActive;
}

// A scenario NPC in a peaceful group (attacks nobody).
static qboolean Holo_Peaceful(void* ent)
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
		const htNpc_t* h = &gHolo.npcs[i];
		if (h->ent == num && h->group >= 0 && h->group < gHolo.numGroups) {
			return gHolo.groups[h->group].attacks == HT_ATTACKS_NONE ? qtrue : qfalse;
		}
	}
	return qfalse;
}

// The side a scenario NPC fights for (TEAM_RED / TEAM_BLUE), or 0: one that
// attacks everyone (or nobody), or not a scenario's.
static int Holo_NpcSide(int num)
{
	if (!gHoloActive) {
		return 0;
	}
	for (int i = 0; i < HT_MAX_NPCS; i++) {
		const htNpc_t* h = &gHolo.npcs[i];
		if (h->ent == num && h->group >= 0 && h->group < gHolo.numGroups) {
			const int attacks = gHolo.groups[h->group].attacks;
			return attacks == TEAM_RED ? TEAM_BLUE : attacks == TEAM_BLUE ? TEAM_RED : 0;
		}
	}
	return 0;
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
	// Props, items, vehicles, looping effects and sounds there from the start.
	static const struct { const char* list; const char* key; int kind; } kPlaced[] = {
		{ "props", "model", HT_PL_PROP }, { "items", "item", HT_PL_ITEM }, { "vehicles", "vehicle", HT_PL_VEHICLE },
		{ "effects", "effect", HT_PL_EFFECT }, { "sounds", "sound", HT_PL_SOUND },
	};
	for (size_t pass = 0; pass < ARRAY_LEN(kPlaced); pass++) {
		cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, kPlaced[pass].list)) {
			if (gHolo.numPlaced >= (int)ARRAY_LEN(gHolo.placed)) break;
			const int kind = kPlaced[pass].kind;
			const char* name = HtStr(it, kPlaced[pass].key, "");
			qboolean ok = name[0] ? qtrue : qfalse;
			if (kind == HT_PL_PROP) {
				ok = (!Q_stricmpn(name, "models/", 7) && Q_stristr(name, ".md3")) ? qtrue : qfalse;
			} else if (kind == HT_PL_ITEM) {
				ok = (!Q_stricmpn(name, "item_", 5) || !Q_stricmpn(name, "weapon_", 7) || !Q_stricmpn(name, "ammo_", 5) || !Q_stricmpn(name, "holdable_", 9)) ? qtrue : qfalse;
			} else if (kind == HT_PL_SOUND) {
				ok = !Q_stricmpn(name, "sound/", 6) ? qtrue : qfalse;
			}
			if (!ok) {
				continue;
			}
			auto& p = gHolo.placed[gHolo.numPlaced++];
			p.kind = kind;
			Holo_Clean(p.name, name, sizeof(p.name));
			HtVec(it, p.org);
			p.yaw = HtNum(it, "yaw", 0.0f);
			p.every = (int)(1000.0f * Q_max(0.2f, Q_min(60.0f, HtNum(it, "every", 1.0f))));
			const cJSON* mn = cJSON_GetObjectItemCaseSensitive(it, "mins");
			const cJSON* mx = cJSON_GetObjectItemCaseSensitive(it, "maxs");
			for (int k = 0; k < 3; k++) {
				p.mins[k] = (float)(cJSON_IsArray(mn) && cJSON_GetArrayItem(mn, k) ? cJSON_GetArrayItem(mn, k)->valuedouble : -16.0);
				p.maxs[k] = (float)(cJSON_IsArray(mx) && cJSON_GetArrayItem(mx, k) ? cJSON_GetArrayItem(mx, k)->valuedouble : 16.0);
			}
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
		g->attacks = !Q_stricmp(at, "team1") ? TEAM_RED : !Q_stricmp(at, "team2") ? TEAM_BLUE : !Q_stricmp(at, "none") ? HT_ATTACKS_NONE : 0;
		g->followClient = -1;
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
			!Q_stricmp(w, "counter") ? HT_WHEN_COUNTER : !Q_stricmp(w, "countdown_end") ? HT_WHEN_COUNTDOWN_END :
			!Q_stricmp(w, "group_in_area") ? HT_WHEN_GROUP_IN_AREA : !Q_stricmp(w, "use") ? HT_WHEN_USE : HT_WHEN_START;
		if (t->when == HT_WHEN_USE) {
			const char* at = HtStr(it, "at", "");
			t->usePt = HT_FIND(gHolo.points, gHolo.numPoints, at);
			t->useArea = (t->usePt < 0) ? HT_FIND(gHolo.areas, gHolo.numAreas, at) : -1;
			t->useRadius = Q_max(16.0f, Q_min(1024.0f, HtNum(it, "radius", 64.0f)));
			t->holdMs = (int)(1000.0f * Q_max(0.0f, Q_min(120.0f, HtNum(it, "hold", 3.0f))));
			const cJSON* bar = cJSON_GetObjectItemCaseSensitive(it, "bar");
			t->useBar = (bar && cJSON_IsFalse(bar)) ? qfalse : qtrue;
			Holo_Clean(t->useLabel, HtStr(it, "label", ""), sizeof(t->useLabel));
			Holo_Clean(t->useSound, HtStr(it, "sound", ""), sizeof(t->useSound));
			t->useSoundEveryMs = (int)(1000.0f * Q_max(0.2f, Q_min(30.0f, HtNum(it, "soundEvery", 1.0f))));
			const char* team = HtStr(it, "team", "any");
			t->useTeam = !Q_stricmp(team, "team1") ? TEAM_RED : !Q_stricmp(team, "team2") ? TEAM_BLUE : 0;
		}
		t->off = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "startOff")) ? qtrue : qfalse;
		const char* cmp = HtStr(it, "compare", ">=");
		t->compare = !Q_stricmp(cmp, "<=") ? -1 : !Q_stricmp(cmp, "==") ? 0 : 1;
		t->seconds = Q_max(0.0f, HtNum(it, "seconds", 0.0f));
		t->count = (int)HtNum(it, "count", 0.0f);
		t->repeat = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "repeat")) ? qtrue : qfalse;
		t->cooldownMs = (int)(Q_max(1.0f, HtNum(it, "cooldown", 5.0f)) * 1000.0f);
		t->ref2 = (t->when == HT_WHEN_GROUP_IN_AREA) ? HT_FIND(gHolo.groups, gHolo.numGroups, HtStr(it, "group", "")) : -1;
		t->ref = (t->when == HT_WHEN_ENTER || t->when == HT_WHEN_ALL_IN_AREA || t->when == HT_WHEN_GROUP_IN_AREA) ? HT_FIND(gHolo.areas, gHolo.numAreas, HtStr(it, "area", "")) :
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
			} else if (!Q_stricmp(d, "respawn")) {
				// Where a side (or both) respawns from now on.
				act->type = HT_DO_RESPAWN;
				const char* team = HtStr(a, "team", "both");
				act->ref = !Q_stricmp(team, "team1") ? TEAM_RED : !Q_stricmp(team, "team2") ? TEAM_BLUE : 0;
				const char* where = HtStr(a, "where", "");
				act->spawnPt = HT_FIND(gHolo.points, gHolo.numPoints, where);
				act->spawnRt = (act->spawnPt < 0) ? HT_FIND(gHolo.routes, gHolo.numRoutes, where) : -1;
			} else if (!Q_stricmp(d, "break")) {
				// A breakable on the map - by its brush model ("*12", so ones
				// without a name too), and its name if it has one.
				act->type = HT_DO_BREAK;
				const char* m = HtStr(a, "model", "");
				act->value = (m[0] == '*') ? atoi(m + 1) : 0;
				Q_strncpyz(act->target, HtStr(a, "target", ""), sizeof(act->target));
				if (act->value <= 0 && !act->target[0]) continue;
			} else if (!Q_stricmp(d, "prop")) {
				// A model placed as a solid object (a misc_model_breakable):
				// crates, barrels, barriers... health 0 = can't be broken.
				act->type = HT_DO_PROP;
				Holo_Clean(act->extra, HtStr(a, "model", ""), sizeof(act->extra));
				act->value = (int)HtNum(a, "yaw", -1000.0f); // -1000 = the point's facing
				act->damage = Q_max(0.0f, Q_min(5000.0f, HtNum(a, "health", 0.0f)));
				const cJSON* mn = cJSON_GetObjectItemCaseSensitive(a, "mins");
				const cJSON* mx = cJSON_GetObjectItemCaseSensitive(a, "maxs");
				for (int k = 0; k < 3; k++) {
					act->propMins[k] = (float)(cJSON_IsArray(mn) && cJSON_GetArrayItem(mn, k) ? cJSON_GetArrayItem(mn, k)->valuedouble : -16.0);
					act->propMaxs[k] = (float)(cJSON_IsArray(mx) && cJSON_GetArrayItem(mx, k) ? cJSON_GetArrayItem(mx, k)->valuedouble : 16.0);
				}
				if (Q_strncmp(act->extra, "models/", 7) || !strstr(act->extra, ".md3")) continue;
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
					!Q_stricmp(b, "idle") ? HT_BEHAVE_IDLE : (!Q_stricmp(b, "follow") || !Q_stricmp(b, "follow_class")) ? HT_BEHAVE_FOLLOW : HT_BEHAVE_HUNT;
				// follow_class: the class to follow (extra); follow: the player who set it off (extra empty)
				Q_strncpyz(act->extra, !Q_stricmp(b, "follow_class") ? HtStr(a, "class", "") : "", sizeof(act->extra));
				if (!Q_stricmp(b, "follow_class") && !act->extra[0]) continue;
				act->route = HT_FIND(gHolo.routes, gHolo.numRoutes, HtStr(a, "route", ""));
				act->center = !Q_stricmp(HtStr(a, "pace", "walk"), "run") ? qfalse : qtrue; // walk the route
				if (act->ref < 0 || (act->value == HT_BEHAVE_ROUTE && (act->route < 0 || !gHolo.routes[act->route].count))) continue;
			} else if (!Q_stricmp(d, "arm")) {
				// A group gets a weapon (its name, looked up in MBII's own table when used).
				act->type = HT_DO_ARM;
				act->ref = HT_FIND(gHolo.groups, gHolo.numGroups, HtStr(a, "group", ""));
				Q_strncpyz(act->extra, HtStr(a, "weapon", ""), sizeof(act->extra));
				if (act->ref < 0 || !act->extra[0]) continue;
			} else if (!Q_stricmp(d, "side")) {
				// A group changes side: who it attacks, as its own "attacks".
				act->type = HT_DO_SIDE;
				act->ref = HT_FIND(gHolo.groups, gHolo.numGroups, HtStr(a, "group", ""));
				const char* at = HtStr(a, "attacks", "all");
				act->value = !Q_stricmp(at, "team1") ? TEAM_RED : !Q_stricmp(at, "team2") ? TEAM_BLUE :
					!Q_stricmp(at, "none") ? HT_ATTACKS_NONE : 0;
				if (act->ref < 0) continue;
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
			if ((act->type == HT_DO_EXPLODE || act->type == HT_DO_EFFECT || act->type == HT_DO_TELEPORT || act->type == HT_DO_PROP) && act->atKind == HT_AT_NONE) {
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
	if (!gHolo.numTriggers && !startsSpawned && !gHolo.numPlaced) {
		Q_strncpyz(err, "nothing happens in it - no triggers, no group spawns at the start, and nothing placed", errSize);
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
static void Holo_SetSide(sharedEntity_t* e, int attacks);

// Puts an NPC on its group's side. Its PERS_TEAM (what the scenario's own
// rules go by - who it can hurt, who it goes for) straight away; MBII's own
// NPC team fields once there's a player in the game to check them against
// - till then it's due, and Holo_Think tries again each second. (A scenario
// started as the map loads - after a mode change - has its NPCs in before
// anyone's picked a class: they'd otherwise keep their .npc file's
// TEAM_FREE and attack everyone.)
// MBII's own idea of an NPC's team, which its team-kill check (OnSameTeam)
// goes by in siege: the client's session team (gclient +0x7b0) and the
// entity's team owner (+0x11c). An NPC gets both from the player it was
// spawned through - so every scenario NPC was on that player's team, and
// killing one from it counted as a team kill. Set to its side, or none
// (TEAM_FREE) for a group that attacks everyone or nobody. MBII hands a new
// NPC its spawner's team on its first think, so it's kept set (Holo_Think).
#define HOLO_OFS_SESSIONTEAM 0x7b0
#define HOLO_OFS_TEAMOWNER   0x11c
static void Holo_SetMbTeam(sharedEntity_t* e, int team)
{
	if (!e || !e->playerState) {
		return;
	}
	int* st = Holo_ClientInt(e->playerState, HOLO_OFS_SESSIONTEAM);
	int* to = (int*)((byte*)e + HOLO_OFS_TEAMOWNER);
	if (*st >= 0 && *st <= 3) {
		*st = team;
	}
	if (*to >= 0 && *to <= 3) {
		*to = team;
	}
}

static void Holo_ApplySide(htNpc_t* h)
{
	if (!h->sideDue || !Holo_NpcUp(h)) {
		return;
	}
	sharedEntity_t* e = SV_GentityNum(h->ent);
	e->playerState->persistant[PERS_TEAM] = (h->sideDue == HT_ATTACKS_NONE) ? TEAM_FREE :
		(h->sideDue == TEAM_RED) ? TEAM_BLUE : TEAM_RED;
	Holo_SetMbTeam(e, e->playerState->persistant[PERS_TEAM]);
	if (Holo_SidesUsable()) {
		Holo_SetSide(e, h->sideDue);
		h->sideDue = 0;
	}
}

// Back to attacking everyone: NPC TEAM_FREE, like Holotable's own NPC types
// (MBII's NPCs on TEAM_FREE go for anyone who isn't).
static void Holo_SetHostile(sharedEntity_t* e)
{
	if (!e || !e->playerState || !Holo_SidesUsable()) {
		return;
	}
	playerState_t* ps = e->playerState;
	const int* pt = Holo_ClientInt(ps, HOLO_OFS_PLAYERTEAM);
	const int* et = Holo_ClientInt(ps, HOLO_OFS_ENEMYTEAM);
	if (*pt < 0 || *pt > 3 || *et < 0 || *et > 3) {
		return; // not what an NPC's team looks like: leave it alone
	}
	*Holo_ClientInt(ps, HOLO_OFS_PLAYERTEAM) = 0; // NPC TEAM_FREE
	*Holo_ClientInt(ps, HOLO_OFS_ENEMYTEAM) = 1;  // NPC TEAM_PLAYER
	ps->persistant[PERS_TEAM] = TEAM_FREE;
}

static void Holo_SetSide(sharedEntity_t* e, int attacks)
{
	if (!e || !e->playerState) {
		return;
	}
	// MBII's team for its team-kill check, whatever else happens.
	Holo_SetMbTeam(e, (!attacks || attacks == HT_ATTACKS_NONE) ? TEAM_FREE : (attacks == TEAM_RED) ? TEAM_BLUE : TEAM_RED);
	if (!attacks || !Holo_SidesUsable()) {
		return;
	}
	playerState_t* ps = e->playerState;
	const int* pt = Holo_ClientInt(ps, HOLO_OFS_PLAYERTEAM);
	const int* et = Holo_ClientInt(ps, HOLO_OFS_ENEMYTEAM);
	if (*pt < 0 || *pt > 3 || *et < 0 || *et > 3) {
		return; // not what an NPC's team looks like: leave it alone
	}
	if (attacks == HT_ATTACKS_NONE) {
		// Peaceful: neutral, like MBII's bartender - it goes for nobody.
		*Holo_ClientInt(ps, HOLO_OFS_PLAYERTEAM) = 3; // NPC TEAM_NEUTRAL
		*Holo_ClientInt(ps, HOLO_OFS_ENEMYTEAM) = 3;
		ps->persistant[PERS_TEAM] = TEAM_FREE;
		return;
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
	for (int i = 0; i < gHolo.numProps; i++) {
		const int num = gHolo.props[i];
		if (num >= MAX_CLIENTS && num < sv.num_entities && gFreeEntity && SV_GentityNum(num)->r.linked) {
			void* old = GVM_BeginNative();
			gFreeEntity(SV_GentityNum(num));
			GVM_EndNative(old);
		}
	}
	gHolo.numProps = 0;
	for (int i = 0; i < gHolo.numVehicles; i++) {
		const int num = gHolo.vehicles[i];
		if (num < MAX_CLIENTS || num >= sv.num_entities || !gFreeEntity || !SV_GentityNum(num)->r.linked) {
			continue;
		}
		qboolean ridden = qfalse;
		for (int c = 0; c < sv_maxclients->integer && !ridden; c++) {
			const client_t* cl = &svs.clients[c];
			ridden = (cl->state == CS_ACTIVE && cl->gentity && cl->gentity->playerState && cl->gentity->playerState->m_iVehicleNum == num) ? qtrue : qfalse;
		}
		if (!ridden) {
			void* old = GVM_BeginNative();
			gFreeEntity(SV_GentityNum(num));
			GVM_EndNative(old);
		}
	}
	gHolo.numVehicles = 0;
	if (gHolo.music) {
		SV_JukeboxFightEnd();
	}
	Holo_RestorePlayers(qfalse);
	gHoloActive = qfalse; // anytime spawn off with it: respawn mode goes back next frame
	gHoloBgNextTry = svs.time + 5000; // the background (if any) back in a moment
	if (gHoloBackground) {
		gHoloBackground = qfalse;
		Com_Printf("Holotable: background %s put away (%s)\n", gHolo.name, how);
		return; // quietly
	}
	// The regulars (the map's background, or g_socialNpcs) come back once it's
	// over - not when it's only reloading, and not on a server that has none.
	const qboolean regularsBack = ((gSocialNpcCount > 0 || Holo_HasBackground()) && Q_stricmpn(how, "reloading", 9)) ? qtrue : qfalse;
	SV_SendServerCommand(NULL, "chat \"^5[Holotable] ^7%s ^7- %s.%s\"\n", gHolo.name, how, regularsBack ? " The regulars are back." : "");
	Com_Printf("Holotable: %s over (%s)\n", gHolo.name, how);
}

// The background steps aside for another scenario.
static void Holo_PutAwayBackground(void)
{
	if (gHoloActive && gHoloBackground) {
		Holo_End("another scenario");
	}
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

static client_t* Holo_NearestPlayer(const vec3_t from, float* distSq, int onlyTeam);

// Places a prop: a blank game entity showing the model, with a solid box -
// players, NPCs and shots stop at it. Its bottom sits on the floor. The box
// can't turn, so a turned prop gets the box round it. Returns the entity,
// or -1.
static int Holo_PlaceModel(const char* name, const vec3_t propMins, const vec3_t propMaxs, float yaw, const vec3_t floor)
{
	if (!gGSpawn || !gModelIndex || gHolo.numProps >= (int)ARRAY_LEN(gHolo.props)) {
		return -1;
	}
	void* old = GVM_BeginNative();
	sharedEntity_t* e = (sharedEntity_t*)gGSpawn();
	const int model = e ? gModelIndex(name) : 0;
	GVM_EndNative(old);
	if (!e) {
		return -1;
	}
	vec3_t org = { floor[0], floor[1], floor[2] - propMins[2] };
	e->s.eType = ET_GENERAL;
	e->s.modelindex = model;
	VectorCopy(org, e->s.pos.trBase);
	VectorCopy(org, e->s.origin);
	VectorCopy(org, e->r.currentOrigin);
	e->s.pos.trType = TR_STATIONARY;
	VectorSet(e->s.apos.trBase, 0.0f, yaw, 0.0f);
	VectorCopy(e->s.apos.trBase, e->s.angles);
	VectorCopy(e->s.apos.trBase, e->r.currentAngles);
	e->s.apos.trType = TR_STATIONARY;
	// The box, turned with it and boxed again (corners round the yaw).
	const float c = cosf(DEG2RAD(yaw)), sn = sinf(DEG2RAD(yaw));
	vec3_t mins = { 99999.0f, 99999.0f, propMins[2] }, maxs = { -99999.0f, -99999.0f, propMaxs[2] };
	for (int k = 0; k < 4; k++) {
		const float x = (k & 1) ? propMaxs[0] : propMins[0], y = (k & 2) ? propMaxs[1] : propMins[1];
		const float rx = x * c - y * sn, ry = x * sn + y * c;
		mins[0] = Q_min(mins[0], rx); maxs[0] = Q_max(maxs[0], rx);
		mins[1] = Q_min(mins[1], ry); maxs[1] = Q_max(maxs[1], ry);
	}
	VectorCopy(mins, e->r.mins);
	VectorCopy(maxs, e->r.maxs);
	e->r.contents = CONTENTS_SOLID;
	e->r.svFlags = 0;
	SV_LinkEntity(e);
	gHolo.props[gHolo.numProps++] = e->s.number;
	return e->s.number;
}

static int Holo_SpawnProp(const htAction_t* act, const vec3_t floor)
{
	return Holo_PlaceModel(act->extra, act->propMins, act->propMaxs, (float)act->value, floor);
}

// Places an item (weapon_, ammo_, item_, holdable_) as if the map had it:
// MBII's own spawner is handed a one-entity "map". Returns the entity, or
// -1. (The new one's found by its entity number - MBII sets it on spawning
// and clears it on freeing; it's linked a moment later.)
static int Holo_PlaceItem(const char* classname, const vec3_t floor)
{
	if (!gParseSpawnVars || !gSpawnFromSpawnVars || gHolo.numProps >= (int)ARRAY_LEN(gHolo.props)) {
		return -1;
	}
	char text[256];
	Com_sprintf(text, sizeof(text), "{ \"classname\" \"%s\" \"origin\" \"%.0f %.0f %.0f\" }", classname, floor[0], floor[1], floor[2] + 16.0f);
	static qboolean was[MAX_GENTITIES];
	const int before = sv.num_entities;
	for (int i = MAX_CLIENTS; i < before; i++) {
		was[i] = (SV_GentityNum(i)->s.number == i) ? qtrue : qfalse;
	}
	char* saved = sv.entityParsePoint;
	sv.entityParsePoint = text;
	void* old = GVM_BeginNative();
	if (gParseSpawnVars(qfalse)) {
		gSpawnFromSpawnVars(qfalse);
	}
	GVM_EndNative(old);
	sv.entityParsePoint = saved;
	for (int i = MAX_CLIENTS; i < sv.num_entities; i++) {
		if (SV_GentityNum(i)->s.number == i && (i >= before || !was[i])) {
			gHolo.props[gHolo.numProps++] = i;
			return i;
		}
	}
	return -1;
}

// A vehicle (swoop, speeder...) at the spot - MBII's NPC spawn as a vehicle,
// facing yaw (-1000 = whichever way). Returns its entity, or -1.
static int Holo_SpawnVehicle(const char* type, const vec3_t spot, float yaw)
{
	client_t* spawner = Social_AnyPlayer();
	for (int i = 0; i < sv_maxclients->integer && !spawner; i++) {
		if (svs.clients[i].state == CS_ACTIVE && svs.clients[i].gentity && svs.clients[i].gentity->playerState) {
			spawner = &svs.clients[i];
		}
	}
	if (!spawner || !gNPCSpawnType) {
		Com_Printf("Holotable: no one in the game to spawn vehicle \"%s\" through\n", type);
		return -1;
	}
	vec3_t at = { spot[0], spot[1], spot[2] + 24.0f };
	playerState_t* pps = spawner->gentity->playerState;
	const float savedYaw = pps->viewangles[YAW];
	if (yaw > -999.0f) {
		pps->viewangles[YAW] = yaw;
	}
	void* old = GVM_BeginNative();
	sharedEntity_t* e = (sharedEntity_t*)gNPCSpawnType(spawner->gentity, (char*)type, NULL, 1 /* vehicle */, 0, 0);
	GVM_EndNative(old);
	pps->viewangles[YAW] = savedYaw;
	if (!e) {
		Com_Printf("Holotable: couldn't spawn vehicle \"%s\"\n", type);
		return -1;
	}
	if (e->playerState) {
		VectorCopy(at, e->playerState->origin);
	}
	VectorCopy(at, e->s.origin);
	VectorCopy(at, e->s.pos.trBase);
	VectorCopy(at, e->r.currentOrigin);
	Com_Printf("Holotable: vehicle %s (entity %d)\n", type, e->s.number);
	return e->s.number;
}

// A looping sound at the spot: an entity with nothing but its loopSound, as
// a map's looping target_speaker - heard round it, all the time.
static int Holo_PlaceSound(const char* path, const vec3_t spot)
{
	if (!gGSpawn || !gSoundIndex || gHolo.numProps >= (int)ARRAY_LEN(gHolo.props)) {
		return -1;
	}
	void* old = GVM_BeginNative();
	sharedEntity_t* e = (sharedEntity_t*)gGSpawn();
	const int snd = e ? gSoundIndex(path) : 0;
	GVM_EndNative(old);
	if (!e) {
		return -1;
	}
	vec3_t at = { spot[0], spot[1], spot[2] + 32.0f };
	e->s.eType = ET_GENERAL;
	e->s.loopSound = snd;
	e->s.loopIsSoundset = qfalse;
	VectorCopy(at, e->s.pos.trBase);
	VectorCopy(at, e->s.origin);
	VectorCopy(at, e->r.currentOrigin);
	e->s.pos.trType = TR_STATIONARY;
	e->r.contents = 0;
	e->r.svFlags = 0;
	SV_LinkEntity(e);
	gHolo.props[gHolo.numProps++] = e->s.number;
	return e->s.number;
}

// Looping effects: each played again every so often while it runs.
static void Holo_EffectsFrame(void)
{
	if (!gEffectIndex || !gPlayEffectID) {
		return;
	}
	for (int i = 0; i < gHolo.numPlaced; i++) {
		auto& p = gHolo.placed[i];
		if (p.kind != HT_PL_EFFECT || svs.time < p.nextAt) {
			continue;
		}
		p.nextAt = svs.time + p.every;
		vec3_t at = { p.org[0], p.org[1], p.org[2] + 4.0f }, up = { 0.0f, 0.0f, 1.0f };
		void* old = GVM_BeginNative();
		gPlayEffectID(gEffectIndex(p.name), at, up);
		GVM_EndNative(old);
	}
}

// The scenario's own props, items, vehicles and sounds, as it starts
// (effects start playing from the next frame).
static void Holo_PlaceAll(void)
{
	int placed = 0, failed = 0;
	for (int i = 0; i < gHolo.numPlaced; i++) {
		auto& p = gHolo.placed[i];
		int num = -1;
		switch (p.kind) {
		case HT_PL_PROP:    num = Holo_PlaceModel(p.name, p.mins, p.maxs, p.yaw, p.org); break;
		case HT_PL_ITEM:    num = Holo_PlaceItem(p.name, p.org); break;
		case HT_PL_SOUND:   num = Holo_PlaceSound(p.name, p.org); break;
		case HT_PL_EFFECT:  num = 0; p.nextAt = 0; break;
		case HT_PL_VEHICLE:
			num = (gHolo.numVehicles < (int)ARRAY_LEN(gHolo.vehicles)) ? Holo_SpawnVehicle(p.name, p.org, p.yaw) : -1;
			if (num >= 0) {
				gHolo.vehicles[gHolo.numVehicles++] = num;
			}
			break;
		}
		(num < 0 ? failed : placed)++;
	}
	if (gHolo.numPlaced) {
		Com_Printf("Holotable: placed %d things%s\n", placed, failed ? va(" (%d couldn't be)", failed) : "");
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
		case HT_DO_PROP: {
			vec3_t at;
			if (!Holo_ActionAt(act, who, at)) {
				break;
			}
			at[2] -= (act->atKind == HT_AT_PLAYER) ? 24.0f : 16.0f; // back down to the floor it stands on
			htAction_t placed = *act;
			if (placed.value == -1000) {
				// No facing given: a point's own, else straight.
				placed.value = (act->atKind == HT_AT_POINT) ? (int)gHolo.points[act->atRef].yaw : 0;
			}
			const int num = Holo_SpawnProp(&placed, at);
			if (num >= 0) {
				const sharedEntity_t* pe = SV_GentityNum(num);
				Com_Printf("Holotable: prop %s at %.0f %.0f %.0f -> entity %d (contents %d, box %.0f %.0f %.0f / %.0f %.0f %.0f)\n",
					act->extra, at[0], at[1], at[2], num, pe->r.contents,
					pe->r.absmin[0], pe->r.absmin[1], pe->r.absmin[2], pe->r.absmax[0], pe->r.absmax[1], pe->r.absmax[2]);
			} else {
				Com_Printf("Holotable: prop %s at %.0f %.0f %.0f not placed\n", act->extra, at[0], at[1], at[2]);
			}
			break;
		}
		case HT_DO_RESPAWN:
			for (int t = TEAM_RED; t <= TEAM_BLUE; t++) {
				if (!act->ref || act->ref == t) {
					gHolo.respawnPt[t] = act->spawnPt;
					gHolo.respawnRt[t] = act->spawnRt;
					gHolo.respawnTurn[t] = 0;
				}
			}
			break;
		case HT_DO_BREAK: {
			// Used, as a button wired to it would - that breaks a breakable
			// whatever its health, and fires what it targets once (it's
			// removed on its next think). Damage only if MBII's use isn't
			// there to call.
			client_t* byCl = (who && who->gentity) ? who : Social_AnyPlayer();
			sharedEntity_t* by = (byCl && byCl->gentity) ? byCl->gentity : SV_GentityNum(ENTITYNUM_WORLD);
			int found = 0;
			for (int i = MAX_CLIENTS; act->value > 0 && i < sv.num_entities; i++) {
				sharedEntity_t* e = SV_GentityNum(i);
				if (!e->r.linked || !e->r.bmodel || e->s.modelindex != act->value) {
					continue;
				}
				found++;
				if (gGlobalUse) {
					void* old = GVM_BeginNative();
					gGlobalUse(e, by, by);
					GVM_EndNative(old);
				} else {
					Holo_Damage(e, e->r.currentOrigin, 100000);
				}
			}
			Com_Printf("Holotable: broke *%d (%d found)\n", act->value, found);
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
			if (!Holo_ActionAt(act, who, at)) {
				break;
			}
			Holo_SpawnVehicle(act->extra, at, act->atKind == HT_AT_POINT ? gHolo.points[act->atRef].yaw : -1000.0f);
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
			if (act->value == HT_BEHAVE_FOLLOW && act->extra[0]) {
				// A class: whoever's nearest playing it (Holo_FollowClassTarget).
				Q_strncpyz(g->followClass, act->extra, sizeof(g->followClass));
				g->followClient = -1;
				Com_Printf("Holotable: group %s follows class %s\n", g->id, g->followClass);
			} else if (act->value == HT_BEHAVE_FOLLOW) {
				// The player who set the trigger off - or, for one no player
				// sets off, whoever's nearest the group.
				g->followClass[0] = '\0';
				g->followClient = -1;
				if (who && Holo_PlayerIn((int)(who - svs.clients))) {
					g->followClient = (int)(who - svs.clients);
				} else {
					for (int k = 0; k < HT_MAX_NPCS && g->followClient < 0; k++) {
						const htNpc_t* h = &gHolo.npcs[k];
						if (h->group == act->ref && Holo_NpcUp(h)) {
							float best = 0.0f;
							client_t* near = Holo_NearestPlayer(SV_GentityNum(h->ent)->playerState->origin, &best, 0);
							g->followClient = near ? (int)(near - svs.clients) : -1;
						}
					}
				}
				Com_Printf("Holotable: group %s follows %s\n", g->id, g->followClient >= 0 ? svs.clients[g->followClient].name : "nobody");
			}
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
		case HT_DO_ARM: {
			// MBII's number for it; every one of the group up switches at its
			// next think (Holo_ArmNpc), any spawned later too.
			const int wp = (gGetIDForString && gWPTable) ? gGetIDForString(gWPTable, act->extra) : -1;
			if (wp <= 0 || !Q_stricmp(act->extra, "WP_SABER")) {
				Com_Printf("Holotable: no weapon \"%s\" to give\n", act->extra);
				break;
			}
			gHolo.groups[act->ref].weapon = wp;
			Com_Printf("Holotable: group %s gets %s (%d)\n", gHolo.groups[act->ref].id, act->extra, wp);
			break;
		}
		case HT_DO_SIDE: {
			// The group's side from now on (any of it spawned later too), and
			// every one of it that's up switches now and lets go of whoever it
			// was after, to pick from the new side's enemies.
			htGroup_t* g = &gHolo.groups[act->ref];
			g->attacks = act->value;
			for (int k = 0; k < HT_MAX_NPCS; k++) {
				htNpc_t* h = &gHolo.npcs[k];
				if (h->group != act->ref || !Holo_NpcUp(h)) {
					continue;
				}
				sharedEntity_t* e = SV_GentityNum(h->ent);
				h->sideDue = 0;
				if (act->value) {
					h->sideDue = act->value;
					Holo_ApplySide(h);
				} else {
					Holo_SetHostile(e);
				}
				if (gClearEnemy) {
					void* old = GVM_BeginNative();
					gClearEnemy(e);
					GVM_EndNative(old);
				}
			}
			Com_Printf("Holotable: group %s now attacks %s\n", g->id, act->value == TEAM_RED ? "team1" :
				act->value == TEAM_BLUE ? "team2" : act->value == HT_ATTACKS_NONE ? "nobody" : "everyone");
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
// NPC types a scenario never spawns: they take the server down. MBII's
// boba_fett is its one CLASS_BOBAFETT NPC (the jetpack / flamethrower AI) -
// six of them in a scenario and the server shut down within seconds,
// "Cvar_Update: handle 5432 out of range" (2026-10-01).
static qboolean Holo_TypeRefused(const char* type)
{
	static const char* const kRefused[] = { "boba_fett" };
	static int warnedAt = -100000;
	for (int i = 0; i < (int)ARRAY_LEN(kRefused); i++) {
		if (!Q_stricmp(type, kRefused[i])) {
			if (svs.time - warnedAt > 30000 || svs.time < warnedAt) {
				warnedAt = svs.time;
				SV_SendServerCommand(NULL, "chat \"^5[Holotable]^7 Not spawning ^3%s^7 - that NPC crashes the server. Pick another type for it on Holotable.\"\n", type);
			}
			Com_Printf("Holotable: refused NPC \"%s\" (it crashes the server)\n", type);
			return qtrue;
		}
	}
	return qfalse;
}

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

		if (Holo_TypeRefused(type)) {
			// As a spawn MBII refused: on to the next.
			if (g->leaderDue) {
				g->leaderDue = qfalse;
			} else {
				g->queued--;
			}
			g->spawned++;
			return HT_SPAWN_FAILED;
		}

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
		htNpc_t* h = &gHolo.npcs[slot];
		memset(h, 0, sizeof(*h));
		gHolo.npcUp[slot] = qtrue;
		h->ent = e->s.number;
		h->group = gi;
		h->spawnedAt = svs.time;
		h->sideDue = g->attacks;
		Holo_ApplySide(h);
		if (!g->attacks) {
			Holo_SetMbTeam(e, TEAM_FREE); // attacks everyone: nobody's team-mate
		}
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
		const htGroup_t* g = &gHolo.groups[h->group];
		if (g->behaviour == HT_BEHAVE_FOLLOW && g->followClient >= 0 && g->followClient < sv_maxclients->integer
			&& Holo_PlayerIn(g->followClient) && svs.time - h->wpGoalAt > 300) {
			const float* to = svs.clients[g->followClient].gentity->playerState->origin;
			const float dist = Distance(e->playerState->origin, to);
			if (dist > HT_FOLLOW_GAP) {
				h->pace = dist > HT_FOLLOW_RUN ? 2 : 1;
				Holo_MoveTo(e, to);
				h->wpGoalAt = svs.time;
			}
		}
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
// the body attacking it till it's gone - or pick one on its own side. Drop
// either so it picks a live, real enemy. gentity_t's enemy is at 0x584 (from G_SetEnemy).
#define HOLO_ENEMY_OFS 0x584
// A group weapon (an "arm" action) onto one of its NPCs: MBII's own
// NPC_ChangeWeapon, run as that NPC (its NPC globals), as its Boba AI
// switches weapons. Not sabers - those need MBII's saber set-up at spawn.
static void Holo_ArmNpc(htNpc_t* h, sharedEntity_t* npc)
{
	const int wp = gHolo.groups[h->group].weapon;
	if (wp <= 0 || h->weaponSet == wp || !gNPCChangeWeapon || !gSetNPCGlobals || !gSaveNPCGlobals || !gRestoreNPCGlobals) {
		return;
	}
	h->weaponSet = wp;
	void* old = GVM_BeginNative();
	gSaveNPCGlobals();
	gSetNPCGlobals(npc);
	gNPCChangeWeapon(wp);
	gRestoreNPCGlobals();
	GVM_EndNative(old);
}

// Peaceful NPCs never keep an enemy: every frame, one picked is let go
// before it can aim or fire. Neutral (NPC TEAM_NEUTRAL both ways, like
// MBII's bartender) should be enough, but MBII's AI still has armed neutral
// NPCs go for each other - its own neutral ones just never carry a weapon.
static void Holo_PeacefulFrame(void)
{
	if (!gClearEnemy) {
		return;
	}
	for (int i = 0; i < HT_MAX_NPCS; i++) {
		const htNpc_t* h = &gHolo.npcs[i];
		if (h->group < 0 || h->group >= gHolo.numGroups || gHolo.groups[h->group].attacks != HT_ATTACKS_NONE || !Holo_NpcUp(h)) {
			continue;
		}
		sharedEntity_t* npc = SV_GentityNum(h->ent);
		if (*(void**)((byte*)npc + HOLO_ENEMY_OFS)) {
			void* old = GVM_BeginNative();
			gClearEnemy(npc);
			GVM_EndNative(old);
		}
	}
}

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
	// Dead, or one of its own side (MBII's AI picking a friend): let it go.
	if (!alive || Holo_SameSide(npc, e)) {
		void* old = GVM_BeginNative();
		gClearEnemy(npc);
		GVM_EndNative(old);
	}
}

// A player's class token ("sc" in their player configstring - what
// scenarios' class limits go by).
static qboolean Holo_PlaysClass(int c, const char* sc)
{
	char cs[MAX_INFO_STRING];
	SV_GetConfigstring(MB2_CS_PLAYERS + c, cs, sizeof(cs));
	return (cs[0] && !Q_stricmp(Info_ValueForKey(cs, "sc"), sc)) ? qtrue : qfalse;
}

// A group following a class: the player it has while they're alive and
// still on it, else whoever's nearest that is (none: -1, and it waits).
static void Holo_FollowClassTarget(htGroup_t* g, const vec3_t from)
{
	if (!g->followClass[0]) {
		return;
	}
	const int c = g->followClient;
	if (c >= 0 && c < sv_maxclients->integer && Holo_PlayerIn(c) && Holo_PlaysClass(c, g->followClass)) {
		return;
	}
	g->followClient = -1;
	float best = 0.0f;
	for (int i = 0; i < sv_maxclients->integer; i++) {
		if (!Holo_PlayerIn(i) || !Holo_PlaysClass(i, g->followClass)) {
			continue;
		}
		const float d = DistanceSquared(svs.clients[i].gentity->playerState->origin, from);
		if (g->followClient < 0 || d < best) {
			g->followClient = i;
			best = d;
		}
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
		Holo_ApplySide(h);
		Holo_ArmNpc(h, npc);
		Holo_SetMbTeam(npc, (!g->attacks || g->attacks == HT_ATTACKS_NONE) ? TEAM_FREE : (g->attacks == TEAM_RED) ? TEAM_BLUE : TEAM_RED);
		Holo_DropDeadEnemy(npc);
		const float* at = npc->playerState->origin;
		float d2 = 0.0f;
		sharedEntity_t* nearest = NULL;
		if (g->attacks == HT_ATTACKS_NONE) {
			if (gClearEnemy) {
				void* old = GVM_BeginNative();
				gClearEnemy(npc); // peaceful: whoever it picked a fight with, it lets go
				GVM_EndNative(old);
			}
		} else {
			nearest = Holo_NearestTarget(at, &d2, g->attacks);
		}
		const float engage2 = g->engage * g->engage;

		switch (g->behaviour) {
		case HT_BEHAVE_IDLE:
			break;
		case HT_BEHAVE_FOLLOW: {
			// After the player its orders named, a few steps behind, running
			// to catch up (Holo_PaceFrame keeps its goal on them). One that
			// attacks everyone goes for them instead; one on a side still
			// takes on enemies who come close, then carries on following.
			Holo_FollowClassTarget(&gHolo.groups[h->group], at);
			const int c = g->followClient;
			if (c < 0 || c >= sv_maxclients->integer || !Holo_PlayerIn(c)) {
				Holo_Pace(h, 0);
				break; // gone or dead: it waits where it is (and follows again when they're back)
			}
			sharedEntity_t* p = svs.clients[c].gentity;
			if (!g->attacks) {
				if (gSetEnemy) {
					void* old = GVM_BeginNative();
					gSetEnemy(npc, p);
					GVM_EndNative(old);
				}
				break;
			}
			const float fight = g->engage > 0.0f ? g->engage : 350.0f;
			if (nearest && d2 < fight * fight) {
				Holo_Pace(h, 0);
				if (gSetEnemy) {
					void* old = GVM_BeginNative();
					gSetEnemy(npc, nearest);
					GVM_EndNative(old);
				}
				break;
			}
			const float dist = Distance(at, p->playerState->origin);
			if (dist > HT_FOLLOW_GAP) {
				Holo_Pace(h, dist > HT_FOLLOW_RUN ? 2 : 1);
				Holo_MoveTo(npc, p->playerState->origin);
				h->wpGoalAt = svs.time;
			} else {
				Holo_Pace(h, 0);
				Holo_MoveTo(npc, at); // close enough: stop here
			}
			break;
		}
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

// A spot inside an area (its circle, from 64 below its floor to its height).
static qboolean Holo_PointInArea(const htArea_t* a, const vec3_t o)
{
	const float dx = o[0] - a->org[0], dy = o[1] - a->org[1];
	return (dx * dx + dy * dy <= a->radius * a->radius && o[2] >= a->org[2] - 64.0f && o[2] <= a->org[2] + a->height) ? qtrue : qfalse;
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
		case HT_WHEN_USE:
			// Whoever's just finished using it (Holo_UseThink).
			if (t->useDoneBy > 0 && t->useDoneBy <= sv_maxclients->integer) {
				who = &svs.clients[t->useDoneBy - 1];
				go = qtrue;
			}
			t->useDoneBy = 0;
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
		case HT_WHEN_GROUP_IN_AREA: {
			// At least N (count, 1 if not set) of a group's NPCs inside an area.
			int in = 0;
			for (int k = 0; k < HT_MAX_NPCS && t->ref >= 0 && t->ref2 >= 0; k++) {
				const htNpc_t* h = &gHolo.npcs[k];
				if (h->group == t->ref2 && Holo_NpcUp(h)) {
					in += Holo_PointInArea(&gHolo.areas[t->ref], SV_GentityNum(h->ent)->r.currentOrigin);
				}
			}
			level = (in >= Q_max(1, t->count)) ? qtrue : qfalse;
			edge = qtrue;
			break;
		}
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

// Players who've just spawned, on a side whose respawn a scenario moved:
// to that point (spread round it) or the route's points in turn.
static void Holo_RespawnFrame(void)
{
	for (int c = 0; c < sv_maxclients->integer; c++) {
		const qboolean up = Holo_PlayerIn(c);
		const qboolean fresh = (up && !gHolo.respawnSeen[c]) ? qtrue : qfalse;
		gHolo.respawnSeen[c] = up;
		if (!fresh) {
			continue;
		}
		client_t* cl = &svs.clients[c];
		const int t = cl->gentity->playerState->persistant[PERS_TEAM];
		if (t != TEAM_RED && t != TEAM_BLUE) {
			continue;
		}
		const int k = gHolo.respawnTurn[t]++;
		if (gHolo.respawnPt[t] >= 0) {
			const htPoint_t* p = &gHolo.points[gHolo.respawnPt[t]];
			Holo_Teleport(cl, p->org, p->yaw, k % 8);
		} else if (Holo_RouteOk(gHolo.respawnRt[t])) {
			const htRoute_t* r = &gHolo.routes[gHolo.respawnRt[t]];
			const float* at = r->pts[k % r->count];
			const float* nxt = r->pts[(k + 1) % r->count];
			Holo_Teleport(cl, at, RAD2DEG(atan2f(nxt[1] - at[1], nxt[0] - at[0])), (k / r->count) % 8);
		}
	}
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
	if (!gHoloBackground && svs.time - gHolo.startedAt > gHolo.timeLimit * 1000) {
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
	Holo_RespawnFrame();
	Holo_EffectsFrame();
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
	Holo_PeacefulFrame();
}

// Loads a scenario file and starts it ("started", or "restarted" by
// !ht restart). qfalse, told to cl, if it won't load.
// The MBII mode (g_Authenticity) a scenario plays in: -1 = whatever the
// server's in. Scenarios that don't say are Full Authentic.
static int Holo_WantedMode(const cJSON* root)
{
	const char* m = HtStr(root, "mode", "fa");
	return !Q_stricmp(m, "keep") ? -1 : !Q_stricmp(m, "open") ? 0 : !Q_stricmp(m, "semi") ? 1 :
		!Q_stricmp(m, "legends") ? 4 : 2;
}

static const char* Holo_ModeName(int mode)
{
	static const char* names[] = { "Open", "Semi-Authentic", "Full Authentic", "Duel", "Legends" };
	return (mode >= 0 && mode < (int)ARRAY_LEN(names)) ? names[mode] : "?";
}

// A scenario waiting for the map to come back in its mode, then started.
static struct {
	qboolean waiting;
	char file[64], label[64], verb[16], who[MAX_NAME_LENGTH];
	int client;            // who asked (-1 = rcon) - still them if the name matches
	int mode;
	int readyAt;           // svs.time to start it (set once the map's back)
	int giveUpAt;          // svs.time to drop it if the map never comes back
} gHoloPending;

static qboolean Holo_Start(client_t* cl, const char* file, const char* label, const char* verb, const char* by = NULL, qboolean extendRound = qtrue);

// Plays a scenario - first reloading the map in its mode if the server's in
// another one (everyone back to class select), then starting it.
// A Legends scenario can bring its own teams: any of the game's team
// configs, by MBII's g_siegeTeam1/2 - every player has them, nothing to
// download. (MBII only goes by them in Legends - its default there is the
// Legends sides, LEG_Good / LEG_Evil.) They take on a map load, so !ht play reloads the map with them;
// the server's own come back when the map changes (SV_HoloMapChange), so
// round restarts on the same map keep them.
static qboolean gHoloTeamsSet = qfalse;   // g_siegeTeam1/2 are a scenario's
static char gHoloTeamsWas[2][64];         // ...and what they were before
static char gHoloTeamsMap[64];            // ...set for this map

// What g_siegeTeam1/2 should be for a scenario's teams: its own, or (""
// - the map's) whatever the server had before any scenario's - which needn't
// be "none" (MBII's default is the Legends sides, LEG_Good / LEG_Evil).
static void Holo_WantedTeams(const char* team1, const char* team2, char out[2][64])
{
	const char* want[2] = { team1, team2 };
	for (int i = 0; i < 2; i++) {
		Q_strncpyz(out[i], want[i][0] ? want[i] : gHoloTeamsSet ? gHoloTeamsWas[i] :
			Cvar_VariableString(i ? "g_siegeTeam2" : "g_siegeTeam1"), sizeof(out[i]));
	}
}

static void Holo_SetTeams(char teams[2][64])
{
	if (!gHoloTeamsSet) {
		Q_strncpyz(gHoloTeamsWas[0], Cvar_VariableString("g_siegeTeam1"), sizeof(gHoloTeamsWas[0]));
		Q_strncpyz(gHoloTeamsWas[1], Cvar_VariableString("g_siegeTeam2"), sizeof(gHoloTeamsWas[1]));
		gHoloTeamsSet = qtrue;
	}
	Q_strncpyz(gHoloTeamsMap, Cvar_VariableString("mapname"), sizeof(gHoloTeamsMap));
	Cvar_Set("g_siegeTeam1", teams[0]);
	Cvar_Set("g_siegeTeam2", teams[1]);
	Com_Printf("Holotable: teams %s / %s (the server's own: %s / %s)\n", teams[0], teams[1], gHoloTeamsWas[0], gHoloTeamsWas[1]);
	if (!Q_stricmp(teams[0], gHoloTeamsWas[0]) && !Q_stricmp(teams[1], gHoloTeamsWas[1])) {
		gHoloTeamsSet = qfalse; // back to the server's own
	}
}

// SV_SpawnServer: a map change puts the server's own teams back.
void SV_HoloMapChange(const char* map)
{
	if (!gHoloTeamsSet || !Q_stricmp(map, gHoloTeamsMap)) {
		return;
	}
	Cvar_Set("g_siegeTeam1", gHoloTeamsWas[0]);
	Cvar_Set("g_siegeTeam2", gHoloTeamsWas[1]);
	gHoloTeamsSet = qfalse;
	Com_Printf("Holotable: new map - the server's own teams back (%s / %s)\n", Cvar_VariableString("g_siegeTeam1"), Cvar_VariableString("g_siegeTeam2"));
}

static void Holo_Play(client_t* cl, const char* file, const char* label, const char* verb)
{
	Holo_PutAwayBackground();
	cJSON* root = Holo_ReadFile(file);
	const int mode = root ? Holo_WantedMode(root) : -1;
	char team1[64] = "", team2[64] = "";
	if (root && mode == 4) {
		Q_strncpyz(team1, HtStr(root, "team1", ""), sizeof(team1));
		Q_strncpyz(team2, HtStr(root, "team2", ""), sizeof(team2));
	}
	cJSON_Delete(root);
	const int now = Cvar_VariableIntegerValue("g_Authenticity");
	char teams[2][64];
	Holo_WantedTeams(team1, team2, teams);
	const qboolean teamsChange = (mode == 4 && (Q_stricmp(teams[0], Cvar_VariableString("g_siegeTeam1"))
		|| Q_stricmp(teams[1], Cvar_VariableString("g_siegeTeam2")))) ? qtrue : qfalse;
	if ((mode < 0 || mode == now) && !teamsChange) {
		Holo_Start(cl, file, label, verb);
		return;
	}
	if (teamsChange) {
		Holo_SetTeams(teams);
	}
	memset(&gHoloPending, 0, sizeof(gHoloPending));
	gHoloPending.waiting = qtrue;
	Q_strncpyz(gHoloPending.file, file, sizeof(gHoloPending.file));
	Q_strncpyz(gHoloPending.label, label, sizeof(gHoloPending.label));
	Q_strncpyz(gHoloPending.verb, verb, sizeof(gHoloPending.verb));
	Q_strncpyz(gHoloPending.who, cl ? cl->name : "", sizeof(gHoloPending.who));
	gHoloPending.client = cl ? (int)(cl - svs.clients) : -1;
	gHoloPending.mode = mode;
	gHoloPending.giveUpAt = svs.time + 90000;
	if (mode != now) {
		SV_SendServerCommand(NULL, "chat \"^5[Holotable] ^5%s^7 plays in ^3%s^7%s - reloading the map now, then it starts. Pick your class again.\"\n",
			label, Holo_ModeName(mode), teamsChange ? " with its own teams" : "");
		Com_Printf("Holotable: switching to mode %d (%s) for %s\n", mode, Holo_ModeName(mode), file);
		Cbuf_AddText(va("mbmode %d %s\n", mode, Cvar_VariableString("mapname")));
	} else {
		SV_SendServerCommand(NULL, "chat \"^5[Holotable] ^5%s^7 brings its own teams - reloading the map with them now, then it starts. Pick your class again.\"\n", label);
		Com_Printf("Holotable: reloading %s with the teams of %s\n", Cvar_VariableString("mapname"), file);
		Cbuf_AddText(va("map %s\n", Cvar_VariableString("mapname")));
	}
}

// Each frame: start a waiting scenario a few seconds after its map's back
// (players reconnecting), or give up on it.
static void Holo_PendingFrame(void)
{
	if (!gHoloPending.waiting) {
		return;
	}
	if (!gHoloPending.readyAt) {
		if (svs.time >= gHoloPending.giveUpAt) {
			gHoloPending.waiting = qfalse;
			Com_Printf("Holotable: the map never came back for %s - not started\n", gHoloPending.file);
		}
		return;
	}
	if (svs.time < gHoloPending.readyAt) {
		return;
	}
	gHoloPending.waiting = qfalse;
	client_t* cl = NULL;
	if (gHoloPending.client >= 0 && gHoloPending.client < sv_maxclients->integer) {
		client_t* c = &svs.clients[gHoloPending.client];
		if (c->state >= CS_CONNECTED && !Q_stricmp(c->name, gHoloPending.who)) {
			cl = c;
		}
	}
	if (Cvar_VariableIntegerValue("g_Authenticity") != gHoloPending.mode) {
		SV_SendServerCommand(NULL, "chat \"^5[Holotable]^7 Couldn't switch to %s - starting %s as it is.\"\n",
			Holo_ModeName(gHoloPending.mode), gHoloPending.label);
	}
	Holo_PutAwayBackground();
	if (!gHoloActive) {
		Holo_Start(cl, gHoloPending.file, gHoloPending.label, gHoloPending.verb);
	}
}

// Holotable saves scenarios' own NPC types into holotable.npc, but MBII only
// reads NPC files as a map or round loads - so a type saved since couldn't
// be spawned ("Couldn't spawn NPC ..."). When the file's changed since they
// were read, MBII reads them all again (NPC_LoadParms only refills its type
// buffer; NPCs already about keep what they have).
static time_t gHoloNpcFileTime = 0;

static time_t Holo_NpcFileTime(void)
{
	struct stat st;
	const char* path = va("%s/%s/ext_data/NPCs/holotable.npc", Cvar_VariableString("fs_basepath"), Cvar_VariableString("fs_game"));
	return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static void Holo_ReloadNpcTypes(void)
{
	const time_t now = Holo_NpcFileTime();
	if (now == gHoloNpcFileTime || !gNPCLoadParms) {
		return;
	}
	gHoloNpcFileTime = now;
	void* old = GVM_BeginNative();
	gNPCLoadParms();
	GVM_EndNative(old);
	Com_Printf("Holotable: holotable.npc has changed - NPC types read again\n");
}

static qboolean Holo_Start(client_t* cl, const char* file, const char* label, const char* verb, const char* by, qboolean extendRound)
{
	char err[128] = "";
	Holo_ReloadNpcTypes();
	if (!Holo_Load(file, err, sizeof(err))) {
		Holo_Reply(cl, va("^1Can't run %s^7: %s", label, err));
		return qfalse;
	}
	gHoloActive = qtrue;
	gHoloBackground = gHoloStartingBackground;
	const qboolean quiet = gHoloBackground;
	gHolo.startedAt = svs.time;
	gHolo.nextSpawn = svs.time + 1500; // the regulars clear out first
	for (int i = 0; i < gHolo.numTriggers; i++) {
		gHolo.triggers[i].nextAt = svs.time + (int)(gHolo.triggers[i].seconds * 1000.0f);
	}
	for (int c = 0; c < sv_maxclients->integer; c++) {
		gHolo.playerUp[c] = Holo_PlayerIn(c);
		gHolo.respawnSeen[c] = gHolo.playerUp[c]; // only spawns from now on are moved
	}
	for (int t = 0; t < 3; t++) {
		gHolo.respawnPt[t] = gHolo.respawnRt[t] = -1; // the map's own spawns
	}
	for (int gi = 0; gi < gHolo.numGroups; gi++) {
		if (gHolo.groups[gi].spawnAtStart) {
			Holo_QueueGroup(gi, -1, -1);
		}
	}
	Holo_PlaceAll();
	Holo_ReadTeamNames();
	if (gHolo.joinTeam) {
		// One side only: MBII's team balance would refuse to stack it.
		Q_strncpyz(gHolo.balanceWas, Cvar_VariableString("g_balance"), sizeof(gHolo.balanceWas));
		Cvar_Set("g_balance", "0");
		gHolo.balanceChanged = qtrue;
	}
	// The round clock runs on at least as long as the scenario can (not for
	// one that came with the round: it lives within it).
	if (extendRound && Holo_ShiftRound(gHolo.timeLimit * 1000)) {
		gHolo.roundExtendMs = gHolo.timeLimit * 1000;
	}
	if (quiet) {
		Com_Printf("Holotable: background %s (%s)\n", gHolo.name, gHolo.file);
		return qtrue; // the background: no announcements
	}
	SV_SendServerCommand(NULL, "chat \"^5[Holotable] ^7%s ^7%s ^5%s^7!\"\n", by ? by : cl ? cl->name : "An admin", verb, gHolo.name);
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
	if (!Q_stricmp(a1, "stop") && gHoloPending.waiting && !gHoloActive && Holo_CanRun(cl)) {
		gHoloPending.waiting = qfalse;
		Holo_Reply(cl, va("Called off ^5%s^7 - it won't start after the reload.", gHoloPending.label));
		return qtrue;
	}
	if (!Q_stricmp(a1, "stop")) {
		if (!Holo_CanRun(cl)) {
			Holo_Reply(cl, "Only admins and scenario runners can stop a scenario.");
		} else if (gHoloActive && gHoloBackground) {
			Holo_End(cl ? va("stopped by %s", cl->name) : "stopped");
			gHoloBgOff = qtrue;
			Holo_Reply(cl, "Background stopped - it's back next round (or map).");
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
		if (gHoloBackground) {
			// The background: read again from its file in a moment, still quietly.
			Holo_End("reloading");
			gHoloBgNextTry = svs.time;
			Holo_Reply(cl, "Background reloading.");
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
		Holo_Play(cl, file, label, "restarted");
		return qtrue;
	}

	Holo_RefreshList();
	const char* map = Cvar_VariableString("mapname");
	if (!a1[0] || !Q_stricmp(a1, "page") || !Q_stricmp(a1, "list")) {
		// A page at a time: chat only shows a handful of lines. Numbered
		// across the pages, so "!ht <n> play" goes by the same numbers - and
		// all of them in the console too.
		const int perPage = 5;
		const int pages = Q_max(1, (gHoloListCount + perPage - 1) / perPage);
		const int page = Q_max(1, Q_min(pages, a2[0] ? atoi(a2) : 1));
		if (gHoloActive && page == 1) {
			Holo_Reply(cl, va("%s: ^5%s^7 - ^5!ht stop^7 ends it, ^5!ht restart^7 reloads it.", gHoloBackground ? "Background" : "Running", gHolo.name));
		}
		if (!gHoloListCount) {
			Holo_Reply(cl, va("No Holotable scenarios for ^3%s^7 yet.", map));
			return qtrue;
		}
		Holo_Reply(cl, va("Scenarios for ^3%s^7 (^5!ht <n> play^7)%s:", map, pages > 1 ? va(" - page %d of %d", page, pages) : ""));
		for (int i = (page - 1) * perPage; i < gHoloListCount && i < page * perPage; i++) {
			Holo_Reply(cl, va("^5%d^7. %s%s%s", i + 1, gHoloList[i].name, gHoloList[i].desc[0] ? " ^9- " : "", gHoloList[i].desc));
		}
		if (page < pages) {
			Holo_Reply(cl, va("^5!ht page %d^7 for more - or open your console (^5~^7) for them all.", page + 1));
		}
		if (cl) {
			SV_SendServerCommand(cl, "print \"Holotable scenarios for %s (!ht <n> play):\n\"", map);
			for (int i = 0; i < gHoloListCount; i++) {
				SV_SendServerCommand(cl, "print \"%d. %s%s%s\n\"", i + 1, gHoloList[i].name, gHoloList[i].desc[0] ? " - " : "", gHoloList[i].desc);
			}
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
	Holo_PutAwayBackground();
	if (gHoloActive || gHoloPending.waiting) {
		Holo_Reply(cl, gHoloActive ? "A scenario's already running - ^5!ht stop^7 first." :
			"A scenario's about to start - the map's reloading for it.");
		return qtrue;
	}
	Holo_Play(cl, gHoloList[pick].file, gHoloList[pick].name, "started");
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

// A player holding use at a "use" trigger's spot: their progress (bar and
// sound as it fills), and the trigger flagged once it's full. Letting go
// or walking away starts it over.
static void Holo_UseThink(client_t* cl, const usercmd_t* cmd, int c)
{
	static int lastCmdTime[MAX_CLIENTS];
	const int dt = Q_max(0, Q_min(250, cmd->serverTime - lastCmdTime[c]));
	lastCmdTime[c] = cmd->serverTime;
	const qboolean holding = (cmd->buttons & BUTTON_USE) ? qtrue : qfalse;
	const qboolean up = Holo_PlayerIn(c);
	const playerState_t* ps = up ? cl->gentity->playerState : NULL;
	for (int i = 0; i < gHolo.numTriggers; i++) {
		htTrigger_t* t = &gHolo.triggers[i];
		if (t->when != HT_WHEN_USE) {
			continue;
		}
		const qboolean pressedBefore = t->usePressed[c];
		t->usePressed[c] = holding;
		qboolean here = (up && !t->off && !(t->fired && !t->repeat) && !t->useDoneBy &&
			!(t->repeat && t->lastFiredAt && svs.time - t->lastFiredAt < t->cooldownMs) &&
			(!t->useTeam || ps->persistant[PERS_TEAM] == t->useTeam)) ? qtrue : qfalse;
		if (here && t->usePt >= 0) {
			const float* p = gHolo.points[t->usePt].org;
			here = (Distance(p, ps->origin) <= t->useRadius && fabsf(ps->origin[2] - p[2]) < 96.0f) ? qtrue : qfalse;
		} else if (here && t->useArea >= 0) {
			here = Holo_InArea(&gHolo.areas[t->useArea], c);
		} else {
			here = qfalse;
		}
		if (!here || !holding) {
			if (t->useHeld[c] > 0 && t->useBar) {
				SV_SendServerCommand(cl, "cp \" \"\n"); // their bar away
			}
			t->useHeld[c] = 0;
			t->useSoundAt[c] = 0;
			continue;
		}
		if (!t->holdMs) {
			// Just press it: on the press, not while it's held down.
			if (!pressedBefore) {
				t->useDoneBy = c + 1;
			}
			continue;
		}
		t->useHeld[c] += dt;
		if (t->useSound[0] && gSoundAtLoc && gSoundIndex && svs.time >= t->useSoundAt[c]) {
			t->useSoundAt[c] = svs.time + t->useSoundEveryMs;
			vec3_t at;
			VectorCopy(ps->origin, at);
			void* old = GVM_BeginNative();
			gSoundAtLoc(at, 0 /* CHAN_AUTO */, gSoundIndex(t->useSound));
			GVM_EndNative(old);
		}
		const int pct = Q_min(100, t->useHeld[c] * 100 / t->holdMs);
		if (t->useHeld[c] >= t->holdMs) {
			t->useDoneBy = c + 1;
			t->useHeld[c] = 0;
			t->useSoundAt[c] = 0;
			if (t->useBar) {
				SV_SendServerCommand(cl, "cp \"%s%s^2Done\"\n", t->useLabel, t->useLabel[0] ? "\n" : "");
			}
		} else if (t->useBar && svs.time - t->useShownAt[c] >= 150) {
			t->useShownAt[c] = svs.time;
			char bar[64];
			const int full = pct / 5;
			int n = 0;
			n += Com_sprintf(bar + n, sizeof(bar) - n, "^2");
			for (int k = 0; k < 20 && n < (int)sizeof(bar) - 4; k++) {
				if (k == full) {
					n += Com_sprintf(bar + n, sizeof(bar) - n, "^9");
				}
				bar[n++] = '|';
				bar[n] = 0;
			}
			SV_SendServerCommand(cl, "cp \"%s%s%s ^7%d%%\"\n", t->useLabel, t->useLabel[0] ? "\n" : "", bar, pct);
		}
		break; // one thing used at a time
	}
}

void SV_HoloClientThink(client_t* cl, usercmd_t* cmd)
{
	Holo_SpeedThink(cl);
	{
		const int uc = cl - svs.clients;
		if (gHoloActive && uc >= 0 && uc < MAX_CLIENTS && svs.time >= gHolo.frozenUntil[uc]) {
			Holo_UseThink(cl, cmd, uc);
		}
	}
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

// --- Holotable auto-play ----------------------------------------------------
//
// Scenarios for the map that's on can start without anyone typing !ht, on
// any server with g_holotable. g_holotableAuto (set by MBIIEZ's Holotable
// plugin):
//   1 - a random one g_holotableAutoMinutes after the last one ended (or the
//       server started), once g_holotableAutoPlayers have been in for 2
//       minutes - the cantina's bar fights;
//   2 - one as every round begins, picked at random if there are several.
// They come from holotable_auto.txt in the instance's own game folder
// (fs_homepath/fs_game), one scenario id a line, kept by the plugin; without
// the file, any for the map can. They play in the mode the server's in (no
// map reload, unlike "!ht <n> play"), and one started for a round doesn't
// stretch the round clock.
//
// g_holotableAutoRestart: a scenario running when the round restarts (which
// clears every NPC) starts again in the new round, however it started.

static int gHoloAutoNext = 0;          // svs.time the next timed one is due (0 = not counting yet)
static int gHoloRoundAt = 0;           // svs.time a new round was set up (0 = nothing waiting on it)
static char gHoloRoundFile[64];        // the scenario to start again in the new round ("" = none)

static int Holo_AutoMode(void)
{
	return (Holo_Enabled() && g_holotableAuto) ? g_holotableAuto->integer : 0;
}

static int Holo_AutoPlayersNeeded(void)
{
	return Q_max(1, g_holotableAutoPlayers ? g_holotableAutoPlayers->integer : 2);
}

// A scenario file ("clone_ambush.json") auto-play may pick.
static qboolean Holo_AutoAllowed(const char* file)
{
	char id[64], line[128], word[128];
	Q_strncpyz(id, file, sizeof(id));
	char* dot = strrchr(id, '.');
	if (dot && !Q_stricmp(dot, ".json")) {
		*dot = '\0';
	}
	FILE* f = fopen(va("%s/%s/holotable_auto.txt", Cvar_VariableString("fs_homepath"), Cvar_VariableString("fs_game")), "r");
	if (!f) {
		return qtrue;
	}
	qboolean ok = qfalse;
	while (!ok && fgets(line, sizeof(line), f)) {
		word[0] = '\0';
		sscanf(line, "%127s", word);
		ok = (word[0] && word[0] != '#' && !Q_stricmp(word, id)) ? qtrue : qfalse;
	}
	fclose(f);
	return ok;
}

// Starts a random allowed scenario for the map that's on.
static qboolean Holo_AutoStart(qboolean extendRound)
{
	int list[HT_MAX_LIST], n = 0;
	Holo_RefreshList();
	for (int i = 0; i < gHoloListCount; i++) {
		if (Holo_AutoAllowed(gHoloList[i].file)) {
			list[n++] = i;
		}
	}
	if (!n) {
		return qfalse;
	}
	const int pick = list[Q_irand(0, n - 1)];
	char file[64], label[64];
	Q_strncpyz(file, gHoloList[pick].file, sizeof(file));
	Q_strncpyz(label, gHoloList[pick].name, sizeof(label));
	Holo_PutAwayBackground();
	if (!Holo_Start(NULL, file, label, "starts", "The Holotable", extendRound)) {
		return qfalse;
	}
	SV_SendServerCommand(NULL, "cp \"^5HOLOTABLE\n^7%s\"\n", label);
	return qtrue;
}

// The timer: counts from when the last scenario ended (or the server
// started), and waits until enough players have been in for 2 minutes - so
// it doesn't go off in someone's face the moment they join.
static void Holo_AutoTimerFrame(void)
{
	static int enoughSince = 0;
	if (Holo_AutoMode() != 1 || !g_holotableAutoMinutes || g_holotableAutoMinutes->integer <= 0
		|| (gHoloActive && !gHoloBackground) || gHoloPending.waiting || gHoloRoundAt) {
		gHoloAutoNext = 0;
		return;
	}
	const int interval = 60000 * g_holotableAutoMinutes->integer;
	if (!gHoloAutoNext || gHoloAutoNext - svs.time > interval) {
		gHoloAutoNext = svs.time + interval;
	}
	if (Holo_Players() < Holo_AutoPlayersNeeded()) {
		enoughSince = 0;
		return;
	}
	if (!enoughSince) {
		enoughSince = svs.time ? svs.time : 1;
	}
	if (svs.time < gHoloAutoNext || svs.time - enoughSince < 120000) {
		return;
	}
	if (!Holo_AutoStart(qtrue)) {
		gHoloAutoNext = svs.time + interval; // none for this map (or it won't load): next time round
	}
}

// A new round (or map): once it's under way, start the scenario that was
// running again, or - every round - a new one.
static void Holo_AutoRoundFrame(void)
{
	if (!gHoloRoundAt) {
		return;
	}
	const int since = svs.time - gHoloRoundAt;
	const qboolean begun = (!gSiegeRoundBegun || *gSiegeRoundBegun) ? qtrue : qfalse;
	if (since < 5000 || (!begun && since < 60000)) {
		return;
	}
	gHoloRoundAt = 0;
	char file[64];
	Q_strncpyz(file, gHoloRoundFile, sizeof(file));
	gHoloRoundFile[0] = '\0';
	if (!Holo_Enabled() || gHoloActive || gHoloPending.waiting || Holo_Players() < 1) {
		return;
	}
	const int mode = Holo_AutoMode();
	if (file[0]) {
		// Only if it's still for this map (the round may have come with a new one).
		cJSON* root = Holo_ReadFile(file);
		char label[64] = "";
		qboolean sameMap = qfalse;
		if (root) {
			sameMap = !Q_stricmp(HtStr(root, "map", ""), Cvar_VariableString("mapname")) ? qtrue : qfalse;
			Holo_Clean(label, HtStr(root, "name", file), sizeof(label));
			cJSON_Delete(root);
		}
		if (sameMap && Holo_Start(NULL, file, label, "restarts", "New round - the Holotable", mode != 2 ? qtrue : qfalse)) {
			return;
		}
	}
	if (mode == 2 && Holo_Players() >= Holo_AutoPlayersNeeded()) {
		Holo_AutoStart(qfalse);
	}
}

// SV_SocialGameInit, before a running scenario is cleared away.
static void Holo_AutoGameInit(void)
{
	gHoloRoundFile[0] = '\0';
	if (gHoloActive && !gHoloBackground && g_holotableAutoRestart && g_holotableAutoRestart->integer) {
		Q_strncpyz(gHoloRoundFile, gHolo.file, sizeof(gHoloRoundFile));
	}
	gHoloRoundAt = svs.time ? svs.time : 1;
	gHoloBackground = qfalse; // cleared with everything else
	gHoloBgOff = qfalse;
	gHoloBgNextTry = 0;
}

// --- Holotable backgrounds ---------------------------------------------------
//
// A map can have a background scenario: holotable_auto.txt "background <id>"
// lines (kept by MBIIEZ's Holotable plugin), the one for the map that's on.
// It's an ordinary scenario - typically peaceful groups (attacks nobody) on
// routes and spots, the cantina's bartender and customers - that plays
// quietly whenever no other one does: once the round's under way, a few
// seconds after another scenario ends, and again if it ever ends itself. Its
// time limit doesn't apply. Any other scenario puts it away.

// The background scenario for the map that's on, if any.
static qboolean Holo_BackgroundFile(char* file, size_t fileSize, char* label, size_t labelSize)
{
	FILE* f = fopen(va("%s/%s/holotable_auto.txt", Cvar_VariableString("fs_homepath"), Cvar_VariableString("fs_game")), "r");
	if (!f) {
		return qfalse;
	}
	const char* map = Cvar_VariableString("mapname");
	qboolean found = qfalse;
	char line[160], id[64];
	while (!found && fgets(line, sizeof(line), f)) {
		if (sscanf(line, "background %63s", id) != 1) {
			continue;
		}
		cJSON* root = Holo_ReadFile(va("%s.json", id));
		if (root && !Q_stricmp(HtStr(root, "map", ""), map)) {
			Com_sprintf(file, fileSize, "%s.json", id);
			Holo_Clean(label, HtStr(root, "name", id), labelSize);
			found = qtrue;
		}
		cJSON_Delete(root);
	}
	fclose(f);
	return found;
}

static qboolean Holo_HasBackground(void)
{
	char file[64], label[64];
	return (Holo_Enabled() && Holo_BackgroundFile(file, sizeof(file), label, sizeof(label))) ? qtrue : qfalse;
}

static void Holo_BackgroundFrame(void)
{
	if (!Holo_Enabled() || gHoloActive || gHoloPending.waiting || gHoloRoundAt || gHoloBgOff
		|| svs.time < gHoloBgNextTry || Holo_Players() < 1) {
		return;
	}
	gHoloBgNextTry = svs.time + 5000;
	char file[64], label[64];
	if (!Holo_BackgroundFile(file, sizeof(file), label, sizeof(label))) {
		return;
	}
	gHoloStartingBackground = qtrue;
	Holo_Start(NULL, file, label, "", NULL, qfalse);
	gHoloStartingBackground = qfalse;
}

static void Holo_AutoFrame(void)
{
	Holo_AutoRoundFrame();
	Holo_AutoTimerFrame();
	Holo_BackgroundFrame();
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
	if (gHoloPending.waiting && !gHoloPending.readyAt) {
		gHoloPending.readyAt = svs.time + 8000; // players reloading the map first
	}
	// A new round or map frees every entity, scenarios included (one may
	// start again once the round is under way: Holo_AutoRoundFrame).
	Holo_AutoGameInit();
	gHoloNpcFileTime = Holo_NpcFileTime(); // MBII has just read the NPC files
	gHoloSidesOk = -1;
	if (gHoloActive) {
		Holo_RestorePlayers(qtrue);
		gHoloActive = qfalse;
		for (int i = 0; i < HT_MAX_NPCS; i++) {
			gHolo.npcs[i].ent = -1;
		}
	}
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

	// A map change unloads the game module and loads it again, and the new
	// handle can match the old one while the module sits somewhere else - so
	// a symbol's address, not the handle, says whether to look them all up
	// again. (Missing that left these pointing into the old image: a crash
	// the first frame after changing to a big map like mb2_veh_boc.)
	void* probe = Sys_LoadFunction(dll, "vmMain");
	if (dll != gSocialDll || probe != gSocialProbe) {
		gSocialDll = dll;
		gSocialProbe = probe;
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
		gNPCLoadParms = (void (*)(void))Sys_LoadFunction(dll, "NPC_LoadParms");
		gFreeEntity = (void (*)(void*))Sys_LoadFunction(dll, "G_FreeEntity");
		gSetEnemy = (void (*)(void*, void*))Sys_LoadFunction(dll, "G_SetEnemy");
		gClearEnemy = (void (*)(void*))Sys_LoadFunction(dll, "G_ClearEnemy");
		gGlobalUse = (void (*)(void*, void*, void*))Sys_LoadFunction(dll, "GlobalUse");
		gGSpawn = (void* (*)(void))Sys_LoadFunction(dll, "G_Spawn");
		gModelIndex = (int (*)(const char*))Sys_LoadFunction(dll, "G_ModelIndex");
		gParseSpawnVars = (qboolean (*)(qboolean))Sys_LoadFunction(dll, "G_ParseSpawnVars");
		gSpawnFromSpawnVars = (void (*)(qboolean))Sys_LoadFunction(dll, "G_SpawnGEntityFromSpawnVars");
		gSetMoveGoal = (void (*)(void*, float*, int, int, int, void*))Sys_LoadFunction(dll, "NPC_SetMoveGoal");
		gSoundOnEnt = (void (*)(void*, int, const char*))Sys_LoadFunction(dll, "G_SoundOnEnt");
		gSaveNPCGlobals = (void (*)(void))Sys_LoadFunction(dll, "SaveNPCGlobals");
		gRestoreNPCGlobals = (void (*)(void))Sys_LoadFunction(dll, "RestoreNPCGlobals");
		gSetNPCGlobals = (void (*)(void*))Sys_LoadFunction(dll, "SetNPCGlobals");
		gNPCChangeWeapon = (void (*)(int))Sys_LoadFunction(dll, "NPC_ChangeWeapon");
		gGetIDForString = (int (*)(void*, const char*))Sys_LoadFunction(dll, "GetIDForString");
		gWPTable = Sys_LoadFunction(dll, "WPTable");
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

	if ((Social_Enabled() || gHoloActive) && gGDamage && !gHookAttempted) {
		Social_InstallHook(); // scenarios need it for their sides, social or not
	}
	if (Social_Enabled()) {
		Social_CheckDuels();
		Social_NpcFrame();
	}
	Holo_PendingFrame();
	Holo_AutoFrame();
	Holo_Frame(); // any server with g_holotable, not only social ones
	if (Social_AnytimeSpawnWanted() || (g_socialBots && g_socialBots->integer)) {
		Social_RescueStuckJoiners();
	}
}
