/*
===========================================================================
bar.cpp — the Cantina bar, part of the economy (!bar)

"!bar" lists the drinks, "!bar <number>" buys one for yourself, and
"!bar round <number>" buys one for every living player on the server
(price x players served). Every order is announced in chat. Switched on
with g_economyBarEnable (on top of the g_creditSystemEnable master switch,
like !buy and !bounty); each drink's price is its own g_barCost_<id> cvar,
0 taking it off the menu.

There's no drinking animation in JKA or MBII, so drinks do something
instead, all through stock playerState_t fields - the same ones spin.cpp's
prizes already rely on in production:

  - size drinks set iModelScale, and put back the class's own scale (MBII
    sets that per class on spawn) when they wear off
  - armour, bacta and cloak are the same stats/holdable writes as spin.cpp;
    jetpack and shockfield go through SV_SpinForceGiveWin since they need
    MBII's cheat-gated "give"
  - Corellian Whiskey makes you drunk: delta_angles is nudged every frame so
    your view sways (the same field the game uses to turn a player's view on
    teleport), and ps->velocity gets the odd sideways shove so you stagger.
    Every nudge is tracked and undone exactly when it wears off

Each drink also makes you glow while it's working (or for 30 seconds, for
the ones that just hand you an item): one of MBII's own powerup visuals -
the Force boon/enlightenment/endarkenment shells, the ysalamiri shell, the
Galak shield, the electric crackle or plasma flames - switched on by
setting its bit in the entityState_t powerups mask cgame draws from
(cg_players.c). The game rebuilds that mask from playerState every frame,
so it's re-applied after each game frame, and it's display-only: pmove and
the rest of the game read playerState, which is never touched.

Timed effects only ever last for the life they were bought in: a new life
(persistant[PERS_SPAWN_COUNT], which MBII bumps on every spawn - same index
in MBII and the engine) means MBII has already reset the player, so the
effect is dropped without touching anything.
===========================================================================
*/

#include "server.h"
#include "spin.h"

#define BAR_SIZE_MS        120000
#define BAR_DRUNK_MS       60000
#define BAR_GLOW_MS        30000    // glow for drinks that just hand you an item
#define BAR_MB2_HI_CLOAK   9        // MBII's holdable index for the cloak (spin.cpp: MB2_HI_CLOAK)

// MBII's powerup numbers (bg_public.h), which differ from base JKA's: these
// are what MBII's cgame checks in entityState_t.powerups to draw each glow.
#define BAR_GLOW_NONE      -1
#define BAR_PW_BATTLESUIT  2        // plasma flames
#define BAR_PW_ENLIGHT     12       // white-blue shell (Force enlightenment)
#define BAR_PW_ENDARK      13       // red shell (Force endarkenment)
#define BAR_PW_BOON        14       // gold shell (Force boon)
#define BAR_PW_YSALAMIRI   15       // green shell
#define BAR_PW_ELECTRIFY   19       // electric crackle
#define BAR_PW_GALAK       21       // shimmering blue shield
#define BAR_PW_COUNT       32

typedef enum {
	BAR_SCALE,      // value = iModelScale while it lasts
	BAR_ARMOR,      // value = armour added
	BAR_BACTA,
	BAR_CLOAK,
	BAR_DRUNK,
	BAR_WIN,        // value = spin_wins_t, granted via SV_SpinForceGiveWin
} barEffect_t;

typedef struct {
	const char* id;         // cvar suffix: g_barCost_<id>
	const char* name;
	const char* blurb;      // what it does, shown on the menu and to the drinker
	int         costDefault;
	barEffect_t effect;
	int         value;
	int         glow;       // BAR_PW_*, or BAR_GLOW_NONE
} barDrink_t;

// Order is the menu numbering, so only ever append.
static const barDrink_t kBarDrinks[] = {
	{ "jawa_juice",        "Jawa Juice",        "shrinks you to Jawa size for 2 minutes", 10, BAR_SCALE, 50,             BAR_PW_BOON },
	{ "hutt_brew",         "Hutt Brew",         "makes you huge for 2 minutes",           15, BAR_SCALE, 175,            BAR_PW_YSALAMIRI },
	{ "blue_milk",         "Blue Milk",         "+100 armour",                            10, BAR_ARMOR, 100,            BAR_PW_GALAK },
	{ "bacta_shot",        "Bacta Shot",        "a bacta tank to heal up with",            8, BAR_BACTA, 0,              BAR_PW_ENLIGHT },
	{ "spotchka",          "Spotchka",          "a cloak generator - vanish at will",     20, BAR_CLOAK, 0,              BAR_GLOW_NONE },
	{ "corellian_whiskey", "Corellian Whiskey", "you get properly drunk for a minute",    12, BAR_DRUNK, 0,              BAR_PW_ENDARK },
	{ "ion_fizz",          "Ion Fizz",          "a shockfield",                           20, BAR_WIN,   WIN_SHOCKFIELD, BAR_PW_ELECTRIFY },
	{ "jet_juice",         "Jet Juice",         "a jetpack",                              22, BAR_WIN,   WIN_JETPACK,    BAR_PW_BATTLESUIT },
};

static cvar_t* gBarCostCvars[ARRAY_LEN(kBarDrinks)];

typedef struct {
	int spawnCount;         // the life these effects belong to
	int scaleUntil;
	int scaleOriginal;
	int scaleSet;
	int drunkUntil;
	int swayYaw;            // delta_angles offset currently applied, in SHORT units
	int swayPitch;
	int nextStumble;
	int glowUntil[BAR_PW_COUNT];
} barState_t;

static barState_t gBarState[MAX_CLIENTS];

void SV_BarInitCvars(void)
{
	for (int i = 0; i < (int)ARRAY_LEN(kBarDrinks); i++) {
		char name[64], def[16], desc[128];
		Com_sprintf(name, sizeof(name), "g_barCost_%s", kBarDrinks[i].id);
		Com_sprintf(def, sizeof(def), "%d", kBarDrinks[i].costDefault);
		Com_sprintf(desc, sizeof(desc), "Bar price in credits for %s (0 = off the menu)", kBarDrinks[i].name);
		gBarCostCvars[i] = Cvar_Get(name, def, CVAR_ARCHIVE, desc);
	}
}

static int Bar_Cost(int i)
{
	return gBarCostCvars[i] ? gBarCostCvars[i]->integer : 0;
}

static qboolean Bar_IsAlive(client_t* cl)
{
	if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
		return qfalse;
	}
	const playerState_t* ps = cl->gentity->playerState;
	return (ps->persistant[PERS_TEAM] != TEAM_SPECTATOR && ps->stats[STAT_HEALTH] > 0) ? qtrue : qfalse;
}

// Effects from an earlier life are MBII's to have reset; forget them.
static barState_t* Bar_StateFor(client_t* cl)
{
	barState_t* st = &gBarState[cl - svs.clients];
	const int spawnCount = cl->gentity->playerState->persistant[PERS_SPAWN_COUNT];
	if (st->spawnCount != spawnCount) {
		memset(st, 0, sizeof(*st));
		st->spawnCount = spawnCount;
	}
	return st;
}

static void Bar_Apply(client_t* cl, int drink)
{
	const barDrink_t* d = &kBarDrinks[drink];
	playerState_t* ps = cl->gentity->playerState;
	barState_t* st = Bar_StateFor(cl);

	switch (d->effect) {
		case BAR_SCALE:
			if (!st->scaleUntil) {
				st->scaleOriginal = ps->iModelScale;
			}
			ps->iModelScale = d->value;
			st->scaleSet = d->value;
			st->scaleUntil = svs.time + BAR_SIZE_MS;
			break;
		case BAR_ARMOR:
			ps->stats[STAT_ARMOR] = Com_Clampi(0, 999, ps->stats[STAT_ARMOR] + d->value);
			break;
		case BAR_BACTA:
			ps->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_MEDPAC_BIG);
			break;
		case BAR_CLOAK:
			ps->stats[STAT_HOLDABLE_ITEMS] |= (1 << BAR_MB2_HI_CLOAK);
			ps->cloakFuel = 100;
			break;
		case BAR_DRUNK:
			if (!st->drunkUntil) {
				st->nextStumble = svs.time + 2000;
			}
			st->drunkUntil = svs.time + BAR_DRUNK_MS;
			break;
		case BAR_WIN:
			SV_SpinForceGiveWin(cl, d->value);
			break;
	}

	if (d->glow != BAR_GLOW_NONE) {
		const int ms = (d->effect == BAR_SCALE) ? BAR_SIZE_MS : (d->effect == BAR_DRUNK) ? BAR_DRUNK_MS : BAR_GLOW_MS;
		st->glowUntil[d->glow] = svs.time + ms;
	}
}

static qboolean Bar_HasGlow(const barState_t* st)
{
	for (int b = 0; b < BAR_PW_COUNT; b++) {
		if (st->glowUntil[b] > svs.time) {
			return qtrue;
		}
	}
	return qfalse;
}

static void Bar_ShowMenu(client_t* cl)
{
	char line[ECONOMY_MENU_LINE_SIZE];

	SV_EconomyMenuBegin(cl);
	Com_sprintf(line, sizeof(line), "^3=== THE BAR === Balance: ^2%d ^3credits ===", cl->economyCredits);
	SV_EconomyMenuAddLine(cl, line);

	for (int i = 0; i < (int)ARRAY_LEN(kBarDrinks); i++) {
		if (Bar_Cost(i) <= 0) {
			continue;
		}
		Com_sprintf(line, sizeof(line), "^3%d^7. ^5%s ^7- %s - ^2%d ^7cr", i + 1, kBarDrinks[i].name, kBarDrinks[i].blurb, Bar_Cost(i));
		SV_EconomyMenuAddLine(cl, line);
	}

	SV_EconomyMenuAddLine(cl, "^7Type ^5!bar <number> ^7to order, e.g. ^5!bar 1");
	SV_EconomyMenuAddLine(cl, "^7Or ^5!bar round <number> ^7to buy one for everyone (price x players)");
	SV_EconomyMenuPump(cl);
}

static int Bar_ParseDrink(client_t* cl, const char* arg)
{
	const int n = atoi(arg);
	if (n < 1 || n > (int)ARRAY_LEN(kBarDrinks) || Bar_Cost(n - 1) <= 0) {
		SV_EconomyPrint(cl, "That's not on the menu. Type !bar to see the drinks.");
		return -1;
	}
	return n - 1;
}

qboolean SV_BarCommand(client_t* cl, const char* args)
{
	char first[32], second[32];
	const int argc = sscanf(args, "%31s %31s", first, second);

	if (!g_economyBarEnable || !g_economyBarEnable->integer) {
		SV_EconomyPrint(cl, "The bar is closed on this server.");
		return qtrue;
	}

	if (argc < 1) {
		Bar_ShowMenu(cl);
		return qtrue;
	}

	if (!Q_stricmp(first, "round")) {
		if (argc < 2) {
			SV_EconomyPrint(cl, "Usage: !bar round <number>, e.g. !bar round 1");
			return qtrue;
		}
		const int drink = Bar_ParseDrink(cl, second);
		if (drink < 0) {
			return qtrue;
		}

		int served = 0;
		for (int i = 0; i < sv_maxclients->integer; i++) {
			served += Bar_IsAlive(&svs.clients[i]);
		}
		if (!served) {
			SV_EconomyPrint(cl, "There's nobody here to buy a round for.");
			return qtrue;
		}

		const int cost = Bar_Cost(drink) * served;
		if (cl->economyCredits < cost) {
			SV_EconomyPrint(cl, va("A round for %d costs %d credits - you have %d.", served, cost, cl->economyCredits));
			return qtrue;
		}

		cl->economyCredits -= cost;
		SV_EconomyPersistCredits(cl);
		for (int i = 0; i < sv_maxclients->integer; i++) {
			if (Bar_IsAlive(&svs.clients[i])) {
				Bar_Apply(&svs.clients[i], drink);
			}
		}
		SV_SendServerCommand(NULL, "chat \"^5[Bar] ^7%s ^7buys a round of ^3%s ^7for everyone! ^7(%s)\"\n",
			cl->name, kBarDrinks[drink].name, kBarDrinks[drink].blurb);
		SV_EconomyPrint(cl, va("Round bought for %d players. New balance: %d", served, cl->economyCredits));
		return qtrue;
	}

	const int drink = Bar_ParseDrink(cl, first);
	if (drink < 0) {
		return qtrue;
	}
	if (!Bar_IsAlive(cl)) {
		SV_EconomyPrint(cl, "You need to be alive to order a drink.");
		return qtrue;
	}
	if (cl->economyCredits < Bar_Cost(drink)) {
		SV_EconomyPrint(cl, va("Not enough credits. Need %d, have %d.", Bar_Cost(drink), cl->economyCredits));
		return qtrue;
	}

	cl->economyCredits -= Bar_Cost(drink);
	SV_EconomyPersistCredits(cl);
	Bar_Apply(cl, drink);
	SV_SendServerCommand(NULL, "chat \"^5[Bar] ^7%s ^7orders a ^3%s^7!\"\n", cl->name, kBarDrinks[drink].name);
	SV_EconomyPrint(cl, va("%s: %s. New balance: %d", kBarDrinks[drink].name, kBarDrinks[drink].blurb, cl->economyCredits));
	return qtrue;
}

static void Bar_DrunkFrame(client_t* cl, barState_t* st)
{
	playerState_t* ps = cl->gentity->playerState;

	if (svs.time >= st->drunkUntil) {
		ps->delta_angles[YAW] -= st->swayYaw;
		ps->delta_angles[PITCH] -= st->swayPitch;
		st->drunkUntil = 0;
		st->swayYaw = 0;
		st->swayPitch = 0;
		st->nextStumble = 0;
		SV_EconomyPrint(cl, "You sober up.");
		return;
	}

	// Two slow, out-of-step waves so it wanders rather than ticks.
	const float t = svs.time / 1000.0f;
	const int wantYaw = ANGLE2SHORT(6.0f * sinf(t * 1.7f) + 2.5f * sinf(t * 3.1f));
	const int wantPitch = ANGLE2SHORT(3.5f * sinf(t * 1.3f));
	ps->delta_angles[YAW] += wantYaw - st->swayYaw;
	ps->delta_angles[PITCH] += wantPitch - st->swayPitch;
	st->swayYaw = wantYaw;
	st->swayPitch = wantPitch;

	if (svs.time >= st->nextStumble) {
		if (ps->groundEntityNum != ENTITYNUM_NONE) {
			const float a = Q_irand(0, 359) * (M_PI / 180.0f);
			ps->velocity[0] += cosf(a) * 110.0f;
			ps->velocity[1] += sinf(a) * 110.0f;
		}
		st->nextStumble = svs.time + Q_irand(1500, 3500);
	}
}

void SV_BarFrame(void)
{
	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		barState_t* st = &gBarState[i];

		if (!st->scaleUntil && !st->drunkUntil && !Bar_HasGlow(st)) {
			continue;
		}
		if (cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
			memset(st, 0, sizeof(*st));
			continue;
		}
		playerState_t* ps = cl->gentity->playerState;
		if (ps->persistant[PERS_SPAWN_COUNT] != st->spawnCount) {
			memset(st, 0, sizeof(*st)); // new life: MBII has reset them already
			continue;
		}

		if (st->scaleUntil && svs.time >= st->scaleUntil) {
			if (ps->iModelScale == st->scaleSet) {
				ps->iModelScale = st->scaleOriginal;
			}
			st->scaleUntil = 0;
			SV_EconomyPrint(cl, "Your drink wears off - back to normal size.");
		}

		// Dead: stop swaying, but leave delta_angles alone - the respawn
		// resets the view, and the new life's check above clears us.
		if (st->drunkUntil && ps->stats[STAT_HEALTH] > 0) {
			Bar_DrunkFrame(cl, st);
		}

		if (ps->stats[STAT_HEALTH] > 0) {
			for (int b = 0; b < BAR_PW_COUNT; b++) {
				if (st->glowUntil[b] > svs.time) {
					cl->gentity->s.powerups |= (1 << b);
				}
			}
		}
	}
}
