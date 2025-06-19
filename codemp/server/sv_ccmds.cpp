/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2005 - 2015, ioquake3 contributors
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#include <thread>
#include <array>

#include "server.h"
#include "spin.h"
#include "game/bg_mb2.h"
#include "qcommon/stringed_ingame.h"
#include "server/sv_gameapi.h"
#include "qcommon/game_version.h"
#include "game/bg_weapons.h"

#define SVTELL_PREFIX "\x19[Server^7\x19]\x19: "
#define SVSAY_PREFIX "Server^7\x19: "
#define SPAWN_VEHICLE_SUFFIX "(Spawns in 5 seconds)"

/*
===============================================================================
Move Client commands
===============================================================================
*/

void SV_ExecuteClientCommandDelayed_h(client_t* cl, char* cmd, int delay)
{
	std::this_thread::sleep_for(std::chrono::seconds(delay));
	Cvar_Set("sv_cheats", "1");
	GVM_RunFrame(sv.time);
	SV_ExecuteClientCommand(cl, cmd, qtrue);
	Cvar_Set("sv_cheats", "0");
	GVM_RunFrame(sv.time);
}

void SV_ClientTimedPowerup_h(client_t* cl, int pu, int duration)
{
	cl->gentity->playerState->powerups[pu] |= (1 << 21);
	std::this_thread::sleep_for(std::chrono::seconds(duration));
	cl->gentity->playerState->powerups[pu] = -1;
}

void SV_ExecuteClientCommandDelayed(client_t* cl, char* cmd, int delay)
{
	//SV_ExecuteClientCommandDelayed_h(cl, cmd, delay);
	std::thread(SV_ExecuteClientCommandDelayed_h, cl, cmd, delay).detach();
}

void SV_ClientTimedPowerup(client_t* cl, int pu, int duration)
{
	//SV_ClientTimedPowerup_h(cl, pu, duration);
	std::thread(SV_ClientTimedPowerup_h, cl, pu, duration).detach();
}

/*
===============================================================================

OPERATOR CONSOLE ONLY COMMANDS

These commands can only be entered from stdin or by a remote operator datagram
===============================================================================
*/

const char *SV_GetStringEdString(char *refSection, char *refName)
{
	//Well, it would've been lovely doing it the above way, but it would mean mixing
	//languages for the client depending on what the server is. So we'll mark this as
	//a stringed reference with @@@ and send the refname to the client, and when it goes
	//to print it will get scanned for the stringed reference indication and dealt with
	//properly.
	static char text[1024]={0};
	Com_sprintf(text, sizeof(text), "@@@%s", refName);
	return text;
}

/*
Reusable version of SV_GetPlayerByHandle() that doesn't
print any silly messages.
*/
client_t * SV_BetterGetPlayerByHandle(const char* handle)
{
	client_t* cl;
	int i;
	char cleanName[64];

	// make sure server is running
	if (!com_sv_running->integer) {
		return NULL;
	}

	// Check whether this is a numeric player handle
	for (i = 0; handle[i] >= '0' && handle[i] <= '9'; i++);

	if (!handle[i])
	{
		int plid = atoi(handle);

		// Check for numeric playerid match
		if (plid >= 0 && plid < sv_maxclients->integer)
		{
			cl = &svs.clients[plid];

			if (cl->state)
				return cl;
		}
	}

	// check for a name match
	for (i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++) {
		if (!cl->state) {
			continue;
		}
		if (!Q_stricmp(cl->name, handle)) {
			return cl;
		}

		Q_strncpyz(cleanName, cl->name, sizeof(cleanName));
		Q_CleanStr(cleanName);
		if (!Q_stricmp(cleanName, handle)) {
			return cl;
		}
	}

	return NULL;
}


/*

==================
SV_GetPlayerByHandle

Returns the player with player id or name from Cmd_Argv(1)
==================
*/
static client_t *SV_GetPlayerByHandle( void ) {
	client_t	*cl;
	int			i;
	char		*s;
	char		cleanName[64];

	// make sure server is running
	if ( !com_sv_running->integer ) {
		return NULL;
	}

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "No player specified.\n" );
		return NULL;
	}

	s = Cmd_Argv(1);

	// Check whether this is a numeric player handle
	for(i = 0; s[i] >= '0' && s[i] <= '9'; i++);

	if(!s[i])
	{
		int plid = atoi(s);

		// Check for numeric playerid match
		if(plid >= 0 && plid < sv_maxclients->integer)
		{
			cl = &svs.clients[plid];

			if(cl->state)
				return cl;
		}
	}

	// check for a name match
	for ( i=0, cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++ ) {
		if ( !cl->state ) {
			continue;
		}
		if ( !Q_stricmp( cl->name, s ) ) {
			return cl;
		}

		Q_strncpyz( cleanName, cl->name, sizeof(cleanName) );
		Q_StripColor( cleanName );
		//Q_CleanStr( cleanName );
		if ( !Q_stricmp( cleanName, s ) ) {
			return cl;
		}
	}

	Com_Printf( "Player %s is not on the server\n", s );

	return NULL;
}

/*
==================
SV_GetPlayerByNum

Returns the player with idnum from Cmd_Argv(1)
==================
*/
static client_t *SV_GetPlayerByNum( void ) {
	client_t	*cl;
	int			i;
	int			idnum;
	char		*s;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		return NULL;
	}

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "No player specified.\n" );
		return NULL;
	}

	s = Cmd_Argv(1);

	for (i = 0; s[i]; i++) {
		if (s[i] < '0' || s[i] > '9') {
			Com_Printf( "Bad slot number: %s\n", s);
			return NULL;
		}
	}
	idnum = atoi( s );
	if ( idnum < 0 || idnum >= sv_maxclients->integer ) {
		Com_Printf( "Bad client slot: %i\n", idnum );
		return NULL;
	}

	cl = &svs.clients[idnum];
	if ( !cl->state ) {
		Com_Printf( "Client %i is not active\n", idnum );
		return NULL;
	}
	return cl;
}

//=========================================================

/*
==================
SV_Map_f

Restart the server on a different map
==================
*/
static void SV_Map_f( void ) {
	char		*cmd = NULL, *map = NULL;
	qboolean	killBots=qfalse, cheat=qfalse;
	char		expanded[MAX_QPATH] = {0}, mapname[MAX_QPATH] = {0};

	map = Cmd_Argv(1);
	if ( !map )
		return;

	// make sure the level exists before trying to change, so that
	// a typo at the server console won't end the game
	if (strchr (map, '\\') ) {
		Com_Printf ("Can't have mapnames with a \\\n");
		return;
	}

	Com_sprintf (expanded, sizeof(expanded), "maps/%s.bsp", map);
	if ( FS_ReadFile (expanded, NULL) == -1 ) {
		Com_Printf ("Can't find map %s\n", expanded);
		return;
	}

	// force latched values to get set
	Cvar_Get ("g_gametype", "0", CVAR_SERVERINFO | CVAR_LATCH );

	cmd = Cmd_Argv(0);
	if ( !Q_stricmpn( cmd, "devmap", 6 ) ) {
		cheat = qtrue;
		killBots = qtrue;
	} else {
		cheat = qfalse;
		killBots = qfalse;
	}

	// save the map name here cause on a map restart we reload the jampconfig.cfg
	// and thus nuke the arguments of the map command
	Q_strncpyz(mapname, map, sizeof(mapname));

	ForceReload_e eForceReload = eForceReload_NOTHING;	// default for normal load

//	if ( !Q_stricmp( cmd, "devmapbsp") ) {	// not relevant in MP codebase
//		eForceReload = eForceReload_BSP;
//	}
//	else
	if ( !Q_stricmp( cmd, "devmapmdl") ) {
		eForceReload = eForceReload_MODELS;
	}
	else
	if ( !Q_stricmp( cmd, "devmapall") ) {
		eForceReload = eForceReload_ALL;
	}

	// start up the map
	SV_SpawnServer( mapname, killBots, eForceReload );

	// set the cheat value
	// if the level was started with "map <levelname>", then
	// cheats will not be allowed.  If started with "devmap <levelname>"
	// then cheats will be allowed
	Cvar_Set( "sv_cheats", cheat ? "1" : "0" );
}


/*
================
SV_MapRestart_f

Completely restarts a level, but doesn't send a new gamestate to the clients.
This allows fair starts with variable load times.
================
*/
static void SV_MapRestart_f( void ) {
	int			i;
	client_t	*client;
	char		*denied;
	qboolean	isBot;
	int			delay;

	// make sure we aren't restarting twice in the same frame
	if ( com_frameTime == sv.serverId ) {
		return;
	}

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( sv.restartTime ) {
		return;
	}

	if (Cmd_Argc() > 1 ) {
		delay = atoi( Cmd_Argv(1) );
	}
	else {
		delay = 5;
	}
	if( delay ) {
		sv.restartTime = sv.time + delay * 1000;
		SV_SetConfigstring( CS_WARMUP, va("%i", sv.restartTime) );
		return;
	}

	// check for changes in variables that can't just be restarted
	// check for maxclients change
	if ( sv_maxclients->modified || sv_gametype->modified ) {
		char	mapname[MAX_QPATH];

		Com_Printf( "variable change -- restarting.\n" );
		// restart the map the slow way
		Q_strncpyz( mapname, Cvar_VariableString( "mapname" ), sizeof( mapname ) );

		SV_SpawnServer( mapname, qfalse, eForceReload_NOTHING );
		return;
	}

	SV_StopAutoRecordDemos();

	// toggle the server bit so clients can detect that a
	// map_restart has happened
	svs.snapFlagServerBit ^= SNAPFLAG_SERVERCOUNT;

	// generate a new serverid
	// TTimo - don't update restartedserverId there, otherwise we won't deal correctly with multiple map_restart
	sv.serverId = com_frameTime;
	Cvar_Set( "sv_serverid", va("%i", sv.serverId ) );

	time( &sv.realMapTimeStarted );
	sv.demosPruned = qfalse;

	// if a map_restart occurs while a client is changing maps, we need
	// to give them the correct time so that when they finish loading
	// they don't violate the backwards time check in cl_cgame.c
	for (i=0 ; i<sv_maxclients->integer ; i++) {
		if (svs.clients[i].state == CS_PRIMED) {
			svs.clients[i].oldServerTime = sv.restartTime;
		}
	}

	// reset all the vm data in place without changing memory allocation
	// note that we do NOT set sv.state = SS_LOADING, so configstrings that
	// had been changed from their default values will generate broadcast updates
	sv.state = SS_LOADING;
	sv.restarting = qtrue;

	SV_RestartGame();

	// run a few frames to allow everything to settle
	for ( i = 0 ;i < 3 ; i++ ) {
		GVM_RunFrame( sv.time );
		sv.time += 100;
		svs.time += 100;
	}

	sv.state = SS_GAME;
	sv.restarting = qfalse;

	// connect and begin all the clients
	for (i=0 ; i<sv_maxclients->integer ; i++) {
		client = &svs.clients[i];

		// send the new gamestate to all connected clients
		if ( client->state < CS_CONNECTED) {
			continue;
		}

		if ( client->netchan.remoteAddress.type == NA_BOT ) {
			isBot = qtrue;
		} else {
			isBot = qfalse;
		}

		// add the map_restart command
		SV_AddServerCommand( client, "map_restart\n" );

		// connect the client again, without the firstTime flag
		denied = GVM_ClientConnect( i, qfalse, isBot );
		if ( denied ) {
			// this generally shouldn't happen, because the client
			// was connected before the level change
			SV_DropClient( client, denied );
			Com_Printf( "SV_MapRestart_f(%d): dropped client %i - denied!\n", delay, i );
			continue;
		}

		if(client->state == CS_ACTIVE)
			SV_ClientEnterWorld(client, &client->lastUsercmd);
		else
		{
			// If we don't reset client->lastUsercmd and are restarting during map load,
			// the client will hang because we'll use the last Usercmd from the previous map,
			// which is wrong obviously.
			SV_ClientEnterWorld(client, NULL);
		}
	}

	// run another frame to allow things to look at all the players
	GVM_RunFrame( sv.time );
	sv.time += 100;
	svs.time += 100;

	SV_BeginAutoRecordDemos();
}

//===============================================================

/*
==================
SV_KickBlankPlayers
==================
*/
static void SV_KickBlankPlayers( void ) {
	client_t	*cl;
	int			i;
	char		cleanName[64];

	// make sure server is running
	if ( !com_sv_running->integer ) {
		return;
	}

	// check for a name match
	for ( i=0, cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++ ) {
		if ( !cl->state ) {
			continue;
		}
		if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
			continue;
		}
		if ( !Q_stricmp( cl->name, "" ) ) {
			SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
			cl->lastPacketTime = svs.time;	// in case there is a funny zombie
			continue;
		}

		Q_strncpyz( cleanName, cl->name, sizeof(cleanName) );
		Q_StripColor( cleanName );
		//Q_CleanStr( cleanName );
		if ( !Q_stricmp( cleanName, "" ) ) {
			SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
			cl->lastPacketTime = svs.time;	// in case there is a funny zombie
		}
	}
}

/*
==================
SV_Kick_f

Kick a user off of the server
==================
*/
static void SV_Kick_f( void ) {
	client_t	*cl;
	int			i;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: kick <player name>\nkick all = kick everyone\nkick allbots = kick all bots\n");
		return;
	}

	if (!Q_stricmp(Cmd_Argv(1), "Padawan"))
	{ //if you try to kick the default name, also try to kick ""
		SV_KickBlankPlayers();
	}

	cl = SV_GetPlayerByHandle();
	if ( !cl ) {
		if ( !Q_stricmp(Cmd_Argv(1), "all") ) {
			for ( i=0, cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++ ) {
				if ( !cl->state ) {
					continue;
				}
				if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
					continue;
				}
				SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
				cl->lastPacketTime = svs.time;	// in case there is a funny zombie
			}
		}
		else if ( !Q_stricmp(Cmd_Argv(1), "allbots") ) {
			for ( i=0, cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++ ) {
				if ( !cl->state ) {
					continue;
				}
				if( cl->netchan.remoteAddress.type != NA_BOT ) {
					continue;
				}
				SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
				cl->lastPacketTime = svs.time;	// in case there is a funny zombie
			}
		}
		return;
	}
	if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
		Com_Printf("Cannot kick host player\n");
		return;
	}

	SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
	cl->lastPacketTime = svs.time;	// in case there is a funny zombie
}

/*
==================
SV_KickBots_f

Kick all bots off of the server
==================
*/
static void SV_KickBots_f( void ) {
	client_t	*cl;
	int			i;

	// make sure server is running
	if( !com_sv_running->integer ) {
		Com_Printf("Server is not running.\n");
		return;
	}

	for( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		if( !cl->state ) {
			continue;
		}

		if( cl->netchan.remoteAddress.type != NA_BOT ) {
			continue;
		}

		SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
		cl->lastPacketTime = svs.time; // in case there is a funny zombie
	}
}

/*
==================
SV_KickAll_f

Kick all users off of the server
==================
*/
static void SV_KickAll_f( void ) {
	client_t *cl;
	int i;

	// make sure server is running
	if( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	for( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		if( !cl->state ) {
			continue;
		}

		if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
			continue;
		}

		SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
		cl->lastPacketTime = svs.time; // in case there is a funny zombie
	}
}

/*
==================
SV_KickNum_f

Kick a user off of the server
==================
*/
static void SV_KickNum_f( void ) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: %s <client number>\n", Cmd_Argv(0));
		return;
	}

	cl = SV_GetPlayerByNum();
	if ( !cl ) {
		return;
	}
	if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
		Com_Printf("Cannot kick host player\n");
		return;
	}

	SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
	cl->lastPacketTime = svs.time;	// in case there is a funny zombie
}

/*
==================
SV_RehashBans_f

Load saved bans from file.
==================
*/
static void SV_RehashBans_f( void )
{
	int index, filelen;
	fileHandle_t readfrom;
	char *textbuf, *curpos, *maskpos, *newlinepos, *endpos;
	char filepath[MAX_QPATH];

	// make sure server is running
	if ( !com_sv_running->integer ) {
		return;
	}

	serverBansCount = 0;

	if ( !sv_banFile->string || !*sv_banFile->string )
		return;

	Com_sprintf( filepath, sizeof( filepath ), "%s/%s", FS_GetCurrentGameDir(), sv_banFile->string );

	if ( (filelen = FS_SV_FOpenFileRead( filepath, &readfrom )) >= 0 )
	{
		if ( filelen < 2 )
		{
			// Don't bother if file is too short.
			FS_FCloseFile( readfrom );
			return;
		}

		curpos = textbuf = (char *)Z_Malloc( filelen, TAG_TEMP_WORKSPACE );

		filelen = FS_Read( textbuf, filelen, readfrom );
		FS_FCloseFile( readfrom );

		endpos = textbuf + filelen;

		for ( index = 0; index < SERVER_MAXBANS && curpos + 2 < endpos; index++ )
		{
			// find the end of the address string
			for ( maskpos = curpos + 2; maskpos < endpos && *maskpos != ' '; maskpos++ );

			if ( maskpos + 1 >= endpos )
				break;

			*maskpos = '\0';
			maskpos++;

			// find the end of the subnet specifier
			for ( newlinepos = maskpos; newlinepos < endpos && *newlinepos != '\n'; newlinepos++ );

			if ( newlinepos >= endpos )
				break;

			*newlinepos = '\0';

			if ( NET_StringToAdr( curpos + 2, &serverBans[index].ip ) )
			{
				serverBans[index].isexception = (qboolean)(curpos[0] != '0');
				serverBans[index].subnet = atoi( maskpos );

				if ( serverBans[index].ip.type == NA_IP &&
					(serverBans[index].subnet < 1 || serverBans[index].subnet > 32) )
				{
					serverBans[index].subnet = 32;
				}
			}

			curpos = newlinepos + 1;
		}

		serverBansCount = index;

		Z_Free( textbuf );
	}
}

/*
==================
SV_WriteBans

Save bans to file.
==================
*/
static void SV_WriteBans( void )
{
	int index;
	fileHandle_t writeto;
	char filepath[MAX_QPATH];

	if ( !sv_banFile->string || !*sv_banFile->string )
		return;

	Com_sprintf( filepath, sizeof( filepath ), "%s/%s", FS_GetCurrentGameDir(), sv_banFile->string );

	if ( (writeto = FS_SV_FOpenFileWrite( filepath )) )
	{
		char writebuf[128];
		serverBan_t *curban;

		for ( index = 0; index < serverBansCount; index++ )
		{
			curban = &serverBans[index];

			Com_sprintf( writebuf, sizeof( writebuf ), "%d %s %d\n",
				curban->isexception, NET_AdrToString( curban->ip ), curban->subnet );
			FS_Write( writebuf, strlen( writebuf ), writeto );
		}

		FS_FCloseFile( writeto );
	}
}

/*
==================
SV_DelBanEntryFromList

Remove a ban or an exception from the list.
==================
*/

static qboolean SV_DelBanEntryFromList( int index ) {
	if ( index == serverBansCount - 1 )
		serverBansCount--;
	else if ( index < (int)ARRAY_LEN( serverBans ) - 1 )
	{
		memmove( serverBans + index, serverBans + index + 1, (serverBansCount - index - 1) * sizeof( *serverBans ) );
		serverBansCount--;
	}
	else
		return qtrue;

	return qfalse;
}

/*
==================
SV_ParseCIDRNotation

Parse a CIDR notation type string and return a netadr_t and suffix by reference
==================
*/

static qboolean SV_ParseCIDRNotation( netadr_t *dest, int *mask, char *adrstr )
{
	char *suffix;

	suffix = strchr( adrstr, '/' );
	if ( suffix )
	{
		*suffix = '\0';
		suffix++;
	}

	if ( !NET_StringToAdr( adrstr, dest ) )
		return qtrue;

	if ( suffix )
	{
		*mask = atoi( suffix );

		if ( dest->type == NA_IP )
		{
			if ( *mask < 1 || *mask > 32 )
				*mask = 32;
		}
		else
			*mask = 32;
	}
	//else if ( dest->type == NA_IP )
	//	*mask = 32;
	else
		*mask = 32;

	return qfalse;
}

/*
==================
SV_AddBanToList

Ban a user from being able to play on this server based on his ip address.
==================
*/

static void SV_AddBanToList( qboolean isexception )
{
	char *banstring;
	char addy2[NET_ADDRSTRMAXLEN];
	netadr_t ip;
	int index, argc, mask;
	serverBan_t *curban;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	argc = Cmd_Argc();

	if ( argc < 2 || argc > 3 )
	{
		Com_Printf( "Usage: %s (ip[/subnet] | clientnum [subnet])\n", Cmd_Argv( 0 ) );
		return;
	}

	if ( serverBansCount >= (int)ARRAY_LEN( serverBans ) )
	{
		Com_Printf( "Error: Maximum number of bans/exceptions exceeded.\n" );
		return;
	}

	banstring = Cmd_Argv( 1 );

	if ( strchr( banstring, '.' ) /*|| strchr( banstring, ':' )*/ )
	{
		// This is an ip address, not a client num.

		if ( SV_ParseCIDRNotation( &ip, &mask, banstring ) )
		{
			Com_Printf( "Error: Invalid address %s\n", banstring );
			return;
		}
	}
	else
	{
		client_t *cl;

		// client num.

		cl = SV_GetPlayerByNum();

		if ( !cl )
		{
			Com_Printf( "Error: Playernum %s does not exist.\n", Cmd_Argv( 1 ) );
			return;
		}

		ip = cl->netchan.remoteAddress;

		if ( argc == 3 )
		{
			mask = atoi( Cmd_Argv( 2 ) );

			if ( ip.type == NA_IP )
			{
				if ( mask < 1 || mask > 32 )
					mask = 32;
			}
			else
				mask = 32;
		}
		else
			mask = 32;
	}

	if ( ip.type != NA_IP )
	{
		Com_Printf( "Error: Can ban players connected via the internet only.\n" );
		return;
	}

	// first check whether a conflicting ban exists that would supersede the new one.
	for ( index = 0; index < serverBansCount; index++ )
	{
		curban = &serverBans[index];

		if ( curban->subnet <= mask )
		{
			if ( (curban->isexception || !isexception) && NET_CompareBaseAdrMask( curban->ip, ip, curban->subnet ) )
			{
				Q_strncpyz( addy2, NET_AdrToString( ip ), sizeof( addy2 ) );

				Com_Printf( "Error: %s %s/%d supersedes %s %s/%d\n", curban->isexception ? "Exception" : "Ban",
					NET_AdrToString( curban->ip ), curban->subnet,
					isexception ? "exception" : "ban", addy2, mask );
				return;
			}
		}
		if ( curban->subnet >= mask )
		{
			if ( !curban->isexception && isexception && NET_CompareBaseAdrMask( curban->ip, ip, mask ) )
			{
				Q_strncpyz( addy2, NET_AdrToString( curban->ip ), sizeof( addy2 ) );

				Com_Printf( "Error: %s %s/%d supersedes already existing %s %s/%d\n", isexception ? "Exception" : "Ban",
					NET_AdrToString( ip ), mask,
					curban->isexception ? "exception" : "ban", addy2, curban->subnet );
				return;
			}
		}
	}

	// now delete bans that are superseded by the new one
	index = 0;
	while ( index < serverBansCount )
	{
		curban = &serverBans[index];

		if ( curban->subnet > mask && (!curban->isexception || isexception) && NET_CompareBaseAdrMask( curban->ip, ip, mask ) )
			SV_DelBanEntryFromList( index );
		else
			index++;
	}

	serverBans[serverBansCount].ip = ip;
	serverBans[serverBansCount].subnet = mask;
	serverBans[serverBansCount].isexception = isexception;

	serverBansCount++;

	SV_WriteBans();

	Com_Printf( "Added %s: %s/%d\n", isexception ? "ban exception" : "ban",
		NET_AdrToString( ip ), mask );
}

/*
==================
SV_DelBanFromList

Remove a ban or an exception from the list.
==================
*/

static void SV_DelBanFromList( qboolean isexception )
{
	int index, count = 0, todel, mask;
	netadr_t ip;
	char *banstring;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 )
	{
		Com_Printf( "Usage: %s (ip[/subnet] | num)\n", Cmd_Argv( 0 ) );
		return;
	}

	banstring = Cmd_Argv( 1 );

	if ( strchr( banstring, '.' ) || strchr( banstring, ':' ) )
	{
		serverBan_t *curban;

		if ( SV_ParseCIDRNotation( &ip, &mask, banstring ) )
		{
			Com_Printf( "Error: Invalid address %s\n", banstring );
			return;
		}

		index = 0;

		while ( index < serverBansCount )
		{
			curban = &serverBans[index];

			if ( curban->isexception == isexception		&&
				curban->subnet >= mask 			&&
				NET_CompareBaseAdrMask( curban->ip, ip, mask ) )
			{
				Com_Printf( "Deleting %s %s/%d\n",
					isexception ? "exception" : "ban",
					NET_AdrToString( curban->ip ), curban->subnet );

				SV_DelBanEntryFromList( index );
			}
			else
				index++;
		}
	}
	else
	{
		todel = atoi( Cmd_Argv( 1 ) );

		if ( todel < 1 || todel > serverBansCount )
		{
			Com_Printf( "Error: Invalid ban number given\n" );
			return;
		}

		for ( index = 0; index < serverBansCount; index++ )
		{
			if ( serverBans[index].isexception == isexception )
			{
				count++;

				if ( count == todel )
				{
					Com_Printf( "Deleting %s %s/%d\n",
						isexception ? "exception" : "ban",
						NET_AdrToString( serverBans[index].ip ), serverBans[index].subnet );

					SV_DelBanEntryFromList( index );

					break;
				}
			}
		}
	}

	SV_WriteBans();
}


/*
==================
SV_ListBans_f

List all bans and exceptions on console
==================
*/

static void SV_ListBans_f( void )
{
	int index, count;
	serverBan_t *ban;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	// List all bans
	for ( index = count = 0; index < serverBansCount; index++ )
	{
		ban = &serverBans[index];
		if ( !ban->isexception )
		{
			count++;

			Com_Printf( "Ban #%d: %s/%d\n", count,
				NET_AdrToString( ban->ip ), ban->subnet );
		}
	}
	// List all exceptions
	for ( index = count = 0; index < serverBansCount; index++ )
	{
		ban = &serverBans[index];
		if ( ban->isexception )
		{
			count++;

			Com_Printf( "Except #%d: %s/%d\n", count,
				NET_AdrToString( ban->ip ), ban->subnet );
		}
	}
}

/*
==================
SV_FlushBans_f

Delete all bans and exceptions.
==================
*/

static void SV_FlushBans_f( void )
{
	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	serverBansCount = 0;

	// empty the ban file.
	SV_WriteBans();

	Com_Printf( "All bans and exceptions have been deleted.\n" );
}

static void SV_BanAddr_f( void )
{
	SV_AddBanToList( qfalse );
}

static void SV_ExceptAddr_f( void )
{
	SV_AddBanToList( qtrue );
}

static void SV_BanDel_f( void )
{
	SV_DelBanFromList( qfalse );
}

static void SV_ExceptDel_f( void )
{
	SV_DelBanFromList( qtrue );
}

static const char *SV_CalcUptime( void ) {
	static char buf[MAX_STRING_CHARS / 4] = { '\0' };
	char tmp[64] = { '\0' };
	time_t currTime;

	time( &currTime );

	int secs = difftime( currTime, svs.startTime );
	int mins = secs / 60;
	int hours = mins / 60;
	int days = hours / 24;

	secs %= 60;
	mins %= 60;
	hours %= 24;
	//days %= 365;

	buf[0] = '\0';
	if ( days > 0 ) {
		Com_sprintf( tmp, sizeof(tmp), "%i days ", days );
		Q_strcat( buf, sizeof(buf), tmp );
	}

	Com_sprintf( tmp, sizeof(tmp), "%ih%im%is", hours, mins, secs );
	Q_strcat( buf, sizeof(buf), tmp );

	return buf;
}

/*
================
SV_Status_f
================
*/
static void SV_Status_f( void )
{
	int				i, humans, bots;
	client_t		*cl;
	playerState_t	*ps;
	const char		*s;
	int				ping;
	char			state[32];
	qboolean		avoidTruncation = qfalse;

	// make sure server is running
	if ( !com_sv_running->integer )
	{
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() > 1 )
	{
		if (!Q_stricmp("notrunc", Cmd_Argv(1)))
		{
			avoidTruncation = qtrue;
		}
	}

	humans = bots = 0;
	for ( i = 0 ; i < sv_maxclients->integer ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			if ( svs.clients[i].netchan.remoteAddress.type != NA_BOT ) {
				humans++;
			}
			else {
				bots++;
			}
		}
	}

#if defined(_WIN32)
#define STATUS_OS "Windows"
#elif defined(__linux__)
#define STATUS_OS "Linux"
#elif defined(MACOS_X)
#define STATUS_OS "OSX"
#else
#define STATUS_OS "Unknown"
#endif

	const char *ded_table[] =
	{
		"listen",
		"lan dedicated",
		"public dedicated",
	};

	char hostname[MAX_HOSTNAMELENGTH] = { 0 };

	Q_strncpyz( hostname, sv_hostname->string, sizeof(hostname) );
	Q_StripColor( hostname );

	Com_Printf( "hostname: %s^7\n", hostname );
	Com_Printf( "version : %s %i\n", VERSION_STRING_DOTTED, PROTOCOL_VERSION );
	Com_Printf( "game    : %s\n", FS_GetCurrentGameDir() );
	Com_Printf( "udp/ip  : %s:%i os(%s) type(%s)\n", Cvar_VariableString( "net_ip" ), Cvar_VariableIntegerValue( "net_port" ), STATUS_OS, ded_table[com_dedicated->integer] );
	Com_Printf( "map     : %s gametype(%i)\n", sv_mapname->string, sv_gametype->integer );
	Com_Printf( "players : %i humans, %i bots (%i max)\n", humans, bots, sv_maxclients->integer - sv_privateClients->integer );
	Com_Printf( "uptime  : %s\n", SV_CalcUptime() );

	Com_Printf ("cl score ping name            address                                 rate \n");
	Com_Printf ("-- ----- ---- --------------- --------------------------------------- -----\n");
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++)
	{
		if ( !cl->state )
			continue;

		if ( cl->state == CS_CONNECTED )
			Q_strncpyz( state, "CON ", sizeof( state ) );
		else if ( cl->state == CS_ZOMBIE )
			Q_strncpyz( state, "ZMB ", sizeof( state ) );
		else {
			ping = cl->ping < 9999 ? cl->ping : 9999;
			Com_sprintf( state, sizeof(state), "%4i", ping );
		}

		ps = SV_GameClientNum( i );
		s = NET_AdrToString( cl->netchan.remoteAddress );

		if (!avoidTruncation)
		{
			Com_Printf ("%2i %5i %s %-15.15s ^7%39s %5i\n",
				i,
				ps->persistant[PERS_SCORE],
				state,
				cl->name,
				s,
				cl->rate
				);
		}
		else
		{
			Com_Printf ("%2i %5i %s %s ^7%39s %5i\n",
				i,
				ps->persistant[PERS_SCORE],
				state,
				cl->name,
				s,
				cl->rate
				);
		}
	}
	Com_Printf ("\n");
}

char	*SV_ExpandNewlines( char *in );


/*
==================
SV_ConSay_f
==================
*/
static void SV_ConSay_f(void) {
	char	text[MAX_SAY_TEXT] = {0};

	if( !com_dedicated->integer ) {
		Com_Printf( "Server is not dedicated.\n" );
		return;
	}

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc () < 2 ) {
		return;
	}

	Cmd_ArgsBuffer( text, sizeof(text) );

	Com_Printf ("broadcast: chat \"" SVSAY_PREFIX "%s\\n\"\n", SV_ExpandNewlines((char *)text) );
	SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "%s\"\n", text);
}


/*
==================
SV_ConTell_f
==================
*/
static void SV_ConTell_f(void) {
	char	text[MAX_SAY_TEXT] = {0};
	client_t	*cl;

	if( !com_dedicated->integer ) {
		Com_Printf( "Server is not dedicated.\n" );
		return;
	}

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc () < 3 ) {
		Com_Printf ("Usage: svtell <client number> <text>\n");
		return;
	}

	cl = SV_GetPlayerByNum();
	if ( !cl ) {
		return;
	}

	Cmd_ArgsFromBuffer( 2, text, sizeof(text) );

	Com_Printf ("tell: svtell to %s" S_COLOR_WHITE ": %s\n", cl->name, SV_ExpandNewlines((char *)text) );
	SV_SendServerCommand(cl, "chat \"" SVTELL_PREFIX S_COLOR_MAGENTA "%s" S_COLOR_WHITE "\"\n", text);
}

const char *forceToggleNamePrints[NUM_FORCE_POWERS] = {
	"HEAL",
	"JUMP",
	"SPEED",
	"PUSH",
	"PULL",
	"MINDTRICK",
	"GRIP",
	"LIGHTNING",
	"DARK RAGE",
	"PROTECT",
	"ABSORB",
	"TEAM HEAL",
	"TEAM REPLENISH",
	"DRAIN",
	"SEEING",
	"SABER OFFENSE",
	"SABER DEFENSE",
	"SABER THROW",
};

static void SV_ForceToggle_f( void ) {
	int bits = Cvar_VariableIntegerValue("g_forcePowerDisable");
	int i, val;
	char *s;

	// make sure server is running
	if( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		for ( i = 0; i<NUM_FORCE_POWERS; i++ ) {
			if ( (bits & (1 << i)) )		Com_Printf( "%2d [X] %s\n", i, forceToggleNamePrints[i] );
			else							Com_Printf( "%2d [ ] %s\n", i, forceToggleNamePrints[i] );
		}
		Com_Printf( "Example usage: forcetoggle 3(toggles PUSH)\n" );
		return;
	}

	s = Cmd_Argv(1);

	if( Q_isanumber( s ) ) {
		val = atoi(s);
		if( val >= 0 && val < NUM_FORCE_POWERS) {
			bits ^= (1 << val);
			Cvar_SetValue("g_forcePowerDisable", bits);
			Com_Printf( "%s %s^7\n", forceToggleNamePrints[val], (bits & (1<<val)) ? "^2Enabled" : "^1Disabled" );
		}
		else {
			for ( i = 0; i<NUM_FORCE_POWERS; i++ ) {
				if ( (bits & (1 << i)) )		Com_Printf( "%2d [X] %s\n", i, forceToggleNamePrints[i] );
				else							Com_Printf( "%2d [ ] %s\n", i, forceToggleNamePrints[i] );
			}
			Com_Printf ("Specified a power that does not exist.\nExample usage: forcetoggle 3\n(toggles PUSH)\n");
		}
	}
	else {
		for ( i = 0; i<NUM_FORCE_POWERS; i++ ) {
			if ( (bits & (1 << i)) )		Com_Printf( "%2d [X] %s\n", i, forceToggleNamePrints[i] );
			else							Com_Printf( "%2d [ ] %s\n", i, forceToggleNamePrints[i] );
		}
		Com_Printf ("Specified a power that does not exist.\nExample usage: forcetoggle 3\n(toggles PUSH)\n");
	}
}

const char *weaponToggleNamePrints[WP_NUM_WEAPONS] = {
	"NO WEAPON",
	"STUN BATON",
	"MELEE",
	"SABER",
	"BRYAR PISTOL",
	"BLASTER",
	"DISRUPTOR",
	"BOWCASTER",
	"REPEATER",
	"DEMP2",
	"FLECHETTE",
	"ROCKET LAUNCHER",
	"THERMAL",
	"TRIP MINE",
	"DET PACK",
	"CONCUSSION",
	"BRYAR OLD",
	"EMPLACED GUN",
	"TURRET"
};

static void SV_WeaponToggle_f( void ) {
	int bits = 0;
	int i, val;
	char *s;
	const char *cvarStr = NULL;

	if ( sv_gametype->integer == GT_DUEL || sv_gametype->integer == GT_POWERDUEL ) {
		cvarStr = "g_duelWeaponDisable";
		bits = Cvar_VariableIntegerValue( "g_duelWeaponDisable" );
	}
	else {
		cvarStr = "g_weaponDisable";
		bits = Cvar_VariableIntegerValue( "g_weaponDisable" );
	}

	// make sure server is running
	if( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		for ( i = 0; i<WP_NUM_WEAPONS; i++ ) {
			if ( (bits & (1 << i)) )		Com_Printf( "%2d [X] %s\n", i, weaponToggleNamePrints[i] );
			else							Com_Printf( "%2d [ ] %s\n", i, weaponToggleNamePrints[i] );
		}
		Com_Printf ("Example usage: weapontoggle 3(toggles SABER)\n");
		return;
	}

	s = Cmd_Argv(1);

	if( Q_isanumber( s ) ) {
		val = atoi(s);
		if( val >= 0 && val < WP_NUM_WEAPONS) {
			bits ^= (1 << val);
			Cvar_SetValue(cvarStr, bits);
			Com_Printf( "%s %s^7\n", weaponToggleNamePrints[val], (bits & (1 << val)) ? "^2Enabled" : "^1Disabled" );
		}
		else {
			for ( i = 0; i<WP_NUM_WEAPONS; i++ ) {
				if ( (bits & (1 << i)) )		Com_Printf( "%2d [X] %s\n", i, weaponToggleNamePrints[i] );
				else							Com_Printf( "%2d [ ] %s\n", i, weaponToggleNamePrints[i] );
			}
			Com_Printf ("Specified a weapon that does not exist.\nExample usage: weapontoggle 3\n(toggles SABER)\n");
		}
	}
	else {
		for ( i = 0; i<WP_NUM_WEAPONS; i++ ) {
			if ( (bits & (1 << i)) )		Com_Printf( "%2d [X] %s\n", i, weaponToggleNamePrints[i] );
			else							Com_Printf( "%2d [ ] %s\n", i, weaponToggleNamePrints[i] );
		}
		Com_Printf ("Specified a weapon that does not exist.\nExample usage: weapontoggle 3\n(toggles SABER)\n");
	}
}

/*
==================
SV_Heartbeat_f

Also called by SV_DropClient, SV_DirectConnect, and SV_SpawnServer
==================
*/
void SV_Heartbeat_f( void ) {
	svs.nextHeartbeatTime = -9999999;
}

/*
===========
SV_Serverinfo_f

Examine the serverinfo string
===========
*/
static void SV_Serverinfo_f( void ) {
	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	Com_Printf ("Server info settings:\n");
	Info_Print ( Cvar_InfoString( CVAR_SERVERINFO ) );
}

/*
===========
SV_Systeminfo_f

Examine or change the serverinfo string
===========
*/
static void SV_Systeminfo_f( void ) {
	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	Com_Printf ("System info settings:\n");
	Info_Print ( Cvar_InfoString_Big( CVAR_SYSTEMINFO ) );
}

/*
===========
SV_DumpUser_f

Examine all a users info strings FIXME: move to game
===========
*/
static void SV_DumpUser_f( void ) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: dumpuser <userid>\n");
		return;
	}

	cl = SV_GetPlayerByHandle();
	if ( !cl ) {
		return;
	}

	Com_Printf( "userinfo\n" );
	Com_Printf( "--------\n" );
	Info_Print( cl->userinfo );
}

/*
=================
SV_KillServer
=================
*/
static void SV_KillServer_f( void ) {
	SV_Shutdown( "killserver" );
}

void SV_WriteDemoMessage ( client_t *cl, msg_t *msg, int headerBytes ) {
	int		len, swlen;

	// write the packet sequence
	len = cl->netchan.outgoingSequence;
	swlen = LittleLong( len );
	FS_Write( &swlen, 4, cl->demo.demofile );

	// skip the packet sequencing information
	len = msg->cursize - headerBytes;
	swlen = LittleLong( len );
	FS_Write( &swlen, 4, cl->demo.demofile );
	FS_Write( msg->data + headerBytes, len, cl->demo.demofile );
}

void SV_StopRecordDemo( client_t *cl ) {
	int		len;

	if ( !cl->demo.demorecording ) {
		Com_Printf( "Client %d is not recording a demo.\n", cl - svs.clients );
		return;
	}

	// finish up
	len = -1;
	FS_Write (&len, 4, cl->demo.demofile);
	FS_Write (&len, 4, cl->demo.demofile);
	FS_FCloseFile (cl->demo.demofile);
	cl->demo.demofile = 0;
	cl->demo.demorecording = qfalse;
	Com_Printf ("Stopped demo for client %d.\n", cl - svs.clients);
}

// stops all recording demos
void SV_StopAutoRecordDemos() {
	if ( svs.clients && sv_autoDemo->integer ) {
		for ( client_t *client = svs.clients; client - svs.clients < sv_maxclients->integer; client++ ) {
			if ( client->demo.demorecording) {
				SV_StopRecordDemo( client );
			}
		}
	}
}

/*
====================
SV_StopRecording_f

stop recording a demo
====================
*/
void SV_StopRecord_f( void ) {
	int		i;

	client_t *cl = NULL;
	if ( Cmd_Argc() == 2 ) {
		int clIndex = atoi( Cmd_Argv( 1 ) );
		if ( clIndex < 0 || clIndex >= sv_maxclients->integer ) {
			Com_Printf( "Unknown client number %d.\n", clIndex );
			return;
		}
		cl = &svs.clients[clIndex];
	} else {
		for (i = 0; i < sv_maxclients->integer; i++) {
			if ( svs.clients[i].demo.demorecording ) {
				cl = &svs.clients[i];
				break;
			}
		}
		if ( cl == NULL ) {
			Com_Printf( "No demo being recorded.\n" );
			return;
		}
	}
	SV_StopRecordDemo( cl );
}

/*
==================
SV_DemoFilename
==================
*/
void SV_DemoFilename( char *buf, int bufSize ) {
	time_t rawtime;
	char timeStr[32] = {0}; // should really only reach ~19 chars

	time( &rawtime );
	strftime( timeStr, sizeof( timeStr ), "%Y-%m-%d_%H-%M-%S", localtime( &rawtime ) ); // or gmtime

	Com_sprintf( buf, bufSize, "demo%s", timeStr );
}

// defined in sv_client.cpp
extern void SV_CreateClientGameStateMessage( client_t *client, msg_t* msg );

void SV_RecordDemo( client_t *cl, char *demoName ) {
	char		name[MAX_OSPATH];
	byte		bufData[MAX_MSGLEN];
	msg_t		msg;
	int			len;

	if ( cl->demo.demorecording ) {
		Com_Printf( "Already recording.\n" );
		return;
	}

	if ( cl->state != CS_ACTIVE ) {
		Com_Printf( "Client is not active.\n" );
		return;
	}

	// open the demo file
	Q_strncpyz( cl->demo.demoName, demoName, sizeof( cl->demo.demoName ) );
	Com_sprintf( name, sizeof( name ), "demos/%s.dm_%d", cl->demo.demoName, PROTOCOL_VERSION );
	Com_Printf( "recording to %s.\n", name );
	cl->demo.demofile = FS_FOpenFileWrite( name );
	if ( !cl->demo.demofile ) {
		Com_Printf ("ERROR: couldn't open.\n");
		return;
	}
	cl->demo.demorecording = qtrue;

	// don't start saving messages until a non-delta compressed message is received
	cl->demo.demowaiting = qtrue;

	cl->demo.isBot = ( cl->netchan.remoteAddress.type == NA_BOT ) ? qtrue : qfalse;
	cl->demo.botReliableAcknowledge = cl->reliableSent;

	// write out the gamestate message
	MSG_Init( &msg, bufData, sizeof( bufData ) );

	// NOTE, MRE: all server->client messages now acknowledge
	int tmp = cl->reliableSent;
	SV_CreateClientGameStateMessage( cl, &msg );
	cl->reliableSent = tmp;

	// finished writing the client packet
	MSG_WriteByte( &msg, svc_EOF );

	// write it to the demo file
	len = LittleLong( cl->netchan.outgoingSequence - 1 );
	FS_Write( &len, 4, cl->demo.demofile );

	len = LittleLong( msg.cursize );
	FS_Write( &len, 4, cl->demo.demofile );
	FS_Write( msg.data, msg.cursize, cl->demo.demofile );

	// the rest of the demo file will be copied from net messages
}

void SV_AutoRecordDemo( client_t *cl ) {
	char demoName[MAX_OSPATH];
	char demoFolderName[MAX_OSPATH];
	char demoFileName[MAX_OSPATH];
	char *demoNames[] = { demoFolderName, demoFileName };
	char date[MAX_OSPATH];
	char folderDate[MAX_OSPATH];
	char folderTreeDate[MAX_OSPATH];
	char demoPlayerName[MAX_NAME_LENGTH];
	time_t rawtime;
	struct tm * timeinfo;
	time( &rawtime );
	timeinfo = localtime( &rawtime );
	strftime( date, sizeof( date ), "%Y-%m-%d_%H-%M-%S", timeinfo );
	timeinfo = localtime( &sv.realMapTimeStarted );
	strftime( folderDate, sizeof( folderDate ), "%Y-%m-%d_%H-%M-%S", timeinfo );
	strftime( folderTreeDate, sizeof( folderTreeDate ), "%Y/%m/%d", timeinfo );
	Q_strncpyz( demoPlayerName, cl->name, sizeof( demoPlayerName ) );
	Q_CleanStr( demoPlayerName );
	Com_sprintf( demoFileName, sizeof( demoFileName ), "%d %s %s %s",
			cl - svs.clients, demoPlayerName, Cvar_VariableString( "mapname" ), date );
	Com_sprintf( demoFolderName, sizeof( demoFolderName ), "%s %s", Cvar_VariableString( "mapname" ), folderDate );
	// sanitize filename
	for ( char **start = demoNames; start - demoNames < (ptrdiff_t)ARRAY_LEN( demoNames ); start++ ) {
		Q_strstrip( *start, "\n\r;:.?*<>|\\/\"", NULL );
	}
	Com_sprintf( demoName, sizeof( demoName ), "autorecord/%s/%s/%s", folderTreeDate, demoFolderName, demoFileName );
	SV_RecordDemo( cl, demoName );
}

static time_t SV_ExtractTimeFromDemoFolder( char *folder ) {
	char *slash = strrchr( folder, '/' );
	if ( slash ) {
		folder = slash + 1;
	}
	size_t timeLen = strlen( "0000-00-00_00-00-00" );
	if ( strlen( folder ) < timeLen ) {
		return 0;
	}
	struct tm timeinfo;
	timeinfo.tm_isdst = 0;
	int numMatched = sscanf( folder + ( strlen(folder) - timeLen ), "%4d-%2d-%2d_%2d-%2d-%2d",
		&timeinfo.tm_year, &timeinfo.tm_mon, &timeinfo.tm_mday, &timeinfo.tm_hour, &timeinfo.tm_min, &timeinfo.tm_sec);
	if ( numMatched < 6 ) {
		// parsing failed
		return 0;
	}
	timeinfo.tm_year -= 1900;
	timeinfo.tm_mon--;
	return mktime( &timeinfo );
}

static int QDECL SV_DemoFolderTimeComparator( const void *arg1, const void *arg2 ) {
	char *left = (char *)arg1, *right = (char *)arg2;
	time_t leftTime = SV_ExtractTimeFromDemoFolder( left );
	time_t rightTime = SV_ExtractTimeFromDemoFolder( right );
	if ( leftTime == 0 && rightTime == 0 ) {
		return -strcmp( left, right );
	} else if ( leftTime == 0 ) {
		return 1;
	} else if ( rightTime == 0 ) {
		return -1;
	}
	return rightTime - leftTime;
}

// returns number of folders found.  pass NULL result pointer for just a count.
static int SV_FindLeafFolders( const char *baseFolder, char *result, int maxResults, int maxFolderLength ) {
	char *fileList = (char *)Z_Malloc( MAX_OSPATH * maxResults, TAG_FILESYS ); // too big for stack since this is recursive
	char fullFolder[MAX_OSPATH];
	int resultCount = 0;
	char *fileName;
	int i;
	int numFiles = FS_GetFileList( baseFolder, "/", fileList, MAX_OSPATH * maxResults );

	fileName = fileList;
	for ( i = 0; i < numFiles; i++ ) {
		if ( Q_stricmp( fileName, "." ) && Q_stricmp( fileName, ".." ) ) {
			char *nextResult = NULL;
			Com_sprintf( fullFolder, sizeof( fullFolder ), "%s/%s", baseFolder, fileName );
			if ( result != NULL ) {
				nextResult = &result[maxFolderLength * resultCount];
			}
			int newResults = SV_FindLeafFolders( fullFolder, nextResult, maxResults - resultCount, maxFolderLength );
			resultCount += newResults;
			if ( result != NULL && resultCount >= maxResults ) {
				break;
			}
			if ( newResults == 0 ) {
				if ( result != NULL ) {
					Q_strncpyz( &result[maxFolderLength * resultCount], fullFolder, maxFolderLength );
				}
				resultCount++;
				if ( result != NULL && resultCount >= maxResults ) {
					break;
				}
			}
		}
		fileName += strlen( fileName ) + 1;
	}

	Z_Free( fileList );

	return resultCount;
}

// starts demo recording on all active clients
void SV_BeginAutoRecordDemos() {
	if ( sv_autoDemo->integer ) {
		for ( client_t *client = svs.clients; client - svs.clients < sv_maxclients->integer; client++ ) {
			if ( client->state == CS_ACTIVE && !client->demo.demorecording ) {
				if ( client->netchan.remoteAddress.type != NA_BOT || sv_autoDemoBots->integer ) {
					SV_AutoRecordDemo( client );
				}
			}
		}
		if ( sv_autoDemoMaxMaps->integer > 0 && sv.demosPruned == qfalse ) {
			char autorecordDirList[500 * MAX_OSPATH], tmpFileList[5 * MAX_OSPATH];
			int autorecordDirListCount = SV_FindLeafFolders( "demos/autorecord", autorecordDirList, 500, MAX_OSPATH );
			int i;

			qsort( autorecordDirList, autorecordDirListCount, MAX_OSPATH, SV_DemoFolderTimeComparator );
			for ( i = sv_autoDemoMaxMaps->integer; i < autorecordDirListCount; i++ ) {
				char *folder = &autorecordDirList[i * MAX_OSPATH], *slash = NULL;
				FS_HomeRmdir( folder, qtrue );
				// if this folder was the last thing in its parent folder (and its parent isn't the root folder),
				// also delete the parent.
				for (;;) {
					slash = strrchr( folder, '/' );
					if ( slash == NULL ) {
						break;
					}
					slash[0] = '\0';
					if ( !strcmp( folder, "demos/autorecord" ) ) {
						break;
					}
					int numFiles = FS_GetFileList( folder, "", tmpFileList, sizeof( tmpFileList ) );
					int numFolders = FS_GetFileList( folder, "/", tmpFileList, sizeof( tmpFileList ) );
					// numFolders will include . and ..
					if ( numFiles == 0 && numFolders == 2 ) {
						// dangling empty folder, delete
						FS_HomeRmdir( folder, qfalse );
					} else {
						break;
					}
				}
			}
			sv.demosPruned = qtrue;
		}
	}
}

// code is a merge of the cl_main.cpp function of the same name and SV_SendClientGameState in sv_client.cpp
static void SV_Record_f( void ) {
	char		demoName[MAX_OSPATH];
	char		name[MAX_OSPATH];
	int			i;
	char		*s;
	client_t	*cl;

	if ( svs.clients == NULL ) {
		Com_Printf( "cannot record server demo - null svs.clients\n" );
		return;
	}

	if ( Cmd_Argc() > 3 ) {
		Com_Printf( "record <demoname> <clientnum>\n" );
		return;
	}


	if ( Cmd_Argc() == 3 ) {
		int clIndex = atoi( Cmd_Argv( 2 ) );
		if ( clIndex < 0 || clIndex >= sv_maxclients->integer ) {
			Com_Printf( "Unknown client number %d.\n", clIndex );
			return;
		}
		cl = &svs.clients[clIndex];
	} else {
		for ( i=0, cl=svs.clients ; i < sv_maxclients->integer ; i++, cl++ )
		{
			if ( !cl->state )
			{
				continue;
			}

			if ( cl->demo.demorecording )
			{
				continue;
			}

			if ( cl->state == CS_ACTIVE )
			{
				break;
			}
		}
	}

	if (cl - svs.clients >= sv_maxclients->integer) {
		Com_Printf( "No active client could be found.\n" );
		return;
	}

	if ( cl->demo.demorecording ) {
		Com_Printf( "Already recording.\n" );
		return;
	}

	if ( cl->state != CS_ACTIVE ) {
		Com_Printf( "Client is not active.\n" );
		return;
	}

	if ( Cmd_Argc() >= 2 ) {
		s = Cmd_Argv( 1 );
		Q_strncpyz( demoName, s, sizeof( demoName ) );
		Com_sprintf( name, sizeof( name ), "demos/%s.dm_%d", demoName, PROTOCOL_VERSION );
	} else {
		// timestamp the file
		SV_DemoFilename( demoName, sizeof( demoName ) );

		Com_sprintf (name, sizeof(name), "demos/%s.dm_%d", demoName, PROTOCOL_VERSION );

		if ( FS_FileExists( name ) ) {
			Com_Printf( "Record: Couldn't create a file\n");
			return;
 		}
	}

	SV_RecordDemo( cl, demoName );
}


/*
==================
SV_CompleteMapName
==================
*/
static void SV_CompleteMapName(char* args, int argNum) {
	if (argNum == 2)
		Field_CompleteFilename("maps", "bsp", qtrue, qfalse);
}


//===========================================================
// ALL CUSTOM SERVER COMMANDS AND FUNCTIONS
//===========================================================

/*
==================
SV_ClientMBClass
Helper Function to clients MBClass 
==================
*/
static int SV_ClientMBClass(client_t* cl) {

	// This function could do with working a LOT BETTER
	// Find pointer where class comes from MBII
	// At the moment, look through commands for one with MBC, ensure models match
	// After 1000 Commands pointers break, so this can stop working mid way through a round

	char* class_id = "0";
	char model = Info_ValueForKey(cl->userinfo, "model")[0];
	int i = 0;

	while (cl->reliableCommands && i < 1000) {
		if (cl->reliableCommands[i]) {
			if (strstr(cl->reliableCommands[i], "mbc") != NULL) {
				class_id = Info_ValueForKey(cl->reliableCommands[i], "mbc");
				if (*class_id) {
					char m_model = Info_ValueForKey(cl->reliableCommands[i], "m")[0];
					if (m_model == model) {
						break;
					}
				}
			}
		}
		i++;
	}

	return atoi(class_id);

}


/*
==================
For Testing
==================
*/
static void SV_WannaTest_f(void) {

	client_t* cl;
	char	tmp[50];

	// make sure server is running
	if (!com_sv_running->integer) {
		Com_Printf("Server is not running.\n");
		return;
	}

	cl = SV_GetPlayerByHandle();
	if (!cl) {
		cl = SV_GetPlayerByNum();
		if (!cl) {
			return;
		}
	}
	
}


/*
==================
Helper, scale a clients model
==================
*/
static void SV_WannaScale(client_t* cl, int scale) {

	cl->gentity->playerState->iModelScale = scale;
	GVM_RunFrame(sv.time);
}

/*
==================
Scale a clients model
==================
*/
static void SV_WannaScale_f(void) {

	client_t* cl;

	if (Cmd_Argc() < 3) {
		Com_Printf("Usage: wannascale <Client> <scale> \nChange client model scale\n");
		return;
	}

	cl = SV_GetPlayerByHandle();
	if (!cl) {
		cl = SV_GetPlayerByNum();
		if (!cl) {
			return;
		}
	}

	SV_WannaScale(cl, atoi(Cmd_Argv(2)));

}

/*
==================
Helper, give a client a weapon
==================
*/
static void SV_WannaGiveWeapon(client_t* cl, int wnum) {

	char	tmp[50];

	sprintf(tmp, "give weaponnum %d", wnum);
	SV_ExecuteClientCommand(cl, tmp, qtrue);
}

/*
==================
Give a client a weapon
==================
*/
static void SV_WannaGiveWeapon_f(void) {

	client_t* cl;

	if (Cmd_Argc() != 3) {
		Com_Printf("Usage: wannagiveweapon <Client> <weapon_power_num> \nGive client weapon\n");
		return;
	}

	cl = SV_GetPlayerByHandle();
	if (!cl) {
		cl = SV_GetPlayerByNum();
		if (!cl) {
			return;
		}
	}

	SV_WannaGiveWeapon(cl, atoi(Cmd_Argv(2)));

}

// Helper: Give all weapons to a client
static void SV_WannaGiveWeaponsAll(client_t* cl) {
    if (!cl || !cl->gentity || !cl->gentity->playerState) {
        Com_Printf("Invalid client entity or player state.\n");
        return;
    }

    for (int weapon = 0; weapon < MB_WEAPON_MAX; ++weapon) {
		// Skip the NO_WEAPON case
		if (weapon == WP_NONE) {
			continue;
		}

		// Give the weapon to the client
		//cl->gentity->playerState->weapons |= (1 << weapon);
		SV_WannaGiveWeapon(cl, weapon);
    }
}

// Command: wannagiveweaponsall <client_id>
static void SV_WannaGiveWeaponsAll_f(void) {
    if (Cmd_Argc() != 2) {
        Com_Printf("Usage: wannagiveweaponsall <client_id>\n");
        return;
    }

    int clientNum = atoi(Cmd_Argv(1));

    if (clientNum < 0 || clientNum >= sv_maxclients->integer) {
        Com_Printf("Invalid client ID.\n");
        return;
    }

    client_t* cl = &svs.clients[clientNum];
    if (cl->state != CS_ACTIVE) {
        Com_Printf("Client not connected.\n");
        return;
    }

    SV_WannaGiveWeaponsAll(cl);
    Com_Printf("Gave all weapons to client %d.\n", clientNum);
}

static void SV_WannaGiveAmmo(client_t* cl, int ammoType, int amount) {
    if (!cl) {
        Com_Printf("Invalid client pointer.\n");
        return;
    }
    // Send a server command to the game VM to give ammo
    char cmd[64];
    Com_sprintf(cmd, sizeof(cmd), "giveammo %d %d", ammoType, amount);
    SV_GameSendServerCommand(cl - svs.clients, cmd);
}

static void SV_WannaGiveAmmo_f(void) {
    if (Cmd_Argc() != 4) {
        Com_Printf("Usage: wannagiveammo <client_id> <ammo_type> <amount>\n");
        return;
    }

    int clientNum = atoi(Cmd_Argv(1));
    int ammoType = atoi(Cmd_Argv(2));
    int amount = atoi(Cmd_Argv(3));

    if (clientNum < 0 || clientNum >= sv_maxclients->integer) {
        Com_Printf("Invalid client ID.\n");
        return;
    }

    client_t* cl = &svs.clients[clientNum];
    if (cl->state != CS_ACTIVE) {
        Com_Printf("Client not connected.\n");
        return;
    }

    SV_WannaGiveAmmo(cl, ammoType, amount);
    Com_Printf("Requested %d of ammo type %d for client %d.\n", amount, ammoType, clientNum);
}



static void SV_WannaGiveAmmoAll_f(void) {
    if (Cmd_Argc() != 3) {
        Com_Printf("Usage: wannagiveallammo <client_id> <amount>\n");
        return;
    }

    int clientNum = atoi(Cmd_Argv(1));
    int amount = atoi(Cmd_Argv(2));

    if (clientNum < 0 || clientNum >= sv_maxclients->integer) {
        Com_Printf("Invalid client ID.\n");
        return;
    }

    client_t* cl = &svs.clients[clientNum];
    if (cl->state != CS_ACTIVE) {
        Com_Printf("Client not connected.\n");
        return;
    }

    SV_WannaGiveAmmoAll(cl, amount);
    Com_Printf("Requested all ammo types set to %d for client %d.\n", amount, clientNum);
}

static void SV_WannaGiveAmmoAll(client_t* cl, int amount) {
    if (!cl) {
        Com_Printf("Invalid client pointer.\n");
        return;
    }
    for (int ammoType = 0; ammoType < MB_AMMO_MAX; ++ammoType) {
        SV_WannaGiveAmmo(cl, ammoType, amount);
    }
}

/*
==================
Helper, give a client a force power
==================
*/
static void SV_WannaForce(client_t* cl, int fpwr) {

	cl->gentity->playerState->fd.forcePowersKnown |= (1 << fpwr);
	cl->gentity->playerState->fd.forcePower = 100;

}

/*
==================
Give a client a force power
==================
*/
static void SV_WannaForce_f(void) {

	client_t* cl;

	if (Cmd_Argc() != 3) {
		Com_Printf("Usage: wannagiveforce <Client> <force_power_num> \nGive client force power\n");
		return;
	}

	cl = SV_GetPlayerByHandle();
	if (!cl) {
		cl = SV_GetPlayerByNum();
		if (!cl) {
			return;
		}
	}

	SV_WannaForce(cl, atoi(Cmd_Argv(2)));

}


/*
==================
Helper, enable / disable cheats without devmap
==================
*/
static void SV_WannaCheat(char* i) {

	Cvar_Set("sv_cheats", i);
	GVM_RunFrame(sv.time);

}

/*
==================
Enable / disable cheats without devmap
==================
*/
static void SV_WannaCheat_f(void) {

	// make sure server is running
	if (!com_sv_running->integer) {
		Com_Printf("Server is not running.\n");
		return;
	}

	if (Cmd_Argc() != 2) {
		Com_Printf("Usage: wannacheat <1|0> \nEnable or Disable sv_cheats\n");
		return;
	}


	if (!strcmp(Cmd_Argv(1), "1")) {
		Com_Printf("Cheating Enabled\n");
	}
	else {
		Com_Printf("Cheating Disabled\n");
	}

	SV_WannaCheat(Cmd_Argv(1));
	GVM_RunFrame(sv.time);
}

/*
==================
Helper, Excute a console command as a client
==================
*/
static void SV_WannaBe(client_t* cl, char* cmd) {

	SV_WannaCheat("1");
	GVM_RunFrame(sv.time);
	SV_ExecuteClientCommand(cl, cmd, qtrue);
	SV_WannaCheat("0");
	GVM_RunFrame(sv.time);
}

/*
==================
Excute a console command as a client
==================
*/
static void SV_WannaBe_f(void) {

	client_t* cl;

	// make sure server is running
	if (!com_sv_running->integer) {
		Com_Printf("Server is not running.\n");
		return;
	}

	if (Cmd_Argc() != 3) {
		Com_Printf("Usage: wannabe <client> <command> \nExecute command as a given client\n");
		return;
	}

	cl = SV_GetPlayerByHandle();
	if (!cl) {
		cl = SV_GetPlayerByNum();
		if (!cl) {
			return;
		}
	}

	SV_WannaBe(cl, Cmd_Argv(2));

}

/*
==================
This is any logic for either excluding, reducing or increasing the chance of a certain spin occuring
==================
*/
std::vector<int> Spin_GeneratePrices(client_t* cl) {

	int cweights[WIN_NUM_WINS] = {};
	std::vector<int> cprizes;

	// Copies our weights for editing
	for (int i = 0; i < WIN_NUM_WINS; i++)
		cweights[i] = weights[i];

	//Bowcaster exclusions: Owned and Ammo > 100
	if ((cl->gentity->playerState->stats[STAT_WEAPONS] & (1 << MB_BOWCASTER)) && cl->gentity->playerState->ammo[MB_AMMO_BOWCASTER_DISRUPTOR] > 100) {
		cweights[WIN_BOWCASTER] = 0;
	}

	//DC15 exclusions: Owned and Ammo > 100
	if ((cl->gentity->playerState->stats[STAT_WEAPONS] & (1 << MB_DC15)) && cl->gentity->playerState->ammo[MB_AMMO_DC15_DLT20_ARM_BLASTER] > 100) {
		cweights[WIN_DC15] = 0;
	}

	//Lightsaber exclusions: Owned
	if ((cl->gentity->playerState->stats[STAT_WEAPONS] & (1 << MB_LIGHTSABER))) {
		cweights[WIN_SABER] = 0;
	}

	//Westar Pistol exclusions: Already owned AND good on ammo
	if ((cl->gentity->playerState->stats[STAT_WEAPONS] & (1 << MB_WESTAR34)) && cl->gentity->playerState->ammo[MB_AMMO_WELSTAR34] > 100) {
		cweights[WIN_WESTAR_PISTOL] = 0;
	}
	
	//Frag Grenade exclusions: Already Owned AND more than one Frag Grenade
	if (cl->gentity->playerState->ammo[MB_AMMO_FRAG_GRENADES] > 1) {
		cweights[WIN_FRAG_GRENADE] = 0;
	}

	//Pulse Grenade exclusions: Already Owned AND more than one Pulse Grenade
	if ((cl->gentity->playerState->stats[STAT_WEAPONS] & (1 << MB_PULSE_GREN)) && cl->gentity->playerState->ammo[MB_AMMO_PULSE_GRENADES] > 1) {
		cweights[WIN_PULSE_GRENADE] = 0;
	}

	//Disruptor Rifle exclusions: Already owned AND more than 100 ammo
	if ((cl->gentity->playerState->stats[STAT_WEAPONS] & (1 << MB_DISRUPTOR)) && cl->gentity->playerState->ammo[MB_AMMO_BOWCASTER_DISRUPTOR] > 100) {
		cweights[WIN_DISRUPTOR] = 0;
	}

	//Projectile Rifle exclusions: Already owned AND more than 10 ammo
	if ((cl->gentity->playerState->stats[STAT_WEAPONS] & (1 << MB_PROJECTILE_RIFLE)) && cl->gentity->playerState->ammo[MB_AMMO_PROJECTILE_RIFLE] > 10) {
		cweights[WIN_PROJECTILE] = 0;
	}

	//DC17 exclusions: Already owned AND more than 100 ammo
	if ((cl->gentity->playerState->stats[STAT_WEAPONS] & (1 << MB_DC17_PISTOL)) && cl->gentity->playerState->ammo[MB_AMMO_DC17_PISTOL] > 100) {
		cweights[WIN_DC17] = 0;
	}

	//Rocket Launcher exclusions: Already owned AND more than 1 rocket
	if ((cl->gentity->playerState->stats[STAT_WEAPONS] & (1 << MB_ROCKET_LAUNCHER)) && cl->gentity->playerState->ammo[MB_AMMO_ROCKETS] > 1) {
		cweights[WIN_ROCKET_LAUNCHER] = 0;
	}

	//T21 exclusions: Already owned AND more than 100 ammo
	if ((cl->gentity->playerState->stats[STAT_WEAPONS] & (1 << MB_T21)) && cl->gentity->playerState->ammo[MB_AMMO_T21_AMMO] > 100) {
		cweights[WIN_T21] = 0;
	}

	//DLT20 exclusions: Already owned AND more than 100 ammo
	if ((cl->gentity->playerState->stats[STAT_WEAPONS] & (1 << MB_DLT) && cl->gentity->playerState->ammo[MB_AMMO_DC15_DLT20_ARM_BLASTER] > 100)) {
		cweights[WIN_DLT] = 0;
	}

	//Arm Blaster exclusions: Already owned AND more than 100 ammo
	if ((cl->gentity->playerState->stats[STAT_WEAPONS] & (1 << MB_ARM_BLASTER) && cl->gentity->playerState->ammo[MB_AMMO_DC15_DLT20_ARM_BLASTER] > 100)) {
		cweights[WIN_ARM_BLASTER] = 0;
	}

	// Armor exclusions: More than 500 Armor
	if ((cl->gentity->playerState->stats[STAT_ARMOR] >= 500)) {
		cweights[WIN_100_ARMOR] = 0;
		cweights[WIN_250_ARMOR] = 0;
	}

	// Health Exclusion, Your health has reached 500
	if ((cl->gentity->health >= 500)) {
		cweights[WIN_100_HEALTH] = 0;
		cweights[WIN_250_HEALTH] = 0;
	}
	
	//Jetpack exclusion: Already has a jetpack
	if (cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_JETPACK)) {
		cweights[WIN_JETPACK] = 0;
	}

	//Cloak exclusion: Already owned
	if ((cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_CLOAK))) {
		cweights[WIN_CLOAK] = 0;
	}

	//EWEB exclusion: Already owned
	if ((cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_EWEB))) {
		cweights[WIN_EWEB] = 0;
	}

	//Sentry Gun exclusion: Already owned
	if (cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_SENTRY_GUN)) {
		cweights[WIN_SENTRY] = 0;
	}

	//Seeker exclusion: Already owned
	if (cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_SEEKER)) {
		cweights[WIN_SEEKER] = 0;
	}

	//Bacta Tank exclusion: Already owned
	if (cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_MEDPAC_BIG)) {
		cweights[WIN_BACTA] = 0;
	}

	//Forcefield exclusion: Already owned
	if (cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] & (1 << HI_SHIELD)) {
		cweights[WIN_FORCEFIELD] = 0;
	}

	//Force Power exclusions: Exclude if you have no force jump or jump level 1
	if (cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_JUMP] == FORCE_LEVEL_0 || cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_JUMP] == FORCE_LEVEL_1) {
		cweights[WIN_FORCE_GRIP] = 0;
		cweights[WIN_FORCE_LIGHTNING] = 0;
		cweights[WIN_FORCE_MINDTRICK] = 0;
		cweights[WIN_FORCE_PULL] = 0;
		cweights[WIN_FORCE_PUSH] = 0;
		cweights[WIN_FORCE_SPEED] = 0;
	}

	/* Exclude these if has any of these powers */
	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_GRIP))
		cweights[WIN_FORCE_GRIP] = 0;

	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_LIGHTNING))
		cweights[WIN_FORCE_LIGHTNING] = 0;

	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_MIND_TRICK))
		cweights[WIN_FORCE_MINDTRICK] = 0;

	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_PULL))
		cweights[WIN_FORCE_PULL] = 0;

	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_PUSH))
		cweights[WIN_FORCE_PUSH] = 0;

	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_SPEED))
		cweights[WIN_FORCE_SPEED] = 0;

	// Force Sensitivity exclusions: Cannot already have force jump
	if (cl->gentity->playerState->fd.forcePowersKnown & (1 << MB_FORCE_JUMP)) {
		cweights[WIN_FORCE_SENSITIVITY] = 0;
	}

	// Now we generate a vector map, with each int being the int of the prize
	for (int i = 0; i < WIN_NUM_WINS; i++) // Loop for every prize
		for (int y = 0; y < cweights[i]; y++) // Now Loop for the number (weight) for that prize
			cprizes.push_back(i);

	return cprizes;

}

/*
==================
Checks if a user has won
==================
*/
qboolean Spin_HasWon(std::vector<int> cprizes, int rando, int prize) {

	if (rando >= cprizes.size())
		return qfalse;

	if (cprizes[rando] == prize) {
		return qtrue;
	}
	else {
		return qfalse;
	}

}

/*
==================
Called when a user runs spin
==================
*/

void SV_Spin(client_t* cl) {

	int		cooldown;
	char	tmp[50];
	char*	playername;
	int		mb_class;
	char*	response;
	int		rando;
	int		spins;
	std::vector<int>	cprizes;
	qboolean valid_spin;

	//cvar_t con_notifytime = Cvar_Get("sv_spin", "1", 0, "");
	//cvar_t con_conspeed = Cvar_Get("sv_spin_cooldown", "20", 0, "Console open/close speed");

	response = "";

	if (!com_sv_running->integer) {
		Com_Printf("Server is not running.\n");
		return;
	}


	playername = cl->name;
	SV_UserinfoChanged(cl);

	// Player is dead / spectating
	if (cl->gentity->playerState->persistant[PERS_TEAM] == TEAM_SPECTATOR) {
		SV_SendServerCommand(cl, "chat \"" SVTELL_PREFIX S_COLOR_RED "%s" S_COLOR_WHITE "\"\n", "You must be alive to spin");
		return;
	}

	// Fetch the Class ID for the client
	//mb_class = SV_ClientMBClass(cl);

	// Testing if we get less crashes
	mb_class = 99;

	// Warn Dekas they cannot spin
	if (mb_class == MB_CLASS_DEKA) {
		SV_SendServerCommand(cl, "chat \"" SVTELL_PREFIX S_COLOR_RED "%s" S_COLOR_WHITE "\"\n", "Dekas cant spin");
		return;
	}

	// Playing is in cooldown, Last SV.TIME calldown was called is saved in userint1
	if (svs.time < cl->gentity->playerState->userInt1) {
		cooldown = (cl->gentity->playerState->userInt1 - svs.time) / 1000;
		response = "still in cooldown";

		if (cooldown > 1) {
			sprintf(tmp, "Spin CoolDown: %d seconds", cooldown);
		}
		else {
			sprintf(tmp, "Spin CoolDown: %d second", cooldown);
		}

		SV_SendServerCommand(cl, "chat \"" S_COLOR_RED "%s" S_COLOR_WHITE "\"\n", tmp);
		return;
	}

	Cvar_Set("sv_cheats", "1");
	Cvar_SetValue("sv_cheats", 1);
	Cvar_SetCheatState();
	GVM_RunFrame(sv.time);

	valid_spin = qfalse;
	spins = 0;

	// Modified Weights for this spin
	cprizes = Spin_GeneratePrices(cl);

	srand(sv.time + cl->gentity->s.clientNum + cl->lastPacketTime);

	do {

		//Generates a random number between the weightTotal and 1 (excludes 0 because 0 is the exclusion list)
		rando = rand() % cprizes.size()+1;

		// Win Bowcaster
		if(Spin_HasWon(cprizes, rando, WIN_BOWCASTER)){	
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_BOWCASTER);
			cl->gentity->playerState->ammo[MB_AMMO_BOWCASTER_DISRUPTOR] = 500;
			SV_WannaGiveWeapon(cl, MB_BOWCASTER);
			Com_Printf("Giving %s^7 a Bowcaster\n", playername);
			response = "You win a Bowcaster";
			valid_spin = qtrue;
			break;
		}

		// Win DC15
		if(Spin_HasWon(cprizes, rando, WIN_DC15)) {
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_DC15);
			cl->gentity->playerState->ammo[MB_AMMO_DC15_DLT20_ARM_BLASTER] = 500;
			SV_WannaGiveWeapon(cl, MB_DC15);
			cl->gentity->playerState->stats[15] |= (1 << 3); //15 for Blobs, darts etc
			Com_Printf("Giving %s^7 a DC15\n", playername);
			response = "You win a DC15";
			valid_spin = qtrue;
			break;
		}

		// Win Lightsaber and a random style
		if(Spin_HasWon(cprizes, rando, WIN_SABER)) {

			char* saberstyle_name = "";

			int saberstyles[] = { MB_SS_BLUE, MB_SS_YELLOW, MB_SS_RED };
			int rand_saberstyle = saberstyles[rand() % 2];

			cl->gentity->playerState->fd.saberAnimLevel = rand_saberstyle;
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_LIGHTSABER);
			SV_WannaGiveWeapon(cl, MB_LIGHTSABER);
			cl->gentity->playerState->fd.saberAnimLevel = rand_saberstyle;

			if (rand_saberstyle == MB_SS_BLUE) saberstyle_name = "Blue";
			if (rand_saberstyle == MB_SS_YELLOW) saberstyle_name = "Yellow";
			if (rand_saberstyle == MB_SS_RED) saberstyle_name = "Red";
	
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SABER_DEFENCE] = 1;
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SABER_OFFENCE] = 1;
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SABER_THROW] = 1;

			Com_Printf("Giving %s^7 a Lightsaber with %s style\n", playername, saberstyle_name);
			Com_sprintf(tmp, sizeof(tmp), "You win a Lightsaber with %s style", saberstyle_name);
			response = tmp;
			valid_spin = qtrue;
			break;
		}

		// Win Westar pistol
		if(Spin_HasWon(cprizes, rando, WIN_WESTAR_PISTOL)) {
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_WESTAR34);
			cl->gentity->playerState->ammo[MB_AMMO_WELSTAR34] = 500;
			SV_WannaGiveWeapon(cl, MB_WESTAR34);
			Com_Printf("Giving %s^7 a Westar 34\n", playername);
			response = "You win a Westar 34";
			valid_spin = qtrue;
			break;			
		}

		// Win Frag Grenades
		if(Spin_HasWon(cprizes, rando, WIN_FRAG_GRENADE)) {
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_FRAG_GREN);
			cl->gentity->playerState->ammo[MB_AMMO_FRAG_GRENADES] += 2;
			SV_WannaGiveWeapon(cl, MB_FRAG_GREN);
			Com_Printf("Giving %s^7 2 Frag Grenades\n", playername);
			response = "You win 2 Frag Grenades";
			valid_spin = qtrue;
			break;
		}

		// Win Pulse Grenades
		if(Spin_HasWon(cprizes, rando, WIN_PULSE_GRENADE)) {
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_PULSE_GREN);
			cl->gentity->playerState->ammo[MB_AMMO_PULSE_GRENADES] += 2;
			SV_WannaGiveWeapon(cl, MB_PULSE_GREN);
			Com_Printf("Giving %s^7 2 Pulse Grenades\n", playername);
			response = "You win 2 Pulse Grenades";
			valid_spin = qtrue;
			break;
		}

		// Win Disruptor Rifle
		if(Spin_HasWon(cprizes, rando, WIN_DISRUPTOR)) {
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_DISRUPTOR);
			cl->gentity->playerState->ammo[MB_AMMO_BOWCASTER_DISRUPTOR] = 500;
			SV_WannaGiveWeapon(cl, MB_DISRUPTOR);
			Com_Printf("Giving %s^7 a Disruptor Rifle\n", playername);
			response = "You win a Disruptor Rifle";
			valid_spin = qtrue;
			break;
		}

		// Win Projectile Rifle
		if(Spin_HasWon(cprizes, rando, WIN_PROJECTILE)) {
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_PROJECTILE_RIFLE);
			cl->gentity->playerState->ammo[MB_AMMO_PROJECTILE_RIFLE] = 20;
			SV_WannaGiveWeapon(cl, MB_PROJECTILE_RIFLE);
			Com_Printf("Giving %s^7 a Projectile Rifle\n", playername);
			response = "You win a Projectile Rifle";
			valid_spin = qtrue;
			break;
		}

		// Win DC17 pistol
		if(Spin_HasWon(cprizes, rando, WIN_DC17)) {
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_DC17_PISTOL);
			cl->gentity->playerState->ammo[MB_AMMO_DC17_PISTOL] = 500;
			SV_WannaGiveWeapon(cl, MB_DC17_PISTOL);
			Com_Printf("Giving %s^7 an DC17 Pistol\n", playername);
			response = "You win a DC17 Pistol";
			valid_spin = qtrue;
			break;
		}

		// Win Rocket Launcher
		if(Spin_HasWon(cprizes, rando, WIN_ROCKET_LAUNCHER)) {
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_ROCKET_LAUNCHER);
			cl->gentity->playerState->ammo[MB_AMMO_ROCKETS] = 3;
			SV_WannaGiveWeapon(cl, MB_ROCKET_LAUNCHER);
			Com_Printf("Giving %s^7 a Rocket Launcher\n", playername);
			response = "You win a Rocket Launcher";
			valid_spin = qtrue;
			break;
		}

		// Win DLT20
		if(Spin_HasWon(cprizes, rando, WIN_DLT)) {
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_DLT);
			cl->gentity->playerState->ammo[MB_AMMO_DC15_DLT20_ARM_BLASTER] = 500;
			SV_WannaGiveWeapon(cl, MB_DLT);
			Com_Printf("Giving %s ^7an a DLT20\n", playername);
			response = "You win a DLT20";
			valid_spin = qtrue;
			break;			
		}

		// Win Arm Blaster
		if(Spin_HasWon(cprizes, rando, WIN_ARM_BLASTER)) {
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_ARM_BLASTER);
			cl->gentity->playerState->ammo[MB_AMMO_DC15_DLT20_ARM_BLASTER] = 500;
			SV_WannaGiveWeapon(cl, MB_ARM_BLASTER);
			Com_Printf("Giving %s ^7an a Arm Blaster\n", playername);
			response = "You win an Arm Blaster";
			valid_spin = qtrue;
			break;	
		}

		// Win T21
		if(Spin_HasWon(cprizes, rando, WIN_T21)) {			
			cl->gentity->playerState->stats[STAT_WEAPONS] |= (1 << MB_T21);
			cl->gentity->playerState->ammo[MB_AMMO_T21_AMMO] = 500;
			SV_WannaGiveWeapon(cl, MB_T21);
			Com_Printf("Giving %s^7 a T21\n", playername);
			response = "You win a T21";
			valid_spin = qtrue;
			break;
		}

		// Win 100 Armor
		if(Spin_HasWon(cprizes, rando, WIN_100_ARMOR)) {
			cl->gentity->playerState->stats[STAT_ARMOR] += 100;
			Com_Printf("Giving %s^7 100 Armor\n", playername);
			response = "You win 100 extra Armor";
			valid_spin = qtrue;
			break;
		}

		// Win 250 Armor
		if(Spin_HasWon(cprizes, rando, WIN_250_ARMOR)) {
			cl->gentity->playerState->stats[STAT_ARMOR] += 250;
			Com_Printf("Giving %s^7 250 Armor\n", playername);
			response = "You win 250 extra Armor";
			valid_spin = qtrue;
			break;
		}

		// Win Jetpack
		if(Spin_HasWon(cprizes, rando, WIN_JETPACK)) {	
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_JETPACK);
			cl->gentity->playerState->jetpackFuel = 100;
			Com_Printf("Giving %s^7 a Jetpack\n", playername);
			response = "You win a Jetpack ";
			valid_spin = qtrue;
			break;
		}

		// Win Cloak
		if(Spin_HasWon(cprizes, rando, WIN_CLOAK)) {
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_CLOAK);
			cl->gentity->playerState->cloakFuel = 100;
			Com_Printf("Giving %s^7 a Cloak Generator\n", playername);
			response = "You win a Cloak Generator ";
			valid_spin = qtrue;
			break;		
		}

		// Win EWEB
		if(Spin_HasWon(cprizes, rando, WIN_EWEB)) {
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_EWEB);
			Com_Printf("Giving %s^7 an EWEB Gun Emplacement\n", playername);
			response = "You win an EWEB Gun Emplacement";
			valid_spin = qtrue;
			break;			
		}

		// Win Auto Sentry
		if(Spin_HasWon(cprizes, rando, WIN_SENTRY)) {			
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_SENTRY_GUN);
			Com_Printf("Giving %s^7 a Automated Sentry Gun\n", playername);
			response = "You win an Automated Sentry Gun";
			valid_spin = qtrue;
			break;
		}

		// Win Seeker Drone
		if(Spin_HasWon(cprizes, rando, WIN_SEEKER)) {
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_SEEKER);
			Com_Printf("Giving %s ^7 a Seeker Droid\n", playername);
			response = "You win a Seeker Droid";
			valid_spin = qtrue;
			break;			
		}

		// Win Bacta
		if(Spin_HasWon(cprizes, rando, WIN_BACTA)) {
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_MEDPAC_BIG);
			Com_Printf("Giving %s ^7 a Tank of Bacta\n", playername);
			response = "You win a Tank of Bacta";
			valid_spin = qtrue;
			break;			
		}

		// Win Forcefield
		if(Spin_HasWon(cprizes, rando, WIN_FORCEFIELD)) {
			cl->gentity->playerState->stats[STAT_HOLDABLE_ITEMS] |= (1 << HI_SHIELD);
			Com_Printf("Giving %s ^7 a Forcefield Generator\n", playername);
			response = "You win a Forcefield Generator";
			valid_spin = qtrue;
			break;			
		}

		// Win Taun Taun
		if(Spin_HasWon(cprizes, rando, WIN_TAUN_TAUN)) {
			SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle tauntaun", 5);
			Com_Printf("Giving %s ^7 a TaunTaun\n", playername);
			response = "You win a TaunTaun " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue;
			break;
		}

		// Win Swoop
		if(Spin_HasWon(cprizes, rando, WIN_SWOOP)) {
			Com_Printf("Giving %s ^7 a Swoop Bike\n", playername);
			int rand_swoop = rand() % 6;
			response = "You win a Swoop Bike " SPAWN_VEHICLE_SUFFIX;

			if (rand_swoop == 0) {
				SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle swoop_mp2", 5);
			}

			if (rand_swoop == 1) {
				SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle swoop_mp", 5);
			}

			if (rand_swoop == 2) {
				SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle swoop_battle_cunning", 5);
			}

			if (rand_swoop == 3) {
				SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle swoop_race_b", 5);
			}

			if (rand_swoop == 4) {
				SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle yavin_swoop", 5);
			}

			if (rand_swoop == 5) {
				SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle swoop_mp2", 5);
			}

			valid_spin = qtrue;
			break;
		}

		// Win Speeder
		if(Spin_HasWon(cprizes, rando, WIN_SPEEDER)) {
			Com_Printf("Giving %s ^7 a Sith Speeder\n", playername);
			response = "You win a Sith Speeder " SPAWN_VEHICLE_SUFFIX;
			SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle sithspeeder_mst", 5);
			valid_spin = qtrue;
			break;
		}

		// Win Dewback
		if(Spin_HasWon(cprizes, rando, WIN_DEWBACK)) {
			Com_Printf("Giving %s ^7 a Dewback\n", playername);
			response = "You win a Dewback " SPAWN_VEHICLE_SUFFIX;
			SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle dewback", 5);
			valid_spin = qtrue;
			break;
		}

		// Win Mech
		if(Spin_HasWon(cprizes, rando, WIN_MECH)) {
			Com_Printf("Giving %s ^7 a Shinrar Mech\n", playername);
			SV_ExecuteClientCommandDelayed(cl, "npc spawn vehicle shinraR", 5);
			SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "%s won a Shinrar Mech! We're in the end-game now...\"\n", playername);
			response = "You win a Mech " SPAWN_VEHICLE_SUFFIX;
			valid_spin = qtrue;
			break;
		}

		// Win XS Size
		if(Spin_HasWon(cprizes, rando, WIN_SIZE_XS)) {
			Com_Printf("Making %s ^7 Extra-Small\n", playername);
			cl->gentity->playerState->iModelScale = 50;
			response = "You are Extra Small!";
			valid_spin = qtrue;
			break;
		}

		// Win S Size
		if(Spin_HasWon(cprizes, rando, WIN_SIZE_S)) {
			Com_Printf("Making %s ^7 Small\n", playername);
			cl->gentity->playerState->iModelScale = 80;
			response = "You are Small!";
			valid_spin = qtrue;
			break;
		}

		// Win Force Sensitvity
		if(Spin_HasWon(cprizes, rando, WIN_FORCE_SENSITIVITY)) {
			Com_Printf("Making %s ^7 Force Sensitive\n", playername);

			/* 300 Force Points, which do not regen */
			cl->gentity->playerState->fd.forcePower = 300;
			cl->gentity->playerState->fd.forcePowerMax = 300;

			/* Give some BP As well */
			cl->gentity->playerState->jetpackFuel = 100;

			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_JUMP);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_JUMP] = FORCE_LEVEL_1;

			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_SABER_OFFENCE);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SABER_OFFENCE] = FORCE_LEVEL_2;

			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_SABER_DEFENCE);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SABER_DEFENCE] = FORCE_LEVEL_2;

			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_PULL);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_PULL] = FORCE_LEVEL_2;

			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_PUSH);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_PUSH] = FORCE_LEVEL_2;

			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_SPEED);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SPEED] = FORCE_LEVEL_2;

			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_SENSE);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SENSE] = FORCE_LEVEL_2;

			response = "You Win force sensitivity!";
			valid_spin = qtrue;
			break;
		}

		// Win Force Speed Level 3
		if(Spin_HasWon(cprizes, rando, WIN_FORCE_SPEED)) {
			Com_Printf("Giving %s ^7 Force Grip Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_SPEED);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_SPEED] = 3;
			response = "You Win Force Speed Level 3!";
			valid_spin = qtrue;
			break;
		}

		// Win Force Push Level 3
		if(Spin_HasWon(cprizes, rando, WIN_FORCE_PUSH)) {
			Com_Printf("Giving %s ^7 Force Push Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_PUSH);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_PUSH] = 3;
			response = "You Win Force Push Level 3!";
			valid_spin = qtrue;
			break;
		}

		// Win Force Pull Level 3
		if(Spin_HasWon(cprizes, rando, WIN_FORCE_PULL)) {
			Com_Printf("Giving %s ^7 Force Pull Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_PULL);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_PULL] = 3;
			response = "You Win Force Pull Level 3!";
			valid_spin = qtrue;
			break;
		}

		// Win Force Lightning Level 3
		if(Spin_HasWon(cprizes, rando, WIN_FORCE_LIGHTNING)) {
			Com_Printf("Giving %s ^7 Force Pull Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_LIGHTNING);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_LIGHTNING] = 3;
			response = "You Win Force Lightning Level 3!";
			valid_spin = qtrue;
			break;
		}

		// Win Force Grip Level 3
		if(Spin_HasWon(cprizes, rando, WIN_FORCE_GRIP)) {
			Com_Printf("Giving %s ^7 Force Grip Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_GRIP);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_GRIP] = 3;
			response = "You Win Force Grip Level 3!";
			valid_spin = qtrue;
			break;
		}

		// Win Force Mind Trick Level 3
		if(Spin_HasWon(cprizes, rando, WIN_FORCE_MINDTRICK)) {
			Com_Printf("Giving %s ^7 Force Mind Trick Level 3\n", playername);
			cl->gentity->playerState->fd.forcePowersKnown |= (1 << MB_FORCE_MIND_TRICK);
			cl->gentity->playerState->fd.forcePowerLevel[MB_FORCE_MIND_TRICK] = 3;
			response = "You Win Force Mind Trick Level 3!";
			valid_spin = qtrue;
			break;
		}

		// Win Invincibility
		if(Spin_HasWon(cprizes, rando, WIN_INVINCIBLE)) {
			Com_Printf("Giving %s ^7 Invincibility\n", playername);
			SV_ExecuteClientCommand(cl, "god", qtrue);
			GVM_RunFrame(sv.time);
			SV_ExecuteClientCommandDelayed(cl, "god", 30);
			SV_ClientTimedPowerup(cl, MB_PW_INVINSIBLE, 30);
			response = "30 Seconds of Invincibility";
			valid_spin = qtrue;
			break;
		}

		// Win 100 Health
		if (Spin_HasWon(cprizes, rando, WIN_100_HEALTH)) {
			Com_Printf("Giving %s ^7 100 Health\n", playername);
			
			cl->gentity->health = cl->gentity->health + 100;
			cl->gentity->playerState->stats[STAT_MAX_HEALTH] = cl->gentity->health;

			response = "You win a 100 Health Boost!";
			valid_spin = qtrue;
			break;
		}

		// Win 250 Health
		if (Spin_HasWon(cprizes, rando, WIN_250_HEALTH)) {
			Com_Printf("Giving %s ^7 250 Health\n", playername);

			cl->gentity->health = cl->gentity->health + 250;
			cl->gentity->playerState->stats[STAT_MAX_HEALTH] = cl->gentity->health;

			response = "You win a 250 Health Boost!";
			valid_spin = qtrue;
			break;
		}

		spins++;

	} while (valid_spin == qfalse || spins < 20);

	if (spins == 20) {
		response = "Something went wrong with you spin. We did 20 spins and you won nothing all 20 times...report to admin";
	}

	Cvar_Set("sv_cheats", "0");

	// Next Spin Time From When
	cl->gentity->playerState->userInt1 = svs.time + sv_spinCooldown->integer * 1000;

	SV_SendServerCommand(cl, "chat \"" S_COLOR_MAGENTA "%s" S_COLOR_WHITE "\"\n", response);

	return;
}

/*
==================
SV_AddOperatorCommands
==================
*/
void SV_AddOperatorCommands( void ) {
	static qboolean	initialized;

	if ( initialized ) {
		return;
	}
	initialized = qtrue;

	// New Commands
	Cmd_AddCommand("wannatest", SV_WannaTest_f, "Used for testing things");
	Cmd_AddCommand("wannaforce", SV_WannaForce_f, "Give a client a force power");
	Cmd_AddCommand("wannagiveweapon", SV_WannaGiveWeapon_f, "Give a player a weapon");
	Cmd_AddCommand("wannagiveweaponsall", SV_WannaGiveWeaponsAll_f, "Give all weapons to a client");
    Cmd_AddCommand("wannagiveammo", SV_WannaGiveAmmo_f, "Set ammo for a client for a specific ammo type");
    Cmd_AddCommand("wannagiveammoall", SV_WannaGiveAmmoAll_f, "Set all ammo types for a client");

	Cmd_AddCommand("wannacheat", SV_WannaCheat_f, "Enable cheats without needing map restart");
	Cmd_AddCommand("wannabe", SV_WannaBe_f, "Execute a command as a given client");
	Cmd_AddCommand("wannascale", SV_WannaScale_f, "Scale a clients model");

	Cmd_AddCommand ("heartbeat", SV_Heartbeat_f, "Sends a heartbeat to the masterserver" );
	Cmd_AddCommand ("kick", SV_Kick_f, "Kick a user from the server" );
	Cmd_AddCommand ("kickbots", SV_KickBots_f, "Kick all bots from the server" );
	Cmd_AddCommand ("kickall", SV_KickAll_f, "Kick all users from the server" );
	Cmd_AddCommand ("kicknum", SV_KickNum_f, "Kick a user from the server by userid" );
	Cmd_AddCommand ("clientkick", SV_KickNum_f, "Kick a user from the server by userid" );
	Cmd_AddCommand ("status", SV_Status_f, "Prints status of server and connected clients" );
	Cmd_AddCommand ("serverinfo", SV_Serverinfo_f, "Prints the serverinfo that is visible in the server browsers" );
	Cmd_AddCommand ("systeminfo", SV_Systeminfo_f, "Prints the systeminfo variables that are replicated to clients" );
	Cmd_AddCommand ("dumpuser", SV_DumpUser_f, "Prints the userinfo for a given userid" );
	Cmd_AddCommand ("map_restart", SV_MapRestart_f, "Restart the current map" );
	Cmd_AddCommand ("sectorlist", SV_SectorList_f);
	Cmd_AddCommand ("map", SV_Map_f, "Load a new map with cheats disabled" );
	Cmd_SetCommandCompletionFunc( "map", SV_CompleteMapName );
	Cmd_AddCommand ("devmap", SV_Map_f, "Load a new map with cheats enabled" );
	Cmd_SetCommandCompletionFunc( "devmap", SV_CompleteMapName );
//	Cmd_AddCommand ("devmapbsp", SV_Map_f);	// not used in MP codebase, no server BSP_cacheing
	Cmd_AddCommand ("devmapmdl", SV_Map_f, "Load a new map with cheats enabled" );
	Cmd_SetCommandCompletionFunc( "devmapmdl", SV_CompleteMapName );
	Cmd_AddCommand ("devmapall", SV_Map_f, "Load a new map with cheats enabled" );
	Cmd_SetCommandCompletionFunc( "devmapall", SV_CompleteMapName );
	Cmd_AddCommand ("killserver", SV_KillServer_f, "Shuts the server down and disconnects all clients" );
	Cmd_AddCommand ("svsay", SV_ConSay_f, "Broadcast server messages to clients" );
	Cmd_AddCommand ("svtell", SV_ConTell_f, "Private message from the server to a user" );
	Cmd_AddCommand ("forcetoggle", SV_ForceToggle_f, "Toggle g_forcePowerDisable bits" );
	Cmd_AddCommand ("weapontoggle", SV_WeaponToggle_f, "Toggle g_weaponDisable bits" );
	Cmd_AddCommand ("svrecord", SV_Record_f, "Record a server-side demo" );
	Cmd_AddCommand ("svstoprecord", SV_StopRecord_f, "Stop recording a server-side demo" );
	Cmd_AddCommand ("sv_rehashbans", SV_RehashBans_f, "Reloads banlist from file" );
	Cmd_AddCommand ("sv_listbans", SV_ListBans_f, "Lists bans" );
	Cmd_AddCommand ("sv_banaddr", SV_BanAddr_f, "Bans a user" );
	Cmd_AddCommand ("sv_exceptaddr", SV_ExceptAddr_f, "Adds a ban exception for a user" );
	Cmd_AddCommand ("sv_bandel", SV_BanDel_f, "Removes a ban" );
	Cmd_AddCommand ("sv_exceptdel", SV_ExceptDel_f, "Removes a ban exception" );
	Cmd_AddCommand ("sv_flushbans", SV_FlushBans_f, "Removes all bans and exceptions" );

}

/*
==================
SV_RemoveOperatorCommands
==================
*/
void SV_RemoveOperatorCommands( void ) {
#if 0
	// removing these won't let the server start again
	Cmd_RemoveCommand ("heartbeat");
	Cmd_RemoveCommand ("kick");
	Cmd_RemoveCommand ("banUser");
	Cmd_RemoveCommand ("banClient");
	Cmd_RemoveCommand ("status");
	Cmd_RemoveCommand ("serverinfo");
	Cmd_RemoveCommand ("systeminfo");
	Cmd_RemoveCommand ("dumpuser");
	Cmd_RemoveCommand ("map_restart");
	Cmd_RemoveCommand ("sectorlist");
	Cmd_RemoveCommand ("svsay");
#endif
}
