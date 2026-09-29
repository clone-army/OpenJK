/*
===========================================================================
pazaak.cpp — Pazaak for credits, part of the economy (!pazaak / !pz)

KOTOR's card game, played in chat between two logged-in players:

  !pazaak <player> <credits>   challenge someone (they have 60s to answer);
                               part of their name is enough
  !pazaak accept | decline     answer a challenge
  !pz play <n>                 play side card n (at most one per turn)
  !pz end                      end your turn - you'll be dealt another card
  !pz stand                    keep your total for the rest of the set
  !pz auto                     let the server play your turn
  !pz forfeit                  give up the match

(!pazaak and !pz are interchangeable for all of them.)

Both stakes are taken when the challenge is accepted and the winner gets
the pot. The server deals from a 1-10 deck; each player also gets four
random side cards (+1..+6 or -1..-6) for the whole match. Over 20 at the end
of your turn is a bust; otherwise whoever's closer to 20 once both have
stood wins the set, and nine cards on your table without busting wins it
outright. Ties replay the set; first to two sets wins the match. A turn
left for 30 seconds stands automatically; leaving the server forfeits.
Switched on with g_economyPazaakEnable.
===========================================================================
*/

#include "server.h"

#define PZ_TARGET           20
#define PZ_TABLE_MAX        9
#define PZ_HAND_SIZE        4
#define PZ_SETS_TO_WIN      2
#define PZ_MAX_SETS         9       // ties don't count; give up and refund after this many
#define PZ_TURN_MS          30000
#define PZ_CHALLENGE_MS     60000

typedef struct {
	qboolean active;
	int      player[2];             // client numbers; [0] is the challenger
	char     handle[2][24];         // accounts, to tell a reused slot from the same player
	int      bet;
	int      total[2];
	int      tableCards[2];
	qboolean stood[2];
	int      hand[2][PZ_HAND_SIZE]; // side cards; 0 = used
	int      setsWon[2];
	int      setsPlayed;
	int      turn;                  // 0 or 1
	qboolean playedThisTurn;
	int      turnDeadline;
} pazaakGame_t;

typedef struct {
	int  target;                    // client challenged, -1 = none
	char targetHandle[24];
	int  bet;
	int  expires;
} pazaakChallenge_t;

static pazaakGame_t      gPzGames[MAX_CLIENTS / 2];
static pazaakChallenge_t gPzChallenges[MAX_CLIENTS]; // indexed by challenger

static void Pz_Print(client_t* cl, const char* text)
{
	SV_SendServerCommand(cl, "chat \"^3[Pazaak]^7 %s\"\n", text);
}

static client_t* Pz_Client(int num)
{
	return &svs.clients[num];
}

// Still the same logged-in player in that slot.
static qboolean Pz_Present(int num, const char* handle)
{
	client_t* cl = Pz_Client(num);
	return (cl->state == CS_ACTIVE && cl->economyHandle[0] && !Q_stricmp(cl->economyHandle, handle)) ? qtrue : qfalse;
}

static pazaakGame_t* Pz_GameOf(int num, int* side)
{
	for (int g = 0; g < (int)ARRAY_LEN(gPzGames); g++) {
		for (int s = 0; s < 2; s++) {
			if (gPzGames[g].active && gPzGames[g].player[s] == num) {
				if (side) {
					*side = s;
				}
				return &gPzGames[g];
			}
		}
	}
	return NULL;
}

static const char* Pz_Card(int v)
{
	return va("%s%d", v > 0 ? "+" : "", v);
}

static const char* Pz_HandText(const pazaakGame_t* g, int side)
{
	static char buf[128];
	buf[0] = '\0';
	for (int i = 0; i < PZ_HAND_SIZE; i++) {
		if (g->hand[side][i]) {
			Q_strcat(buf, sizeof(buf), va("^5%d)^7%s  ", i + 1, Pz_Card(g->hand[side][i])));
		}
	}
	return buf[0] ? buf : "^7(none left)";
}

static void Pz_Both(pazaakGame_t* g, const char* text)
{
	Pz_Print(Pz_Client(g->player[0]), text);
	Pz_Print(Pz_Client(g->player[1]), text);
}

static const char* Pz_Name(const pazaakGame_t* g, int side)
{
	return Pz_Client(g->player[side])->name;
}

static void Pz_Payout(client_t* cl, int amount)
{
	cl->economyCredits += amount;
	SV_EconomyPersistCredits(cl);
}

static void Pz_EndMatch(pazaakGame_t* g, int winner, const char* how)
{
	const int pot = g->bet * 2;
	if (winner < 0) {
		// No result: everyone gets their stake back.
		for (int s = 0; s < 2; s++) {
			if (Pz_Present(g->player[s], g->handle[s])) {
				Pz_Payout(Pz_Client(g->player[s]), g->bet);
			} else {
				SV_EconomyAddCreditsToAccount(g->handle[s], g->bet);
			}
		}
		Pz_Both(g, va("No winner (%s) - stakes returned.", how));
		for (int s = 0; s < 2; s++) {
			if (Pz_Present(g->player[s], g->handle[s])) {
				SV_EconomyResultBanner(Pz_Client(g->player[s]), "Pazaak", 0);
			}
		}
	} else {
		if (Pz_Present(g->player[winner], g->handle[winner])) {
			Pz_Payout(Pz_Client(g->player[winner]), pot);
		} else {
			SV_EconomyAddCreditsToAccount(g->handle[winner], pot);
		}
		SV_SendServerCommand(NULL, "chat \"^3[Pazaak] ^7%s ^7beat %s ^7at Pazaak%s and won ^2%d ^7credits!\"\n",
			Pz_Name(g, winner), Pz_Name(g, 1 - winner), how, pot);
		for (int s = 0; s < 2; s++) {
			if (Pz_Present(g->player[s], g->handle[s])) {
				SV_EconomyResultBanner(Pz_Client(g->player[s]), "Pazaak", s == winner ? g->bet : -g->bet);
			}
		}
	}
	memset(g, 0, sizeof(*g));
}

static void Pz_BeginTurn(pazaakGame_t* g);

static void Pz_StartSet(pazaakGame_t* g, int starter)
{
	for (int s = 0; s < 2; s++) {
		g->total[s] = 0;
		g->tableCards[s] = 0;
		g->stood[s] = qfalse;
	}
	g->turn = starter;
	g->setsPlayed++;
	Pz_Both(g, va("^3Set %d^7 - sets: %s ^2%d^7, %s ^2%d^7. First to %d wins.",
		g->setsPlayed, Pz_Name(g, 0), g->setsWon[0], Pz_Name(g, 1), g->setsWon[1], PZ_SETS_TO_WIN));
	Pz_BeginTurn(g);
}

static void Pz_SetResult(pazaakGame_t* g, int winner, const char* why)
{
	if (winner < 0) {
		Pz_Both(g, va("Set tied at %d - it doesn't count.", g->total[0]));
	} else {
		g->setsWon[winner]++;
		Pz_Both(g, va("%s ^7wins the set (%s).", Pz_Name(g, winner), why));
		if (g->setsWon[winner] >= PZ_SETS_TO_WIN) {
			Pz_EndMatch(g, winner, "");
			return;
		}
	}
	if (g->setsPlayed >= PZ_MAX_SETS) {
		Pz_EndMatch(g, -1, "too many tied sets");
		return;
	}
	// The set's loser (or, on a tie, the other player) starts the next.
	Pz_StartSet(g, winner < 0 ? 1 - g->turn : 1 - winner);
}

static void Pz_CheckBothStood(pazaakGame_t* g)
{
	if (!g->stood[0] || !g->stood[1]) {
		return;
	}
	if (g->total[0] == g->total[1]) {
		Pz_SetResult(g, -1, "");
	} else {
		const int w = g->total[0] > g->total[1] ? 0 : 1;
		Pz_SetResult(g, w, va("%d to %d", g->total[w], g->total[1 - w]));
	}
}

// The current player's turn is over; move on.
static void Pz_NextTurn(pazaakGame_t* g)
{
	const int me = g->turn;
	if (g->total[me] > PZ_TARGET) {
		Pz_SetResult(g, 1 - me, va("%s ^7bust with %d", Pz_Name(g, me), g->total[me]));
		return;
	}
	if (g->tableCards[me] >= PZ_TABLE_MAX) {
		Pz_SetResult(g, me, "nine cards without busting");
		return;
	}
	if (g->stood[0] && g->stood[1]) {
		Pz_CheckBothStood(g);
		return;
	}
	if (!g->stood[1 - me]) {
		g->turn = 1 - me;
	}
	Pz_BeginTurn(g);
}

static void Pz_Stand(pazaakGame_t* g)
{
	const int me = g->turn;
	g->stood[me] = qtrue;
	Pz_Both(g, va("%s ^7stands on ^3%d^7.", Pz_Name(g, me), g->total[me]));
	Pz_NextTurn(g);
}

static void Pz_BeginTurn(pazaakGame_t* g)
{
	const int me = g->turn;
	const int card = Q_irand(1, 10);

	g->total[me] += card;
	g->tableCards[me]++;
	g->playedThisTurn = qfalse;
	g->turnDeadline = svs.time + PZ_TURN_MS;

	Pz_Print(Pz_Client(g->player[1 - me]), va("%s ^7draws %d - total ^3%d^7.", Pz_Name(g, me), card, g->total[me]));
	Pz_Print(Pz_Client(g->player[me]), va("^2Your turn:^7 you draw %d - total ^3%d^7 (they have %d%s).",
		card, g->total[me], g->total[1 - me], g->stood[1 - me] ? ", standing" : ""));

	if (g->total[me] == PZ_TARGET) {
		Pz_Stand(g);
		return;
	}
	Pz_Print(Pz_Client(g->player[me]), va("Side cards: %s", Pz_HandText(g, me)));
	Pz_Print(Pz_Client(g->player[me]), "^5!pz play <n>^7, then ^5!pz end ^7(draw again next turn) or ^5!pz stand^7 - or ^5!pz auto ^7to let it pick. 30s.");
}

static void Pz_Challenge(client_t* cl, const char* who, const char* amountStr)
{
	const int me = cl - svs.clients;
	client_t* target = SV_EconomyFindPlayer(cl, who);
	const int bet = atoi(amountStr);

	if (!target) {
		return;
	}
	if (target == cl) {
		Pz_Print(cl, "You can't play Pazaak against yourself.");
		return;
	}
	if (!target->economyHandle[0]) {
		Pz_Print(cl, va("%s ^7isn't logged in, so can't bet credits.", target->name));
		return;
	}
	if (bet <= 0) {
		Pz_Print(cl, "The bet must be more than 0 credits.");
		return;
	}
	if (cl->economyCredits < bet) {
		Pz_Print(cl, va("You only have %d credits.", cl->economyCredits));
		return;
	}
	if (Pz_GameOf(me, NULL) || Pz_GameOf(target - svs.clients, NULL)) {
		Pz_Print(cl, "One of you is already in a game.");
		return;
	}

	pazaakChallenge_t* ch = &gPzChallenges[me];
	ch->target = target - svs.clients;
	Q_strncpyz(ch->targetHandle, target->economyHandle, sizeof(ch->targetHandle));
	ch->bet = bet;
	ch->expires = svs.time + PZ_CHALLENGE_MS;

	Pz_Print(cl, va("You challenged %s ^7to Pazaak for %d credits. Waiting for them to accept...", target->name, bet));
	Pz_Print(target, va("%s ^7challenges you to Pazaak for ^2%d ^7credits each! ^5!pazaak accept ^7or ^5!pazaak decline ^7(60s).",
		cl->name, bet));
}

static void Pz_Answer(client_t* cl, qboolean accept)
{
	const int me = cl - svs.clients;
	int from = -1;

	for (int i = 0; i < MAX_CLIENTS; i++) {
		const pazaakChallenge_t* ch = &gPzChallenges[i];
		if (ch->expires > svs.time && ch->target == me && !Q_stricmp(ch->targetHandle, cl->economyHandle)) {
			from = i;
			break;
		}
	}
	if (from < 0) {
		Pz_Print(cl, "Nobody's challenged you to Pazaak right now.");
		return;
	}

	pazaakChallenge_t* ch = &gPzChallenges[from];
	client_t* challenger = Pz_Client(from);
	const int bet = ch->bet;
	memset(ch, 0, sizeof(*ch));
	ch->target = -1;

	if (!accept) {
		Pz_Print(cl, "Challenge declined.");
		if (challenger->state == CS_ACTIVE) {
			Pz_Print(challenger, va("%s ^7declined your Pazaak challenge.", cl->name));
		}
		return;
	}
	if (challenger->state != CS_ACTIVE || !challenger->economyHandle[0]) {
		Pz_Print(cl, "They've left.");
		return;
	}
	if (Pz_GameOf(me, NULL) || Pz_GameOf(from, NULL)) {
		Pz_Print(cl, "One of you is already in a game.");
		return;
	}
	if (cl->economyCredits < bet || challenger->economyCredits < bet) {
		Pz_Print(cl, "One of you can't cover the bet any more.");
		Pz_Print(challenger, "One of you can't cover the Pazaak bet any more.");
		return;
	}

	pazaakGame_t* g = NULL;
	for (int i = 0; i < (int)ARRAY_LEN(gPzGames); i++) {
		if (!gPzGames[i].active) {
			g = &gPzGames[i];
			break;
		}
	}
	if (!g) {
		Pz_Print(cl, "Every Pazaak table is busy - try again shortly.");
		return;
	}

	memset(g, 0, sizeof(*g));
	g->active = qtrue;
	g->player[0] = from;
	g->player[1] = me;
	Q_strncpyz(g->handle[0], challenger->economyHandle, sizeof(g->handle[0]));
	Q_strncpyz(g->handle[1], cl->economyHandle, sizeof(g->handle[1]));
	g->bet = bet;
	for (int s = 0; s < 2; s++) {
		client_t* p = Pz_Client(g->player[s]);
		p->economyCredits -= bet;
		SV_EconomyPersistCredits(p);
		for (int i = 0; i < PZ_HAND_SIZE; i++) {
			g->hand[s][i] = Q_irand(1, 6) * (Q_irand(0, 1) ? 1 : -1);
		}
	}

	SV_SendServerCommand(NULL, "chat \"^3[Pazaak] ^7%s ^7and %s ^7are playing Pazaak for ^2%d ^7credits.\"\n",
		challenger->name, cl->name, bet * 2);
	Pz_StartSet(g, Q_irand(0, 1));
}

// Plays side card n (1-based). qfalse if it can't be played.
static qboolean Pz_PlayCard(pazaakGame_t* g, int side, int n)
{
	if (g->playedThisTurn || n < 1 || n > PZ_HAND_SIZE || !g->hand[side][n - 1]) {
		return qfalse;
	}
	const int v = g->hand[side][n - 1];
	g->hand[side][n - 1] = 0;
	g->total[side] += v;
	g->playedThisTurn = qtrue;
	Pz_Both(g, va("%s ^7plays %s - total ^3%d^7.", Pz_Name(g, side), Pz_Card(v), g->total[side]));
	return qtrue;
}

// The side card that lands the total closest to 20 without going over, as
// long as it lands above floor; 0 if none does.
static int Pz_BestCard(const pazaakGame_t* g, int side, int floor)
{
	int best = 0, bestTotal = floor;
	for (int i = 0; i < PZ_HAND_SIZE; i++) {
		const int v = g->hand[side][i];
		const int t = g->total[side] + v;
		if (v && t <= PZ_TARGET && t > bestTotal) {
			best = i + 1;
			bestTotal = t;
		}
	}
	return best;
}

// "!pz auto": plays the turn for you. Rescue a bust or hit 20 with a side
// card where one does it; against someone standing, beat them if you can;
// otherwise stand on 18 or more and draw again below that.
static void Pz_AutoTurn(pazaakGame_t* g, int side)
{
	const int opp = 1 - side;
	int card;

	if (!g->playedThisTurn) {
		if (g->total[side] > PZ_TARGET && (card = Pz_BestCard(g, side, 0)) != 0) {
			Pz_PlayCard(g, side, card);                                 // save the bust
		} else if ((card = Pz_BestCard(g, side, PZ_TARGET - 1)) != 0) {
			Pz_PlayCard(g, side, card);                                 // straight to 20
		} else if (g->stood[opp] && g->total[side] <= g->total[opp] &&
			(card = Pz_BestCard(g, side, g->total[opp])) != 0) {
			Pz_PlayCard(g, side, card);                                 // beat the stander
		}
	}
	if (g->total[side] == PZ_TARGET || g->total[side] > PZ_TARGET) {
		Pz_Stand(g);                                                    // 20, or bust anyway
	} else if (g->stood[opp]) {
		if (g->total[side] > g->total[opp]) {
			Pz_Stand(g);
		} else {
			Pz_NextTurn(g);                                             // behind: draw again
		}
	} else if (g->total[side] >= 18) {
		Pz_Stand(g);
	} else {
		Pz_NextTurn(g);
	}
}

qboolean SV_PazaakCommand(client_t* cl, const char* args)
{
	char a[64], b[64];
	const int argc = sscanf(args, "%63s %63s", a, b);
	int side;

	if (!g_economyPazaakEnable || !g_economyPazaakEnable->integer) {
		Pz_Print(cl, "There's no Pazaak table on this server.");
		return qtrue;
	}

	if (argc < 1) {
		Pz_Print(cl, "^5!pazaak <player> <credits> ^7to challenge someone. Closest to 20 without going over wins the set, and the first to 2 sets wins the pot.");
		Pz_Print(cl, "In a game: ^5!pz play <n>^7, ^5!pz end^7, ^5!pz stand^7, ^5!pz auto ^7(it picks for you), ^5!pz forfeit^7.");
		Pz_Print(cl, "^5!pazaak ^7and ^5!pz ^7work the same for everything.");
		return qtrue;
	}

	if (!Q_stricmp(a, "accept") || !Q_stricmp(a, "decline")) {
		Pz_Answer(cl, !Q_stricmp(a, "accept") ? qtrue : qfalse);
		return qtrue;
	}

	pazaakGame_t* g = Pz_GameOf(cl - svs.clients, &side);

	if (!Q_stricmp(a, "forfeit")) {
		if (!g) {
			Pz_Print(cl, "You're not in a game.");
		} else {
			Pz_EndMatch(g, 1 - side, " (forfeit)");
		}
		return qtrue;
	}

	if (!Q_stricmp(a, "play") || !Q_stricmp(a, "end") || !Q_stricmp(a, "stand") || !Q_stricmp(a, "auto")) {
		if (!g) {
			Pz_Print(cl, "You're not in a game. ^5!pazaak <player> <credits> ^7to start one.");
			return qtrue;
		}
		if (g->turn != side) {
			Pz_Print(cl, "It's not your turn.");
			return qtrue;
		}
		if (!Q_stricmp(a, "end")) {
			Pz_NextTurn(g);
		} else if (!Q_stricmp(a, "stand")) {
			Pz_Stand(g);
		} else if (!Q_stricmp(a, "auto")) {
			Pz_AutoTurn(g, side);
		} else {
			const int n = (argc >= 2) ? atoi(b) : 0;
			if (g->playedThisTurn) {
				Pz_Print(cl, "Only one side card per turn.");
			} else if (!Pz_PlayCard(g, side, n)) {
				Pz_Print(cl, va("Pick a card you still have: %s", Pz_HandText(g, side)));
			} else if (g->total[side] == PZ_TARGET) {
				Pz_Stand(g);
			} else {
				Pz_Print(cl, "^5!pz end ^7or ^5!pz stand^7.");
			}
		}
		return qtrue;
	}

	if (argc < 2) {
		Pz_Print(cl, "Usage: ^5!pazaak <player> <credits>");
		return qtrue;
	}
	Pz_Challenge(cl, a, b);
	return qtrue;
}

void SV_PazaakFrame(void)
{
	for (int i = 0; i < (int)ARRAY_LEN(gPzGames); i++) {
		pazaakGame_t* g = &gPzGames[i];
		if (!g->active) {
			continue;
		}
		for (int s = 0; s < 2; s++) {
			if (!Pz_Present(g->player[s], g->handle[s])) {
				// Name from the slot may be gone; announce with what's left.
				Pz_EndMatch(g, 1 - s, " (their opponent left)");
				break;
			}
		}
		if (g->active && svs.time >= g->turnDeadline) {
			Pz_Print(Pz_Client(g->player[g->turn]), "Out of time - you stand.");
			Pz_Stand(g);
		}
	}
}
