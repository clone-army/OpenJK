/*
===========================================================================
bar.cpp — the Cantina bar, part of the economy (!bar)

"!bar" lists the drinks and "!bar <number>" orders one; every order is
announced in chat. Switched on with g_economyBarEnable (on top of the
g_creditSystemEnable master switch, like !buy and !bounty); each drink's
price is its own g_barCost_<id> cvar, 0 taking it off the menu.

There's no drinking animation in JKA or MBII, and the bar is meant for the
no-damage social server, so drinks are all about how you look, move and
steer - nothing that matters in a fight. Everything goes through stock
playerState_t / entityState_t / usercmd_t fields:

  - size drinks set iModelScale (the same field spin.cpp's size prizes use),
    and put back the class's own scale, which MBII sets per class on spawn
  - Corellian Whiskey makes you drunk: delta_angles is nudged every frame so
    your view sways (the field the game itself uses to turn a player's view
    on teleport), and ps->velocity gets regular shoves so you stagger. Every
    nudge is tracked and undone exactly when it wears off. Tatooine Twister
    spins your view the same way
  - Bubble Brew (hiccup hops), Moon Milk (part of gravity cancelled while
    airborne) and Sugar Rush (ground speed boosted) push ps->velocity after
    each game frame
  - the control drinks rewrite the player's usercmd_t before the game module
    ever sees it (SV_BarClientThink, called from SV_ClientThink like gun
    game's weapon clamp): reversed, slowed, stuck running forward, stuck
    crouching. The client still predicts its own unaltered input, so these
    feel a little rubbery - which rather suits a drink
  - the rest are looks: one of MBII's own powerup visuals - the cloak
    shimmer, the Hoth freeze (slowed animation), flames, electric crackle -
    switched on by setting its bit in the entityState_t powerups mask cgame
    draws from (cg_players.c). The game rebuilds that mask from playerState
    every frame, so it's re-applied after each game frame; it's display-only,
    as pmove and the rest of the game read playerState, never touched here.
    Every drink also gives a glow the same way while it's working.

Effects only ever last for the life they were bought in: a new life
(persistant[PERS_SPAWN_COUNT], which MBII bumps on every spawn - same index
in MBII and the engine) means MBII has already reset the player, so the
effect is dropped without touching anything.
===========================================================================
*/

#include "server.h"

#define BAR_MB2_PW_COUNT   32

// MBII's powerup numbers (bg_public.h), which differ from base JKA's: these
// are what MBII's cgame checks in entityState_t.powerups to draw each one.
#define BAR_LOOK_NONE      -1
#define BAR_PW_FLAMES      1        // PW_QUAD, "hijacked for flameburning effects"
#define BAR_PW_CLOAKED     11       // cloak shimmer
#define BAR_PW_ENLIGHT     12       // white-blue shell (Force enlightenment)
#define BAR_PW_ENDARK      13       // red shell (Force endarkenment)
#define BAR_PW_BOON        14       // gold shell (Force boon)
#define BAR_PW_YSALAMIRI   15       // green shell
#define BAR_PW_ELECTRIFY   19       // electric crackle
#define BAR_PW_FREEZE      20       // Hoth freeze: slowed-down animation
#define BAR_PW_GALAK       21       // shimmering blue shield

#define BAR_MOON_GRAVITY_CANCEL  0.65f   // share of gravity Moon Milk cancels in the air
#define BAR_RUSH_MAX_SPEED       900.0f  // Sugar Rush ground speed cap (normal run is ~250)
#define BAR_RUSH_BOOST           1.12f   // per server frame, while moving on the ground

typedef enum {
	BAR_SCALE,      // value = iModelScale while it lasts
	BAR_DRUNK,
	BAR_SPIN,
	BAR_HICCUP,
	BAR_MOON,
	BAR_RUSH,
	BAR_SLOW,
	BAR_REVERSE,
	BAR_RUNAWAY,
	BAR_CROUCH,
	BAR_LOOK,       // just the visual in .look
	BAR_NUM_EFFECTS
} barEffect_t;

typedef struct {
	const char* id;         // cvar suffix: g_barCost_<id>
	const char* name;
	const char* blurb;      // what it does, shown on the menu and to the drinker
	int         costDefault;
	barEffect_t effect;
	int         value;
	int         seconds;
	int         look;       // BAR_PW_* powerup visual for the main effect, or BAR_LOOK_NONE
	int         glow;       // BAR_PW_* shell/aura while it lasts, or BAR_LOOK_NONE
} barDrink_t;

// Order is the menu numbering.
static const barDrink_t kBarDrinks[] = {
	{ "jawa_juice",        "Jawa Juice",        "shrinks you to Jawa size for 2 minutes",       10, BAR_SCALE,   50,  120, BAR_LOOK_NONE,    BAR_PW_BOON },
	{ "hutt_brew",         "Hutt Brew",         "makes you huge for 2 minutes",                 15, BAR_SCALE,   175, 120, BAR_LOOK_NONE,    BAR_PW_YSALAMIRI },
	{ "corellian_whiskey", "Corellian Whiskey", "gets you properly drunk for 90 seconds",       12, BAR_DRUNK,   0,   90,  BAR_LOOK_NONE,    BAR_PW_ENDARK },
	{ "tatooine_twister",  "Tatooine Twister",  "your head spins for 20 seconds",               10, BAR_SPIN,    0,   20,  BAR_LOOK_NONE,    BAR_PW_GALAK },
	{ "bubble_brew",       "Bubble Brew",       "hiccups - you hop about for a minute",         10, BAR_HICCUP,  0,   60,  BAR_LOOK_NONE,    BAR_PW_ENLIGHT },
	{ "moon_milk",         "Moon Milk",         "low gravity for a minute",                     12, BAR_MOON,    0,   60,  BAR_LOOK_NONE,    BAR_PW_ENLIGHT },
	{ "sugar_rush",        "Sugar Rush",        "super speed for 30 seconds",                   15, BAR_RUSH,    0,   30,  BAR_PW_ELECTRIFY, BAR_PW_BOON },
	{ "bantha_sludge",     "Bantha Sludge",     "you can barely move for a minute",             10, BAR_SLOW,    0,   60,  BAR_LOOK_NONE,    BAR_PW_YSALAMIRI },
	{ "backwards_brandy",  "Backwards Brandy",  "your controls are reversed for a minute",      12, BAR_REVERSE, 0,   60,  BAR_LOOK_NONE,    BAR_PW_ENDARK },
	{ "runaway_rum",       "Runaway Rum",       "you can't stop running for 30 seconds",        12, BAR_RUNAWAY, 0,   30,  BAR_LOOK_NONE,    BAR_PW_GALAK },
	{ "low_ceiling_lager", "Low-Ceiling Lager", "stuck crouching for a minute",                 10, BAR_CROUCH,  0,   60,  BAR_LOOK_NONE,    BAR_PW_BOON },
	{ "spotchka",          "Spotchka",          "you shimmer nearly invisible for 45 seconds",  20, BAR_LOOK,    0,   45,  BAR_PW_CLOAKED,   BAR_LOOK_NONE },
	{ "hoth_chiller",      "Hoth Chiller",      "frozen in slow motion for a minute",           12, BAR_LOOK,    0,   60,  BAR_PW_FREEZE,    BAR_LOOK_NONE },
	{ "mustafar_magma",    "Mustafar Magma",    "you're on fire (just for show) for a minute",  12, BAR_LOOK,    0,   60,  BAR_PW_FLAMES,    BAR_LOOK_NONE },
	{ "ion_fizz",          "Ion Fizz",          "you crackle with electricity for a minute",    12, BAR_LOOK,    0,   60,  BAR_PW_ELECTRIFY, BAR_LOOK_NONE },
};

static cvar_t* gBarCostCvars[ARRAY_LEN(kBarDrinks)];

typedef struct {
	int spawnCount;         // the life these effects belong to
	int until[BAR_NUM_EFFECTS];
	int scaleOriginal;
	int scaleSet;
	int swayYaw;            // drunk: delta_angles offset currently applied, in SHORT units
	int swayPitch;
	int nextStumble;
	int spinLastTime;
	int nextHiccup;
	int lastFrameTime;
	int visualUntil[BAR_MB2_PW_COUNT];
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
	const int team = ps->persistant[PERS_TEAM];
	return ((team == TEAM_RED || team == TEAM_BLUE) && ps->stats[STAT_HEALTH] > 0) ? qtrue : qfalse;
}

static qboolean Bar_Active(const barState_t* st, barEffect_t e)
{
	return (st->until[e] > svs.time) ? qtrue : qfalse;
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
	const int until = svs.time + d->seconds * 1000;
	const qboolean fresh = st->until[d->effect] ? qfalse : qtrue;

	switch (d->effect) {
		case BAR_SCALE:
			if (fresh) {
				st->scaleOriginal = ps->iModelScale;
			}
			ps->iModelScale = d->value;
			st->scaleSet = d->value;
			break;
		case BAR_DRUNK:
			if (fresh) {
				st->nextStumble = svs.time + 1000;
			}
			break;
		case BAR_SPIN:
			if (fresh) {
				st->spinLastTime = svs.time;
			}
			break;
		case BAR_HICCUP:
			if (fresh) {
				st->nextHiccup = svs.time + 1500;
			}
			break;
		default:
			break;
	}
	st->until[d->effect] = until;
	st->lastFrameTime = svs.time;

	if (d->look != BAR_LOOK_NONE) {
		st->visualUntil[d->look] = until;
	}
	if (d->glow != BAR_LOOK_NONE) {
		st->visualUntil[d->glow] = until;
	}
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
	SV_EconomyMenuPump(cl);
}

qboolean SV_BarCommand(client_t* cl, const char* args)
{
	char first[32];

	if (!g_economyBarEnable || !g_economyBarEnable->integer) {
		SV_EconomyPrint(cl, "The bar is closed on this server.");
		return qtrue;
	}

	if (sscanf(args, "%31s", first) != 1) {
		Bar_ShowMenu(cl);
		return qtrue;
	}

	const int n = atoi(first);
	if (n < 1 || n > (int)ARRAY_LEN(kBarDrinks) || Bar_Cost(n - 1) <= 0) {
		SV_EconomyPrint(cl, "That's not on the menu. Type !bar to see the drinks.");
		return qtrue;
	}
	const int drink = n - 1;

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

// Rewrites the player's input before the game module sees it.
void SV_BarClientThink(client_t* cl, usercmd_t* cmd)
{
	barState_t* st = &gBarState[cl - svs.clients];

	if (!cl->gentity || !cl->gentity->playerState ||
		cl->gentity->playerState->persistant[PERS_SPAWN_COUNT] != st->spawnCount) {
		return;
	}

	if (Bar_Active(st, BAR_REVERSE)) {
		cmd->forwardmove = (signed char)Com_Clampi(-127, 127, -cmd->forwardmove);
		cmd->rightmove = (signed char)Com_Clampi(-127, 127, -cmd->rightmove);
	}
	if (Bar_Active(st, BAR_RUNAWAY)) {
		cmd->forwardmove = 127;
	}
	if (Bar_Active(st, BAR_SLOW)) {
		cmd->forwardmove /= 4;
		cmd->rightmove /= 4;
	}
	if (Bar_Active(st, BAR_CROUCH)) {
		cmd->upmove = -127;
	}
}

static void Bar_Expire(client_t* cl, barState_t* st, barEffect_t e, const char* message)
{
	st->until[e] = 0;
	SV_EconomyPrint(cl, message);
}

static void Bar_DrunkFrame(client_t* cl, barState_t* st)
{
	playerState_t* ps = cl->gentity->playerState;

	if (!Bar_Active(st, BAR_DRUNK)) {
		ps->delta_angles[YAW] -= st->swayYaw;
		ps->delta_angles[PITCH] -= st->swayPitch;
		st->swayYaw = 0;
		st->swayPitch = 0;
		Bar_Expire(cl, st, BAR_DRUNK, "You sober up.");
		return;
	}

	// Two slow, out-of-step waves so it wanders rather than ticks.
	const float t = svs.time / 1000.0f;
	const int wantYaw = ANGLE2SHORT(14.0f * sinf(t * 1.6f) + 6.0f * sinf(t * 2.9f));
	const int wantPitch = ANGLE2SHORT(7.0f * sinf(t * 1.2f) + 3.0f * sinf(t * 2.3f));
	ps->delta_angles[YAW] += wantYaw - st->swayYaw;
	ps->delta_angles[PITCH] += wantPitch - st->swayPitch;
	st->swayYaw = wantYaw;
	st->swayPitch = wantPitch;

	if (svs.time >= st->nextStumble) {
		if (ps->groundEntityNum != ENTITYNUM_NONE) {
			const float a = Q_irand(0, 359) * (M_PI / 180.0f);
			ps->velocity[0] += cosf(a) * 240.0f;
			ps->velocity[1] += sinf(a) * 240.0f;
		}
		st->nextStumble = svs.time + Q_irand(900, 2200);
	}
}

static void Bar_MovementFrame(client_t* cl, barState_t* st, float dt)
{
	playerState_t* ps = cl->gentity->playerState;
	const qboolean onGround = (ps->groundEntityNum != ENTITYNUM_NONE) ? qtrue : qfalse;

	if (st->until[BAR_SPIN]) {
		if (!Bar_Active(st, BAR_SPIN)) {
			Bar_Expire(cl, st, BAR_SPIN, "The room stops spinning.");
		} else {
			// 90 degrees a second, left where it ends - no need to undo a spin.
			ps->delta_angles[YAW] += ANGLE2SHORT(90.0f * dt);
		}
	}

	if (st->until[BAR_HICCUP]) {
		if (!Bar_Active(st, BAR_HICCUP)) {
			Bar_Expire(cl, st, BAR_HICCUP, "Your hiccups stop.");
		} else if (svs.time >= st->nextHiccup) {
			if (onGround) {
				ps->velocity[2] += 280.0f;
			}
			st->nextHiccup = svs.time + Q_irand(1500, 3500);
		}
	}

	if (st->until[BAR_MOON]) {
		if (!Bar_Active(st, BAR_MOON)) {
			Bar_Expire(cl, st, BAR_MOON, "Gravity's back.");
		} else if (!onGround) {
			ps->velocity[2] += ps->gravity * BAR_MOON_GRAVITY_CANCEL * dt;
		}
	}

	if (st->until[BAR_RUSH]) {
		if (!Bar_Active(st, BAR_RUSH)) {
			Bar_Expire(cl, st, BAR_RUSH, "Your sugar rush wears off.");
		} else if (onGround) {
			const float speed = sqrtf(ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1]);
			if (speed > 50.0f && speed < BAR_RUSH_MAX_SPEED) {
				const float boost = Q_min(BAR_RUSH_BOOST, BAR_RUSH_MAX_SPEED / speed);
				ps->velocity[0] *= boost;
				ps->velocity[1] *= boost;
			}
		}
	}

	// The input-rewriting drinks only need their end announced.
	if (st->until[BAR_SLOW] && !Bar_Active(st, BAR_SLOW)) {
		Bar_Expire(cl, st, BAR_SLOW, "You can move properly again.");
	}
	if (st->until[BAR_REVERSE] && !Bar_Active(st, BAR_REVERSE)) {
		Bar_Expire(cl, st, BAR_REVERSE, "Your controls are back to normal.");
	}
	if (st->until[BAR_RUNAWAY] && !Bar_Active(st, BAR_RUNAWAY)) {
		Bar_Expire(cl, st, BAR_RUNAWAY, "You can finally stop running.");
	}
	if (st->until[BAR_CROUCH] && !Bar_Active(st, BAR_CROUCH)) {
		Bar_Expire(cl, st, BAR_CROUCH, "You can stand up again.");
	}
}

static qboolean Bar_AnythingActive(const barState_t* st)
{
	for (int e = 0; e < BAR_NUM_EFFECTS; e++) {
		if (st->until[e]) {
			return qtrue;
		}
	}
	for (int b = 0; b < BAR_MB2_PW_COUNT; b++) {
		if (st->visualUntil[b] > svs.time) {
			return qtrue;
		}
	}
	return qfalse;
}

void SV_BarFrame(void)
{
	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		barState_t* st = &gBarState[i];

		if (!Bar_AnythingActive(st)) {
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

		const float dt = (svs.time - st->lastFrameTime) / 1000.0f;
		st->lastFrameTime = svs.time;

		if (st->until[BAR_SCALE] && !Bar_Active(st, BAR_SCALE)) {
			if (ps->iModelScale == st->scaleSet) {
				ps->iModelScale = st->scaleOriginal;
			}
			Bar_Expire(cl, st, BAR_SCALE, "Your drink wears off - back to normal size.");
		}

		// Dead: stop moving them about, but leave delta_angles alone - the
		// respawn resets the view, and the new life's check above clears us.
		if (ps->stats[STAT_HEALTH] <= 0) {
			continue;
		}
		if (st->until[BAR_DRUNK]) {
			Bar_DrunkFrame(cl, st);
		}
		Bar_MovementFrame(cl, st, dt);

		for (int b = 0; b < BAR_MB2_PW_COUNT; b++) {
			if (st->visualUntil[b] > svs.time) {
				cl->gentity->s.powerups |= (1 << b);
			}
		}
	}
}
