/*
===========================================================================
betting.cpp — betting on duels, part of the economy (!bet)

  !bet start                a fighter opens their duel to bets (!bets start too): both
                             are frozen while bets come in. One fight at a
                             time takes bets, until it's decided
  !bet                       the fight taking bets, and what's backing whom
  !bet <fighter> <credits>   back a fighter (fuzzy name match, any case)

Duels are tracked straight from the players' own playerState -
duelInProgress and duelIndex, stock fields, the same ones social.cpp's
duel handling reads - so any duel counts however it was started. A duel
only takes bets if one of its fighters opens it with "!bet start" within
BET_OPEN_MS of it starting, while both are still at full health; then both
fighters are frozen for g_betWindowSeconds (30) - their input is rewritten
before the game sees it, like the bar's control drinks, so nobody lands a
hit - and bets are only taken during that freeze: up to g_betMax credits a
duel (100, 0 = no limit), on one side only, never on your own duel. The
two fighters see a countdown to the fight on screen, with the pot so far.

The stake is taken when you bet; back the winner and you get it back, plus
a flat win bonus (g_betWinBonus, 20 - capped at your stake so a 1-credit
bet can't farm it, and the only credits betting creates), plus a share of
the losing bets in proportion to your stake. Paid into the account, so it
counts if you've left. Losing bets are lost - though if nobody backed the
winner, they get g_betLoserRefund percent (25) back. A duel with no clear
winner - a fighter leaving, say - refunds everyone. Switched on with
g_economyBetEnable.
===========================================================================
*/

#include "server.h"

#define BET_MAX_DUELS   16
#define BET_MAX_BETS    128
#define BET_OPEN_MS     10000   // how long after a duel starts a fighter can open it to bets

typedef struct {
	qboolean active;
	int      id;                // shown on !bet; counts up per map
	int      fighter[2];        // client numbers
	char     handle[2][MAX_NAME_LENGTH]; // names at the start, for announcements
	int      spawnCount[2];
	int      started;           // svs.time
	qboolean betsOpened;        // a fighter opened it to bets (once only)
	int      betsUntil;         // bets taken, and fighters frozen, until then
	int      countdownShown;    // last seconds-left shown to the fighters
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

// winner: 0 or 1, or -1 to refund everyone. Winners get their stake back,
// the win bonus (up to their stake) and a share of the losing bets by
// stake. If nobody backed the winner, losers get g_betLoserRefund percent
// of their stake back and the rest is gone.
static void Bet_Resolve(betDuel_t* duel, int winner)
{
	int winners = 0, losers = 0, paid = 0;
	const int pot = Bet_Backing(duel->id, 0) + Bet_Backing(duel->id, 1);
	const int winningSide = (winner >= 0) ? Bet_Backing(duel->id, winner) : 0;
	// Nobody backed the winner: losing bets still lose, but get a little back.
	const int refundPct = (winningSide <= 0) ? Com_Clampi(0, 100, g_betLoserRefund ? g_betLoserRefund->integer : 25) : 0;

	for (int i = 0; i < BET_MAX_BETS; i++) {
		bet_t* b = &gBets[i];
		if (!b->active || b->duelId != duel->id) {
			continue;
		}
		client_t* cl = &svs.clients[b->bettor];
		const qboolean here = (cl->state == CS_ACTIVE && !Q_stricmp(cl->economyHandle, b->account)) ? qtrue : qfalse;

		// For the web panel: every bet as it settles.
		{
			const int bonus = Q_min(b->amount, g_betWinBonus ? Q_max(0, g_betWinBonus->integer) : 20);
			const int share = (winningSide > 0) ? (int)((long long)b->amount * (pot - winningSide) / winningSide) : 0;
			const int net = (winner < 0) ? 0 : (b->side == winner) ? bonus + share : (b->amount * refundPct / 100) - b->amount;
			SV_GameResult("bet", b->account, here ? cl->name : b->account,
				winner < 0 ? "refund" : (b->side == winner ? "win" : "loss"), b->amount, net,
				va("%s vs %s", duel->handle[0], duel->handle[1]), va("backed %s", duel->handle[b->side]));
		}
		if (winner < 0) {
			Bet_Pay(b, b->amount);
			if (here) {
				Bet_Print(cl, va("That duel ended without a winner - your %d credits are back.", b->amount));
				SV_EconomyResultBanner(cl, "Bet", 0);
			}
		} else if (b->side == winner) {
			const int bonus = Q_min(b->amount, g_betWinBonus ? Q_max(0, g_betWinBonus->integer) : 20);
			const int share = (int)((long long)b->amount * (pot - winningSide) / winningSide);
			const int payout = b->amount + bonus + share;
			Bet_Pay(b, payout);
			winners++;
			paid += payout;
			if (here) {
				Bet_Print(cl, va("Your fighter won! ^2%d ^7credits back (stake %d + bonus %d + %d from the losers).",
					payout, b->amount, bonus, share));
				SV_EconomyResultBanner(cl, "Bet won", payout - b->amount);
			}
		} else {
			const int refund = b->amount * refundPct / 100;
			losers++;
			if (refund > 0) {
				Bet_Pay(b, refund);
			}
			if (here) {
				Bet_Print(cl, refund > 0
					? va("Your fighter lost, and nobody backed the winner - %d of your %d credits back.", refund, b->amount)
					: va("Your fighter lost - there go your %d credits.", b->amount));
				SV_EconomyResultBanner(cl, "Bet lost", refund - b->amount);
			}
		}
		memset(b, 0, sizeof(*b));
	}

	if (winner >= 0 && (winners || losers)) {
		SV_SendServerCommand(NULL, "chat \"^6[Bet] ^7%s ^7beat %s^7! %d winning bettor%s paid ^2%d ^7credits, %d lost.\"\n",
			duel->handle[winner], duel->handle[1 - winner], winners, winners == 1 ? "" : "s", paid, losers);
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
				for (int s = 0; s < 2; s++) {
					Bet_Print(&svs.clients[duel->fighter[s]],
						va("Want bets on this fight? ^5!bet start ^7in the next %ds.", BET_OPEN_MS / 1000));
				}
			}
		}
		duel->seen = qtrue;

		// Frozen fighters get a countdown on screen, refreshed each second.
		if (duel->betsUntil > svs.time) {
			const int secs = (duel->betsUntil - svs.time + 999) / 1000;
			if (secs != duel->countdownShown) {
				duel->countdownShown = secs;
				const int pot = Bet_Backing(duel->id, 0) + Bet_Backing(duel->id, 1);
				for (int s = 0; s < 2; s++) {
					SV_SendServerCommand(&svs.clients[duel->fighter[s]],
						"cp \"^6Bets are open ^7- ^2%d ^7cr on the fight\n^7Fight in ^3%d\"\n", pot, secs);
				}
			}
		}

		if (duel->betsUntil && svs.time >= duel->betsUntil) {
			duel->betsUntil = 0;
			SV_SendServerCommand(NULL, "chat \"^6[Bet] ^7Bets are closed on %s ^7vs %s^7 - ^1FIGHT!\"\n", duel->handle[0], duel->handle[1]);
			for (int s = 0; s < 2; s++) {
				SV_SendServerCommand(&svs.clients[duel->fighter[s]], "cp \"^1FIGHT!\"\n");
			}
		}
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
	const betDuel_t* duel = NULL;
	for (int d = 0; d < BET_MAX_DUELS; d++) {
		if (gBetDuels[d].active && gBetDuels[d].betsOpened) {
			duel = &gBetDuels[d];
			break;
		}
	}
	if (!duel) {
		Bet_Print(cl, "No fight is taking bets right now. Duelling? ^5!bet start ^7in the first 10 seconds.");
		return;
	}

	const int left = duel->betsUntil - svs.time;
	SV_EconomyMenuBegin(cl);
	SV_EconomyMenuAddLine(cl, va("^6=== %s ^7vs %s ^6=== %s", duel->handle[0], duel->handle[1],
		left > 0 ? va("^2bets open %ds", (left + 999) / 1000) : "^1bets closed - fighting"));
	for (int s = 0; s < 2; s++) {
		SV_EconomyMenuAddLine(cl, va("^3%s ^7to win - ^2%d ^7cr backing", duel->handle[s], Bet_Backing(duel->id, s)));
	}
	for (int i = 0; i < BET_MAX_BETS; i++) {
		const bet_t* bt = &gBets[i];
		if (bt->active && bt->duelId == duel->id && !Q_stricmp(bt->account, cl->economyHandle)) {
			SV_EconomyMenuAddLine(cl, va("^7Your bet: ^2%d ^7on %s", bt->amount, duel->handle[bt->side]));
		}
	}
	SV_EconomyMenuAddLine(cl, va("^7Winners get their bet back + up to ^2%d ^7bonus + a share of the losing bets. ^5!bet ricks 50",
		g_betWinBonus ? g_betWinBonus->integer : 20));
	SV_EconomyMenuPump(cl);
}

// "!bet start" from a fighter: opens their duel to bets and freezes both.
static void Bet_OpenDuel(client_t* cl)
{
	const int me = cl - svs.clients;
	betDuel_t* duel = NULL;

	for (int d = 0; d < BET_MAX_DUELS; d++) {
		if (gBetDuels[d].active && (gBetDuels[d].fighter[0] == me || gBetDuels[d].fighter[1] == me)) {
			duel = &gBetDuels[d];
			break;
		}
	}
	if (!duel) {
		Bet_Print(cl, "You need to be in a duel to open it to bets.");
		return;
	}
	if (duel->betsOpened) {
		Bet_Print(cl, "This duel has already been opened to bets.");
		return;
	}
	for (int d = 0; d < BET_MAX_DUELS; d++) {
		if (gBetDuels[d].active && gBetDuels[d].betsOpened) {
			Bet_Print(cl, va("A fight with bets is already running (%s ^7vs %s^7) - wait for it to finish.",
				gBetDuels[d].handle[0], gBetDuels[d].handle[1]));
			return;
		}
	}
	if (svs.time - duel->started > BET_OPEN_MS) {
		Bet_Print(cl, va("Too late - bets have to be opened in the first %d seconds of a duel.", BET_OPEN_MS / 1000));
		return;
	}
	for (int s = 0; s < 2; s++) {
		const playerState_t* ps = svs.clients[duel->fighter[s]].gentity->playerState;
		if (ps->stats[STAT_HEALTH] < ps->stats[STAT_MAX_HEALTH]) {
			Bet_Print(cl, "Too late - the fight's already started.");
			return;
		}
	}
	if (Bet_WindowMs() <= 0) {
		Bet_Print(cl, "Betting windows are switched off on this server.");
		return;
	}

	duel->betsOpened = qtrue;
	duel->betsUntil = svs.time + Bet_WindowMs();
	SV_SendServerCommand(NULL, "chat \"^6[Bet] ^7%s ^7vs %s ^7is taking bets for %ds! ^5!bet <fighter> <credits>^7, e.g. ^5!bet %s 50\"\n",
		duel->handle[0], duel->handle[1], Bet_WindowMs() / 1000, duel->handle[0]);
	// The fighters' on-screen countdown starts on the next frame (SV_BetFrame).
}

// Fighters in a duel that's taking bets can look around, but not move,
// attack or use Force powers until the window closes.
void SV_BetClientThink(client_t* cl, usercmd_t* cmd)
{
	const int me = cl - svs.clients;
	for (int d = 0; d < BET_MAX_DUELS; d++) {
		const betDuel_t* duel = &gBetDuels[d];
		if (duel->active && duel->betsUntil > svs.time && (duel->fighter[0] == me || duel->fighter[1] == me)) {
			cmd->forwardmove = 0;
			cmd->rightmove = 0;
			cmd->upmove = 0;
			cmd->buttons = 0;
			cmd->generic_cmd = 0;
			return;
		}
	}
}

qboolean SV_BetCommand(client_t* cl, const char* args)
{
	char who[64], amountStr[64];
	const int me = cl - svs.clients;

	if (!Bet_Enabled()) {
		Bet_Print(cl, "There's no betting on this server.");
		return qtrue;
	}
	const int argc = sscanf(args, "%63s %63s", who, amountStr);
	if (argc >= 1 && (!Q_stricmp(who, "start") || !Q_stricmp(who, "open"))) {
		Bet_OpenDuel(cl);
		return qtrue;
	}
	if (argc != 2) {
		Bet_List(cl);
		return qtrue;
	}

	// The fight taking bets, and the fighter that best matches the name.
	betDuel_t* duel = NULL;
	for (int d = 0; d < BET_MAX_DUELS; d++) {
		if (gBetDuels[d].active && gBetDuels[d].betsOpened) {
			duel = &gBetDuels[d];
			break;
		}
	}
	if (!duel) {
		Bet_Print(cl, "No fight is taking bets right now.");
		return qtrue;
	}
	const int score0 = SV_FuzzyNameScore(duel->handle[0], who);
	const int score1 = SV_FuzzyNameScore(duel->handle[1], who);
	if (!score0 && !score1) {
		Bet_Print(cl, va("\"%s\" doesn't match %s ^7or %s^7.", who, duel->handle[0], duel->handle[1]));
		return qtrue;
	}
	if (score0 == score1) {
		Bet_Print(cl, va("\"%s\" matches both fighters - type more of the name.", who));
		return qtrue;
	}
	const int side = (score0 > score1) ? 0 : 1;

	if (duel->fighter[0] == me || duel->fighter[1] == me) {
		Bet_Print(cl, "You can't bet on your own duel.");
		return qtrue;
	}
	if (duel->betsUntil <= svs.time) {
		Bet_Print(cl, duel->betsOpened ? "Betting on that duel has closed." : "That duel isn't taking bets.");
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
