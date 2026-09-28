/*
===========================================================================
chance.cpp — Chance, a coin flip for credits, part of the economy (!chance)

  !chance <player> <credits>   challenge someone (part of their name is enough)
  !chance red | blue           accept a challenge and pick your colour
  !chance decline              turn one down

The challenged player picks a colour and the challenger gets the other one;
the server rolls red or blue, 50/50, and whoever's colour comes up takes
both stakes. Both have to be logged in and able to cover the bet; a
challenge lasts 60 seconds. Switched on with g_economyChanceEnable.
===========================================================================
*/

#include "server.h"

#define CHANCE_CHALLENGE_MS 60000

typedef struct {
	int  target;            // client challenged
	char targetHandle[24];
	int  bet;
	int  expires;
} chanceChallenge_t;

static chanceChallenge_t gChanceChallenges[MAX_CLIENTS]; // indexed by challenger

static void Chance_Print(client_t* cl, const char* text)
{
	SV_SendServerCommand(cl, "chat \"^3[Chance]^7 %s\"\n", text);
}

static void Chance_Challenge(client_t* cl, const char* who, const char* amountStr)
{
	client_t* target = SV_EconomyFindPlayer(cl, who);
	const int bet = atoi(amountStr);

	if (!target) {
		return;
	}
	if (target == cl) {
		Chance_Print(cl, "You can't play against yourself.");
		return;
	}
	if (!target->economyHandle[0]) {
		Chance_Print(cl, va("%s ^7isn't logged in, so can't bet credits.", target->name));
		return;
	}
	if (bet <= 0) {
		Chance_Print(cl, "The bet must be more than 0 credits.");
		return;
	}
	if (cl->economyCredits < bet) {
		Chance_Print(cl, va("You only have %d credits.", cl->economyCredits));
		return;
	}

	chanceChallenge_t* ch = &gChanceChallenges[cl - svs.clients];
	ch->target = target - svs.clients;
	Q_strncpyz(ch->targetHandle, target->economyHandle, sizeof(ch->targetHandle));
	ch->bet = bet;
	ch->expires = svs.time + CHANCE_CHALLENGE_MS;

	Chance_Print(cl, va("You challenged %s ^7to Chance for %d credits. They pick red or blue...", target->name, bet));
	Chance_Print(target, va("%s ^7challenges you to Chance for ^2%d ^7credits each! Pick a colour: ^1!chance red ^7or ^4!chance blue^7, "
		"or ^5!chance decline ^7(60s).", cl->name, bet));
}

static void Chance_Answer(client_t* cl, const char* answer)
{
	const int me = cl - svs.clients;
	int from = -1;

	for (int i = 0; i < MAX_CLIENTS; i++) {
		const chanceChallenge_t* ch = &gChanceChallenges[i];
		if (ch->expires > svs.time && ch->target == me && !Q_stricmp(ch->targetHandle, cl->economyHandle)) {
			from = i;
			break;
		}
	}
	if (from < 0) {
		Chance_Print(cl, "Nobody's challenged you to Chance right now.");
		return;
	}

	chanceChallenge_t* ch = &gChanceChallenges[from];
	client_t* challenger = &svs.clients[from];
	const int bet = ch->bet;
	memset(ch, 0, sizeof(*ch));

	if (!Q_stricmp(answer, "decline")) {
		Chance_Print(cl, "Challenge declined.");
		if (challenger->state == CS_ACTIVE) {
			Chance_Print(challenger, va("%s ^7declined your Chance challenge.", cl->name));
		}
		return;
	}
	if (challenger->state != CS_ACTIVE || !challenger->economyHandle[0]) {
		Chance_Print(cl, "They've left.");
		return;
	}
	if (cl->economyCredits < bet || challenger->economyCredits < bet) {
		Chance_Print(cl, "One of you can't cover the bet any more.");
		Chance_Print(challenger, "One of you can't cover the Chance bet any more.");
		return;
	}

	const qboolean pickedRed = !Q_stricmp(answer, "red") ? qtrue : qfalse;
	const qboolean rolledRed = Q_irand(0, 1) ? qtrue : qfalse;
	client_t* winner = (pickedRed == rolledRed) ? cl : challenger;
	client_t* loser = (winner == cl) ? challenger : cl;

	loser->economyCredits -= bet;
	SV_EconomyPersistCredits(loser);
	winner->economyCredits += bet;
	SV_EconomyPersistCredits(winner);

	SV_SendServerCommand(NULL, "chat \"^3[Chance] ^7%s ^7(%s^7) vs %s ^7(%s^7) for ^2%d ^7credits... it's %s^7! %s ^7wins!\"\n",
		cl->name, pickedRed ? "^1red" : "^4blue", challenger->name, pickedRed ? "^4blue" : "^1red", bet * 2,
		rolledRed ? "^1RED" : "^4BLUE", winner->name);
}

qboolean SV_ChanceCommand(client_t* cl, const char* args)
{
	char a[64], b[64];
	const int argc = sscanf(args, "%63s %63s", a, b);

	if (!g_economyChanceEnable || !g_economyChanceEnable->integer) {
		Chance_Print(cl, "There's no Chance on this server.");
		return qtrue;
	}
	if (argc < 1) {
		Chance_Print(cl, "^5!chance <player> <credits> ^7to challenge someone. They pick red or blue, the server rolls, "
			"and the winning colour takes the pot.");
		return qtrue;
	}
	if (!Q_stricmp(a, "red") || !Q_stricmp(a, "blue") || !Q_stricmp(a, "decline")) {
		Chance_Answer(cl, a);
		return qtrue;
	}
	if (argc < 2) {
		Chance_Print(cl, "Usage: ^5!chance <player> <credits>");
		return qtrue;
	}
	Chance_Challenge(cl, a, b);
	return qtrue;
}
