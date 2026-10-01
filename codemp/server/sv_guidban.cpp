/*
===========================================================================
GUID bans

One list of banned ja_guids for every server, kept in a text file beside the
game data (fs_basepath/<game>/guidbans.txt, or sv_guidBanFile) so all the
instances and the web panel's GUID Bans page share it. A player whose
ja_guid is on it is refused at connect, and one already in a server when the
file changes (a ban added on the web panel or another server) is dropped
within a few seconds.

A client's ja_guid is salted with the server's address (cl_guidServerUniq),
so the same player has a different GUID on every server. A ban follows him
by IP instead: any GUID seen on an IP that a banned GUID was seen on in the
last GUIDBAN_LINK_DAYS is banned too, as it connects (or straight away, if
it's already in a server).

Every drop bumps that line's count. A refused client keeps resending its
connect every few seconds, so one ban counts at most once a minute.

Each line, tab separated (times are unix seconds, 0 for never):
  GUID  drops  last drop  last name  last ip  added  from ip ban  note
Only the GUID is needed when editing by hand. "from ip ban" is the IP whose
ban brought this one in (mbiiez/bansync.py bans the GUIDs seen on an IP when
it's banned, and lifts them with it), empty otherwise.

Every connect is noted in guidseen.txt beside it, kept for GUIDSEEN_DAYS:
  ip  GUID  last seen  name

Both files are read and written under an flock on "<guidbans.txt>.lock",
which mbiiez/guidbans.py takes as well.
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
#define GUIDBAN_LINK_DAYS		7
#define GUIDSEEN_MAX			16384
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

typedef struct {
	char	ip[NET_ADDRSTRMAXLEN];
	char	guid[64];
	long	seen;
	char	name[MAX_NAME_LENGTH * 2];
} guidSeen_t;

static guidBan_t	guidBans[GUIDBAN_MAX];
static int			guidBanCount;
static guidSeen_t	guidSeen[GUIDSEEN_MAX];
static int			guidSeenCount;
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

// guidseen.txt, in the same directory
static const char *GuidSeen_Path( void ) {
	static char path[MAX_OSPATH];
	const char *banPath = GuidBan_Path();
	const char *slash = strrchr( banPath, '/' );
	const int dirLen = slash ? (int)( slash - banPath ) + 1 : 0;

	Com_sprintf( path, sizeof( path ), "%.*sguidseen.txt", dirLen, banPath );
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

// tabs and line breaks would break the files' columns
static void GuidBan_CopyField( char *dest, const char *src, int size ) {
	Q_strncpyz( dest, src, size );
	for ( char *p = dest; *p; p++ ) {
		if ( *p == '\t' || *p == '\r' || *p == '\n' ) {
			*p = ' ';
		}
	}
}

// "1.2.3.4:29070" -> "1.2.3.4"
static void GuidBan_Addr( char *dest, const char *ip, int size ) {
	Q_strncpyz( dest, ip, size );
	dest[strcspn( dest, ":" )] = '\0';
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

static void GuidSeen_Load( void ) {
	FILE *f = fopen( GuidSeen_Path(), "r" );
	char line[512];
	const long now = (long)time( NULL );

	guidSeenCount = 0;
	if ( !f ) {
		return;
	}
	while ( guidSeenCount < GUIDSEEN_MAX && fgets( line, sizeof( line ), f ) ) {
		guidSeen_t *s = &guidSeen[guidSeenCount];
		Com_Memset( s, 0, sizeof( *s ) );
		line[strcspn( line, "\r\n" )] = '\0';
		if ( sscanf( line, "%47[^\t]\t%63[^\t]\t%ld\t%63[^\t]", s->ip, s->guid, &s->seen, s->name ) < 3 ) {
			continue;
		}
		if ( now - s->seen > GUIDSEEN_DAYS * 86400L ) {
			continue;
		}
		guidSeenCount++;
	}
	fclose( f );
}

static void GuidSeen_Save( void ) {
	char tmp[MAX_OSPATH + 8];
	Com_sprintf( tmp, sizeof( tmp ), "%s.tmp", GuidSeen_Path() );

	FILE *f = fopen( tmp, "w" );
	if ( !f ) {
		return;
	}
	for ( int i = 0; i < guidSeenCount; i++ ) {
		const guidSeen_t *s = &guidSeen[i];
		fprintf( f, "%s\t%s\t%ld\t%s\n", s->ip, s->guid, s->seen, s->name );
	}
	fclose( f );
	rename( tmp, GuidSeen_Path() );
}

// note this GUID on this IP now (with guidSeen loaded)
static void GuidSeen_Note( const char *addr, const char *guid, const char *name ) {
	guidSeen_t *s = NULL;

	for ( int i = 0; i < guidSeenCount; i++ ) {
		if ( !strcmp( guidSeen[i].ip, addr ) && !Q_stricmp( guidSeen[i].guid, guid ) ) {
			s = &guidSeen[i];
			break;
		}
	}
	if ( !s ) {
		if ( guidSeenCount >= GUIDSEEN_MAX ) {
			// full: lose the oldest (the file's kept in the order seen)
			memmove( &guidSeen[0], &guidSeen[1], ( GUIDSEEN_MAX - 1 ) * sizeof( guidSeen[0] ) );
			guidSeenCount--;
		}
		s = &guidSeen[guidSeenCount++];
		Com_Memset( s, 0, sizeof( *s ) );
		Q_strncpyz( s->ip, addr, sizeof( s->ip ) );
		Q_strncpyz( s->guid, guid, sizeof( s->guid ) );
	}
	s->seen = (long)time( NULL );
	GuidBan_CopyField( s->name, name, sizeof( s->name ) );
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

// a banned GUID seen on this IP lately, if any (with both lists loaded)
static guidBan_t *GuidBan_BannedOnIp( const char *addr ) {
	const long now = (long)time( NULL );

	for ( int i = 0; i < guidSeenCount; i++ ) {
		const guidSeen_t *s = &guidSeen[i];
		if ( strcmp( s->ip, addr ) || now - s->seen > GUIDBAN_LINK_DAYS * 86400L ) {
			continue;
		}
		guidBan_t *b = GuidBan_Find( s->guid );
		if ( b ) {
			return b;
		}
	}
	return NULL;
}

static guidBan_t *GuidBan_Add( const char *guid, const char *name, const char *addr, const char *banIp, const char *note ) {
	if ( guidBanCount >= GUIDBAN_MAX ) {
		Com_Printf( "GUID bans: the list is full (%d)\n", GUIDBAN_MAX );
		return NULL;
	}
	guidBan_t *b = &guidBans[guidBanCount++];
	Com_Memset( b, 0, sizeof( *b ) );
	sscanf( guid, "%63s", b->guid );
	GuidBan_CopyField( b->name, name, sizeof( b->name ) );
	GuidBan_CopyField( b->ip, addr, sizeof( b->ip ) );
	b->added = (long)time( NULL );
	GuidBan_CopyField( b->banIp, banIp, sizeof( b->banIp ) );
	GuidBan_CopyField( b->note, note, sizeof( b->note ) );
	return b;
}

// this GUID is banned or, seen on the same IP as a banned one, now is
static guidBan_t *GuidBan_Check( const char *guid, const char *name, const char *addr ) {
	guidBan_t *b = GuidBan_Find( guid );

	if ( !b ) {
		const guidBan_t *linked = GuidBan_BannedOnIp( addr );
		if ( linked ) {
			char note[256];
			Com_sprintf( note, sizeof( note ), "same IP (%s) as banned %s", addr, linked->guid );
			b = GuidBan_Add( guid, name, addr, linked->banIp, note );
			if ( b ) {
				Com_Printf( "GUID ban: %s banned too - %s\n", b->guid, note );
			}
		}
	}
	return b;
}

static void GuidBan_CountDrop( guidBan_t *b, const char *name, const char *addr ) {
	const long now = (long)time( NULL );

	if ( now - b->lastDrop >= GUIDBAN_RECOUNT_SECONDS ) {
		b->drops++;
		b->lastDrop = now;
	}
	GuidBan_CopyField( b->name, name, sizeof( b->name ) );
	GuidBan_CopyField( b->ip, addr, sizeof( b->ip ) );
}

/*
==================
SV_GuidBanned

SV_DirectConnect: notes the connect in guidseen.txt, then is this
userinfo's ja_guid banned (or on a banned GUID's IP)? Counts the drop if so.
==================
*/
qboolean SV_GuidBanned( const char *userinfo, const char *ip ) {
	const char *guid = Info_ValueForKey( userinfo, "ja_guid" );
	const char *name = Info_ValueForKey( userinfo, "name" );
	char addr[NET_ADDRSTRMAXLEN];

	if ( !guid[0] ) {
		return qfalse;
	}
	GuidBan_Addr( addr, ip, sizeof( addr ) );

	const int fd = GuidBan_Lock();
	GuidBan_Load();
	GuidSeen_Load();
	GuidSeen_Note( addr, guid, name );
	GuidSeen_Save();
	guidBan_t *b = GuidBan_Check( guid, name, addr );
	if ( b ) {
		GuidBan_CountDrop( b, name, addr );
		GuidBan_Save();
		Com_Printf( "GUID ban: refused %s" S_COLOR_WHITE " (%s, %s), drop %d\n", name, addr, b->guid, b->drops );
	}
	GuidBan_Unlock( fd );
	return b ? qtrue : qfalse;
}

// drop everyone in the server who's banned (with both lists loaded)
static void GuidBan_DropConnectedLocked( void ) {
	qboolean changed = qfalse;

	if ( !com_sv_running->integer || !svs.clients ) {
		return;
	}

	for ( int i = 0; i < sv_maxclients->integer; i++ ) {
		client_t *cl = &svs.clients[i];
		char addr[NET_ADDRSTRMAXLEN];

		if ( cl->state < CS_CONNECTED || cl->netchan.remoteAddress.type == NA_BOT ) {
			continue;
		}
		GuidBan_Addr( addr, NET_AdrToString( cl->netchan.remoteAddress ), sizeof( addr ) );
		guidBan_t *b = GuidBan_Check( Info_ValueForKey( cl->userinfo, "ja_guid" ), cl->name, addr );
		if ( !b ) {
			continue;
		}
		GuidBan_CountDrop( b, cl->name, addr );
		Com_Printf( "GUID ban: dropped %s" S_COLOR_WHITE " (%s, %s), drop %d\n", cl->name, addr, b->guid, b->drops );
		changed = qtrue;
		SV_DropClient( cl, "was banned" );
	}
	if ( changed ) {
		GuidBan_Save();
	}
}

static void GuidBan_DropConnected( void ) {
	const int fd = GuidBan_Lock();
	GuidBan_Load();
	GuidSeen_Load();
	GuidBan_DropConnectedLocked();
	GuidBan_Unlock( fd );
}

/*
==================
SV_GuidBanFrame

Every few seconds, if the ban list has changed, drop anyone on it.
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
	char guid[64] = "", name[MAX_NAME_LENGTH * 2] = "", addr[NET_ADDRSTRMAXLEN] = "";

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "Usage: banguid <client number | guid> [note]\n" );
		return;
	}

	const char *arg = Cmd_Argv( 1 );
	if ( strlen( arg ) <= 2 && Q_isanumber( arg ) ) {
		const int n = atoi( arg );
		if ( !com_sv_running->integer || !svs.clients || n < 0 || n >= sv_maxclients->integer || svs.clients[n].state < CS_CONNECTED ) {
			Com_Printf( "banguid: no client %d\n", n );
			return;
		}
		client_t *cl = &svs.clients[n];
		sscanf( Info_ValueForKey( cl->userinfo, "ja_guid" ), "%63s", guid );
		if ( !guid[0] ) {
			Com_Printf( "banguid: client %d has no GUID\n", n );
			return;
		}
		Q_strncpyz( name, cl->name, sizeof( name ) );
		GuidBan_Addr( addr, NET_AdrToString( cl->netchan.remoteAddress ), sizeof( addr ) );
	} else {
		sscanf( arg, "%63s", guid );
	}

	const int fd = GuidBan_Lock();
	GuidBan_Load();
	GuidSeen_Load();
	if ( GuidBan_Find( guid ) ) {
		Com_Printf( "banguid: %s is already banned\n", guid );
	} else if ( GuidBan_Add( guid, name, addr, "", Cmd_ArgsFrom( 2 ) ) ) {
		GuidBan_Save();
		Com_Printf( "banguid: banned %s\n", guid );
		// drops them, and anyone here on their IPs, now
		GuidBan_DropConnectedLocked();
	}
	GuidBan_Unlock( fd );
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
