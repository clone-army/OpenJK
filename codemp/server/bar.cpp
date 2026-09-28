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
    your view sways hard (the field the game itself uses to turn a player's
    view on teleport). Every nudge is tracked and undone exactly when it
    wears off. Tatooine Twister spins your view the same way
  - Bubble Brew (hiccup hops), Moon Milk (part of gravity cancelled while
    airborne) and Sugar Rush (ground speed boosted) push ps->velocity after
    each game frame
  - the control drinks rewrite the player's usercmd_t before the game module
    ever sees it (SV_BarClientThink, called from SV_ClientThink like gun
    game's weapon clamp): reversed, slowed, stuck running forward, stuck
    crouching. The client still predicts its own unaltered input, so these
    feel a little rubbery - which rather suits a drink
  - Death Stick and Spice combine two of the above (speed + hiccups,
    low gravity + spin). Spice also greys out your view: MBII's cgame draws
    its Force Rage "recovery" tint while fd.forceRageRecoveryTime is ahead
    of the clock (it also slows you to 75% while it lasts, bg_pmove.c)
  - most drinks play one of MBII's own effects while they last - spice
    smoke, a confusion swirl, bubbles, frost, dust, flames, sparks
    (G_EffectIndex / G_PlayEffectID, found by name like everything else).
    A drinker's first-person camera sits at their eyes - inside anything at
    their head, above anything at their waist - so head effects play just in
    front of the face for everyone, and body/feet effects play on the body
    for everyone else (SVF_NOTSINGLECLIENT) plus a copy in front of the
    drinker's view for them alone (SVF_SINGLECLIENT). Ones with a sound only
    play every few seconds
  - Spotchka is the one look that only others can see: MBII's cloak
    shimmer, switched on by setting its bit in the entityState_t powerups
    mask cgame draws from (cg_players.c) after each game frame. It's
    display-only; pmove and the game read playerState, never touched here.
    (Other powerup looks aren't used: players never see their own - the
    engine sends MBII's playerState powerups field as a 1-bit value.)

Drink too much, too fast (within 5 minutes): the 8th order knocks you out
cold for a few seconds (MBII's own G_Knockdown); the 10th is alcohol
poisoning and the 3rd Spice an overdose - MBII's own /kill (its usual 5
second countdown), and everyone is told what killed you when you drop.
Every order burps (plus a Jawa line for Jawa Juice, a cough for spice and
death sticks), through MBII's G_SoundOnEnt.

Effects only ever last for the life they were bought in: a new life
(persistant[PERS_SPAWN_COUNT], which MBII bumps on every spawn - same index
in MBII and the engine) means MBII has already reset the player, so the
effect is dropped without touching anything.
===========================================================================
*/

#include "server.h"
#include "sv_gameapi.h"
#include "sys/sys_loadlib.h"

// Where on the player a drink's effect plays.
#define BAR_FX_HEAD 0
#define BAR_FX_BODY 1
#define BAR_FX_FEET 2

// MBII's own effect functions, found by name in the game module (as
// social.cpp does): the smoke is a real world effect, so everyone -
// including the smoker - sees it, unlike the powerup glows.
static void* gBarFxDll = NULL;
static int (*gBarEffectIndex)(const char* name) = NULL;
static void* (*gBarPlayEffectID)(int fxID, float* org, float* ang) = NULL;
static void (*gBarSoundOnEnt)(void* ent, int channel, const char* path) = NULL;
static void (*gBarKnockdown)(void* victim, int attacker, int addTime, int downVelocity, qboolean quickGetup) = NULL;

static qboolean Bar_ResolveFx(void)
{
	void* dll = GVM_GetDllHandle();
	if (dll != gBarFxDll) {
		gBarFxDll = dll;
		gBarEffectIndex = dll ? (int (*)(const char*))Sys_LoadFunction(dll, "G_EffectIndex") : NULL;
		gBarPlayEffectID = dll ? (void* (*)(int, float*, float*))Sys_LoadFunction(dll, "G_PlayEffectID") : NULL;
		gBarSoundOnEnt = dll ? (void (*)(void*, int, const char*))Sys_LoadFunction(dll, "G_SoundOnEnt") : NULL;
		gBarKnockdown = dll ? (void (*)(void*, int, int, int, qboolean))Sys_LoadFunction(dll, "G_Knockdown") : NULL;
	}
	return (gBarEffectIndex && gBarPlayEffectID) ? qtrue : qfalse;
}

// A sound on the player, heard by everyone nearby (MBII's G_SoundOnEnt).
static void Bar_Sound(client_t* cl, const char* path)
{
	if (!Bar_ResolveFx() || !gBarSoundOnEnt) {
		return;
	}
	void* old = GVM_BeginNative();
	gBarSoundOnEnt(cl->gentity, CHAN_AUTO, path);
	GVM_EndNative(old);
}

static void Bar_OrderSounds(client_t* cl, const char* id)
{
	if (!Q_stricmp(id, "corellian_whiskey")) {
		SV_EmoteTrigger(cl, "choke"); // it goes down the wrong way
	}
	Bar_Sound(cl, va("sound/chars/chefporkins/misc/burp%d.wav", Q_irand(1, 7)));
	if (!Q_stricmp(id, "jawa_juice")) {
		Bar_Sound(cl, va("Sound/Chars/r_jawa_bane/misc/gloat%d.mp3", Q_irand(1, 3)));
	} else if (!Q_stricmp(id, "death_stick") || !Q_stricmp(id, "spice")) {
		Bar_Sound(cl, "sound/chars/grievous/misc/cough.mp3");
	}
}

// Plays effect id at org; svFlags limits who gets it (SVF_SINGLECLIENT /
// SVF_NOTSINGLECLIENT, relative to client), 0 for everyone.
static void Bar_PlayFx(int id, vec3_t org, int client, int svFlags)
{
	vec3_t ang = { -90.0f, 0.0f, 0.0f }; // pointing up
	void* old = GVM_BeginNative();
	sharedEntity_t* te = (sharedEntity_t*)gBarPlayEffectID(id, org, ang);
	GVM_EndNative(old);
	if (te && svFlags) {
		te->r.svFlags |= svFlags;
		te->r.singleClient = client;
	}
}

static void Bar_Puff(client_t* cl, const char* fx, int where)
{
	if (!Bar_ResolveFx()) {
		return;
	}
	const playerState_t* ps = cl->gentity->playerState;
	const int client = cl - svs.clients;
	vec3_t eye, forward, org;

	// Registered per map (configstrings reset on a new map); a no-op lookup after the first.
	const int id = GVM_CallEffectIndex(gBarEffectIndex, fx);
	if (id <= 0) {
		return;
	}

	VectorCopy(ps->origin, eye);
	eye[2] += ps->viewheight;
	AngleVectors(ps->viewangles, forward, NULL, NULL);

	if (where == BAR_FX_HEAD) {
		// Just in front of the face, for everyone: the drinker sees it, and
		// to everyone else smoke looks exhaled.
		VectorMA(eye, 16.0f, forward, org);
		Bar_PlayFx(id, org, client, 0);
		return;
	}

	// On the body (or at the feet) for everyone else...
	VectorCopy(ps->origin, org);
	if (where == BAR_FX_FEET) {
		org[2] -= 20.0f;
	}
	Bar_PlayFx(id, org, client, SVF_NOTSINGLECLIENT);

	// ...and a copy in front of the drinker's view for them alone.
	VectorMA(eye, 48.0f, forward, org);
	org[2] -= (where == BAR_FX_FEET) ? 24.0f : 10.0f;
	Bar_PlayFx(id, org, client, SVF_SINGLECLIENT);
}

#define BAR_MB2_PW_COUNT   32

// MBII's powerup numbers (bg_public.h), which differ from base JKA's: these
// are what MBII's cgame checks in entityState_t.powerups to draw each one.
#define BAR_LOOK_NONE      -1
#define BAR_PW_CLOAKED     11       // cloak shimmer

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
	barEffect_t effect2;    // a second effect at the same time, or BAR_LOOK for none
	const char* fx;         // effect played on the player while it lasts, or NULL
	int         fxEveryMs;  // how often (effects with a sound get a long gap)
	int         fxWhere;    // BAR_FX_*
} barDrink_t;

// Order is the menu numbering.
static const barDrink_t kBarDrinks[] = {
	{ "jawa_juice",        "Jawa Juice",        "shrinks you to Jawa size for 2 minutes",       10, BAR_SCALE,   50,  120, BAR_LOOK_NONE,    BAR_LOOK_NONE, BAR_LOOK, NULL, 0, BAR_FX_HEAD },
	{ "hutt_brew",         "Hutt Brew",         "makes you huge for 2 minutes",                 15, BAR_SCALE,   175, 120, BAR_LOOK_NONE,    BAR_LOOK_NONE, BAR_LOOK, NULL, 0, BAR_FX_HEAD },
	{ "corellian_whiskey", "Corellian Whiskey", "gets you properly drunk for 90 seconds",       12, BAR_DRUNK,   0,   90,  BAR_LOOK_NONE,    BAR_LOOK_NONE, BAR_LOOK, "effects/Force/confusion_red", 1000, BAR_FX_HEAD },
	{ "tatooine_twister",  "Tatooine Twister",  "your head spins for 20 seconds",               10, BAR_SPIN,    0,   20,  BAR_LOOK_NONE,    BAR_LOOK_NONE, BAR_LOOK, "effects/Tatooine/dustcloud", 500, BAR_FX_FEET },
	{ "bubble_brew",       "Bubble Brew",       "hiccups - you hop about for a minute",         10, BAR_HICCUP,  0,   60,  BAR_LOOK_NONE,    BAR_LOOK_NONE, BAR_LOOK, "effects/Saber/water_boilbubble_nosnd", 500, BAR_FX_HEAD },
	{ "moon_milk",         "Moon Milk",         "low gravity for a minute",                     12, BAR_MOON,    0,   60,  BAR_LOOK_NONE,    BAR_LOOK_NONE, BAR_LOOK, NULL, 0, BAR_FX_HEAD },
	{ "sugar_rush",        "Sugar Rush",        "super speed for 30 seconds",                   15, BAR_RUSH,    0,   30,  BAR_LOOK_NONE, BAR_LOOK_NONE, BAR_LOOK, "effects/Tatooine/dustcloud", 500, BAR_FX_FEET },
	{ "bantha_sludge",     "Bantha Sludge",     "you can barely move for a minute",             10, BAR_SLOW,    0,   60,  BAR_LOOK_NONE,    BAR_LOOK_NONE, BAR_LOOK, "effects/Flamethrower/poisoned", 3000, BAR_FX_BODY },
	{ "backwards_brandy",  "Backwards Brandy",  "your controls are reversed for a minute",      12, BAR_REVERSE, 0,   60,  BAR_LOOK_NONE,    BAR_LOOK_NONE, BAR_LOOK, "effects/Force/confusion_red", 1000, BAR_FX_HEAD },
	{ "runaway_rum",       "Runaway Rum",       "you can't stop running for 30 seconds",        12, BAR_RUNAWAY, 0,   30,  BAR_LOOK_NONE,    BAR_LOOK_NONE, BAR_LOOK, "effects/Tatooine/dustcloud", 500, BAR_FX_FEET },
	{ "low_ceiling_lager", "Low-Ceiling Lager", "stuck crouching for a minute",                 10, BAR_CROUCH,  0,   60,  BAR_LOOK_NONE,    BAR_LOOK_NONE, BAR_LOOK, NULL, 0, BAR_FX_HEAD },
	{ "spotchka",          "Spotchka",          "you shimmer nearly invisible for 45 seconds",  20, BAR_LOOK,    0,   45,  BAR_PW_CLOAKED,   BAR_LOOK_NONE, BAR_LOOK, NULL, 0, BAR_FX_HEAD },
	{ "hoth_chiller",      "Hoth Chiller",      "frost forms all over you for a minute",           12, BAR_LOOK,    0,   60,  BAR_LOOK_NONE,    BAR_LOOK_NONE, BAR_LOOK, "effects/Flamethrower/ice", 1000, BAR_FX_BODY },
	{ "mustafar_magma",    "Mustafar Magma",    "you're on fire (just for show) for a minute",  12, BAR_LOOK,    0,   60,  BAR_LOOK_NONE,    BAR_LOOK_NONE, BAR_LOOK, "effects/Emitter/flamestiny", 2000, BAR_FX_BODY },
	{ "ion_fizz",          "Ion Fizz",          "you crackle with electricity for a minute",    12, BAR_LOOK,    0,   60,  BAR_LOOK_NONE, BAR_LOOK_NONE, BAR_LOOK, "effects/Swords/shock_person", 3000, BAR_FX_BODY },
	{ "death_stick",       "Death Stick",       "you want to go home and rethink your life",    20, BAR_RUSH,    0,   30,  BAR_LOOK_NONE, BAR_LOOK_NONE,    BAR_HICCUP, "effects/spice/pipe_smoke", 500, BAR_FX_HEAD },
	{ "spice",             "Spice",             "floaty, spinny and hazy for 45 seconds", 20, BAR_MOON,    0,   45,  BAR_LOOK_NONE,    BAR_LOOK_NONE,   BAR_SPIN, "effects/spice/pipe_smoke", 500, BAR_FX_HEAD },
};

static cvar_t* gBarCostCvars[ARRAY_LEN(kBarDrinks)];

typedef struct {
	int spawnCount;         // the life these effects belong to
	int until[BAR_NUM_EFFECTS];
	int scaleOriginal;
	int scaleSet;
	int swayYaw;            // drunk: delta_angles offset currently applied, in SHORT units
	int swayPitch;
	int spinLastTime;
	int nextHiccup;
	int lastFrameTime;
	int visualUntil[BAR_MB2_PW_COUNT];
	const barDrink_t* fxDrink; // playing its effect until fxUntil
	int fxUntil;
	int nextPuff;
	int hazeUntil;          // Spice: grey view until then
} barState_t;

static barState_t gBarState[MAX_CLIENTS];

// The tab: recent orders per player, across lives (the drinking doesn't stop
// because you died), cleared when the slot empties. Drives passing out,
// alcohol poisoning and the spice overdose.
#define BAR_TAB_WINDOW_MS     300000
#define BAR_TAB_SIZE          16
#define BAR_SPICE_OVERDOSE    3       // spice orders within the window that kill you
#define BAR_PASSOUT_ORDERS    8       // orders within the window that knock you out
#define BAR_POISONING_ORDERS  10      // orders within the window that kill you
#define BAR_PASSOUT_MS        4000    // extra time on the floor, on top of MBII's own
#define BAR_OVERDOSE_WAIT_MS  15000   // longer than MBII's 5s /kill countdown
typedef struct {
	int orderTimes[BAR_TAB_SIZE];
	int spiceTimes[BAR_TAB_SIZE];
	int overdoseSpawnCount;             // the life that's dying
	int overdoseUntil;                  // 0 = not dying
	const char* overdoseCause;          // "alcohol poisoning", "a spice overdose"
} barTab_t;
static barTab_t gBarTab[MAX_CLIENTS];

static const char* const kPassOutLines[] = {
	"%s ^7has passed out.",
	"%s ^7is taking a little nap on the floor.",
	"%s ^7has hit the deck. Someone check they're breathing.",
	"%s ^7face-planted into the bar.",
	"Lights out for %s^7.",
};
#define BAR_RANDOM_LINE(lines) (lines[Q_irand(0, ARRAY_LEN(lines) - 1)])

// Records an order and returns how many are on the tab within the window,
// including this one.
static int Bar_TabAdd(int* times)
{
	int oldest = 0, count = 1;
	for (int i = 0; i < BAR_TAB_SIZE; i++) {
		if (times[i] && svs.time - times[i] < BAR_TAB_WINDOW_MS) {
			count++;
		}
		if (times[i] < times[oldest]) {
			oldest = i;
		}
	}
	times[oldest] = svs.time;
	return count;
}

static void Bar_Announce(client_t* cl, const char* fmt)
{
	SV_SendServerCommand(NULL, "chat \"^5[Bar] ^7%s\"\n", va(fmt, cl->name));
}

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
	// A spectator following someone carries a copy of their playerState.
	if (ps->clientNum != cl - svs.clients) {
		return qfalse;
	}
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

static void Bar_StartEffect(client_t* cl, barState_t* st, const barDrink_t* d, barEffect_t effect, int until)
{
	playerState_t* ps = cl->gentity->playerState;
	const qboolean fresh = st->until[effect] ? qfalse : qtrue;

	switch (effect) {
		case BAR_SCALE:
			if (fresh) {
				st->scaleOriginal = ps->iModelScale;
			}
			ps->iModelScale = d->value;
			st->scaleSet = d->value;
			break;
		case BAR_HICCUP:
			if (fresh) {
				st->nextHiccup = svs.time + 1500;
			}
			break;
		default:
			break;
	}
	st->until[effect] = until;
}

static void Bar_Apply(client_t* cl, int drink)
{
	const barDrink_t* d = &kBarDrinks[drink];
	barState_t* st = Bar_StateFor(cl);
	const int until = svs.time + d->seconds * 1000;

	Bar_StartEffect(cl, st, d, d->effect, until);
	if (d->effect2 != BAR_LOOK) {
		Bar_StartEffect(cl, st, d, d->effect2, until);
	}
	st->lastFrameTime = svs.time;

	if (d->look != BAR_LOOK_NONE) {
		st->visualUntil[d->look] = until;
	}
	if (d->glow != BAR_LOOK_NONE) {
		st->visualUntil[d->glow] = until;
	}
	if (d->fx) {
		st->fxDrink = d;
		st->fxUntil = until;
		st->nextPuff = svs.time;
	}
	if (!Q_stricmp(d->id, "spice")) {
		st->hazeUntil = until;
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

// Drunk (or spiced) to death: MBII's own /kill - a 5 second countdown, then
// they drop - and Bar_OverdoseFrame tells everyone why once they do.
static void Bar_Kill(client_t* cl, barTab_t* tab, const char* cause)
{
	memset(tab->orderTimes, 0, sizeof(tab->orderTimes));
	memset(tab->spiceTimes, 0, sizeof(tab->spiceTimes));
	tab->overdoseSpawnCount = cl->gentity->playerState->persistant[PERS_SPAWN_COUNT];
	tab->overdoseUntil = svs.time + BAR_OVERDOSE_WAIT_MS;
	tab->overdoseCause = cause;
	Cmd_TokenizeString("kill");
	GVM_ClientCommand(cl - svs.clients);
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
	SV_SendServerCommand(NULL, "chat \"^5[Bar] ^7%s ^7orders %s^3%s^7!\"\n", cl->name,
		!Q_stricmp(kBarDrinks[drink].id, "spice") ? "" : "a ", kBarDrinks[drink].name);
	SV_EconomyPrint(cl, va("%s: %s. New balance: %d", kBarDrinks[drink].name, kBarDrinks[drink].blurb, cl->economyCredits));

	Bar_OrderSounds(cl, kBarDrinks[drink].id);

	barTab_t* tab = &gBarTab[cl - svs.clients];
	const int orders = Bar_TabAdd(tab->orderTimes);
	const int spice = !Q_stricmp(kBarDrinks[drink].id, "spice") ? Bar_TabAdd(tab->spiceTimes) : 0;

	if (spice >= BAR_SPICE_OVERDOSE) {
		Bar_Kill(cl, tab, "a spice overdose");
	} else if (orders >= BAR_POISONING_ORDERS) {
		Bar_Kill(cl, tab, "alcohol poisoning");
	} else if (orders == BAR_PASSOUT_ORDERS && gBarKnockdown && Bar_IsAlive(cl)) {
		// MBII's own knockdown: flat on the floor for a few seconds.
		void* old = GVM_BeginNative();
		gBarKnockdown(cl->gentity, cl - svs.clients, BAR_PASSOUT_MS, 0, qfalse);
		GVM_EndNative(old);
		Bar_Sound(cl, "sound/dreamtime/grogu_asleep.wav");
		Bar_Announce(cl, BAR_RANDOM_LINE(kPassOutLines));
	}
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
	const int wantYaw = ANGLE2SHORT(24.0f * sinf(t * 1.6f) + 10.0f * sinf(t * 2.9f));
	const int wantPitch = ANGLE2SHORT(12.0f * sinf(t * 1.2f) + 5.0f * sinf(t * 2.3f));
	ps->delta_angles[YAW] += wantYaw - st->swayYaw;
	ps->delta_angles[PITCH] += wantPitch - st->swayPitch;
	st->swayYaw = wantYaw;
	st->swayPitch = wantPitch;
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
	return (st->fxUntil > svs.time || st->hazeUntil > svs.time) ? qtrue : qfalse;
}

// Tells everyone what killed them once it actually does.
static void Bar_OverdoseFrame(client_t* cl, barTab_t* tab)
{
	if (!tab->overdoseUntil) {
		return;
	}
	if (svs.time >= tab->overdoseUntil || cl->state != CS_ACTIVE || !cl->gentity || !cl->gentity->playerState) {
		tab->overdoseUntil = 0; // survived somehow (or left) - say nothing
		return;
	}
	const playerState_t* ps = cl->gentity->playerState;
	if (ps->stats[STAT_HEALTH] > 0 && ps->persistant[PERS_SPAWN_COUNT] == tab->overdoseSpawnCount) {
		return;
	}
	tab->overdoseUntil = 0;
	SV_SendServerCommand(NULL, "chat \"^5[Bar] ^7%s ^1died from %s.\"\n", cl->name, tab->overdoseCause);
	SV_SendServerCommand(NULL, "cp \"%s\n^1died from %s\"\n", cl->name, tab->overdoseCause);
}

void SV_BarFrame(void)
{
	for (int i = 0; i < sv_maxclients->integer; i++) {
		client_t* cl = &svs.clients[i];
		barState_t* st = &gBarState[i];

		if (cl->state < CS_CONNECTED) {
			memset(&gBarTab[i], 0, sizeof(gBarTab[i]));
		}
		Bar_OverdoseFrame(cl, &gBarTab[i]);
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

		// Kept a moment ahead of the clock; it lapses by itself when Spice ends.
		if (st->hazeUntil > svs.time) {
			ps->fd.forceRageRecoveryTime = sv.time + 250;
		}

		if (st->fxDrink && st->fxUntil > svs.time && svs.time >= st->nextPuff) {
			Bar_Puff(cl, st->fxDrink->fx, st->fxDrink->fxWhere);
			st->nextPuff = svs.time + st->fxDrink->fxEveryMs;
		}
	}
}
