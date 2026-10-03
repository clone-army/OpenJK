/*
===========================================================================
game_results.cpp — a log of every game played for credits (or prizes)

One JSON line per settled result, appended to <fs_game>/game_results.log -
shared by every server process, like the economy's accounts file, so the
web panel can show wins, plays, credits won and lost, leaderboards and
history from it. Appends are one write() each under an exclusive flock(),
so lines from servers writing at once never interleave.

  {"t":1759500000,"port":29071,"server":"CloneArmy|NA|Social","game":"pazaak",
   "account":"ricks","name":"CA [212] CE-Ricks","result":"win","stake":50,
   "net":50,"vs":"mensis","details":"sets 2-1"}

result: win, loss, push, refund (stake back), or prize (spin). stake: what
they put in; net: credits up or down once settled (0 for a push or refund).
account: their economy login ("" if none); name: their in-game name at the
time, colour codes stripped.
===========================================================================
*/

#include "server.h"

#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/file.h>

#define GAME_RESULTS_FILE "game_results.log"

// s as a JSON string's contents: quotes, backslashes and control characters escaped.
static void GameResults_Escape( char *out, int outSize, const char *s ) {
	int n = 0;
	for ( ; s && *s && n < outSize - 7; s++ ) {
		const unsigned char c = (unsigned char)*s;
		if ( c == '"' || c == '\\' ) {
			out[n++] = '\\';
			out[n++] = (char)c;
		} else if ( c < 0x20 ) {
			n += Com_sprintf( out + n, outSize - n, "\\u%04x", c );
		} else {
			out[n++] = (char)c;
		}
	}
	out[n] = '\0';
}

static void GameResults_Clean( char *out, int outSize, const char *s ) {
	Q_strncpyz( out, s ? s : "", outSize );
	Q_StripColor( out );
}

void SV_GameResult( const char *game, const char *account, const char *name, const char *result,
	int stake, int net, const char *vs, const char *details ) {
	char path[MAX_OSPATH], line[1400];
	char eGame[64], eAccount[64], eName[128], eResult[32], eVs[128], eDetails[512], eServer[160];
	char clean[MAX_STRING_CHARS];

	GameResults_Escape( eGame, sizeof( eGame ), game );
	GameResults_Escape( eAccount, sizeof( eAccount ), account );
	GameResults_Clean( clean, sizeof( clean ), name );
	GameResults_Escape( eName, sizeof( eName ), clean );
	GameResults_Escape( eResult, sizeof( eResult ), result );
	GameResults_Clean( clean, sizeof( clean ), vs );
	GameResults_Escape( eVs, sizeof( eVs ), clean );
	GameResults_Clean( clean, sizeof( clean ), details );
	GameResults_Escape( eDetails, sizeof( eDetails ), clean );
	GameResults_Clean( clean, sizeof( clean ), Cvar_VariableString( "sv_hostname" ) );
	GameResults_Escape( eServer, sizeof( eServer ), clean );

	const int len = Com_sprintf( line, sizeof( line ),
		"{\"t\":%ld,\"port\":%d,\"server\":\"%s\",\"game\":\"%s\",\"account\":\"%s\",\"name\":\"%s\","
		"\"result\":\"%s\",\"stake\":%d,\"net\":%d,\"vs\":\"%s\",\"details\":\"%s\"}\n",
		(long)time( NULL ), Cvar_VariableIntegerValue( "net_port" ), eServer, eGame, eAccount, eName,
		eResult, stake, net, eVs, eDetails );

	Com_sprintf( path, sizeof( path ), "%s/%s/%s",
		Cvar_VariableString( "fs_basepath" ), Cvar_VariableString( "fs_game" ), GAME_RESULTS_FILE );
	const int fd = open( path, O_WRONLY | O_APPEND | O_CREAT, 0644 );
	if ( fd < 0 ) {
		return;
	}
	if ( flock( fd, LOCK_EX ) == 0 ) {
		const ssize_t wrote = write( fd, line, (size_t)len );
		(void)wrote;
		flock( fd, LOCK_UN );
	}
	close( fd );
}

void SV_GameResultClient( client_t *cl, const char *game, const char *result, int stake, int net,
	const char *vs, const char *details ) {
	if ( !cl ) {
		return;
	}
	SV_GameResult( game, cl->economyHandle, cl->name, result, stake, net, vs, details );
}
