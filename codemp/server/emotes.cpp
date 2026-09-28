/*
===========================================================================
emotes.cpp — chat emotes for social servers (!sit, !handsup, !emotes ...)

Plays one of MBII's own player animations on the player, through MBII's
exported G_SetAnim, with the animation looked up by name in MBII's exported
animTable - so MBII's own animation numbering (which isn't the engine's)
never matters. The "hold" emotes (sit, slump, hands up, cower, play dead)
are re-applied as they run out, and last until the player moves, jumps or
attacks; the others play once. Available on servers running g_socialMode.
===========================================================================
*/

#include "server.h"
#include "sv_gameapi.h"
#include "sys/sys_loadlib.h"

// MBII bg_public.h
#define EMOTE_SETANIM_TORSO   1
#define EMOTE_SETANIM_BOTH    3
#define EMOTE_ANIM_FLAGS      3       // SETANIM_FLAG_OVERRIDE | SETANIM_FLAG_HOLD
#define EMOTE_REAPPLY_MS      300     // re-apply a hold emote this close to running out
#define EMOTE_MAX_MS          600000

typedef struct {
	const char* cmd;
	const char* anim;       // MBII animation name
	int         parts;
	qboolean    hold;       // keep going until they move
	const char* desc;
} emote_t;

static const emote_t kEmotes[] = {
	{ "sit",       "BOTH_SIT1",             EMOTE_SETANIM_BOTH,  qtrue,  "sit down" },
	{ "slump",     "BOTH_SIT3",             EMOTE_SETANIM_BOTH,  qtrue,  "slump, elbows on knees" },
	{ "handsup",   "TORSO_SURRENDER_START", EMOTE_SETANIM_TORSO, qtrue,  "hands up" },
	{ "cower",     "BOTH_COWER1",           EMOTE_SETANIM_BOTH,  qtrue,  "cower" },
	{ "playdead",  "BOTH_DEAD1",            EMOTE_SETANIM_BOTH,  qtrue,  "play dead" },
	{ "nod",       "BOTH_HEADNOD",          EMOTE_SETANIM_BOTH,  qfalse, "nod" },
	{ "shakehead", "BOTH_HEADSHAKE",        EMOTE_SETANIM_BOTH,  qfalse, "shake your head" },
	{ "talk",      "BOTH_TALK1",            EMOTE_SETANIM_BOTH,  qfalse, "gesture while talking" },
};

typedef struct {
	const emote_t* emote;
	int anim;
	int spawnCount;
	int until;
} emoteState_t;

static emoteState_t gEmoteState[MAX_CLIENTS];

static void* gEmoteDll = NULL;
static const stringID_table_t* gAnimTable = NULL;
static void (*gSetAnim)(void* ent, int parts, int anim, int flags) = NULL;

static qboolean Emote_Resolve(void)
{
	void* dll = GVM_GetDllHandle();
	if (dll != gEmoteDll) {
		gEmoteDll = dll;
		gAnimTable = dll ? (const stringID_table_t*)Sys_LoadFunction(dll, "animTable") : NULL;
		gSetAnim = dll ? (void (*)(void*, int, int, int))Sys_LoadFunction(dll, "G_SetAnim") : NULL;
	}
	return (gAnimTable && gSetAnim) ? qtrue : qfalse;
}

static int Emote_AnimByName(const char* name)
{
	for (int i = 0; i < 4096 && gAnimTable[i].name; i++) {
		if (!Q_stricmp(gAnimTable[i].name, name)) {
			return gAnimTable[i].id;
		}
	}
	return -1;
}

static qboolean Emote_Enabled(void)
{
	return (g_socialMode && g_socialMode->integer) ? qtrue : qfalse;
}

static void Emote_Play(client_t* cl, const emoteState_t* st)
{
	void* old = GVM_BeginNative();
	gSetAnim(cl->gentity, st->emote->parts, st->anim, EMOTE_ANIM_FLAGS);
	GVM_EndNative(old);
}

static qboolean Emote_CanPlay(client_t* cl)
{
	const playerState_t* ps = cl->gentity->playerState;
	const int team = ps->persistant[PERS_TEAM];
	return ((team == TEAM_RED || team == TEAM_BLUE) && ps->stats[STAT_HEALTH] > 0 &&
		ps->groundEntityNum != ENTITYNUM_NONE) ? qtrue : qfalse;
}

// Handles "!<emote>" and "!emotes" in chat; qfalse for anything else.
qboolean SV_EmoteCommand(client_t* cl, const char* command)
{
	if (!Emote_Enabled()) {
		return qfalse;
	}

	if (!Q_stricmp(command, "emotes")) {
		char line[256] = "^5Emotes:^7";
		for (int i = 0; i < (int)ARRAY_LEN(kEmotes); i++) {
			Q_strcat(line, sizeof(line), va(" !%s", kEmotes[i].cmd));
		}
		SV_SendServerCommand(cl, "chat \"%s ^7- move to stop.\"\n", line);
		return qtrue;
	}

	const emote_t* e = NULL;
	for (int i = 0; i < (int)ARRAY_LEN(kEmotes); i++) {
		if (!Q_stricmp(command, kEmotes[i].cmd)) {
			e = &kEmotes[i];
			break;
		}
	}
	if (!e) {
		return qfalse;
	}

	if (!cl->gentity || !cl->gentity->playerState || !Emote_Resolve()) {
		return qtrue;
	}
	if (!Emote_CanPlay(cl)) {
		SV_SendServerCommand(cl, "chat \"^5[Emote]^7 You need to be alive and standing on something.\"\n");
		return qtrue;
	}
	const int anim = Emote_AnimByName(e->anim);
	if (anim < 0) {
		SV_SendServerCommand(cl, "chat \"^5[Emote]^7 That emote isn't available on this server.\"\n");
		return qtrue;
	}

	emoteState_t* st = &gEmoteState[cl - svs.clients];
	st->emote = e;
	st->anim = anim;
	st->spawnCount = cl->gentity->playerState->persistant[PERS_SPAWN_COUNT];
	st->until = e->hold ? svs.time + EMOTE_MAX_MS : 0;
	Emote_Play(cl, st);
	if (!e->hold) {
		st->emote = NULL; // one-shot: nothing to keep up
	}
	return qtrue;
}

void SV_EmotesFrame(void)
{
	for (int i = 0; i < sv_maxclients->integer; i++) {
		emoteState_t* st = &gEmoteState[i];
		if (!st->emote) {
			continue;
		}
		client_t* cl = &svs.clients[i];
		if (!Emote_Enabled() || cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState || !Emote_Resolve()) {
			st->emote = NULL;
			continue;
		}
		playerState_t* ps = cl->gentity->playerState;
		if (ps->persistant[PERS_SPAWN_COUNT] != st->spawnCount || !Emote_CanPlay(cl)) {
			st->emote = NULL; // died, respawned or fell - MBII has moved them on already
			continue;
		}

		const usercmd_t* cmd = &cl->lastUsercmd;
		if (cmd->forwardmove || cmd->rightmove || cmd->upmove ||
			(cmd->buttons & (BUTTON_ATTACK | BUTTON_ALT_ATTACK)) || svs.time >= st->until) {
			// Let MBII's movement animations take straight back over.
			ps->legsTimer = 0;
			ps->torsoTimer = 0;
			st->emote = NULL;
			continue;
		}

		const int left = (st->emote->parts == EMOTE_SETANIM_TORSO) ? ps->torsoTimer : Q_min(ps->torsoTimer, ps->legsTimer);
		if (left < EMOTE_REAPPLY_MS) {
			Emote_Play(cl, st);
		}
	}
}
