/*
===========================================================================
jukebox.cpp — the Cantina jukebox, part of the economy (!jukebox)

"!jukebox" lists the tracks and "!jukebox <number>" pays to put one on for
the whole server; every pick is announced in chat. Switched on with
g_economyJukeboxEnable (on top of the g_creditSystemEnable master switch,
like !buy and !bar); g_jukeboxCost is the price of a track and
g_jukeboxCooldown how long one plays before anyone can change it.

The music is the stock CS_MUSIC configstring (2 in both MBII and the
engine): cgame restarts the background track whenever it changes
(CG_StartMusic, cg_main.c), exactly as it does for a map's own worldspawn
"music" key. Every track is one MBII already ships in its pk3s, so nobody
downloads anything. G_InitGame puts the map's own music back on the next
round.
===========================================================================
*/

#include "server.h"

typedef struct {
	const char* name;
	const char* path;   // as a map's "music" key: no extension
} jukeboxTrack_t;

// Order is the menu numbering.
static const jukeboxTrack_t kJukeboxTracks[] = {
	{ "Cantina Band",              "music/mp/Cantina" },
	{ "Cantina Band (house mix)",  "music/cantina2" },
	{ "Nar Shaddaa Cantina",       "music/ns/nsmb2_cantina" },
	{ "Nightclub",                 "music/nightclub" },
	{ "Club Mix",                  "music/desertbreak/club1" },
	{ "Jabba's Sail Barge",        "music/mp/SailBarge" },
	{ "Duel of the Fates",         "music/mb2_dotf/dotf" },
	{ "Battle of the Heroes",      "music/mustafarduel/mustafarduel" },
	{ "KotOR Mix",                 "Music/KotorMix" },
	{ "LEGO Star Wars",            "Music/legosw" },
	{ "Benny Hill",                "music/mp/BennyHill" },
	{ "Hammer Time",               "music/hammertime" },
	{ "Crazy Train",               "music/crazytrain/crazytrain" },
	{ "Strings of Life",           "music/tlp/strings-of-life2" },
	{ "The Ultimate Showdown",     "Music/ultimate_showdown" },
	{ "Pokemon Town",              "Music/pokemon/ptown" },
	{ "Teenage Mutant Ninja Turtles", "Music/tmnt" },
	{ "Halo",                      "Music/halo_for" },
	{ "Portal",                    "music/portal" },
	{ "Mortal Kombat",             "Music/mk" },
	{ "Lord of the Rings",         "Music/lotr_hd" },
};

static int gJukeboxNextChange = 0;

qboolean SV_JukeboxCommand(client_t* cl, const char* args)
{
	char first[32];
	char line[ECONOMY_MENU_LINE_SIZE];

	if (!g_economyJukeboxEnable || !g_economyJukeboxEnable->integer) {
		SV_EconomyPrint(cl, "There's no jukebox on this server.");
		return qtrue;
	}

	const int cost = g_jukeboxCost ? Q_max(0, g_jukeboxCost->integer) : 0;

	if (sscanf(args, "%31s", first) != 1) {
		SV_EconomyMenuBegin(cl);
		Com_sprintf(line, sizeof(line), "^3=== JUKEBOX === ^7Any track: ^2%d ^7cr  Balance: ^2%d", cost, cl->economyCredits);
		SV_EconomyMenuAddLine(cl, line);
		for (int i = 0; i < (int)ARRAY_LEN(kJukeboxTracks); i++) {
			Com_sprintf(line, sizeof(line), "^3%d^7. ^5%s", i + 1, kJukeboxTracks[i].name);
			SV_EconomyMenuAddLine(cl, line);
		}
		SV_EconomyMenuAddLine(cl, "^7Type ^5!jukebox <number> ^7to play it for everyone, e.g. ^5!jukebox 1");
		SV_EconomyMenuPump(cl);
		return qtrue;
	}

	const int n = atoi(first);
	if (n < 1 || n > (int)ARRAY_LEN(kJukeboxTracks)) {
		SV_EconomyPrint(cl, "That's not on the jukebox. Type !jukebox to see the tracks.");
		return qtrue;
	}
	if (svs.time < gJukeboxNextChange) {
		SV_EconomyPrint(cl, va("Let this one play - the jukebox is free again in %d seconds.",
			(gJukeboxNextChange - svs.time + 999) / 1000));
		return qtrue;
	}
	if (cl->economyCredits < cost) {
		SV_EconomyPrint(cl, va("Not enough credits. Need %d, have %d.", cost, cl->economyCredits));
		return qtrue;
	}

	const jukeboxTrack_t* t = &kJukeboxTracks[n - 1];
	cl->economyCredits -= cost;
	SV_EconomyPersistCredits(cl);
	SV_SetConfigstring(CS_MUSIC, t->path);
	gJukeboxNextChange = svs.time + (g_jukeboxCooldown ? Q_max(0, g_jukeboxCooldown->integer) : 60) * 1000;

	SV_SendServerCommand(NULL, "chat \"^5[Jukebox] ^7%s ^7put on ^3%s^7.\"\n", cl->name, t->name);
	SV_EconomyPrint(cl, va("Now playing: %s. New balance: %d", t->name, cl->economyCredits));
	return qtrue;
}
