/*
===========================================================================
GUID bans

One list of banned ja_guids for every server, kept in a text file beside the
game data (fs_basepath/<game>/guidbans.txt, or sv_guidBanFile) so all the
instances and the web panel's GUID Bans page share it. A player whose
ja_guid is on it is refused at connect, and one already in a server when the
file changes (a ban added on the web panel or another server) is dropped
within a few seconds.

Every drop bumps that line's count. A refused client keeps resending its
connect every few seconds, so one ban counts at most once a minute.

Each line, tab separated (times are unix seconds, 0 for never):
  GUID  drops  last drop  last name  last ip  added  from ip ban  note
Only the GUID is needed when editing by hand. "from ip ban" is the IP whose
ban brought this one in (mbiiez/bansync.py bans the GUIDs seen on an IP when
it's banned, and lifts them with it), empty otherwise.

So that can work, every connect is noted in guidseen.txt beside it:
  ip  GUID  last seen  name
kept for GUIDSEEN_DAYS.

Both files are read and written under an flock on "<file>.lock", which
mbiiez/guidbans.py takes as well.
===========================================================================
*/

#include "server.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GUIDBAN_MAX				1024
#define GUIDBAN_RECOUNT_SECONDS	60
#define GUIDBAN_CHECK_MSEC		5000
#define GUIDSEEN_DAYS			30

typedef struct {
	char	guid[64];
	int		drops;
	long	lastDrop;
	char	name[MAX_NAME_LENGTH * 2];
	char	ip[NET_ADDRSTRMAXLEN];
	long	added;
	char	banIp[NET_ADDRSTRMAXLEN];
	char	note[256];
} guidBan_t;

static guidBan_t	guidBans[GUIDBAN_MAX];
static int			guidBanCount;
static cvar_t		*sv_guidBanFile;

static const char *GuidBan_Path( void ) {
	static char path[MAX_OSPATH];

	if ( !sv_guidBanFile ) {
		sv_guidBanFile = Cvar_Get( "sv_guidBanFile", "", CVAR_ARCHIVE, "GUID ban list shared by every server (default fs_basepath/<game>/guidbans.txt)" );
	}
	if ( sv_guidBanFile->string[0] ) {
		Q_strncpyz( path, sv_guidBanFile->string, sizeof( path ) );
	} else {
		Com_sprintf( path, sizeof( path ), "%s/%s/guidbans.txt", Cvar_VariableString( "fs_basepath" ), FS_GetCurrentGameDir() );
	}
	return path;
}

static int GuidBan_Lock( void ) {
	const int fd = open( va( "%s.lock", GuidBan_Path() ), O_RDWR | O_CREAT, 0644 );
	if ( fd >= 0 ) {
		flock( fd, LOCK_EX );
	}
	return fd;
}

static void GuidBan_Unlock( int fd ) {
	if ( fd >= 0 ) {
		flock( fd, LOCK_UN );
		close( fd );
	}
}

// tabs and line breaks would break the file's columns
static void GuidBan_CopyField( char *dest, const char *src, int size ) {
	Q_strncpyz( dest, src, size );
	for ( char *p = dest; *p; p++ ) {
		if ( *p == '\t' || *p == '\r' || *p == '\n' ) {
			*p = ' ';
		}
	}
}

static void GuidBan_Load( void ) {
	FILE *f = fopen( GuidBan_Path(), "r" );
	char line[1024];

	guidBanCount = 0;
	if ( !f ) {
		return;
	}
	while ( guidBanCount < GUIDBAN_MAX && fgets( line, sizeof( line ), f ) ) {
		char *fields[8] = { NULL };
		int n = 0;

		line[strcspn( line, "\r\n" )] = '\0';
		if ( !line[0] || line[0] == '#' ) {
			continue;
		}
		fields[n++] = line;
		for ( char *p = line; *p && n < 8; p++ ) {
			if ( *p == '\t' ) {
				*p = '\0';
				fields[n++] = p + 1;
			}
		}

		guidBan_t *b = &guidBans[guidBanCount];
		Com_Memset( b, 0, sizeof( *b ) );
		sscanf( fields[0], "%63s", b->guid );	// drops stray spaces
		if ( !b->guid[0] ) {
			continue;
		}
		if ( fields[1] ) b->drops = atoi( fields[1] );
		if ( fields[2] ) b->lastDrop = atol( fields[2] );
		if ( fields[3] ) Q_strncpyz( b->name, fields[3], sizeof( b->name ) );
		if ( fields[4] ) Q_strncpyz( b->ip, fields[4], sizeof( b->ip ) );
		if ( fields[5] ) b->added = atol( fields[5] );
		if ( fields[6] ) Q_strncpyz( b->banIp, fields[6], sizeof( b->banIp ) );
		if ( fields[7] ) Q_strncpyz( b->note, fields[7], sizeof( b->note ) );
		guidBanCount++;
	}
	fclose( f );
}

static void GuidBan_Save( void ) {
	FILE *f = fopen( GuidBan_Path(), "w" );

	if ( !f ) {
		Com_Printf( "GUID bans: can't write %s\n", GuidBan_Path() );
		return;
	}
	fprintf( f, "# GUID bans for every server. Tab separated, times in unix seconds:\n" );
	fprintf( f, "# GUID\tdrops\tlast drop\tlast name\tlast ip\tadded\tfrom ip ban\tnote\n" );
	for ( int i = 0; i < guidBanCount; i++ ) {
		const guidBan_t *b = &guidBans[i];
		fprintf( f, "%s\t%d\t%ld\t%s\t%s\t%ld\t%s\t%s\n", b->guid, b->drops, b->lastDrop, b->name, b->ip, b->added, b->banIp, b->note );
	}
	fclose( f );
}

static guidBan_t *GuidBan_Find( const char *guid ) {
	if ( !guid || !guid[0] ) {
		return NULL;
	}
	for ( int i = 0; i < guidBanCount; i++ ) {
		if ( !Q_stricmp( guidBans[i].guid, guid ) ) {
			return &guidBans[i];
		}
	}
	return NULL;
}

static void GuidBan_CountDrop( guidBan_t *b, const char *name, const char *ip ) {
	const long now = (long)time( NULL );

	if ( now - b->lastDrop >= GUIDBAN_RECOUNT_SECONDS ) {
		b->drops++;
		b->lastDrop = now;
	}
	GuidBan_CopyField( b->name, name, sizeof( b->name ) );
	GuidBan_CopyField( b->ip, ip, sizeof( b->ip ) );
}

/*
==================
SV_GuidBanned

SV_DirectConnect: is this userinfo's ja_guid banned? Counts the drop if so.
==================
*/
qboolean SV_GuidBanned( const char *userinfo, const char *ip ) {
	const char *guid = Info_ValueForKey( userinfo, "ja_guid" );

	if ( !guid[0] ) {
		return qfalse;
	}

	const int fd = GuidBan_Lock();
	GuidBan_Load();
	guidBan_t *b = GuidBan_Find( guid );
	if ( b ) {
		GuidBan_CountDrop( b, Info_ValueForKey( userinfo, "name" ), ip );
		GuidBan_Save();
		Com_Printf( "GUID ban: refused %s" S_COLOR_WHITE " (%s, %s), drop %d\n", Info_ValueForKey( userinfo, "name" ), ip, b->guid, b->drops );
	}
	GuidBan_Unlock( fd );
	return b ? qtrue : qfalse;
}

/*
==================
SV_GuidSeen

SV_DirectConnect: note this GUID connecting from this IP in guidseen.txt,
dropping anything older than GUIDSEEN_DAYS.
==================
*/
void SV_GuidSeen( const char *userinfo, const char *ip ) {
	const char *guid = Info_ValueForKey( userinfo, "ja_guid" );
	char path[MAX_OSPATH], tmp[MAX_OSPATH + 8], addr[NET_ADDRSTRMAXLEN], name[MAX_NAME_LENGTH * 2];
	char line[512];

	if ( !guid[0] ) {
		return;
	}
	// the IP without its port
	Q_strncpyz( addr, ip, sizeof( addr ) );
	addr[strcspn( addr, ":" )] = '\0';
	GuidBan_CopyField( name, Info_ValueForKey( userinfo, "name" ), sizeof( name ) );

	Com_sprintf( path, sizeof( path ), "%s", GuidBan_Path() );
	char *slash = strrchr( path, '/' );
	Q_strncpyz( slash ? slash + 1 : path, "guidseen.txt", sizeof( path ) - ( slash ? slash + 1 - path : 0 ) );
	Com_sprintf( tmp, sizeof( tmp ), "%s.tmp", path );

	const long now = (long)time( NULL );
	const int fd = GuidBan_Lock();
	FILE *out = fopen( tmp, "w" );
	if ( out ) {
		FILE *in = fopen( path, "r" );
		if ( in ) {
			while ( fgets( line, sizeof( line ), in ) ) {
				char lineIp[64], lineGuid[64];
				long seen;
				if ( sscanf( line, "%63[^\t]\t%63[^\t]\t%ld", lineIp, lineGuid, &seen ) != 3 ) {
					continue;
				}
				if ( now - seen > GUIDSEEN_DAYS * 86400L ) {
					continue;
				}
				if ( !strcmp( lineIp, addr ) && !Q_stricmp( lineGuid, guid ) ) {
					continue;	// rewritten below
				}
				fputs( line, out );
			}
			fclose( in );
		}
		fprintf( out, "%s\t%s\t%ld\t%s\n", addr, guid, now, name );
		fclose( out );
		rename( tmp, path );
	}
	GuidBan_Unlock( fd );
}

// drop everyone in the server whose GUID is on the list
static void GuidBan_DropConnected( void ) {
	qboolean changed = qfalse;
	const int fd = GuidBan_Lock();

	GuidBan_Load();
	for ( int i = 0; i < sv_maxclients->integer; i++ ) {
		client_t *cl = &svs.clients[i];
		if ( cl->state < CS_CONNECTED || cl->netchan.remoteAddress.type == NA_BOT ) {
			continue;
		}
		guidBan_t *b = GuidBan_Find( Info_ValueForKey( cl->userinfo, "ja_guid" ) );
		if ( !b ) {
			continue;
		}
		GuidBan_CountDrop( b, cl->name, NET_AdrToString( cl->netchan.remoteAddress ) );
		Com_Printf( "GUID ban: dropped %s" S_COLOR_WHITE " (%s, %s), drop %d\n", cl->name, b->ip, b->guid, b->drops );
		changed = qtrue;
		SV_DropClient( cl, "was banned" );
	}
	if ( changed ) {
		GuidBan_Save();
	}
	GuidBan_Unlock( fd );
}

/*
==================
SV_GuidBanFrame

Every few seconds, if the file has changed, drop anyone on it.
==================
*/
void SV_GuidBanFrame( void ) {
	static int nextCheck;
	static struct timespec lastMtime;
	const int now = Sys_Milliseconds();
	struct stat st;

	if ( now < nextCheck && nextCheck - now <= GUIDBAN_CHECK_MSEC ) {
		return;
	}
	nextCheck = now + GUIDBAN_CHECK_MSEC;

	if ( stat( GuidBan_Path(), &st ) != 0 ) {
		return;
	}
	if ( st.st_mtim.tv_sec == lastMtime.tv_sec && st.st_mtim.tv_nsec == lastMtime.tv_nsec ) {
		return;
	}
	lastMtime = st.st_mtim;
	GuidBan_DropConnected();
}

static void SV_BanGuid_f( void ) {
	char guid[64], name[MAX_NAME_LENGTH * 2] = "", ip[NET_ADDRSTRMAXLEN] = "";

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "Usage: banguid <client number | guid> [note]\n" );
		return;
	}

	const char *arg = Cmd_Argv( 1 );
	if ( strlen( arg ) <= 2 && Q_isanumber( arg ) ) {
		const int n = atoi( arg );
		if ( n < 0 || n >= sv_maxclients->integer || svs.clients[n].state < CS_CONNECTED ) {
			Com_Printf( "banguid: no client %d\n", n );
			return;
		}
		client_t *cl = &svs.clients[n];
		Q_strncpyz( guid, Info_ValueForKey( cl->userinfo, "ja_guid" ), sizeof( guid ) );
		if ( !guid[0] ) {
			Com_Printf( "banguid: client %d has no GUID\n", n );
			return;
		}
		GuidBan_CopyField( name, cl->name, sizeof( name ) );
		Q_strncpyz( ip, NET_AdrToString( cl->netchan.remoteAddress ), sizeof( ip ) );
	} else {
		sscanf( arg, "%63s", guid );
	}

	const int fd = GuidBan_Lock();
	GuidBan_Load();
	if ( GuidBan_Find( guid ) ) {
		Com_Printf( "banguid: %s is already banned\n", guid );
	} else if ( guidBanCount >= GUIDBAN_MAX ) {
		Com_Printf( "banguid: the list is full (%d)\n", GUIDBAN_MAX );
	} else {
		guidBan_t *b = &guidBans[guidBanCount++];
		Com_Memset( b, 0, sizeof( *b ) );
		Q_strncpyz( b->guid, guid, sizeof( b->guid ) );
		Q_strncpyz( b->name, name, sizeof( b->name ) );
		Q_strncpyz( b->ip, ip, sizeof( b->ip ) );
		b->added = (long)time( NULL );
		GuidBan_CopyField( b->note, Cmd_ArgsFrom( 2 ), sizeof( b->note ) );
		GuidBan_Save();
		Com_Printf( "banguid: banned %s\n", guid );
	}
	GuidBan_Unlock( fd );

	GuidBan_DropConnected();
}

static void SV_UnbanGuid_f( void ) {
	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "Usage: unbanguid <guid>\n" );
		return;
	}

	const int fd = GuidBan_Lock();
	GuidBan_Load();
	guidBan_t *b = GuidBan_Find( Cmd_Argv( 1 ) );
	if ( b ) {
		Com_Printf( "unbanguid: unbanned %s\n", b->guid );
		*b = guidBans[--guidBanCount];
		GuidBan_Save();
	} else {
		Com_Printf( "unbanguid: %s isn't banned\n", Cmd_Argv( 1 ) );
	}
	GuidBan_Unlock( fd );
}

static void SV_GuidBans_f( void ) {
	const int fd = GuidBan_Lock();
	GuidBan_Load();
	GuidBan_Unlock( fd );

	Com_Printf( "%d GUID ban(s) in %s\n", guidBanCount, GuidBan_Path() );
	for ( int i = 0; i < guidBanCount; i++ ) {
		const guidBan_t *b = &guidBans[i];
		Com_Printf( "%s  drops %d  last %s" S_COLOR_WHITE " (%s)  %s\n", b->guid, b->drops, b->name[0] ? b->name : "-", b->ip[0] ? b->ip : "-", b->note );
	}
}

void SV_GuidBanInit( void ) {
	GuidBan_Path();	// registers sv_guidBanFile
	Cmd_AddCommand( "banguid", SV_BanGuid_f, "Ban a GUID on every server: banguid <client number | guid> [note]" );
	Cmd_AddCommand( "unbanguid", SV_UnbanGuid_f, "Unban a GUID: unbanguid <guid>" );
	Cmd_AddCommand( "guidbans", SV_GuidBans_f, "List the GUID bans" );
}
