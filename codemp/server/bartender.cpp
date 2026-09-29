/*
===========================================================================
bartender.cpp — ask the Cantina's bartender anything, part of the economy

  !bartender <question>      ask (also !barkeep); costs g_bartenderCost

The engine only takes the question and the payment; the answer comes from
an AI model called by MBIIEZ's creditsystem plugin, which holds the API key
(so it never touches a cvar). The plugin collects questions over rcon with
"bartenderpoll" every second or so and answers each one with
"bartenderreply <id> <text>", or "bartenderreply <id> !fail" to give the
player their credits back. A question nobody answers within
BARTENDER_TIMEOUT_MS is refunded too, and if the plugin hasn't polled
lately (not running, or no key) nobody is charged in the first place.

g_bartenderCooldown limits how often one player can ask, and
g_bartenderDailyCap caps the questions this server sends a day, so the API
bill has a ceiling. g_bartenderPublic shows questions and answers to the
whole cantina (1) or only to whoever asked (0). Switched on with
g_economyBartenderEnable, which the plugin sets only when it has a key.
===========================================================================
*/

#include "server.h"
#include <time.h>

#define BARTENDER_MAX_PENDING    32
#define BARTENDER_TIMEOUT_MS     60000
#define BARTENDER_POLL_STALE_MS  10000
#define BARTENDER_QUESTION_MAX   160
#define BARTENDER_LINE_CHARS     140
#define BARTENDER_MAX_LINES      4

typedef enum { BT_FREE, BT_QUEUED, BT_SENT } btState_t;

typedef struct {
	btState_t state;
	int       id;
	int       slot;
	char      handle[24];
	char      name[MAX_NETNAME];
	char      question[BARTENDER_QUESTION_MAX];
	int       cost;
	int       expires;
} btRequest_t;

static btRequest_t gBtRequests[BARTENDER_MAX_PENDING];
static int gBtNextId = 1;
static int gBtLastPoll = -BARTENDER_POLL_STALE_MS * 2;
static int gBtNextAsk[MAX_CLIENTS];
static int gBtDay = -1;                 // day of the year the count is for
static int gBtAskedToday = 0;

static void Bartender_Print(client_t* cl, const char* text)
{
	SV_SendServerCommand(cl, "chat \"^3[Bartender]^7 %s\"\n", text);
}

static int Bartender_Cost(void)
{
	return g_bartenderCost ? Q_max(0, g_bartenderCost->integer) : 0;
}

static qboolean Bartender_Public(void)
{
	return (!g_bartenderPublic || g_bartenderPublic->integer) ? qtrue : qfalse;
}

// Quotes would end the chat string early; take them out of anything a
// player or the AI wrote before it goes back out in a server command.
static void Bartender_Clean(char* s)
{
	for (; *s; s++) {
		if (*s == '"') {
			*s = '\'';
		} else if (*s == '\n' || *s == '\r' || *s == '\t') {
			*s = ' ';
		}
	}
}

static qboolean Bartender_StillHere(const btRequest_t* req, client_t** out)
{
	client_t* cl = &svs.clients[req->slot];
	if (cl->state == CS_ACTIVE && !Q_stricmp(cl->economyHandle, req->handle)) {
		*out = cl;
		return qtrue;
	}
	*out = NULL;
	return qfalse;
}

static void Bartender_Refund(btRequest_t* req, const char* why)
{
	client_t* cl;
	if (Bartender_StillHere(req, &cl)) {
		cl->economyCredits += req->cost;
		SV_EconomyPersistCredits(cl);
		Bartender_Print(cl, va("%s Your %d credits are back.", why, req->cost));
	} else if (req->cost > 0) {
		SV_EconomyAddCreditsToAccount(req->handle, req->cost);
	}
	memset(req, 0, sizeof(*req));
}

// The answer, split into chat-sized lines at spaces.
static void Bartender_Deliver(btRequest_t* req, const char* answer)
{
	client_t* cl;
	char text[BARTENDER_LINE_CHARS * BARTENDER_MAX_LINES];
	const char* p;
	int lines = 0;

	if (!Bartender_StillHere(req, &cl)) {
		memset(req, 0, sizeof(*req));   // they left, the answer goes nowhere
		return;
	}

	SV_SocialNpcGesture("bartender", "BOTH_TALK1");
	Com_sprintf(text, sizeof(text), "%s^7: %s", req->name, answer);
	Bartender_Clean(text);

	p = text;
	while (*p && lines < BARTENDER_MAX_LINES) {
		char line[BARTENDER_LINE_CHARS + 1];
		int len = (int)strlen(p);

		if (len > BARTENDER_LINE_CHARS) {
			len = BARTENDER_LINE_CHARS;
			while (len > BARTENDER_LINE_CHARS / 2 && p[len] != ' ') {
				len--;
			}
		}
		Q_strncpyz(line, p, len + 1);
		SV_SendServerCommand(Bartender_Public() ? NULL : cl, "chat \"^3[Bartender]^7 %s%s\"\n",
			lines ? "" : "@", line);
		p += len;
		while (*p == ' ') {
			p++;
		}
		lines++;
	}
	memset(req, 0, sizeof(*req));
}

qboolean SV_BartenderCommand(client_t* cl, const char* args)
{
	const int slot = cl - svs.clients;
	const int cost = Bartender_Cost();
	char question[BARTENDER_QUESTION_MAX];
	btRequest_t* req = NULL;

	if (!g_economyBartenderEnable || !g_economyBartenderEnable->integer) {
		Bartender_Print(cl, "There's no one behind the bar on this server.");
		return qtrue;
	}

	while (*args == ' ') {
		args++;
	}
	if (!*args) {
		Bartender_Print(cl, va("^5!bartender <question> ^7- ask the bartender anything: drinks, the cantina, "
			"gossip, advice. %d credits a question.", cost));
		return qtrue;
	}

	if (svs.time - gBtLastPoll > BARTENDER_POLL_STALE_MS) {
		Bartender_Print(cl, "The bartender's stepped out - try again later. (You weren't charged.)");
		return qtrue;
	}
	if (gBtNextAsk[slot] > svs.time) {
		Bartender_Print(cl, va("The bartender's still wiping a glass. Ask again in %ds.",
			(gBtNextAsk[slot] - svs.time + 999) / 1000));
		return qtrue;
	}
	for (int i = 0; i < BARTENDER_MAX_PENDING; i++) {
		if (gBtRequests[i].state != BT_FREE) {
			if (gBtRequests[i].slot == slot && !Q_stricmp(gBtRequests[i].handle, cl->economyHandle)) {
				Bartender_Print(cl, "Hold on, the bartender's still answering your last one.");
				return qtrue;
			}
		} else if (!req) {
			req = &gBtRequests[i];
		}
	}
	if (!req) {
		Bartender_Print(cl, "The bartender's swamped - try again in a moment. (You weren't charged.)");
		return qtrue;
	}

	{
		const time_t now = time(NULL);
		const struct tm* lt = localtime(&now);
		if (lt && lt->tm_yday != gBtDay) {
			gBtDay = lt->tm_yday;
			gBtAskedToday = 0;
		}
	}
	if (g_bartenderDailyCap && g_bartenderDailyCap->integer > 0 && gBtAskedToday >= g_bartenderDailyCap->integer) {
		Bartender_Print(cl, "The bartender's done talking for today. Come back tomorrow. (You weren't charged.)");
		return qtrue;
	}
	if (cl->economyCredits < cost) {
		Bartender_Print(cl, va("A question costs %d credits - you've got %d.", cost, cl->economyCredits));
		return qtrue;
	}

	Q_strncpyz(question, args, sizeof(question));
	Bartender_Clean(question);

	cl->economyCredits -= cost;
	SV_EconomyPersistCredits(cl);
	gBtAskedToday++;
	gBtNextAsk[slot] = svs.time + 1000 * (g_bartenderCooldown ? Q_max(0, g_bartenderCooldown->integer) : 0);

	req->state = BT_QUEUED;
	req->id = gBtNextId++;
	req->slot = slot;
	Q_strncpyz(req->handle, cl->economyHandle, sizeof(req->handle));
	Q_strncpyz(req->name, cl->name, sizeof(req->name));
	Bartender_Clean(req->name);
	Q_strncpyz(req->question, question, sizeof(req->question));
	req->cost = cost;
	req->expires = svs.time + BARTENDER_TIMEOUT_MS;

	if (Bartender_Public()) {
		SV_SendServerCommand(NULL, "chat \"^3[Bar] ^7%s ^7asks the bartender: ^5%s\"\n", req->name, question);
	} else {
		Bartender_Print(cl, va("You ask: ^5%s ^7(-%d credits)", question, cost));
	}
	return qtrue;
}

// rcon "bartenderpoll": hands the plugin every waiting question, one per
// line: BT <tab> id <tab> handle <tab> credits <tab> name <tab> question
void SV_BartenderPoll_f(void)
{
	gBtLastPoll = svs.time;
	for (int i = 0; i < BARTENDER_MAX_PENDING; i++) {
		btRequest_t* req = &gBtRequests[i];
		client_t* cl;

		if (req->state != BT_QUEUED) {
			continue;
		}
		req->state = BT_SENT;
		char name[MAX_NETNAME];
		Q_strncpyz(name, req->name, sizeof(name));
		Q_CleanStr(name);
		Com_Printf("BT\t%d\t%s\t%d\t%s\t%s\n", req->id, req->handle,
			Bartender_StillHere(req, &cl) ? cl->economyCredits : 0, name, req->question);
	}
}

// rcon "bartenderreply <id> <answer>", or "<id> !fail" to refund.
void SV_BartenderReply_f(void)
{
	if (Cmd_Argc() < 3) {
		Com_Printf("Usage: bartenderreply <id> <answer | !fail>\n");
		return;
	}
	const int id = atoi(Cmd_Argv(1));
	for (int i = 0; i < BARTENDER_MAX_PENDING; i++) {
		btRequest_t* req = &gBtRequests[i];
		if (req->state == BT_FREE || req->id != id) {
			continue;
		}
		if (!Q_stricmp(Cmd_Argv(2), "!fail")) {
			Bartender_Refund(req, "The bartender didn't catch that.");
		} else {
			Bartender_Deliver(req, Cmd_ArgsFrom(2));
		}
		return;
	}
}

void SV_BartenderFrame(void)
{
	for (int i = 0; i < BARTENDER_MAX_PENDING; i++) {
		btRequest_t* req = &gBtRequests[i];
		if (req->state != BT_FREE && req->expires <= svs.time) {
			Bartender_Refund(req, "The bartender got distracted.");
		}
	}
}
