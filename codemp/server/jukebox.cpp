/*
===========================================================================
jukebox.cpp — the Cantina jukebox, part of the economy (!jukebox)

  !jukebox                   the featured tracks
  !jukebox <words>           search every track by name and category
  !jukebox search <words>    the same, spelled out
  !jukebox <number>          play that track for the whole server
  !jukebox random            play a random track

Every track MBII ships in its official pk3s is in the catalogue
(jukebox_tracks.h), each with a friendly name and a category, so nobody
downloads anything. Every pick is announced in chat. Switched on with
g_economyJukeboxEnable (on top of the g_creditSystemEnable master switch,
like !buy and !bar); g_jukeboxCost is the price of a track and
g_jukeboxCooldown how long one plays before anyone can change it.

With g_jukeboxAutoplay on, the jukebox is never idle: while nobody's pick
is playing it plays random tracks (a minute or longer, each for at most
g_jukeboxAutoplayMax seconds), a paid pick plays in full and random play
carries on after it, and a new round or map (which puts the map's own
music back) starts a fresh random track. Nothing changes while the server
is empty. Track lengths come from jukebox_tracks.h.

The music is the stock CS_MUSIC configstring (2 in both MBII and the
engine): cgame restarts the background track whenever it changes
(CG_StartMusic, cg_main.c), exactly as it does for a map's own worldspawn
"music" key, taking an intro and a loop: a pick is the intro and the map's
own music the loop, so the track plays through once and then the map's
music comes back by itself. G_InitGame resets it every round anyway.
===========================================================================
*/

#include "server.h"

typedef struct {
	const char* name;
	const char* path;       // as a map's "music" key: no extension
	const char* category;
	int         seconds;    // its length, measured from the pk3
} jukeboxTrack_t;

#include "jukebox_tracks.h"

#define JUKEBOX_TRACK_COUNT   ((int)ARRAY_LEN(kJukeboxTracks))
#define JUKEBOX_SEARCH_MAX    20

static int gJukeboxNextChange = 0;
static char gJukeboxLastSet[MAX_STRING_CHARS];   // what we last put in CS_MUSIC
static char gJukeboxMapMusic[MAX_QPATH];         // the map's own track, to go back to
static int gJukeboxAutoNext = 0;                 // autoplay: when the current track is over (0 = now)
static int gJukeboxAutoLast = -1;                // autoplay: last random track, not to repeat

#define JUKEBOX_AUTO_MIN_SECONDS 60              // shorter tracks are stingers, not songs

static qboolean Jukebox_Autoplay(void)
{
	return (g_economyJukeboxEnable && g_economyJukeboxEnable->integer &&
		g_jukeboxAutoplay && g_jukeboxAutoplay->integer) ? qtrue : qfalse;
}

static void Jukebox_SetMusic(const char* music)
{
	Q_strncpyz(gJukeboxLastSet, music, sizeof(gJukeboxLastSet));
	SV_SetConfigstring(CS_MUSIC, gJukeboxLastSet);
}

// If CS_MUSIC isn't what we last set, the map (or a new round) set it, so
// that's the music to return to: its loop track, or its only track.
static void Jukebox_NoteMapMusic(void)
{
	const char* cur = sv.configstrings[CS_MUSIC] ? sv.configstrings[CS_MUSIC] : "";
	if (!Q_stricmp(cur, gJukeboxLastSet)) {
		return;
	}
	char intro[MAX_QPATH] = "", loop[MAX_QPATH] = "";
	sscanf(cur, "%63s %63s", intro, loop);
	Q_strncpyz(gJukeboxMapMusic, loop[0] ? loop : intro, sizeof(gJukeboxMapMusic));
}

static int Jukebox_Cost(void)
{
	return g_jukeboxCost ? Q_max(0, g_jukeboxCost->integer) : 0;
}

static const char* Jukebox_Line(int i)
{
	return va("^3%d^7. ^5%s ^7(%s)", i + 1, kJukeboxTracks[i].name, kJukeboxTracks[i].category);
}

// Every word has to appear in the name or the category.
static qboolean Jukebox_Matches(const jukeboxTrack_t* t, const char* words)
{
	char buf[MAX_STRING_CHARS];
	Q_strncpyz(buf, words, sizeof(buf));
	for (char* w = strtok(buf, " "); w; w = strtok(NULL, " ")) {
		if (!Q_stristr(t->name, w) && !Q_stristr(t->category, w)) {
			return qfalse;
		}
	}
	return qtrue;
}

static void Jukebox_ShowFeatured(client_t* cl)
{
	SV_EconomyMenuBegin(cl);
	SV_EconomyMenuAddLine(cl, va("^3=== JUKEBOX === ^7Any track: ^2%d ^7cr  Balance: ^2%d", Jukebox_Cost(), cl->economyCredits));
	for (int i = 0; i < JUKEBOX_FEATURED; i++) {
		SV_EconomyMenuAddLine(cl, Jukebox_Line(i));
	}
	SV_EconomyMenuAddLine(cl, va("^7^5!jukebox <number> ^7to play, ^5!jukebox <words> ^7to search all %d tracks, ^5!jukebox random",
		JUKEBOX_TRACK_COUNT));
	SV_EconomyMenuPump(cl);
}

static void Jukebox_Search(client_t* cl, const char* words)
{
	int found = 0, shown = 0;

	SV_EconomyMenuBegin(cl);
	SV_EconomyMenuAddLine(cl, va("^3=== JUKEBOX: \"%s\" ===", words));
	for (int i = 0; i < JUKEBOX_TRACK_COUNT; i++) {
		if (!Jukebox_Matches(&kJukeboxTracks[i], words)) {
			continue;
		}
		found++;
		if (shown < JUKEBOX_SEARCH_MAX) {
			SV_EconomyMenuAddLine(cl, Jukebox_Line(i));
			shown++;
		}
	}
	if (!found) {
		SV_EconomyMenuAddLine(cl, "^7Nothing matched. Try a shorter word, or a category: ^5star wars^7, ^5duels^7, ^5kotor^7, ^5games^7, ^5party^7, ^5maps");
	} else if (found > shown) {
		SV_EconomyMenuAddLine(cl, va("^7...and %d more - add another word to narrow it down.", found - shown));
	}
	if (found) {
		SV_EconomyMenuAddLine(cl, va("^5!jukebox <number> ^7to play one (^2%d ^7cr).", Jukebox_Cost()));
	}
	SV_EconomyMenuPump(cl);
}

static void Jukebox_Play(client_t* cl, int index)
{
	const int cost = Jukebox_Cost();

	if (svs.time < gJukeboxNextChange) {
		SV_EconomyPrint(cl, va("Let this one play - the jukebox is free again in %d seconds.",
			(gJukeboxNextChange - svs.time + 999) / 1000));
		return;
	}
	if (cl->economyCredits < cost) {
		SV_EconomyPrint(cl, va("Not enough credits. Need %d, have %d.", cost, cl->economyCredits));
		return;
	}

	const jukeboxTrack_t* t = &kJukeboxTracks[index];
	cl->economyCredits -= cost;
	SV_EconomyPersistCredits(cl);
	Jukebox_NoteMapMusic();
	if (Jukebox_Autoplay()) {
		// Plays in full, then random play carries on.
		Jukebox_SetMusic(t->path);
		gJukeboxAutoNext = svs.time + Q_max(t->seconds, 30) * 1000;
	} else {
		// Intro = the pick, loop = the map's music: one play-through, then back.
		Jukebox_SetMusic(gJukeboxMapMusic[0] ? va("%s %s", t->path, gJukeboxMapMusic) : t->path);
	}
	gJukeboxNextChange = svs.time + (g_jukeboxCooldown ? Q_max(0, g_jukeboxCooldown->integer) : 60) * 1000;

	SV_SendServerCommand(NULL, "chat \"^5[Jukebox] ^7%s ^7put on ^3%s^7.\"\n", cl->name, t->name);
	SV_EconomyPrint(cl, va("Now playing: %s. New balance: %d", t->name, cl->economyCredits));
}

// Autoplay: a random track whenever the last one's over.
void SV_JukeboxFrame(void)
{
	if (!Jukebox_Autoplay()) {
		gJukeboxAutoNext = 0;
		return;
	}
	// A new round or map puts its own music back: carry on straight away.
	const char* cur = sv.configstrings[CS_MUSIC] ? sv.configstrings[CS_MUSIC] : "";
	if (gJukeboxLastSet[0] && Q_stricmp(cur, gJukeboxLastSet)) {
		gJukeboxLastSet[0] = '\0';
		gJukeboxAutoNext = 0;
	}
	if (gJukeboxAutoNext && svs.time < gJukeboxAutoNext) {
		return;
	}

	qboolean anyone = qfalse;
	for (int i = 0; i < sv_maxclients->integer && !anyone; i++) {
		const client_t* c = &svs.clients[i];
		anyone = (c->state == CS_ACTIVE && c->netchan.remoteAddress.type != NA_BOT) ? qtrue : qfalse;
	}
	if (!anyone) {
		return;
	}

	int pick = -1;
	for (int tries = 0; tries < 50; tries++) {
		const int n = Q_irand(0, JUKEBOX_TRACK_COUNT - 1);
		if (kJukeboxTracks[n].seconds >= JUKEBOX_AUTO_MIN_SECONDS && n != gJukeboxAutoLast) {
			pick = n;
			break;
		}
	}
	if (pick < 0) {
		gJukeboxAutoNext = svs.time + 60000;
		return;
	}

	const jukeboxTrack_t* t = &kJukeboxTracks[pick];
	const int maxMs = (g_jukeboxAutoplayMax && g_jukeboxAutoplayMax->integer > 0) ? g_jukeboxAutoplayMax->integer * 1000 : 0;
	gJukeboxAutoLast = pick;
	Jukebox_SetMusic(t->path);
	gJukeboxAutoNext = svs.time + (maxMs ? Q_min(t->seconds * 1000, maxMs) : t->seconds * 1000);
	SV_SendServerCommand(NULL, "chat \"^5[Jukebox] ^7Now playing ^3%s^7.\"\n", t->name);
}

qboolean SV_JukeboxCommand(client_t* cl, const char* args)
{
	if (!g_economyJukeboxEnable || !g_economyJukeboxEnable->integer) {
		SV_EconomyPrint(cl, "There's no jukebox on this server.");
		return qtrue;
	}

	while (*args == ' ') {
		args++;
	}
	if (!*args) {
		Jukebox_ShowFeatured(cl);
		return qtrue;
	}

	if (!Q_stricmp(args, "random")) {
		Jukebox_Play(cl, Q_irand(0, JUKEBOX_TRACK_COUNT - 1));
		return qtrue;
	}

	if (!Q_stricmpn(args, "search", 6) && (args[6] == ' ' || !args[6])) {
		args += 6;
		while (*args == ' ') {
			args++;
		}
		if (!*args) {
			SV_EconomyPrint(cl, "Search for what? e.g. ^5!jukebox search cloud city");
			return qtrue;
		}
		Jukebox_Search(cl, args);
		return qtrue;
	}

	// A plain number plays that track; anything else is a search.
	const char* p = args;
	while (*p >= '0' && *p <= '9') {
		p++;
	}
	if (p != args && !*p) {
		const int n = atoi(args);
		if (n < 1 || n > JUKEBOX_TRACK_COUNT) {
			SV_EconomyPrint(cl, va("There are %d tracks - pick 1 to %d, or search with ^5!jukebox <words>^7.",
				JUKEBOX_TRACK_COUNT, JUKEBOX_TRACK_COUNT));
			return qtrue;
		}
		Jukebox_Play(cl, n - 1);
		return qtrue;
	}

	Jukebox_Search(cl, args);
	return qtrue;
}
