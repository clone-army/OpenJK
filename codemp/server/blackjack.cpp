/*
===========================================================================
blackjack.cpp — blackjack against the house, part of the economy

  !blackjack <credits>       deal a hand for that bet (also !bj)
  !bj hit | stand | double   play it (twist and stick work too)
  !blackjack                 the rules, or your hand if you're playing

One player against the dealer, from a freshly shuffled deck every hand.
Blackjack pays 3:2, a win 1:1 and a tie gives the bet back; the dealer
draws to 16 and stands on every 17. Doubling (first two cards only) doubles
the bet for exactly one more card. A hand left for BJ_TURN_MS stands by
itself, and so does the hand of anyone who leaves, paid into their account.

Unlike the player-vs-player games this is against the house: bets that are
lost leave the economy and wins are paid by the server. Switched on with
g_economyBlackjackEnable; g_blackjackMaxBet caps one hand's bet.
===========================================================================
*/

#include "server.h"

#define BJ_TURN_MS      30000
#define BJ_MAX_CARDS    12

typedef struct {
	qboolean active;
	char     handle[24];
	int      bet;
	int      deck[52];
	int      next;               // next card to deal from deck
	int      player[BJ_MAX_CARDS], numPlayer;
	int      dealer[BJ_MAX_CARDS], numDealer;
	int      turnEnds;
} bjHand_t;

static bjHand_t gBjHands[MAX_CLIENTS];

static void Bj_Print(client_t* cl, const char* text)
{
	SV_SendServerCommand(cl, "chat \"^3[Blackjack]^7 %s\"\n", text);
}

static int Bj_MaxBet(void)
{
	return g_blackjackMaxBet ? Q_max(1, g_blackjackMaxBet->integer) : 50;
}

// Cards are 1..13 (ace..king).
static const char* Bj_CardName(int card)
{
	static const char* names[] = { "?", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };
	return (card >= 1 && card <= 13) ? names[card] : "?";
}

static int Bj_Value(const int* cards, int count)
{
	int total = 0, aces = 0;
	for (int i = 0; i < count; i++) {
		if (cards[i] == 1) {
			aces++;
			total += 11;
		} else {
			total += Q_min(cards[i], 10);
		}
	}
	while (total > 21 && aces--) {
		total -= 10;
	}
	return total;
}

static qboolean Bj_IsBlackjack(const int* cards, int count)
{
	return (count == 2 && Bj_Value(cards, count) == 21) ? qtrue : qfalse;
}

static int Bj_Draw(bjHand_t* h)
{
	return h->deck[h->next++];
}

static const char* Bj_CardsText(const int* cards, int count, qboolean hideSecond)
{
	static char buf[4][96];
	static int which;
	char* out = buf[which++ & 3];
	out[0] = '\0';
	for (int i = 0; i < count; i++) {
		if (i) {
			Q_strcat(out, 96, " ");
		}
		Q_strcat(out, 96, (hideSecond && i == 1) ? "?" : Bj_CardName(cards[i]));
	}
	return out;
}

static client_t* Bj_Owner(int slot, const bjHand_t* h)
{
	client_t* cl = &svs.clients[slot];
	return (cl->state >= CS_CONNECTED && !Q_stricmp(cl->economyHandle, h->handle)) ? cl : NULL;
}

static void Bj_ShowHand(client_t* cl, const bjHand_t* h)
{
	Bj_Print(cl, va("You: ^3%s ^7(%d)  Dealer: ^1%s ^7- ^5!bj hit^7, ^5!bj stand%s",
		Bj_CardsText(h->player, h->numPlayer, qfalse), Bj_Value(h->player, h->numPlayer),
		Bj_CardsText(h->dealer, h->numDealer, qtrue),
		h->numPlayer == 2 ? "^7 or ^5!bj double" : ""));
}

// Pays out (0 for a loss) and ends the hand. The owner may have left, in
// which case it goes into their account. banner is the big centre-screen
// line (e.g. "^2You win +50").
static void Bj_Settle(int slot, bjHand_t* h, int payout, const char* resultText, const char* bannerText)
{
	client_t* cl = Bj_Owner(slot, h);
	char result[128], banner[64];

	// Callers build these with va(), whose few buffers everything below
	// reuses - copy them first, or the result line comes out as card names.
	Q_strncpyz(result, resultText, sizeof(result));
	Q_strncpyz(banner, bannerText, sizeof(banner));

	SV_GameResult("blackjack", h->handle, cl ? cl->name : h->handle,
		payout > h->bet ? "win" : payout == h->bet ? "push" : "loss", h->bet, payout - h->bet, "dealer",
		va("you %d (%d cards), dealer %d", Bj_Value(h->player, h->numPlayer), h->numPlayer, Bj_Value(h->dealer, h->numDealer)));

	if (payout > 0) {
		if (cl) {
			cl->economyCredits += payout;
			SV_EconomyPersistCredits(cl);
		} else {
			SV_EconomyAddCreditsToAccount(h->handle, payout);
		}
	}
	if (cl) {
		Bj_Print(cl, va("You: ^3%s ^7(%d)  Dealer: ^1%s ^7(%d) - %s Balance: %d",
			Bj_CardsText(h->player, h->numPlayer, qfalse), Bj_Value(h->player, h->numPlayer),
			Bj_CardsText(h->dealer, h->numDealer, qfalse), Bj_Value(h->dealer, h->numDealer),
			result, cl->economyCredits));
		SV_SendServerCommand(cl, "cp \"^3Blackjack\n%s\"\n", banner);
	}
	memset(h, 0, sizeof(*h));
}

// The player's done: the dealer plays, and the hand is settled.
static void Bj_Finish(int slot, bjHand_t* h)
{
	const int you = Bj_Value(h->player, h->numPlayer);

	if (you > 21) {
		Bj_Settle(slot, h, 0, va("^1Bust! ^7You lose %d credits.", h->bet), va("^1Bust ^7- you lose %d credits", h->bet));
		return;
	}
	while (Bj_Value(h->dealer, h->numDealer) < 17 && h->numDealer < BJ_MAX_CARDS) {
		h->dealer[h->numDealer++] = Bj_Draw(h);
	}
	const int dealer = Bj_Value(h->dealer, h->numDealer);

	if (dealer > 21) {
		Bj_Settle(slot, h, h->bet * 2, va("^2Dealer busts - you win %d credits!", h->bet), va("^2You win +%d credits!", h->bet));
	} else if (you > dealer) {
		Bj_Settle(slot, h, h->bet * 2, va("^2You win %d credits!", h->bet), va("^2You win +%d credits!", h->bet));
	} else if (you == dealer) {
		Bj_Settle(slot, h, h->bet, "^3Push ^7- your bet's back.", "^3Push ^7- your credits are back");
	} else {
		Bj_Settle(slot, h, 0, va("^1Dealer wins. ^7You lose %d credits.", h->bet), va("^1Dealer wins ^7- you lose %d credits", h->bet));
	}
}

static void Bj_Deal(client_t* cl, const char* amountStr)
{
	const int slot = cl - svs.clients;
	bjHand_t* h = &gBjHands[slot];
	const int bet = atoi(amountStr);

	if (bet <= 0) {
		Bj_Print(cl, "Usage: ^5!blackjack <credits>^7, e.g. ^5!blackjack 20");
		return;
	}
	if (bet > Bj_MaxBet()) {
		Bj_Print(cl, va("The table limit is %d credits a hand.", Bj_MaxBet()));
		return;
	}
	if (cl->economyCredits < bet) {
		Bj_Print(cl, va("You only have %d credits.", cl->economyCredits));
		return;
	}

	cl->economyCredits -= bet;
	SV_EconomyPersistCredits(cl);

	memset(h, 0, sizeof(*h));
	h->active = qtrue;
	Q_strncpyz(h->handle, cl->economyHandle, sizeof(h->handle));
	h->bet = bet;
	for (int i = 0; i < 52; i++) {
		h->deck[i] = i % 13 + 1;
	}
	for (int i = 51; i > 0; i--) {
		const int j = Q_irand(0, i);
		const int t = h->deck[i];
		h->deck[i] = h->deck[j];
		h->deck[j] = t;
	}
	h->player[h->numPlayer++] = Bj_Draw(h);
	h->dealer[h->numDealer++] = Bj_Draw(h);
	h->player[h->numPlayer++] = Bj_Draw(h);
	h->dealer[h->numDealer++] = Bj_Draw(h);
	h->turnEnds = svs.time + BJ_TURN_MS;

	SV_SendServerCommand(NULL, "chat \"^3[Blackjack] ^7%s ^7is playing blackjack for ^2%d ^7credits.\"\n", cl->name, bet);

	const qboolean youBj = Bj_IsBlackjack(h->player, h->numPlayer);
	const qboolean dealerBj = Bj_IsBlackjack(h->dealer, h->numDealer);
	if (youBj && dealerBj) {
		Bj_Settle(slot, h, bet, "^3You both have blackjack ^7- push, your bet's back.", "^3Push ^7- your credits are back");
	} else if (youBj) {
		Bj_Settle(slot, h, bet + bet * 3 / 2, va("^2Blackjack! ^7You win %d credits!", bet * 3 / 2), va("^2BLACKJACK! You win +%d credits!", bet * 3 / 2));
	} else if (dealerBj) {
		Bj_Settle(slot, h, 0, va("^1Dealer has blackjack. ^7You lose %d credits.", bet), va("^1Dealer blackjack ^7- you lose %d credits", bet));
	} else {
		Bj_ShowHand(cl, h);
	}
}

qboolean SV_BlackjackCommand(client_t* cl, const char* args)
{
	const int slot = cl - svs.clients;
	bjHand_t* h = &gBjHands[slot];
	char a[64];

	if (!g_economyBlackjackEnable || !g_economyBlackjackEnable->integer) {
		Bj_Print(cl, "There's no blackjack table on this server.");
		return qtrue;
	}
	// A hand left over from someone else who had this slot.
	if (h->active && !Bj_Owner(slot, h)) {
		Bj_Finish(slot, h);
	}

	if (sscanf(args, "%63s", a) != 1) {
		if (h->active) {
			Bj_ShowHand(cl, h);
		} else {
			Bj_Print(cl, va("^5!blackjack <credits> ^7deals you a hand against the dealer (up to %d). "
				"Get closer to 21 than the dealer without going over. Blackjack pays 3:2.", Bj_MaxBet()));
		}
		return qtrue;
	}

	if (!h->active) {
		if (!Q_stricmp(a, "hit") || !Q_stricmp(a, "stand") || !Q_stricmp(a, "double") ||
			!Q_stricmp(a, "twist") || !Q_stricmp(a, "stick") ||
			!Q_stricmp(a, "h") || !Q_stricmp(a, "s") || !Q_stricmp(a, "d")) {
			Bj_Print(cl, "You're not playing a hand. ^5!blackjack <credits> ^7to deal one.");
		} else {
			Bj_Deal(cl, a);
		}
		return qtrue;
	}

	if (!Q_stricmp(a, "hit") || !Q_stricmp(a, "h") || !Q_stricmp(a, "twist")) {
		h->player[h->numPlayer++] = Bj_Draw(h);
		h->turnEnds = svs.time + BJ_TURN_MS;
		const int you = Bj_Value(h->player, h->numPlayer);
		if (you >= 21 || h->numPlayer >= BJ_MAX_CARDS) {
			Bj_Finish(slot, h);
		} else {
			Bj_ShowHand(cl, h);
		}
	} else if (!Q_stricmp(a, "stand") || !Q_stricmp(a, "s") || !Q_stricmp(a, "stick")) {
		Bj_Finish(slot, h);
	} else if (!Q_stricmp(a, "double") || !Q_stricmp(a, "d")) {
		if (h->numPlayer != 2) {
			Bj_Print(cl, "You can only double on your first two cards.");
		} else if (cl->economyCredits < h->bet) {
			Bj_Print(cl, va("Doubling needs another %d credits - you've got %d.", h->bet, cl->economyCredits));
		} else {
			cl->economyCredits -= h->bet;
			SV_EconomyPersistCredits(cl);
			h->bet *= 2;
			h->player[h->numPlayer++] = Bj_Draw(h);
			Bj_Print(cl, va("Doubled to %d credits - one more card: ^3%s", h->bet, Bj_CardName(h->player[h->numPlayer - 1])));
			Bj_Finish(slot, h);
		}
	} else {
		Bj_Print(cl, "You're mid-hand: ^5!bj hit^7 (twist), ^5!bj stand ^7(stick) or ^5!bj double^7.");
	}
	return qtrue;
}

// Stands a hand left too long, or whose player left.
void SV_BlackjackFrame(void)
{
	for (int i = 0; i < MAX_CLIENTS; i++) {
		bjHand_t* h = &gBjHands[i];
		if (!h->active) {
			continue;
		}
		client_t* cl = Bj_Owner(i, h);
		if (!cl || svs.time >= h->turnEnds) {
			if (cl) {
				Bj_Print(cl, "Time's up - you stand.");
			}
			Bj_Finish(i, h);
		}
	}
}
