/*
===========================================================================
betting.cpp — betting on duels, part of the economy (!bet)

  !bet                       each fighter in each duel as a numbered option:
                             "1. Ricks vs Cody (Ricks to win) - 150 cr backing"
  !bet <number> <credits>    back that option
  !bet <fighter> <credits>   or back a fighter by (part of) their name

Duels are tracked straight from the players' own playerState -
duelInProgress and duelIndex, stock fields, the same ones social.cpp's
duel handling reads - so any duel counts however it was started. Bets are
only taken in the first g_betWindowSeconds of a duel (30), up to g_betMax
credits a duel (100, 0 = no limit), on one side only, and never on a duel
you're in. The stake is taken when you bet and goes into the duel's pot;
the winning side splits the whole pot in proportion to what each put in,
so no credits are made or lost overall (if nobody backed the loser,
winners just get their stake back). Paid into the account, so it counts if
you've left. A duel with no clear winner - a fighter leaving, say - or
with nobody on the winning side refunds everyone. Switched on with
g_economyBetEnable.
===========================================================================
*/

#include "server.h"

#define BET_MAX_DUELS   16
#define BET_MAX_BETS    128

typedef struct {
	qboolean active;
	int      id;                // shown on !bet; counts up per map
	int      fighter[2];        // client numbers
	char     handle[2][MAX_NAME_LENGTH]; // names at the start, for announcements
	int      spawnCount[2];
	int      started;           // svs.time
	qboolean seen;              // still duelling this frame
} betDuel_t;

typedef struct {
	qboolean active;
	int      duelId;
	int      side;              // 0 or 1: which fighter they backed
	int      bettor;            // client number
	char     account[24];       // economy handle, to pay out if they've gone
	int      amount;
} bet_t;

static betDuel_t gBetDuels[BET_MAX_DUELS];
static bet_t     gBets[BET_MAX_BETS];
static int       gBetNextDuelId = 1;

static void Bet_Print(client_t* cl, const char* text)
{
	SV_SendServerCommand(cl, "chat \"^6[Bet]^7 %s\"\n", text);
}

static qboolean Bet_Enabled(void)
{
	return (g_economyBetEnable && g_economyBetEnable->integer) ? qtrue : qfalse;
}

static int Bet_WindowMs(void)
{
	return Q_max(0, g_betWindowSeconds ? g_betWindowSeconds->integer : 30) * 1000;
}

// Credits backing one side of a duel.
static int Bet_Backing(int duelId, int side)
{
	int total = 0;
	for (int i = 0; i < BET_MAX_BETS; i++) {
		if (gBets[i].active && gBets[i].duelId == duelId && gBets[i].side == side) {
			total += gBets[i].amount;
		}
	}
	return total;
}

// This client's own playerState, if they're duelling: their opponent.
static int Bet_OpponentOf(int i)
{
	client_t* cl = &svs.clients[i];
	if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
		return -1;
	}
	const playerState_t* ps = cl->gentity->playerState;
	if (ps->clientNum != i || !ps->duelInProgress || ps->duelIndex < 0 || ps->duelIndex >= sv_maxclients->integer) {
		return -1;
	}
	return ps->duelIndex;
}

static betDuel_t* Bet_FindDuel(int a, int b)
{
	for (int d = 0; d < BET_MAX_DUELS; d++) {
		betDuel_t* duel = &gBetDuels[d];
		if (duel->active && ((duel->fighter[0] == a && duel->fighter[1] == b) || (duel->fighter[0] == b && duel->fighter[1] == a))) {
			return duel;
		}
	}
	return NULL;
}

static betDuel_t* Bet_DuelById(int id)
{
	for (int d = 0; d < BET_MAX_DUELS; d++) {
		if (gBetDuels[d].active && gBetDuels[d].id == id) {
			return &gBetDuels[d];
		}
	}
	return NULL;
}

static void Bet_Pay(bet_t* b, int amount)
{
	client_t* cl = &svs.clients[b->bettor];
	if (cl->state == CS_ACTIVE && !Q_stricmp(cl->economyHandle, b->account)) {
		cl->economyCredits += amount;
		SV_EconomyPersistCredits(cl);
	} else {
		SV_EconomyAddCreditsToAccount(b->account, amount);
	}
}

// winner: 0 or 1, or -1 to refund everyone. The winning side splits the
// whole pot by stake; nobody on the winning side refunds everyone.
static void Bet_Resolve(betDuel_t* duel, int winner)
{
	int winners = 0, losers = 0, paid = 0;
	const int pot = Bet_Backing(duel->id, 0) + Bet_Backing(duel->id, 1);
	const int winningSide = (winner >= 0) ? Bet_Backing(duel->id, winner) : 0;

	if (winningSide <= 0) {
		winner = -1;
	}

	for (int i = 0; i < BET_MAX_BETS; i++) {
		bet_t* b = &gBets[i];
		if (!b->active || b->duelId != duel->id) {
			continue;
		}
		client_t* cl = &svs.clients[b->bettor];
		const qboolean here = (cl->state == CS_ACTIVE && !Q_stricmp(cl->economyHandle, b->account)) ? qtrue : qfalse;

		if (winner < 0) {
			Bet_Pay(b, b->amount);
			if (here) {
				Bet_Print(cl, va("No winning bets on that duel - your %d credits are back.", b->amount));
			}
		} else if (b->side == winner) {
			const int payout = (int)((long long)b->amount * pot / winningSide);
			Bet_Pay(b, payout);
			winners++;
			paid += payout;
			if (here) {
				Bet_Print(cl, va("Your fighter won! ^2+%d ^7credits.", payout));
			}
		} else {
			losers++;
			if (here) {
				Bet_Print(cl, va("Your fighter lost - there go your %d credits.", b->amount));
			}
		}
		memset(b, 0, sizeof(*b));
	}

	if (winner >= 0 && (winners || losers)) {
		SV_SendServerCommand(NULL, "chat \"^6[Bet] ^7%s ^7beat %s^7! The ^2%d ^7credit pot goes to %d bettor%s (%d lost).\"\n",
			duel->handle[winner], duel->handle[1 - winner], pot, winners, winners == 1 ? "" : "s", losers);
	}
	memset(duel, 0, sizeof(*duel));
}

// Winner of a duel that just ended: the fighter still standing, in the same
// life, when the other one isn't. Anything else - someone left, or both
// somehow alive - has no clear winner.
static int Bet_Winner(const betDuel_t* duel)
{
	qboolean up[2];
	for (int s = 0; s < 2; s++) {
		client_t* cl = &svs.clients[duel->fighter[s]];
		if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
			return -1;
		}
		const playerState_t* ps = cl->gentity->playerState;
		up[s] = (ps->clientNum == duel->fighter[s] && ps->stats[STAT_HEALTH] > 0 &&
			ps->persistant[PERS_SPAWN_COUNT] == duel->spawnCount[s]) ? qtrue : qfalse;
	}
	if (up[0] == up[1]) {
		return -1;
	}
	return up[0] ? 0 : 1;
}

void SV_BetFrame(void)
{
	for (int d = 0; d < BET_MAX_DUELS; d++) {
		gBetDuels[d].seen = qfalse;
	}

	for (int i = 0; i < sv_maxclients->integer; i++) {
		const int opp = Bet_OpponentOf(i);
		if (opp < 0 || opp < i || Bet_OpponentOf(opp) != i) {
			continue; // each pair once, from its lower slot, and only when mutual
		}
		betDuel_t* duel = Bet_FindDuel(i, opp);
		if (!duel) {
			for (int d = 0; d < BET_MAX_DUELS; d++) {
				if (!gBetDuels[d].active) {
					duel = &gBetDuels[d];
					break;
				}
			}
			if (!duel) {
				continue;
			}
			memset(duel, 0, sizeof(*duel));
			duel->active = qtrue;
			duel->id = gBetNextDuelId++;
			duel->fighter[0] = i;
			duel->fighter[1] = opp;
			for (int s = 0; s < 2; s++) {
				client_t* f = &svs.clients[duel->fighter[s]];
				Q_strncpyz(duel->handle[s], f->name, sizeof(duel->handle[s]));
				duel->spawnCount[s] = f->gentity->playerState->persistant[PERS_SPAWN_COUNT];
			}
			duel->started = svs.time;
			if (Bet_Enabled() && Bet_WindowMs() > 0) {
				SV_SendServerCommand(NULL, "chat \"^6[Bet] ^7%s ^7vs %s ^7- place your bets! ^5!bet <fighter> <credits> ^7(%ds)\"\n",
					duel->handle[0], duel->handle[1], Bet_WindowMs() / 1000);
			}
		}
		duel->seen = qtrue;
	}

	for (int d = 0; d < BET_MAX_DUELS; d++) {
		betDuel_t* duel = &gBetDuels[d];
		if (duel->active && !duel->seen) {
			Bet_Resolve(duel, Bet_Winner(duel));
		}
	}
}

static void Bet_List(client_t* cl)
{
	int shown = 0;

	SV_EconomyMenuBegin(cl);
	SV_EconomyMenuAddLine(cl, "^6=== DUELS === ^7The winning side splits the whole pot.");
	for (int d = 0; d < BET_MAX_DUELS; d++) {
		const betDuel_t* duel = &gBetDuels[d];
		if (!duel->active) {
			continue;
		}
		const int left = Bet_WindowMs() - (svs.time - duel->started);
		const char* status = left > 0 ? va("^2open %ds", (left + 999) / 1000) : "^1closed";
		for (int s = 0; s < 2; s++) {
			SV_EconomyMenuAddLine(cl, va("^3%d^7. %s ^7vs %s ^7(^3%s ^7to win) - ^2%d ^7cr backing - %s",
				d * 2 + s + 1, duel->handle[0], duel->handle[1], duel->handle[s], Bet_Backing(duel->id, s), status));
		}
		shown++;
	}
	if (!shown) {
		SV_EconomyMenuAddLine(cl, "^7No duels going on right now.");
	}
	for (int i = 0; i < BET_MAX_BETS; i++) {
		const bet_t* b = &gBets[i];
		const betDuel_t* duel = b->active ? Bet_DuelById(b->duelId) : NULL;
		if (duel && !Q_stricmp(b->account, cl->economyHandle)) {
			SV_EconomyMenuAddLine(cl, va("^7Your bet: ^2%d ^7on %s", b->amount, duel->handle[b->side]));
		}
	}
	SV_EconomyMenuAddLine(cl, "^5!bet <number> <credits> ^7to back someone, e.g. ^5!bet 1 50");
	SV_EconomyMenuPump(cl);
}

qboolean SV_BetCommand(client_t* cl, const char* args)
{
	char who[64], amountStr[64];
	const int me = cl - svs.clients;

	if (!Bet_Enabled()) {
		Bet_Print(cl, "There's no betting on this server.");
		return qtrue;
	}
	if (sscanf(args, "%63s %63s", who, amountStr) != 2) {
		Bet_List(cl);
		return qtrue;
	}

	// An option number from the list, or part of a fighter's name.
	betDuel_t* duel = NULL;
	int side = -1, matches = 0;
	qboolean byNumber = qfalse;
	const char* p = who;
	while (*p >= '0' && *p <= '9') {
		p++;
	}
	if (p != who && !*p) {
		const int opt = atoi(who) - 1;
		if (opt < 0 || opt / 2 >= BET_MAX_DUELS || !gBetDuels[opt / 2].active) {
			Bet_Print(cl, "That's not on the list. Type ^5!bet ^7to see the duels.");
			return qtrue;
		}
		duel = &gBetDuels[opt / 2];
		side = opt % 2;
		matches = 1;
		byNumber = qtrue;
	}
	for (int d = 0; d < BET_MAX_DUELS && !byNumber; d++) {
		betDuel_t* dl = &gBetDuels[d];
		if (!dl->active) {
			continue;
		}
		for (int s = 0; s < 2; s++) {
			char clean[MAX_NAME_LENGTH];
			Q_strncpyz(clean, dl->handle[s], sizeof(clean));
			Q_CleanStr(clean);
			if (Q_stristr(clean, who)) {
				duel = dl;
				side = s;
				matches++;
			}
		}
	}
	if (!matches) {
		Bet_Print(cl, va("Nobody duelling right now matches \"%s\". Type ^5!bet ^7to see the duels.", who));
		return qtrue;
	}
	if (matches > 1) {
		Bet_Print(cl, va("\"%s\" matches more than one fighter - type more of the name.", who));
		return qtrue;
	}
	if (duel->fighter[0] == me || duel->fighter[1] == me) {
		Bet_Print(cl, "You can't bet on your own duel.");
		return qtrue;
	}
	if (svs.time - duel->started > Bet_WindowMs()) {
		Bet_Print(cl, "Betting on that duel has closed.");
		return qtrue;
	}

	const int amount = atoi(amountStr);
	if (amount <= 0) {
		Bet_Print(cl, "Bet at least 1 credit.");
		return qtrue;
	}

	bet_t* mine = NULL;
	for (int i = 0; i < BET_MAX_BETS; i++) {
		if (gBets[i].active && gBets[i].duelId == duel->id && !Q_stricmp(gBets[i].account, cl->economyHandle)) {
			mine = &gBets[i];
			break;
		}
	}
	if (mine && mine->side != side) {
		Bet_Print(cl, va("You've already backed %s ^7in this duel.", duel->handle[mine->side]));
		return qtrue;
	}
	const int max = g_betMax ? g_betMax->integer : 100;
	const int total = amount + (mine ? mine->amount : 0);
	if (max > 0 && total > max) {
		Bet_Print(cl, va("The most you can bet on one duel is %d credits.", max));
		return qtrue;
	}
	if (cl->economyCredits < amount) {
		Bet_Print(cl, va("You only have %d credits.", cl->economyCredits));
		return qtrue;
	}
	if (!mine) {
		for (int i = 0; i < BET_MAX_BETS; i++) {
			if (!gBets[i].active) {
				mine = &gBets[i];
				break;
			}
		}
		if (!mine) {
			Bet_Print(cl, "The bookie's full - try again in a moment.");
			return qtrue;
		}
		memset(mine, 0, sizeof(*mine));
		mine->active = qtrue;
		mine->duelId = duel->id;
		mine->side = side;
		mine->bettor = me;
		Q_strncpyz(mine->account, cl->economyHandle, sizeof(mine->account));
	}

	cl->economyCredits -= amount;
	SV_EconomyPersistCredits(cl);
	mine->amount += amount;

	Bet_Print(cl, va("You've got ^2%d ^7on %s^7. Pot so far: ^2%d ^7(%d on them, %d against).", mine->amount, duel->handle[side],
		Bet_Backing(duel->id, 0) + Bet_Backing(duel->id, 1), Bet_Backing(duel->id, side), Bet_Backing(duel->id, 1 - side)));
	SV_SendServerCommand(NULL, "chat \"^6[Bet] ^7%s ^7bets %d on %s^7!\"\n", cl->name, amount, duel->handle[side]);
	return qtrue;
}
