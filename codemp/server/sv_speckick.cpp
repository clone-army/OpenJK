/*
===========================================================================
Spectator kick (g_specKickRounds)

Players who sit in spectator round after round take a slot someone else
could play in. With g_specKickRounds N (0 = off), anyone still spectating
when N rounds have started is dropped - warned the round before. Joining
a team clears their count. Bots, and players logged in as admins (the
same admins as social mode's), are left alone.

A round is a fresh map or MBII's round-to-round map_restart: both re-init
the game (SV_SpecKickRoundStart, from sv_gameapi.cpp). The count is taken
a few seconds later (SV_SpecKickFrame), once everyone's back in after a map
change. A player's team is the "t" in their MB2_CS_PLAYERS configstring
(3 = spectator), as the blocked-class check reads it.
===========================================================================
*/

#include "server.h"

#define SPECKICK_DELAY_MS 10000

static struct {
	int  rounds;                 // rounds started with them in spectator, in a row
	char who[MAX_NAME_LENGTH + 64];  // who this slot's count is for (name + address)
} gSpecKick[MAX_CLIENTS];
static int gSpecKickCheckAt = 0;     // svs.time to take the count (0 = none due)

void SV_SpecKickRoundStart( void ) {
	gSpecKickCheckAt = svs.time + SPECKICK_DELAY_MS;
}

static qboolean SV_SpecKickSpectating( int c ) {
	char cs[MAX_STRING_CHARS];
	SV_GetConfigstring( MB2_CS_PLAYERS + c, cs, sizeof( cs ) );
	return ( cs[0] && atoi( Info_ValueForKey( cs, "t" ) ) == TEAM_SPECTATOR ) ? qtrue : qfalse;
}

void SV_SpecKickFrame( void ) {
	if ( !gSpecKickCheckAt || svs.time < gSpecKickCheckAt ) {
		return;
	}
	gSpecKickCheckAt = 0;
	const int limit = ( g_specKickRounds && g_specKickRounds->integer > 0 ) ? g_specKickRounds->integer : 0;

	for ( int c = 0; c < sv_maxclients->integer && c < MAX_CLIENTS; c++ ) {
		client_t *cl = &svs.clients[c];
		if ( cl->state != CS_ACTIVE || cl->netchan.remoteAddress.type == NA_BOT ) {
			gSpecKick[c].rounds = 0;
			gSpecKick[c].who[0] = '\0';
			continue;
		}
		// Someone new in this slot starts from nothing.
		const char *who = va( "%s|%s", cl->name, NET_AdrToString( cl->netchan.remoteAddress ) );
		if ( Q_stricmp( who, gSpecKick[c].who ) ) {
			Q_strncpyz( gSpecKick[c].who, who, sizeof( gSpecKick[c].who ) );
			gSpecKick[c].rounds = 0;
		}
		if ( !SV_SpecKickSpectating( c ) ) {
			gSpecKick[c].rounds = 0;
			continue;
		}
		gSpecKick[c].rounds++;
		if ( !limit || SV_SocialIsAdmin( cl ) ) {
			continue;
		}
		if ( gSpecKick[c].rounds >= limit ) {
			Com_Printf( "Spectator kick: %s spectated for %d rounds\n", cl->name, gSpecKick[c].rounds );
			SV_DropClient( cl, va( "was removed for spectating %d rounds in a row", gSpecKick[c].rounds ) );
			gSpecKick[c].rounds = 0;
			gSpecKick[c].who[0] = '\0';
		} else if ( gSpecKick[c].rounds == limit - 1 ) {
			SV_SendServerCommand( cl, "chat \"^3You've spectated %d round%s in a row - join a team next round, or you'll be removed to free the slot.\"\n",
				gSpecKick[c].rounds, gSpecKick[c].rounds == 1 ? "" : "s" );
		}
	}
}
