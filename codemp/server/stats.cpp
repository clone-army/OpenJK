/*
===========================================================================
stats.cpp — Native kill/death/suicide/playtime stats for OpenJK / MB2

Tracked in real time from the same stable playerState_t fields the economy/
gungame/killstreak systems already rely on (see gungame.cpp's header for why
only those fields are trustworthy - codemp/game/*.c, MBII's real game logic,
isn't compiled into this build and its private state can't be reached from
here). Shared across every instance, same as the economy accounts file: a
player is tracked by their economy login handle (cl->economyHandle) when
they're logged in, so stats follow the same persistent identity as their
credits do, or by their current in-game name when they aren't - so stats
still work on instances that don't run the economy plugin at all, or for a
player who hasn't bothered to register.

Genuine-death detection: a naive stats[STAT_HEALTH]-crosses-zero check
turned out to be exactly the signal that misfires at round transitions -
see killstreak.cpp's header for the full live-tested history. This checks
the more direct root-cause signal instead: ps->pm_type == PM_DEAD. Verified
against the actual MBII source (moviebattles-2, not compiled into this
build but available to read): G_Damage()/player_die() set
persistant[PERS_ATTACKER] and pm_type together, synchronously, on every
real death, and PERS_ATTACKER is never set to a mere assister - only
whoever actually dealt the damage - so this is also naturally immune to
crediting a kill to someone who only assisted.

Caution on the moviebattles-2 source itself, in case a future change is
tempted to pull a numeric constant from it directly: it's a separate dump,
not necessarily the exact revision actually compiled into the game module
this server loads. Its pmtype_t enum has extra values (PM_CRYOFREEZE,
PM_NOCLIP_DEAD, PM_BARGED) this project's own bg_public.h doesn't, which
would shift every later enum value's number if it were a true match -
gungame.cpp's PM_NORMAL/PM_JETPACK/PM_FLOAT checks (this project's own
values) are already live-tested and working, which couldn't be true if the
real module used moviebattles-2's shifted numbering. So PM_DEAD here is
this project's own symbol (proven consistent with what's actually
running), not anything imported from that dump - only the *behavioral*
understanding above (what AddScore/G_Damage/player_die actually do) comes
from it, which holds regardless of exact enum numbering.

Shared file, cross-process locking: mutations hold a single transaction lock.
Legacy implementation rationale (superseded below): same tradeoff
(each mutation reloads-then-saves as its own atomic, flock()'d step, not
combined into one lock spanning both) as economy_accounts.dat - see
SV_EconomyAccountsSave's comment in sv_client.cpp for the full reasoning.
Here the stakes are lower still (a display counter, not currency), so that
same narrow last-write-wins window matters even less.
===========================================================================
*/

#include "server.h"
#include "stats.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

#define STATS_FILE				"player_stats.dat"
#define STATS_MAX_PLAYERS		4096
#define STATS_NAME_SIZE			40
#define STATS_PLAYTIME_FLUSH_MS	60000	// how often each active client's playtime is persisted

typedef struct statsRecord_s {
	char	name[STATS_NAME_SIZE];	// economyHandle if logged in at the time, else raw player name
	int		kills;
	int		deaths;
	int		suicides;
	int		playtimeSeconds;
} statsRecord_t;

static statsRecord_t gStatsRecords[STATS_MAX_PLAYERS];
static int gStatsRecordCount = 0;

struct statsClientState_t {
	qboolean	active;
	int			lastHealth;
	int			lastFlushTime;	// svs.time this client's playtime was last persisted
};
static statsClientState_t gStatsState[MAX_CLIENTS];
static qboolean gStatsWasEnabled = qfalse;

// Hold one exclusive lock across each reload, mutation and save.

static void Stats_Path( char *out, int outSize ) {
	Com_sprintf( out, outSize, "%s/%s/%s",
		Cvar_VariableString( "fs_basepath" ), Cvar_VariableString( "fs_game" ), STATS_FILE );
}

// Same-inode lock shared with MBIIEZ sync; never release between read and write.
static int gStatsTxnFd = -1;
class StatsTransaction {
public:
    StatsTransaction() {
        char path[MAX_OSPATH];
        Stats_Path( path, sizeof( path ) );
        gStatsTxnFd = open( path, O_RDWR | O_CREAT, 0600 );
        if ( gStatsTxnFd >= 0 && flock( gStatsTxnFd, LOCK_EX ) != 0 ) {
            close( gStatsTxnFd );
            gStatsTxnFd = -1;
        }
    }
    ~StatsTransaction() {
        if ( gStatsTxnFd >= 0 ) {
            flock( gStatsTxnFd, LOCK_UN );
            close( gStatsTxnFd );
            gStatsTxnFd = -1;
        }
    }
    bool valid() const { return gStatsTxnFd >= 0; }
    StatsTransaction( const StatsTransaction& ) = delete;
    StatsTransaction& operator=( const StatsTransaction& ) = delete;
};

static void Stats_Load( void ) {
	char filepath[MAX_QPATH];
	int fd;
	off_t filelen;
	char *buf, *line, *nextline;

	gStatsRecordCount = 0;

	Stats_Path( filepath, sizeof( filepath ) );

	fd = gStatsTxnFd >= 0 ? gStatsTxnFd : open( filepath, O_RDONLY );
	if ( fd < 0 ) {
		return;
	}

	if ( gStatsTxnFd < 0 && flock( fd, LOCK_SH ) != 0 ) {
		if ( gStatsTxnFd < 0 ) close( fd );
		return;
	}

	filelen = lseek( fd, 0, SEEK_END );
	lseek( fd, 0, SEEK_SET );

	if ( filelen <= 0 ) {
		if ( gStatsTxnFd < 0 ) flock( fd, LOCK_UN );
		if ( gStatsTxnFd < 0 ) close( fd );
		return;
	}

	buf = (char *)Z_Malloc( (int)filelen + 1, TAG_TEMP_WORKSPACE );
	filelen = read( fd, buf, (size_t)filelen );
	if ( gStatsTxnFd < 0 ) flock( fd, LOCK_UN );
	if ( gStatsTxnFd < 0 ) close( fd );

	if ( filelen <= 0 ) {
		Z_Free( buf );
		return;
	}
	buf[filelen] = '\0';

	line = buf;
	while ( line && *line && gStatsRecordCount < STATS_MAX_PLAYERS ) {
		char *fields[5];
		int fieldCount = 0;
		char *cursor = line;

		nextline = strchr( line, '\n' );
		if ( nextline ) {
			*nextline = '\0';
		}

		fields[fieldCount++] = cursor;
		while ( fieldCount < 5 ) {
			char *pipe = strchr( cursor, '|' );
			if ( !pipe ) {
				break;
			}
			*pipe = '\0';
			cursor = pipe + 1;
			fields[fieldCount++] = cursor;
		}

		if ( fieldCount == 5 && fields[0][0] ) {
			statsRecord_t *rec = &gStatsRecords[gStatsRecordCount++];
			Com_Memset( rec, 0, sizeof( *rec ) );
			Q_strncpyz( rec->name, fields[0], sizeof( rec->name ) );
			rec->kills = atoi( fields[1] );
			rec->deaths = atoi( fields[2] );
			rec->suicides = atoi( fields[3] );
			rec->playtimeSeconds = atoi( fields[4] );
		}

		line = nextline ? nextline + 1 : NULL;
	}

	Z_Free( buf );
}

static void Stats_Save( void ) {
	char filepath[MAX_QPATH];
	int fd;
	int i;

	Stats_Path( filepath, sizeof( filepath ) );

	fd = gStatsTxnFd >= 0 ? gStatsTxnFd : open( filepath, O_WRONLY | O_CREAT, 0600 );
	if ( fd < 0 ) {
		return;
	}

	if ( gStatsTxnFd < 0 && flock( fd, LOCK_EX ) != 0 ) {
		if ( gStatsTxnFd < 0 ) close( fd );
		return;
	}

	if ( ftruncate( fd, 0 ) != 0 ) { /* best-effort; nothing else to do here */ }
	lseek( fd, 0, SEEK_SET );

	for ( i = 0; i < gStatsRecordCount; i++ ) {
		statsRecord_t *rec = &gStatsRecords[i];
		char line[STATS_NAME_SIZE + 64];
		int len = Com_sprintf( line, sizeof( line ), "%s|%d|%d|%d|%d\n",
			rec->name, rec->kills, rec->deaths, rec->suicides, rec->playtimeSeconds );
		if ( write( fd, line, (size_t)len ) != len ) { /* best-effort; nothing else to do here */ }
	}

	if ( gStatsTxnFd < 0 ) flock( fd, LOCK_UN );
	if ( gStatsTxnFd < 0 ) close( fd );
}

static statsRecord_t *Stats_Find( const char *key ) {
	int i;
	for ( i = 0; i < gStatsRecordCount; i++ ) {
		if ( !Q_stricmp( gStatsRecords[i].name, key ) ) {
			return &gStatsRecords[i];
		}
	}
	return NULL;
}

static statsRecord_t *Stats_FindOrCreate( const char *key ) {
	statsRecord_t *rec = Stats_Find( key );
	if ( rec ) {
		return rec;
	}
	if ( gStatsRecordCount >= STATS_MAX_PLAYERS ) {
		return NULL;
	}
	rec = &gStatsRecords[gStatsRecordCount++];
	Com_Memset( rec, 0, sizeof( *rec ) );
	Q_strncpyz( rec->name, key, sizeof( rec->name ) );
	return rec;
}

// Logged-in players are keyed by their economy handle; everyone else by
// their current in-game name. These two are deliberately kept in separate
// namespaces ("h:"/"n:" prefixes) rather than sharing one flat key space -
// nicknames are unrestricted (any player can type any name at any time),
// while handles are validated alnum/underscore-only by !register
// (SV_EconomyValidateHandle), so without this a player could set their
// nickname to literally match someone's registered handle and pollute (or,
// reading back via !stats, see) that real account's stats while never
// actually logging in as them. Both prefixes are applied unconditionally
// here, never taken from anything the player controls, so a nickname can
// never be crafted to produce a colliding "h:"-prefixed key: the most a
// literal "h:ricks" nickname can ever become is "n:h:ricks".
static void Stats_Key( client_t *cl, char *out, int outSize ) {
	if ( cl->economyHandle[0] ) {
		Com_sprintf( out, outSize, "h:%s", cl->economyHandle );
		return;
	}

	{
		char nameBuf[STATS_NAME_SIZE];
		char *p;

		// Sanitized the same way as before: a literal '|' in a player's
		// chosen name can never break the file's field delimiter.
		Q_strncpyz( nameBuf, cl->name, sizeof( nameBuf ) );
		for ( p = nameBuf; *p; p++ ) {
			if ( *p == '|' ) {
				*p = ' ';
			}
		}
		Com_sprintf( out, outSize, "n:%s", nameBuf );
	}
}

// The clean, player-facing form of whatever Stats_Key resolved to - for
// display only (chat output, "no stats yet" message), never for lookup.
static const char *Stats_DisplayName( client_t *cl ) {
	return cl->economyHandle[0] ? cl->economyHandle : cl->name;
}

static void Stats_RecordKill( const char *killerKey, const char *victimKey ) {
	statsRecord_t *killer, *victim;
	StatsTransaction transaction;
	if ( !transaction.valid() ) return;
	Stats_Load();
	killer = Stats_FindOrCreate( killerKey );
	if ( killer ) {
		killer->kills++;
	}
	victim = Stats_FindOrCreate( victimKey );
	if ( victim ) {
		victim->deaths++;
	}
	Stats_Save();
}

static void Stats_RecordSuicide( const char *key ) {
	statsRecord_t *rec;
	StatsTransaction transaction;
	if ( !transaction.valid() ) return;
	Stats_Load();
	rec = Stats_FindOrCreate( key );
	if ( rec ) {
		rec->deaths++;
		rec->suicides++;
	}
	Stats_Save();
}

static void Stats_RecordDeath( const char *key ) {
	statsRecord_t *rec;
	StatsTransaction transaction;
	if ( !transaction.valid() ) return;
	Stats_Load();
	rec = Stats_FindOrCreate( key );
	if ( rec ) {
		rec->deaths++;
	}
	Stats_Save();
}

static void Stats_AddPlaytime( const char *key, int seconds ) {
	statsRecord_t *rec;
	if ( seconds <= 0 ) {
		return;
	}
	StatsTransaction transaction;
	if ( !transaction.valid() ) return;
	Stats_Load();
	rec = Stats_FindOrCreate( key );
	if ( rec ) {
		rec->playtimeSeconds += seconds;
	}
	Stats_Save();
}

void SV_StatsShowCommand( client_t *cl ) {
	char key[STATS_NAME_SIZE];
	const char *displayName = Stats_DisplayName( cl );
	const qboolean loggedIn = cl->economyHandle[0] ? qtrue : qfalse;
	statsRecord_t *rec;
	char buf[512];
	int len = 0;

	Stats_Load();
	Stats_Key( cl, key, sizeof( key ) );
	rec = Stats_Find( key );

	if ( !rec ) {
		SV_SendServerCommand( cl, "chat \"^2[Stats]^7 No stats recorded yet for %s - go get some kills!\"\n", displayName );
		return;
	}

	{
		const float kd = rec->deaths > 0 ? (float)rec->kills / (float)rec->deaths : (float)rec->kills;
		const int hours = rec->playtimeSeconds / 3600;
		const int minutes = (rec->playtimeSeconds % 3600) / 60;

		len += Com_sprintf( buf + len, sizeof( buf ) - len, "^3=== STATS for %s (all servers) ===\n", displayName );
		len += Com_sprintf( buf + len, sizeof( buf ) - len,
			"^7Kills: ^2%d ^7Deaths: ^1%d ^7Suicides: ^1%d\n", rec->kills, rec->deaths, rec->suicides );
		len += Com_sprintf( buf + len, sizeof( buf ) - len, "^7K/D: ^5%.2f\n", kd );
		len += Com_sprintf( buf + len, sizeof( buf ) - len, "^7Time played: ^5%dh %dm", hours, minutes );

		if ( !loggedIn ) {
			len += Com_sprintf( buf + len, sizeof( buf ) - len,
				"\n^7(Tracked by your current name only - ^5!register^7 to keep these permanently.)" );
		}
	}

	SV_SendServerCommand( cl, "chat \"%s\"\n", buf );
}

void SV_StatsClientDisconnect( client_t *cl ) {
	int clientNum;

	if ( !cl ) {
		return;
	}
	clientNum = (int)(cl - svs.clients);
	if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
		return;
	}

	if ( g_statsEnable && g_statsEnable->integer && gStatsState[clientNum].active ) {
		const int elapsedSeconds = (svs.time - gStatsState[clientNum].lastFlushTime) / 1000;
		if ( elapsedSeconds > 0 ) {
			char key[STATS_NAME_SIZE];
			Stats_Key( cl, key, sizeof( key ) );
			Stats_AddPlaytime( key, elapsedSeconds );
		}
	}

	Com_Memset( &gStatsState[clientNum], 0, sizeof( gStatsState[clientNum] ) );
}

void SV_StatsFrame( void ) {
	int i;

	if ( !g_statsEnable || !g_statsEnable->integer ) {
		if ( gStatsWasEnabled ) {
			Com_Memset( gStatsState, 0, sizeof( gStatsState ) );
			gStatsWasEnabled = qfalse;
		}
		return;
	}
	if ( !gStatsWasEnabled ) Com_Printf( "MBIIEZ_STATS_TRANSACTION_V1\n" );
	gStatsWasEnabled = qtrue;

	for ( i = 0; i < sv_maxclients->integer; i++ ) {
		client_t *cl = &svs.clients[i];
		playerState_t *ps;
		int health;

		if ( cl->state < CS_ACTIVE || !cl->gentity || !cl->gentity->playerState ) {
			continue;
		}
		ps = cl->gentity->playerState;
		health = ps->stats[STAT_HEALTH];

		if ( !gStatsState[i].active ) {
			gStatsState[i].active = qtrue;
			gStatsState[i].lastHealth = health;
			gStatsState[i].lastFlushTime = svs.time;
			continue;
		}

		if ( svs.time - gStatsState[i].lastFlushTime >= STATS_PLAYTIME_FLUSH_MS ) {
			const int elapsedSeconds = (svs.time - gStatsState[i].lastFlushTime) / 1000;
			if ( elapsedSeconds > 0 ) {
				char key[STATS_NAME_SIZE];
				Stats_Key( cl, key, sizeof( key ) );
				Stats_AddPlaytime( key, elapsedSeconds );
				gStatsState[i].lastFlushTime = svs.time;
			}
		}

		// See file header for why PM_DEAD is required here, not just health
		// crossing zero: it's what rules out a round-transition artifact
		// rather than a genuine death, for all three cases below.
		if ( gStatsState[i].lastHealth > 0 && health <= 0 && ps->pm_type == MB2_PM_DEAD ) {
			const int attackerNum = ps->persistant[PERS_ATTACKER];
			char key[STATS_NAME_SIZE];
			Stats_Key( cl, key, sizeof( key ) );

			if ( attackerNum == i ) {
				Stats_RecordSuicide( key );
			} else if ( attackerNum >= 0 && attackerNum < sv_maxclients->integer &&
				svs.clients[attackerNum].state >= CS_ACTIVE ) {
				char attackerKey[STATS_NAME_SIZE];
				Stats_Key( &svs.clients[attackerNum], attackerKey, sizeof( attackerKey ) );
				Stats_RecordKill( attackerKey, key );
			} else {
				Stats_RecordDeath( key );
			}
		}

		gStatsState[i].lastHealth = health;
	}
}
