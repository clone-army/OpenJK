/*
===========================================================================
raffle.cpp — the raffle, part of the economy (!raffle)

A draw every g_raffleIntervalMinutes (default 60, on the clock - so on the
hour by default). Ticket sales open g_raffleOpenMinutes (default 10) before
each draw, announced with the ticket price (g_raffleTicketPrice) and how to
buy, with reminders at 5 minutes and 1 minute. "!raffle <count>" buys
tickets (logged-in players only), "!raffle" shows the pool, your tickets
and the time left. One ticket is drawn at random and its owner gets the
whole pool, paid straight into their account so it counts even if they've
gone. At least g_raffleMinEntrants different players (default 5) have to
enter for a draw to happen; otherwise everyone gets their credits back.
Switched on with g_economyRaffleEnable.

Tickets are keyed by account handle - reconnecting keeps them - and saved
to fs_homepath/fs_game/raffle_state.dat on every sale, so a restart during
an open raffle doesn't lose anyone's credits: it picks up where it left off
(drawing straight away if the draw time passed while it was down).
===========================================================================
*/

#include "server.h"
#include <time.h>

#define RAFFLE_MAX_ENTRIES  256
#define RAFFLE_STATE_FILE   "raffle_state.dat"

typedef struct {
	char handle[24];
	int  tickets;
	int  paid;                 // exact credits spent, for refunds
} raffleEntry_t;

static struct {
	qboolean loaded;
	time_t   drawTime;
	qboolean open;
	int      remindedAt;       // minutes-left mark last announced
	int      pool;
	int      entryCount;
	raffleEntry_t entries[RAFFLE_MAX_ENTRIES];
	time_t   lastTick;
} gRaffle;

static void Raffle_Announce(const char* text)
{
	SV_SendServerCommand(NULL, "chat \"^6[Raffle]^7 %s\"\n", text);
}

static void Raffle_Print(client_t* cl, const char* text)
{
	SV_SendServerCommand(cl, "chat \"^6[Raffle]^7 %s\"\n", text);
}

static int Raffle_IntervalSecs(void)
{
	return Q_max(5, g_raffleIntervalMinutes ? g_raffleIntervalMinutes->integer : 60) * 60;
}

static int Raffle_OpenSecs(void)
{
	const int open = Q_max(1, g_raffleOpenMinutes ? g_raffleOpenMinutes->integer : 10) * 60;
	return Q_min(open, Raffle_IntervalSecs());
}

static int Raffle_MinEntrants(void)
{
	return Q_max(1, g_raffleMinEntrants ? g_raffleMinEntrants->integer : 5);
}

static int Raffle_Price(void)
{
	return Q_max(1, g_raffleTicketPrice ? g_raffleTicketPrice->integer : 5);
}

static time_t Raffle_NextDraw(time_t now)
{
	const int iv = Raffle_IntervalSecs();
	return (now / iv + 1) * iv;
}

static void Raffle_Path(char* out, int size)
{
	Com_sprintf(out, size, "%s/%s/%s", Cvar_VariableString("fs_homepath"), Cvar_VariableString("fs_game"), RAFFLE_STATE_FILE);
}

static void Raffle_Save(void)
{
	char path[MAX_OSPATH];
	Raffle_Path(path, sizeof(path));
	FILE* f = fopen(path, "w");
	if (!f) {
		Com_Printf("Raffle: couldn't save %s\n", path);
		return;
	}
	fprintf(f, "%ld %d\n", (long)gRaffle.drawTime, gRaffle.pool);
	for (int i = 0; i < gRaffle.entryCount; i++) {
		fprintf(f, "%s %d %d\n", gRaffle.entries[i].handle, gRaffle.entries[i].tickets, gRaffle.entries[i].paid);
	}
	fclose(f);
}

static void Raffle_Load(void)
{
	char path[MAX_OSPATH];
	long drawTime;
	int pool;

	gRaffle.loaded = qtrue;
	Raffle_Path(path, sizeof(path));
	FILE* f = fopen(path, "r");
	if (!f) {
		return;
	}
	if (fscanf(f, "%ld %d", &drawTime, &pool) == 2 && pool > 0) {
		gRaffle.drawTime = (time_t)drawTime;
		gRaffle.pool = pool;
		char handle[24];
		int tickets, paid;
		while (gRaffle.entryCount < RAFFLE_MAX_ENTRIES && fscanf(f, "%23s %d %d", handle, &tickets, &paid) == 3) {
			if (tickets > 0) {
				Q_strncpyz(gRaffle.entries[gRaffle.entryCount].handle, handle, sizeof(handle));
				gRaffle.entries[gRaffle.entryCount].tickets = tickets;
				gRaffle.entries[gRaffle.entryCount].paid = paid;
				gRaffle.entryCount++;
			}
		}
		Com_Printf("Raffle: restored %d entries, pool %d\n", gRaffle.entryCount, gRaffle.pool);
	}
	fclose(f);
}

static void Raffle_Reset(time_t now)
{
	gRaffle.open = qfalse;
	gRaffle.remindedAt = 0;
	gRaffle.pool = 0;
	gRaffle.entryCount = 0;
	gRaffle.drawTime = Raffle_NextDraw(now);
	Raffle_Save();
}

static int Raffle_TicketsOf(const char* handle)
{
	for (int i = 0; i < gRaffle.entryCount; i++) {
		if (!Q_stricmp(gRaffle.entries[i].handle, handle)) {
			return gRaffle.entries[i].tickets;
		}
	}
	return 0;
}

static int Raffle_TotalTickets(void)
{
	int total = 0;
	for (int i = 0; i < gRaffle.entryCount; i++) {
		total += gRaffle.entries[i].tickets;
	}
	return total;
}

static const char* Raffle_TimeLeft(time_t now)
{
	const int secs = (int)Q_max(0, (long)(gRaffle.drawTime - now));
	return va("%d:%02d", secs / 60, secs % 60);
}

static void Raffle_Draw(time_t now)
{
	const int total = Raffle_TotalTickets();
	if (total <= 0) {
		if (gRaffle.open) {
			Raffle_Announce("No tickets were bought, so no draw this time.");
		}
		Raffle_Reset(now);
		return;
	}

	if (gRaffle.entryCount < Raffle_MinEntrants()) {
		for (int i = 0; i < gRaffle.entryCount; i++) {
			SV_EconomyAddCreditsToAccount(gRaffle.entries[i].handle, gRaffle.entries[i].paid);
			SV_GameResult("raffle", gRaffle.entries[i].handle, gRaffle.entries[i].handle, "refund", gRaffle.entries[i].paid, 0, "",
				"too few entered");
		}
		Raffle_Announce(va("Only %d %s entered - the raffle needs at least %d. Everyone's credits have been refunded.",
			gRaffle.entryCount, gRaffle.entryCount == 1 ? "person" : "people", Raffle_MinEntrants()));
		Raffle_Reset(now);
		return;
	}

	int pick = Q_irand(0, total - 1);
	const raffleEntry_t* winner = &gRaffle.entries[0];
	for (int i = 0; i < gRaffle.entryCount; i++) {
		if (pick < gRaffle.entries[i].tickets) {
			winner = &gRaffle.entries[i];
			break;
		}
		pick -= gRaffle.entries[i].tickets;
	}

	// Into the account itself: a logged-in session picks the change up
	// through the economy's normal sync, and it counts if they've left.
	SV_EconomyAddCreditsToAccount(winner->handle, gRaffle.pool);
	for (int i = 0; i < gRaffle.entryCount; i++) {
		const raffleEntry_t* e = &gRaffle.entries[i];
		SV_GameResult("raffle", e->handle, e->handle, e == winner ? "win" : "loss", e->paid,
			e == winner ? gRaffle.pool - e->paid : -e->paid, "", va("%d of %d tickets, pool %d", e->tickets, total, gRaffle.pool));
	}

	// Show the winner's in-game name if they're still here.
	const char* name = winner->handle;
	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		if (cl->state == CS_ACTIVE && !Q_stricmp(cl->economyHandle, winner->handle)) {
			name = cl->name;
			break;
		}
	}
	Raffle_Announce(va("^3%s ^7wins the raffle - ^2%d ^7credits! (%d tickets sold)", name, gRaffle.pool, total));
	SV_SendServerCommand(NULL, "cp \"%s\n^7wins the raffle - ^2%d ^7credits!\"\n", name, gRaffle.pool);
	Raffle_Reset(now);
}

qboolean SV_RaffleCommand(client_t* cl, const char* args)
{
	char first[32];
	const time_t now = time(NULL);

	if (!g_economyRaffleEnable || !g_economyRaffleEnable->integer) {
		Raffle_Print(cl, "There's no raffle on this server.");
		return qtrue;
	}
	if (!gRaffle.loaded || !gRaffle.drawTime) {
		Raffle_Print(cl, "The raffle is just getting set up - try again in a moment.");
		return qtrue;
	}

	if (sscanf(args, "%31s", first) != 1) {
		if (!gRaffle.open) {
			const int opensIn = (int)(gRaffle.drawTime - Raffle_OpenSecs() - now);
			Raffle_Print(cl, va("Closed. Tickets go on sale in %d:%02d (^2%d ^7cr each), draw %d minutes after that.",
				Q_max(0, opensIn) / 60, Q_max(0, opensIn) % 60, Raffle_Price(), Raffle_OpenSecs() / 60));
		} else {
			Raffle_Print(cl, va("Pool: ^2%d ^7credits. Your tickets: ^3%d^7 of %d. %d/%d people entered. Draw in %s.",
				gRaffle.pool, Raffle_TicketsOf(cl->economyHandle), Raffle_TotalTickets(), gRaffle.entryCount,
				Raffle_MinEntrants(), Raffle_TimeLeft(now)));
			Raffle_Print(cl, va("^5!raffle <count> ^7to buy (^2%d ^7cr each).", Raffle_Price()));
		}
		return qtrue;
	}

	if (!gRaffle.open) {
		Raffle_Print(cl, "Tickets aren't on sale yet - type ^5!raffle ^7to see when they are.");
		return qtrue;
	}

	const int count = atoi(first);
	if (count < 1 || count > 1000) {
		Raffle_Print(cl, "Buy between 1 and 1000 tickets, e.g. ^5!raffle 10");
		return qtrue;
	}
	const int cost = count * Raffle_Price();
	if (cl->economyCredits < cost) {
		Raffle_Print(cl, va("%d tickets cost %d credits - you have %d.", count, cost, cl->economyCredits));
		return qtrue;
	}

	raffleEntry_t* e = NULL;
	for (int i = 0; i < gRaffle.entryCount; i++) {
		if (!Q_stricmp(gRaffle.entries[i].handle, cl->economyHandle)) {
			e = &gRaffle.entries[i];
			break;
		}
	}
	if (!e) {
		if (gRaffle.entryCount >= RAFFLE_MAX_ENTRIES) {
			Raffle_Print(cl, "The raffle's full this time, sorry.");
			return qtrue;
		}
		e = &gRaffle.entries[gRaffle.entryCount++];
		Q_strncpyz(e->handle, cl->economyHandle, sizeof(e->handle));
		e->tickets = 0;
		e->paid = 0;
	}

	cl->economyCredits -= cost;
	SV_EconomyPersistCredits(cl);
	e->tickets += count;
	e->paid += cost;
	gRaffle.pool += cost;
	Raffle_Save();

	Raffle_Print(cl, va("You bought %d tickets - you now have ^3%d^7. Pool: ^2%d^7. Draw in %s.",
		count, e->tickets, gRaffle.pool, Raffle_TimeLeft(now)));
	Raffle_Announce(va("%s ^7bought %d tickets - the pool is now ^2%d ^7credits!", cl->name, count, gRaffle.pool));
	return qtrue;
}

void SV_RaffleFrame(void)
{
	if (!g_economyRaffleEnable || !g_economyRaffleEnable->integer) {
		return;
	}
	const time_t now = time(NULL);
	if (now == gRaffle.lastTick) {
		return; // once a second is plenty
	}
	gRaffle.lastTick = now;

	if (!gRaffle.loaded) {
		Raffle_Load();
		if (!gRaffle.drawTime) {
			Raffle_Reset(now);
		}
	}

	if (now >= gRaffle.drawTime) {
		Raffle_Draw(now);
		return;
	}

	const int left = (int)(gRaffle.drawTime - now);
	if (!gRaffle.open && left <= Raffle_OpenSecs()) {
		gRaffle.open = qtrue;
		gRaffle.remindedAt = left / 60 + 1;
		Raffle_Announce(va("^3The raffle is open!^7 Tickets are ^2%d ^7credits each - type ^5!raffle <count>^7, e.g. ^5!raffle 10^7.",
			Raffle_Price()));
		Raffle_Announce(va("Draw in %d minutes. Pool so far: ^2%d ^7credits. Winner takes the lot - needs at least %d people to enter, "
			"or everyone's refunded.", (left + 59) / 60, gRaffle.pool, Raffle_MinEntrants()));
		return;
	}

	// Reminders at 5 minutes and 1 minute to go.
	if (gRaffle.open) {
		const int mark = (left <= 60) ? 1 : (left <= 300) ? 5 : 0;
		if (mark && mark < gRaffle.remindedAt) {
			gRaffle.remindedAt = mark;
			Raffle_Announce(va("%d minute%s left! Pool: ^2%d ^7credits, %d/%d people in. ^5!raffle <count> ^7to buy tickets (^2%d ^7cr each).",
				mark, mark == 1 ? "" : "s", gRaffle.pool, gRaffle.entryCount, Raffle_MinEntrants(), Raffle_Price()));
		}
	}
}
