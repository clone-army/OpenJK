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

// sv_client.c -- server code for dealing with clients

#include "server.h"
#include "qcommon/stringed_ingame.h"
#include "qcommon/md5.h"
#include "spin.h"

#include <ctype.h>

// Raw POSIX file I/O + advisory locking for the shared economy accounts
// file (see SV_EconomyAccountsPath) - every instance is a separate OS
// process, so writes across them need real cross-process locking the
// engine's own FS_ layer doesn't provide.
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <time.h>

#ifdef USE_INTERNAL_ZLIB
#include "zlib/zlib.h"
#else
#include <zlib.h>
#endif

#include "server/sv_gameapi.h"

static const int kEconomyKillReward = 5;
static const int kEconomyRoundReward = 1;


static void SV_CloseDownload( client_t *cl );

/*
=================
SV_GetChallenge

A "getchallenge" OOB command has been received
Returns a challenge number that can be used
in a subsequent connectResponse command.
We do this to prevent denial of service attacks that
flood the server with invalid connection IPs.  With a
challenge, they must give a valid IP address.

If we are authorizing, a challenge request will cause a packet
to be sent to the authorize server.

When an authorizeip is returned, a challenge response will be
sent to that ip.

ioquake3/openjk: we added a possibility for clients to add a challenge
to their packets, to make it more difficult for malicious servers
to hi-jack client connections.
=================
*/
void SV_GetChallenge( netadr_t from ) {
	int		challenge;
	int		clientChallenge;

	// ignore if we are in single player
	/*
	if ( Cvar_VariableValue( "g_gametype" ) == GT_SINGLE_PLAYER || Cvar_VariableValue("ui_singlePlayerActive")) {
		return;
	}
	*/
	if (Cvar_VariableValue("ui_singlePlayerActive"))
	{
		return;
	}

	// Prevent using getchallenge as an amplifier
	if ( SVC_RateLimitAddress( from, 10, 1000 ) ) {
		if ( com_developer->integer ) {
			Com_Printf( "SV_GetChallenge: rate limit from %s exceeded, dropping request\n",
				NET_AdrToString( from ) );
		}
		return;
	}

	// Create a unique challenge for this client without storing state on the server
	challenge = SV_CreateChallenge(from);

	// Grab the client's challenge to echo back (if given)
	clientChallenge = atoi(Cmd_Argv(1));

	NET_OutOfBandPrint( NS_SERVER, from, "challengeResponse %i %i", challenge, clientChallenge );
}

/*
==================
SV_IsBanned

Check whether a certain address is banned
==================
*/

static qboolean SV_IsBanned( netadr_t *from, qboolean isexception )
{
	int index;
	serverBan_t *curban;

	if ( !serverBansCount ) {
		return qfalse;
	}

	if ( !isexception )
	{
		// If this is a query for a ban, first check whether the client is excepted
		if ( SV_IsBanned( from, qtrue ) )
			return qfalse;
	}

	for ( index = 0; index < serverBansCount; index++ )
	{
		curban = &serverBans[index];

		if ( curban->isexception == isexception )
		{
			if ( NET_CompareBaseAdrMask( curban->ip, *from, curban->subnet ) )
				return qtrue;
		}
	}

	return qfalse;
}

/*
==================
SV_DirectConnect

A "connect" OOB command has been received
==================
*/
void SV_DirectConnect( netadr_t from ) {
	char		userinfo[MAX_INFO_STRING];
	int			i;
	client_t	*cl, *newcl;
	client_t	temp;
	sharedEntity_t *ent;
	int			clientNum;
	int			version;
	int			qport;
	int			challenge;
	char		*password;
	int			startIndex;
	char		*denied;
	int			count;
	char		*ip;

	Com_DPrintf ("SVC_DirectConnect ()\n");

	// Check whether this client is banned.
	if ( SV_IsBanned( &from, qfalse ) )
	{
		NET_OutOfBandPrint( NS_SERVER, from, "print\nYou are banned from this server.\n" );
		Com_DPrintf( "    rejected connect from %s (banned)\n", NET_AdrToString(from) );
		return;
	}

	Q_strncpyz( userinfo, Cmd_Argv(1), sizeof(userinfo) );

	version = atoi( Info_ValueForKey( userinfo, "protocol" ) );
	if ( version != PROTOCOL_VERSION ) {
		NET_OutOfBandPrint( NS_SERVER, from, "print\nServer uses protocol version %i (yours is %i).\n", PROTOCOL_VERSION, version );
		Com_DPrintf ("    rejected connect from version %i\n", version);
		return;
	}

	challenge = atoi( Info_ValueForKey( userinfo, "challenge" ) );
	qport = atoi( Info_ValueForKey( userinfo, "qport" ) );

	// quick reject
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {

/* This was preventing sv_reconnectlimit from working.  It seems like commenting this
   out has solved the problem.  HOwever, if there is a future problem then it could
   be this.

		if ( cl->state == CS_FREE ) {
			continue;
		}
*/

		if ( NET_CompareBaseAdr( from, cl->netchan.remoteAddress )
			&& ( cl->netchan.qport == qport
			|| from.port == cl->netchan.remoteAddress.port ) ) {
			if (( svs.time - cl->lastConnectTime)
				< (sv_reconnectlimit->integer * 1000)) {
				NET_OutOfBandPrint( NS_SERVER, from, "print\nReconnect rejected : too soon\n" );
				Com_DPrintf ("%s:reconnect rejected : too soon\n", NET_AdrToString (from));
				return;
			}
			break;
		}
	}

	// don't let "ip" overflow userinfo string
	if ( NET_IsLocalAddress (from) )
		ip = "localhost";
	else
		ip = (char *)NET_AdrToString( from );
	if( ( strlen( ip ) + strlen( userinfo ) + 4 ) >= MAX_INFO_STRING ) {
		NET_OutOfBandPrint( NS_SERVER, from,
			"print\nUserinfo string length exceeded.  "
			"Try removing setu cvars from your config.\n" );
		return;
	}
	Info_SetValueForKey( userinfo, "ip", ip );

	// see if the challenge is valid (localhost clients don't need to challenge)
	if (!NET_IsLocalAddress(from))
	{
		// Verify the received challenge against the expected challenge
		if (!SV_VerifyChallenge(challenge, from))
		{
			NET_OutOfBandPrint( NS_SERVER, from, "print\nIncorrect challenge for your address.\n" );
			return;
		}
	}

	newcl = &temp;
	Com_Memset (newcl, 0, sizeof(client_t));

	// if there is already a slot for this ip, reuse it
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
		if ( cl->state == CS_FREE ) {
			continue;
		}
		if ( NET_CompareBaseAdr( from, cl->netchan.remoteAddress )
			&& ( cl->netchan.qport == qport
			|| from.port == cl->netchan.remoteAddress.port ) ) {
			Com_Printf ("%s:reconnect\n", NET_AdrToString (from));
			newcl = cl;
			// VVFIXME - both SOF2 and Wolf remove this call, claiming it blows away the user's info
			// disconnect the client from the game first so any flags the
			// player might have are dropped
			GVM_ClientDisconnect( newcl - svs.clients );
			//
			goto gotnewcl;
		}
	}

	// find a client slot
	// if "sv_privateClients" is set > 0, then that number
	// of client slots will be reserved for connections that
	// have "password" set to the value of "sv_privatePassword"
	// Info requests will report the maxclients as if the private
	// slots didn't exist, to prevent people from trying to connect
	// to a full server.
	// This is to allow us to reserve a couple slots here on our
	// servers so we can play without having to kick people.

	// check for privateClient password
	password = Info_ValueForKey( userinfo, "password" );
	if ( !strcmp( password, sv_privatePassword->string ) ) {
		startIndex = 0;
	} else {
		// skip past the reserved slots
		startIndex = sv_privateClients->integer;
	}

	newcl = NULL;
	for ( i = startIndex; i < sv_maxclients->integer ; i++ ) {
		cl = &svs.clients[i];
		if (cl->state == CS_FREE) {
			newcl = cl;
			break;
		}
	}

	if ( !newcl ) {
		if ( NET_IsLocalAddress( from ) ) {
			count = 0;
			for ( i = startIndex; i < sv_maxclients->integer ; i++ ) {
				cl = &svs.clients[i];
				if (cl->netchan.remoteAddress.type == NA_BOT) {
					count++;
				}
			}
			// if they're all bots
			if (count >= sv_maxclients->integer - startIndex) {
				SV_DropClient(&svs.clients[sv_maxclients->integer - 1], "only bots on server");
				newcl = &svs.clients[sv_maxclients->integer - 1];
			}
			else {
				Com_Error( ERR_FATAL, "server is full on local connect\n" );
				return;
			}
		}
		else {
			const char *SV_GetStringEdString(char *refSection, char *refName);
			NET_OutOfBandPrint( NS_SERVER, from, va("print\n%s\n", SV_GetStringEdString("MP_SVGAME","SERVER_IS_FULL")));
			Com_DPrintf ("Rejected a connection.\n");
			return;
		}
	}

	// we got a newcl, so reset the reliableSequence and reliableAcknowledge
	cl->reliableAcknowledge = 0;
	cl->reliableSequence = 0;

gotnewcl:

	// build a new connection
	// accept the new client
	// this is the only place a client_t is ever initialized
	*newcl = temp;
	clientNum = newcl - svs.clients;
	ent = SV_GentityNum( clientNum );
	newcl->gentity = ent;

	// save the challenge
	newcl->challenge = challenge;

	// save the address
	Netchan_Setup (NS_SERVER, &newcl->netchan , from, qport);

	// save the userinfo
	Q_strncpyz( newcl->userinfo, userinfo, sizeof(newcl->userinfo) );

	// get the game a chance to reject this connection or modify the userinfo
	denied = GVM_ClientConnect( clientNum, qtrue, qfalse ); // firstTime = qtrue
	if ( denied ) {
		NET_OutOfBandPrint( NS_SERVER, from, "print\n%s\n", denied );
		Com_DPrintf ("Game rejected a connection: %s.\n", denied);
		return;
	}

	SV_UserinfoChanged( newcl );

	// send the connect packet to the client
	NET_OutOfBandPrint( NS_SERVER, from, "connectResponse" );

	Com_DPrintf( "Going from CS_FREE to CS_CONNECTED for %s\n", newcl->name );

	newcl->state = CS_CONNECTED;
	newcl->nextSnapshotTime = svs.time;
	newcl->lastPacketTime = svs.time;
	newcl->lastConnectTime = svs.time;

	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	newcl->gamestateMessageNum = -1;

	newcl->lastUserInfoChange = 0; //reset the delay
	newcl->lastUserInfoCount = 0; //reset the count

	// if this was the first client on the server, or the last client
	// the server can hold, send a heartbeat to the master.
	count = 0;
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			count++;
		}
	}
	if ( count == 1 || count == sv_maxclients->integer ) {
		SV_Heartbeat_f();
	}
}


/*
=====================
SV_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing -- SV_FinalMessage() will handle that
=====================
*/
void SV_DropClient( client_t *drop, const char *reason ) {
	int		i;
	const bool isBot = drop->netchan.remoteAddress.type == NA_BOT;

	if ( drop->state == CS_ZOMBIE ) {
		return;		// already dropped
	}

	// Kill any download
	SV_CloseDownload( drop );

	// tell everyone why they got dropped
	SV_SendServerCommand( NULL, "print \"%s" S_COLOR_WHITE " %s\n\"", drop->name, reason );

	// call the prog function for removing a client
	// this will remove the body, among other things
	GVM_ClientDisconnect( drop - svs.clients );

	// clear this slot's gun game tier so whoever connects into it next
	// doesn't inherit someone else's progress
	SV_GunGameClientDisconnect( (int)(drop - svs.clients) );

	// same, for kill streak state
	SV_KillstreakClientDisconnect( (int)(drop - svs.clients) );

	// flush this client's final partial playtime segment before their name/
	// economyHandle (used as the stats key) are gone
	SV_StatsClientDisconnect( drop );

	// a bounty on someone who's gone can never be collected - give it back
	SV_EconomyBountyRefund( drop, "they left" );

	// don't let whoever connects into this slot next inherit a stale mute
	// or message count from a completely different player
	drop->chatMsgCount = 0;
	drop->chatWindowStart = 0;
	drop->chatMutedUntil = 0;
	drop->gunraySpecTime = 0;

	// add the disconnect command
	SV_SendServerCommand( drop, "disconnect \"%s\"", reason );

	if ( isBot ) {
		SV_BotFreeClient( drop - svs.clients );
	}

	// nuke user info
	SV_SetUserinfo( drop - svs.clients, "" );

	if ( isBot ) {
		// bots shouldn't go zombie, as there's no real net connection.
		drop->state = CS_FREE;
	} else {
		Com_DPrintf( "Going to CS_ZOMBIE for %s\n", drop->name );
		drop->state = CS_ZOMBIE;		// become free in a few seconds
	}

	if ( drop->demo.demorecording ) {
		SV_StopRecordDemo( drop );
	}

	// if this was the last client on the server, send a heartbeat
	// to the master so it is known the server is empty
	// send a heartbeat now so the master will get up to date info
	// if there is already a slot for this ip, reuse it
	for (i=0 ; i < sv_maxclients->integer ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			break;
		}
	}
	if ( i == sv_maxclients->integer ) {
		SV_Heartbeat_f();
	}
}

void SV_CreateClientGameStateMessage( client_t *client, msg_t *msg ) {
	int			start;
	entityState_t	*base, nullstate;

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, msg );

	// send the gamestate
	MSG_WriteByte( msg, svc_gamestate );
	MSG_WriteLong( msg, client->reliableSequence );

	// write the configstrings
	for ( start = 0 ; start < MAX_CONFIGSTRINGS ; start++ ) {
		if (sv.configstrings[start][0]) {
			MSG_WriteByte( msg, svc_configstring );
			MSG_WriteShort( msg, start );
			MSG_WriteBigString( msg, sv.configstrings[start] );
		}
	}

	// write the baselines
	Com_Memset( &nullstate, 0, sizeof( nullstate ) );
	for ( start = 0 ; start < MAX_GENTITIES; start++ ) {
		base = &sv.svEntities[start].baseline;
		if ( !base->number ) {
			continue;
		}
		MSG_WriteByte( msg, svc_baseline );
		MSG_WriteDeltaEntity( msg, &nullstate, base, qtrue );
	}

	MSG_WriteByte( msg, svc_EOF );

	MSG_WriteLong( msg, client - svs.clients);

	// write the checksum feed
	MSG_WriteLong( msg, sv.checksumFeed);

	// For old RMG system.
	MSG_WriteShort ( msg, 0 );
}

/*
================
SV_SendClientGameState

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each new map load.

It will be resent if the client acknowledges a later message but has
the wrong gamestate.
================
*/
void SV_SendClientGameState( client_t *client ) {
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// MW - my attempt to fix illegible server message errors caused by
	// packet fragmentation of initial snapshot.
	while(client->state&&client->netchan.unsentFragments)
	{
		// send additional message fragments if the last message
		// was too large to send at once

		Com_Printf ("[ISM]SV_SendClientGameState() [2] for %s, writing out old fragments\n", client->name);
		SV_Netchan_TransmitNextFragment(&client->netchan);
	}

	Com_DPrintf ("SV_SendClientGameState() for %s\n", client->name);
	Com_DPrintf( "Going from CS_CONNECTED to CS_PRIMED for %s\n", client->name );
	if ( client->state == CS_CONNECTED )
		client->state = CS_PRIMED;
	client->pureAuthentic = 0;
	client->gotCP = qfalse;

	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	client->gamestateMessageNum = client->netchan.outgoingSequence;

	SV_CreateClientGameStateMessage( client, &msg );

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}


void SV_SendClientMapChange( client_t *client )
{
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( &msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, &msg );

	// send the gamestate
	MSG_WriteByte( &msg, svc_mapchange );

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}

/*
==================
SV_ClientEnterWorld
==================
*/
void SV_ClientEnterWorld( client_t *client, usercmd_t *cmd ) {
	int		clientNum;
	sharedEntity_t *ent;

	Com_DPrintf( "Going from CS_PRIMED to CS_ACTIVE for %s\n", client->name );
	client->state = CS_ACTIVE;

	// resend all configstrings using the cs commands since these are
	// no longer sent when the client is CS_PRIMED
	SV_UpdateConfigstrings( client );

	// set up the entity for the client
	clientNum = client - svs.clients;
	ent = SV_GentityNum( clientNum );
	ent->s.number = clientNum;
	client->gentity = ent;

	client->lastUserInfoChange = 0; //reset the delay
	client->lastUserInfoCount = 0; //reset the count
	
	client->gentity->playerState->userInt1 = 0; //reset spin delay

	client->deltaMessage = -1;
	client->nextSnapshotTime = svs.time;	// generate a snapshot immediately

	if(cmd)
		memcpy(&client->lastUsercmd, cmd, sizeof(client->lastUsercmd));
	else
		memset(&client->lastUsercmd, '\0', sizeof(client->lastUsercmd));

	// call the game begin function
	GVM_ClientBegin( client - svs.clients );

	// Re-apply gun game's current tier weapon after the game module's own
	// spawn/loadout logic has run, so it isn't immediately overwritten.
	SV_GunGameClientBegin( client );

	SV_BeginAutoRecordDemos();
}

/*
============================================================

CLIENT COMMAND EXECUTION

============================================================
*/

/*
==================
SV_CloseDownload

clear/free any download vars
==================
*/
static void SV_CloseDownload( client_t *cl ) {
	int i;

	// EOF
	if (cl->download) {
		FS_FCloseFile( cl->download );
	}
	cl->download = 0;
	*cl->downloadName = 0;

	// Free the temporary buffer space
	for (i = 0; i < MAX_DOWNLOAD_WINDOW; i++) {
		if (cl->downloadBlocks[i]) {
			Z_Free( cl->downloadBlocks[i] );
			cl->downloadBlocks[i] = NULL;
		}
	}

}

/*
==================
SV_StopDownload_f

Abort a download if in progress
==================
*/
static void SV_StopDownload_f( client_t *cl ) {
	if ( cl->state == CS_ACTIVE )
		return;

	if (*cl->downloadName)
		Com_DPrintf( "clientDownload: %d : file \"%s\" aborted\n", cl - svs.clients, cl->downloadName );

	SV_CloseDownload( cl );
}

/*
==================
SV_DoneDownload_f

Downloads are finished
==================
*/
static void SV_DoneDownload_f( client_t *cl ) {
	if ( cl->state == CS_ACTIVE )
		return;

	Com_DPrintf( "clientDownload: %s Done\n", cl->name);
	// resend the game state to update any clients that entered during the download
	SV_SendClientGameState(cl);
}

/*
==================
SV_NextDownload_f

The argument will be the last acknowledged block from the client, it should be
the same as cl->downloadClientBlock
==================
*/
static void SV_NextDownload_f( client_t *cl )
{
	int block = atoi( Cmd_Argv(1) );

	if ( cl->state == CS_ACTIVE )
		return;

	if (block == cl->downloadClientBlock) {
		Com_DPrintf( "clientDownload: %d : client acknowledge of block %d\n", cl - svs.clients, block );

		// Find out if we are done.  A zero-length block indicates EOF
		if (cl->downloadBlockSize[cl->downloadClientBlock % MAX_DOWNLOAD_WINDOW] == 0) {
			Com_Printf( "clientDownload: %d : file \"%s\" completed\n", cl - svs.clients, cl->downloadName );
			SV_CloseDownload( cl );
			return;
		}

		cl->downloadSendTime = svs.time;
		cl->downloadClientBlock++;
		return;
	}
	// We aren't getting an acknowledge for the correct block, drop the client
	// FIXME: this is bad... the client will never parse the disconnect message
	//			because the cgame isn't loaded yet
	SV_DropClient( cl, "broken download" );
}

/*
==================
SV_BeginDownload_f
==================
*/
static void SV_BeginDownload_f( client_t *cl ) {
	if ( cl->state == CS_ACTIVE )
		return;

	// Kill any existing download
	SV_CloseDownload( cl );

	// cl->downloadName is non-zero now, SV_WriteDownloadToClient will see this and open
	// the file itself
	Q_strncpyz( cl->downloadName, Cmd_Argv(1), sizeof(cl->downloadName) );
}

/*
==================
SV_WriteDownloadToClient

Check to see if the client wants a file, open it if needed and start pumping the client
Fill up msg with data
==================
*/
void SV_WriteDownloadToClient(client_t *cl, msg_t *msg)
{
	int curindex;
	int rate;
	int blockspersnap;
	int unreferenced = 1;
	char errorMessage[1024];
	char pakbuf[MAX_QPATH], *pakptr;
	int numRefPaks;

	if (!*cl->downloadName)
		return;	// Nothing being downloaded

	if(!cl->download)
	{
		qboolean idPack = qfalse;
		qboolean missionPack = qfalse;

 		// Chop off filename extension.
		Com_sprintf(pakbuf, sizeof(pakbuf), "%s", cl->downloadName);
		pakptr = strrchr(pakbuf, '.');

		if(pakptr)
		{
			*pakptr = '\0';

			// Check for pk3 filename extension
			if(!Q_stricmp(pakptr + 1, "pk3"))
			{
				const char *referencedPaks = FS_ReferencedPakNames();

				// Check whether the file appears in the list of referenced
				// paks to prevent downloading of arbitrary files.
				Cmd_TokenizeStringIgnoreQuotes(referencedPaks);
				numRefPaks = Cmd_Argc();

				for(curindex = 0; curindex < numRefPaks; curindex++)
				{
					if(!FS_FilenameCompare(Cmd_Argv(curindex), pakbuf))
					{
						unreferenced = 0;

						// now that we know the file is referenced,
						// check whether it's legal to download it.
						missionPack = FS_idPak(pakbuf, "missionpack");
						idPack = missionPack;
						idPack = (qboolean)(idPack || FS_idPak(pakbuf, BASEGAME));

						break;
					}
				}
			}
		}

		cl->download = 0;

		// We open the file here
		if ( !sv_allowDownload->integer ||
			idPack || unreferenced ||
			( cl->downloadSize = FS_SV_FOpenFileRead( cl->downloadName, &cl->download ) ) < 0 ) {
			// cannot auto-download file
			if(unreferenced)
			{
				Com_Printf("clientDownload: %d : \"%s\" is not referenced and cannot be downloaded.\n", (int) (cl - svs.clients), cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" is not referenced and cannot be downloaded.", cl->downloadName);
			}
			else if (idPack) {
				Com_Printf("clientDownload: %d : \"%s\" cannot download id pk3 files\n", (int) (cl - svs.clients), cl->downloadName);
				if(missionPack)
				{
					Com_sprintf(errorMessage, sizeof(errorMessage), "Cannot autodownload Team Arena file \"%s\"\n"
									"The Team Arena mission pack can be found in your local game store.", cl->downloadName);
				}
				else
				{
					Com_sprintf(errorMessage, sizeof(errorMessage), "Cannot autodownload id pk3 file \"%s\"", cl->downloadName);
				}
			}
			else if ( !sv_allowDownload->integer ) {
				Com_Printf("clientDownload: %d : \"%s\" download disabled\n", (int) (cl - svs.clients), cl->downloadName);
				if (sv_pure->integer) {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
										"You will need to get this file elsewhere before you "
										"can connect to this pure server.\n", cl->downloadName);
				} else {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
                    "The server you are connecting to is not a pure server, "
                    "set autodownload to No in your settings and you might be "
                    "able to join the game anyway.\n", cl->downloadName);
				}
			} else {
        // NOTE TTimo this is NOT supposed to happen unless bug in our filesystem scheme?
        //   if the pk3 is referenced, it must have been found somewhere in the filesystem
				Com_Printf("clientDownload: %d : \"%s\" file not found on server\n", (int) (cl - svs.clients), cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" not found on server for autodownloading.\n", cl->downloadName);
			}
			MSG_WriteByte( msg, svc_download );
			MSG_WriteShort( msg, 0 ); // client is expecting block zero
			MSG_WriteLong( msg, -1 ); // illegal file size
			MSG_WriteString( msg, errorMessage );

			*cl->downloadName = 0;

			if(cl->download)
				FS_FCloseFile(cl->download);

			return;
		}

		Com_Printf( "clientDownload: %d : beginning \"%s\"\n", (int) (cl - svs.clients), cl->downloadName );

		// Init
		cl->downloadCurrentBlock = cl->downloadClientBlock = cl->downloadXmitBlock = 0;
		cl->downloadCount = 0;
		cl->downloadEOF = qfalse;
	}

	// Perform any reads that we need to
	while (cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW &&
		cl->downloadSize != cl->downloadCount) {

		curindex = (cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW);

		if (!cl->downloadBlocks[curindex])
			cl->downloadBlocks[curindex] = (unsigned char *)Z_Malloc( MAX_DOWNLOAD_BLKSIZE, TAG_DOWNLOAD, qtrue );

		cl->downloadBlockSize[curindex] = FS_Read( cl->downloadBlocks[curindex], MAX_DOWNLOAD_BLKSIZE, cl->download );

		if (cl->downloadBlockSize[curindex] < 0) {
			// EOF right now
			cl->downloadCount = cl->downloadSize;
			break;
		}

		cl->downloadCount += cl->downloadBlockSize[curindex];

		// Load in next block
		cl->downloadCurrentBlock++;
	}

	// Check to see if we have eof condition and add the EOF block
	if (cl->downloadCount == cl->downloadSize &&
		!cl->downloadEOF &&
		cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW) {

		cl->downloadBlockSize[cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW] = 0;
		cl->downloadCurrentBlock++;

		cl->downloadEOF = qtrue;  // We have added the EOF block
	}

	// Loop up to window size times based on how many blocks we can fit in the
	// client snapMsec and rate

	// based on the rate, how many bytes can we fit in the snapMsec time of the client
	// normal rate / snapshotMsec calculation
	rate = cl->rate;
	if ( sv_maxRate->integer ) {
		if ( sv_maxRate->integer < 1000 ) {
			Cvar_Set( "sv_MaxRate", "1000" );
		}
		if ( sv_maxRate->integer < rate ) {
			rate = sv_maxRate->integer;
		}
	}

	if (!rate) {
		blockspersnap = 1;
	} else {
		blockspersnap = ( (rate * cl->snapshotMsec) / 1000 + MAX_DOWNLOAD_BLKSIZE ) /
			MAX_DOWNLOAD_BLKSIZE;
	}

	if (blockspersnap < 0)
		blockspersnap = 1;

	while (blockspersnap--) {

		// Write out the next section of the file, if we have already reached our window,
		// automatically start retransmitting

		if (cl->downloadClientBlock == cl->downloadCurrentBlock)
			return; // Nothing to transmit

		if (cl->downloadXmitBlock == cl->downloadCurrentBlock) {
			// We have transmitted the complete window, should we start resending?

			//FIXME:  This uses a hardcoded one second timeout for lost blocks
			//the timeout should be based on client rate somehow
			if (svs.time - cl->downloadSendTime > 1000)
				cl->downloadXmitBlock = cl->downloadClientBlock;
			else
				return;
		}

		// Send current block
		curindex = (cl->downloadXmitBlock % MAX_DOWNLOAD_WINDOW);

		MSG_WriteByte( msg, svc_download );
		MSG_WriteShort( msg, cl->downloadXmitBlock );

		// block zero is special, contains file size
		if ( cl->downloadXmitBlock == 0 )
			MSG_WriteLong( msg, cl->downloadSize );

		MSG_WriteShort( msg, cl->downloadBlockSize[curindex] );

		// Write the block
		if ( cl->downloadBlockSize[curindex] ) {
			MSG_WriteData( msg, cl->downloadBlocks[curindex], cl->downloadBlockSize[curindex] );
		}

		Com_DPrintf( "clientDownload: %d : writing block %d\n", (int) (cl - svs.clients), cl->downloadXmitBlock );

		// Move on to the next block
		// It will get sent with next snap shot.  The rate will keep us in line.
		cl->downloadXmitBlock++;

		cl->downloadSendTime = svs.time;
	}
}

/*
=================
SV_Disconnect_f

The client is going to disconnect, so remove the connection immediately  FIXME: move to game?
=================
*/
const char *SV_GetStringEdString(char *refSection, char *refName);
static void SV_Disconnect_f( client_t *cl ) {
//	SV_DropClient( cl, "disconnected" );
	SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","DISCONNECTED") );
}

/*
=================
SV_VerifyPaks_f

If we are pure, disconnect the client if they do no meet the following conditions:

1. the first two checksums match our view of cgame and ui
2. there are no any additional checksums that we do not have

This routine would be a bit simpler with a goto but i abstained

=================
*/
static void SV_VerifyPaks_f( client_t *cl ) {
	int nChkSum1, nChkSum2, nClientPaks, nServerPaks, i, j, nCurArg;
	int nClientChkSum[1024];
	int nServerChkSum[1024];
	const char *pPaks, *pArg;
	qboolean bGood = qtrue;

	// if we are pure, we "expect" the client to load certain things from
	// certain pk3 files, namely we want the client to have loaded the
	// ui and cgame that we think should be loaded based on the pure setting
	//
	if ( sv_pure->integer != 0 ) {

		bGood = qtrue;
		nChkSum1 = nChkSum2 = 0;
		// we run the game, so determine which cgame and ui the client "should" be running
		//dlls are valid too now -rww
		bGood = (qboolean)(FS_FileIsInPAK("cgamex86.dll", &nChkSum1) == 1);

		if (bGood)
			bGood = (qboolean)(FS_FileIsInPAK("uix86.dll", &nChkSum2) == 1);

		nClientPaks = Cmd_Argc();

		// start at arg 1 ( skip cl_paks )
		nCurArg = 1;

		// we basically use this while loop to avoid using 'goto' :)
		while (bGood) {

			// must be at least 6: "cl_paks cgame ui @ firstref ... numChecksums"
			// numChecksums is encoded
			if (nClientPaks < 6) {
				bGood = qfalse;
				break;
			}
			// verify first to be the cgame checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum1 ) {
				bGood = qfalse;
				break;
			}
			// verify the second to be the ui checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum2 ) {
				bGood = qfalse;
				break;
			}
			// should be sitting at the delimeter now
			pArg = Cmd_Argv(nCurArg++);
			if (*pArg != '@') {
				bGood = qfalse;
				break;
			}
			// store checksums since tokenization is not re-entrant
			for (i = 0; nCurArg < nClientPaks; i++) {
				nClientChkSum[i] = atoi(Cmd_Argv(nCurArg++));
			}

			// store number to compare against (minus one cause the last is the number of checksums)
			nClientPaks = i - 1;

			// make sure none of the client check sums are the same
			// so the client can't send 5 the same checksums
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nClientPaks; j++) {
					if (i == j)
						continue;
					if (nClientChkSum[i] == nClientChkSum[j]) {
						bGood = qfalse;
						break;
					}
				}
				if (bGood == qfalse)
					break;
			}
			if (bGood == qfalse)
				break;

			// get the pure checksums of the pk3 files loaded by the server
			pPaks = FS_LoadedPakPureChecksums();
			Cmd_TokenizeString( pPaks );
			nServerPaks = Cmd_Argc();
			if (nServerPaks > 1024)
				nServerPaks = 1024;

			for (i = 0; i < nServerPaks; i++) {
				nServerChkSum[i] = atoi(Cmd_Argv(i));
			}

			// check if the client has provided any pure checksums of pk3 files not loaded by the server
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nServerPaks; j++) {
					if (nClientChkSum[i] == nServerChkSum[j]) {
						break;
					}
				}
				if (j >= nServerPaks) {
					bGood = qfalse;
					break;
				}
			}
			if ( bGood == qfalse ) {
				break;
			}

			// check if the number of checksums was correct
			nChkSum1 = sv.checksumFeed;
			for (i = 0; i < nClientPaks; i++) {
				nChkSum1 ^= nClientChkSum[i];
			}
			nChkSum1 ^= nClientPaks;
			if (nChkSum1 != nClientChkSum[nClientPaks]) {
				bGood = qfalse;
				break;
			}

			// break out
			break;
		}

		cl->gotCP = qtrue;

		if (bGood) {
			cl->pureAuthentic = 1;
		}
		else {
			cl->pureAuthentic = 0;
			cl->nextSnapshotTime = -1;
			cl->state = CS_ACTIVE;
			SV_SendClientSnapshot( cl );
			SV_DropClient( cl, "Unpure client detected. Invalid .PK3 files referenced!" );
		}
	}
}

/*
=================
SV_ResetPureClient_f
=================
*/
static void SV_ResetPureClient_f( client_t *cl ) {
	cl->pureAuthentic = 0;
	cl->gotCP = qfalse;
}

/*
=================
SV_UserinfoChanged

Pull specific info from a newly changed userinfo string
into a more C friendly form.
=================
*/
void SV_UserinfoChanged( client_t *cl ) {
	char	*val=NULL, *ip=NULL;
	int		i=0, len=0;

	// name for C code
	Q_strncpyz( cl->name, Info_ValueForKey (cl->userinfo, "name"), sizeof(cl->name) );

	// rate command

	// if the client is on the same subnet as the server and we aren't running an
	// internet public server, assume they don't need a rate choke
	if ( Sys_IsLANAddress( cl->netchan.remoteAddress ) && com_dedicated->integer != 2 && sv_lanForceRate->integer == 1 ) {
		cl->rate = 100000;	// lans should not rate limit
	} else {
		val = Info_ValueForKey (cl->userinfo, "rate");
		if (sv_ratePolicy->integer == 1)
		{
			// NOTE: what if server sets some dumb sv_clientRate value?
			cl->rate = sv_clientRate->integer;
		}
		else if( sv_ratePolicy->integer == 2)
		{
			i = atoi(val);
			if (!i) {
				i = sv_maxRate->integer; //FIXME old code was 3000 here, should increase to 5000 instead or maxRate?
			}
			i = Com_Clampi(1000, 100000, i);
			i = Com_Clampi( sv_minRate->integer, sv_maxRate->integer, i );
			if (i != cl->rate) {
				cl->rate = i;
			}
		}
	}

	// snaps command
	//Note: cl->snapshotMsec is also validated in sv_main.cpp -> SV_CheckCvars if sv_fps, sv_snapsMin or sv_snapsMax is changed
	int minSnaps = Com_Clampi(1, sv_snapsMax->integer, sv_snapsMin->integer); // between 1 and sv_snapsMax ( 1 <-> 40 )
	int maxSnaps = Q_min(sv_fps->integer, sv_snapsMax->integer); // can't produce more than sv_fps snapshots/sec, but can send less than sv_fps snapshots/sec
	val = Info_ValueForKey(cl->userinfo, "snaps");
	cl->wishSnaps = atoi(val);
	if (!cl->wishSnaps)
		cl->wishSnaps = maxSnaps;
	if (sv_snapsPolicy->integer == 1)
	{
		cl->wishSnaps = sv_fps->integer;
		i = 1000 / sv_fps->integer;
		if (i != cl->snapshotMsec) {
			// Reset next snapshot so we avoid desync between server frame time and snapshot send time
			cl->nextSnapshotTime = -1;
			cl->snapshotMsec = i;
		}
	}
	else if (sv_snapsPolicy->integer == 2)
	{
		i = 1000 / Com_Clampi(minSnaps, maxSnaps, cl->wishSnaps);
		if (i != cl->snapshotMsec) {
			// Reset next snapshot so we avoid desync between server frame time and snapshot send time
			cl->nextSnapshotTime = -1;
			cl->snapshotMsec = i;
		}
	}

	// TTimo
	// maintain the IP information
	// the banning code relies on this being consistently present
	if( NET_IsLocalAddress(cl->netchan.remoteAddress) )
		ip = "localhost";
	else
		ip = (char*)NET_AdrToString( cl->netchan.remoteAddress );

	val = Info_ValueForKey( cl->userinfo, "ip" );
	if( val[0] )
		len = strlen( ip ) - strlen( val ) + strlen( cl->userinfo );
	else
		len = strlen( ip ) + 4 + strlen( cl->userinfo );

	if( len >= MAX_INFO_STRING )
		SV_DropClient( cl, "userinfo string length exceeded" );
	else
		Info_SetValueForKey( cl->userinfo, "ip", ip );
}

#define INFO_CHANGE_MIN_INTERVAL	6000 //6 seconds is reasonable I suppose
#define INFO_CHANGE_MAX_COUNT		3 //only allow 3 changes within the 6 seconds

/*
==================
SV_UpdateUserinfo_f
==================
*/
static void SV_UpdateUserinfo_f( client_t *cl ) {
	char *arg = Cmd_Argv(1);

	// Stop random empty /userinfo calls without hurting anything
	if( !arg || !*arg )
		return;

	Q_strncpyz( cl->userinfo, arg, sizeof(cl->userinfo) );

#ifdef FINAL_BUILD
	if (cl->lastUserInfoChange > svs.time)
	{
		cl->lastUserInfoCount++;

		if (cl->lastUserInfoCount >= INFO_CHANGE_MAX_COUNT)
		{
		//	SV_SendServerCommand(cl, "print \"Warning: Too many info changes, last info ignored\n\"\n");
			SV_SendServerCommand(cl, "print \"@@@TOO_MANY_INFO\n\"\n");
			return;
		}
	}
	else
#endif
	{
		cl->lastUserInfoCount = 0;
		cl->lastUserInfoChange = svs.time + INFO_CHANGE_MIN_INTERVAL;
	}

	SV_UserinfoChanged( cl );
	// call prog code to allow overrides
	GVM_ClientUserinfoChanged( cl - svs.clients );

}

typedef struct ucmd_s {
	const char	*name;
	void	(*func)( client_t *cl );
} ucmd_t;

static ucmd_t ucmds[] = {
	{"userinfo", SV_UpdateUserinfo_f},
	{"disconnect", SV_Disconnect_f},
	{"cp", SV_VerifyPaks_f},
	{"vdr", SV_ResetPureClient_f},
	{"download", SV_BeginDownload_f},
	{"nextdl", SV_NextDownload_f},
	{"stopdl", SV_StopDownload_f},
	{"donedl", SV_DoneDownload_f},

	{NULL, NULL}
};

// --- Economy shop catalog (backed by the "spin" win system) ---------------
//
// Every purchasable item (other than the plain ammo refill) is granted via
// SV_SpinForceGiveWin(), which reuses the exact same per-win granting logic
// as the "!spin" chat command and the "spinwin" rcon command (weapon/ammo
// assignment, holdable flags, size changes, etc.) so nothing has to be
// reimplemented here. Vehicles, NPC spawns, and the debug-only
// WIN_ALL_SKILLS entry are intentionally not offered for purchase.
//
// Each item's cost is a live server cvar ("g_shopCost_<name>"); a cost of 0
// disables that item. See SV_EconomyShopInitCvars() below.

typedef struct economyItemDef_s {
	const char *name;       // purchase name: "!buy <name>" (matches spinwin names)
	const char *category;   // menu grouping: "!buy <category>" to browse
	int         winIndex;   // spin_wins_t value, or -1 for the ammo refill special-case
	int         costDefault;
} economyItemDef_t;

static const economyItemDef_t svEconomyItemDefs[] = {
	// Pistols & Light Sidearms
	{ "bryar",           "pistols",   WIN_BRYAR,            8 },
	{ "clone_pistol",    "pistols",   WIN_CLONE_PISTOL,     8 },
	{ "mando_pistol",    "pistols",   WIN_MANDO_PISTOL,    10 },
	{ "heavy_pistol",    "pistols",   WIN_HEAVY_PISTOL,    10 },
	{ "bryar_old",       "pistols",   WIN_BRYAR_OLD,        8 },
	{ "ee3",             "pistols",   WIN_EE3,             10 },
	// Blasters & Carbines
	{ "blaster",         "rifles",    WIN_BLASTER,         12 },
	{ "dc_carbine",      "rifles",    WIN_DC_CARBINE,      15 },
	{ "cr2",             "rifles",    WIN_CR2,             15 },
	{ "e22",             "rifles",    WIN_E22,             15 },
	{ "dlt19",           "rifles",    WIN_DLT19,           18 },
	{ "trad_bowcaster",  "rifles",    WIN_TRAD_BOWCASTER,  15 },
	{ "disruptor",       "rifles",    WIN_DISRUPTOR,       22 },
	{ "bowcaster",       "rifles",    WIN_BOWCASTER,       20 },
	{ "repeater",        "rifles",    WIN_REPEATER,        20 },
	{ "clone_rifle",     "rifles",    WIN_CLONE_RIFLE,     18 },
	{ "a280",            "rifles",    WIN_A280,            18 },
	{ "dlt20a",          "rifles",    WIN_DLT20A,          18 },
	{ "m5",              "rifles",    WIN_M5,              18 },
	{ "t21",             "rifles",    WIN_T21,             15 },
	{ "ee4",             "rifles",    WIN_EE4,             18 },
	{ "amban",           "rifles",    WIN_AMBAN,           25 },
	{ "proj",            "rifles",    WIN_PROJ,            22 },
	{ "sbd",             "rifles",    WIN_SBD,             20 },
	// Special Weapons
	{ "demp2",           "special",   WIN_DEMP2,           25 },
	{ "flechette",       "special",   WIN_FLECHETTE,       22 },
	{ "concussion",      "special",   WIN_CONCUSSION,      22 },
	{ "thrower",         "special",   WIN_THROWER,         20 },
	{ "minigun",         "special",   WIN_MINIGUN,         30 },
	{ "shotgun",         "special",   WIN_SHOTGUN,         18 },
	// Heavy Launchers
	{ "rocket_launcher", "launchers", WIN_ROCKET_LAUNCHER, 35 },
	{ "plx1",            "launchers", WIN_PLX1,            35 },
	// Grenades & Explosives
	{ "frag_nade",       "nades",     WIN_FRAG_NADE,        8 },
	{ "pulse_nade",      "nades",     WIN_PULSE_NADE,       8 },
	{ "thermal",         "nades",     WIN_THERMAL,         10 },
	{ "real_td",         "nades",     WIN_REAL_TD,         10 },
	{ "fire_nade",       "nades",     WIN_FIRE_NADE,       10 },
	{ "sonic_nade",      "nades",     WIN_SONIC_NADE,      10 },
	{ "cryo_nade",       "nades",     WIN_CRYO_NADE,       10 },
	{ "conc_nade",       "nades",     WIN_CONC_NADE,       10 },
	{ "trip_mine",       "nades",     WIN_TRIP_MINE,       12 },
	{ "det_pack",        "nades",     WIN_DET_PACK,        15 },
	// Melee
	{ "saber",           "melee",     WIN_SABER,           30 },
	// Equipment
	{ "100_armor",       "gadgets",   WIN_100_ARMOR,       10 },
	{ "250_armor",       "gadgets",   WIN_250_ARMOR,       20 },
	{ "cloak",           "gadgets",   WIN_CLOAK,           20 },
	{ "eweb",            "gadgets",   WIN_EWEB,            25 },
	{ "sentry",          "gadgets",   WIN_SENTRY,          15 },
	{ "seeker",          "gadgets",   WIN_SEEKER,          10 },
	{ "bacta",           "gadgets",   WIN_BACTA,            5 },
	{ "forcefield",      "gadgets",   WIN_FORCEFIELD,      20 },
	{ "spawner",         "gadgets",   WIN_SPAWNER,         25 },
	{ "stimpack",        "gadgets",   WIN_STIMPACK,         8 },
	{ "jetpack",         "gadgets",   WIN_JETPACK,         22 },
	{ "shockfield",      "gadgets",   WIN_SHOCKFIELD,      20 },
	{ "protocol",        "gadgets",   WIN_PROTOCOL,        15 },
	// Fun / Size
	{ "size_xs",         "size",      WIN_SIZE_XS,         10 },
	{ "size_s",          "size",      WIN_SIZE_S,           8 },
	{ "size_l",          "size",      WIN_SIZE_L,          12 },
	{ "size_xl",         "size",      WIN_SIZE_XL,         18 },
	// Ammo refill (not a spin win — handled directly)
	{ "ammo",            "ammo",      -1,                   6 },
};

// Top-level category list shown by "!buy" with no arguments.
static const char *svEconomyShopCategories[] = {
	"pistols", "rifles", "special", "launchers", "nades", "melee", "gadgets", "size", "ammo"
};

// One cvar per item: "g_shopCost_<name>". A cost of 0 disables that item.
// Registered once at server startup by SV_EconomyShopInitCvars(); admins can
// change any of them live via rcon ("set g_shopCost_bryar 0") with no reload
// step needed since the cvar's current value is read at purchase time.
static cvar_t *svEconomyItemCostCvars[ARRAY_LEN( svEconomyItemDefs )];

void SV_EconomyShopInitCvars( void ) {
	int i;
	for ( i = 0; i < (int)ARRAY_LEN( svEconomyItemDefs ); i++ ) {
		char cvarName[64];
		char defaultStr[16];
		char desc[128];

		Com_sprintf( cvarName, sizeof( cvarName ), "g_shopCost_%s", svEconomyItemDefs[i].name );
		Com_sprintf( defaultStr, sizeof( defaultStr ), "%d", svEconomyItemDefs[i].costDefault );
		Com_sprintf( desc, sizeof( desc ), "Shop cost in credits for '%s' (0 = disabled)", svEconomyItemDefs[i].name );
		svEconomyItemCostCvars[i] = Cvar_Get( cvarName, defaultStr, CVAR_ARCHIVE, desc );
	}
}

static int SV_EconomyItemCost( int index ) {
	return svEconomyItemCostCvars[index] ? svEconomyItemCostCvars[index]->integer : 0;
}

static qboolean SV_EconomyItemEnabled( int index ) {
	return SV_EconomyItemCost( index ) > 0 ? qtrue : qfalse;
}

// A category only "exists" (for menu/lookup purposes) if it has at least one
// item whose cost cvar is currently > 0. Fully-disabled categories are hidden
// from "!buy" and treated the same as an unknown category if typed directly.
static qboolean SV_EconomyCategoryExists( const char *category ) {
	int i;
	for ( i = 0; i < (int)ARRAY_LEN( svEconomyItemDefs ); i++ ) {
		if ( !Q_stricmp( category, svEconomyItemDefs[i].category ) && SV_EconomyItemEnabled( i ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

static int SV_EconomyFindItemByName( const char *name ) {
	int i;
	for ( i = 0; i < (int)ARRAY_LEN( svEconomyItemDefs ); i++ ) {
		if ( !Q_stricmp( name, svEconomyItemDefs[i].name ) ) {
			return i;
		}
	}
	return -1;
}

// --- Persistent economy accounts (!register / !login) ---------------------
//
// Shared by every instance (see SV_EconomyAccountsPath: fs_basepath/fs_game
// is the same "/opt/openjk/MBII" for all of them, unlike fs_homepath) so an
// account and its credits carry over between servers. Since each instance
// is a genuinely separate OS process, this can't just be an in-memory cache
// loaded once like everything else here used to be - SV_EconomyAccountsLoad
// is called fresh immediately before every read AND every write (see
// SV_EconomyAccountsEnsureLoaded), and both Load and Save take a real
// cross-process flock() around their I/O, so a read never sees another
// process's write half-finished and two writes never interleave.
//
// Residual limitation: a reload and its matching save are each atomic on
// their own, but not combined into one lock spanning both. Two different
// people registering/logging in/earning credits on two different instances
// within the same instant could still have the later write overwrite the
// earlier one - narrow (needs two real people acting within a fraction of a
// second, on two different servers) and never corrupts the file or crashes
// anything, just a rare last-write-wins for that one instant. Closing that
// gap fully would mean holding one lock across an entire multi-step chat
// command - PIN hashing, RNG, validation branches with early returns - and
// a lock left held by a stuck/crashed step is a worse failure mode than the
// gap it would close.
#define ECONOMY_ACCOUNTS_FILE		"economy_accounts.dat"
#define ECONOMY_MAX_ACCOUNTS		1024
#define ECONOMY_HANDLE_SIZE			24	// must match client_t::economyHandle
#define ECONOMY_PIN_LEN				4
#define ECONOMY_SALT_SIZE			16
#define ECONOMY_HASH_SIZE			MD5_DIGEST_SIZE
#define ECONOMY_LOGIN_MAX_ATTEMPTS	5
#define ECONOMY_LOGIN_LOCKOUT_MS	60000
#define ECONOMY_LOGIN_LOCKOUT_MAX_SECS	3600

// Paced multi-line menu delivery for "!buy" - see SV_EconomyMenuBegin/
// AddLine/Pump below. Must match client_t::economyMenuLines in server.h.
// 28 lines comfortably covers the largest category (rifles, 18 items) plus
// a header and footer line.
#define ECONOMY_MENU_LINE_DELAY_MS	700

typedef struct economyAccount_s {
	char		handle[ECONOMY_HANDLE_SIZE];
	byte		salt[ECONOMY_SALT_SIZE];
	byte		hash[ECONOMY_HASH_SIZE];	// HMAC-MD5(key=salt, msg=pin)
	int			credits;
	int			failedAttempts;
	int			lockoutUntil;				// svs.time value; login rejected while svs.time < lockoutUntil
} economyAccount_t;

static economyAccount_t svEconomyAccounts[ECONOMY_MAX_ACCOUNTS];
static int svEconomyAccountCount = 0;

static void SV_EconomyBytesToHex( const byte *in, int inLen, char *out ) {
	static const char *hexd = "0123456789abcdef";
	int i;
	for ( i = 0; i < inLen; i++ ) {
		out[i * 2] = hexd[in[i] >> 4];
		out[i * 2 + 1] = hexd[in[i] & 0xF];
	}
	out[inLen * 2] = '\0';
}

static void SV_EconomyHexToBytes( const char *hex, byte *out, int outLen ) {
	int i;
	for ( i = 0; i < outLen; i++ ) {
		char byteStr[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
		out[i] = (byte)strtoul( byteStr, NULL, 16 );
	}
}

// fs_basepath and fs_game are identical across every instance (all of them
// point at the same /opt/openjk MBII install; only fs_homepath differs per
// instance), so this resolves to the same absolute file for all of them -
// unlike ECONOMY_ACCOUNTS_FILE's old location under the engine's per-
// instance FS_SV_ save path.
// --- Daily login bonus ------------------------------------------------------
//
// The first !login in any 24 hours, on any instance, pays g_economyDailyBonus.
// Shared by every instance the same way as the accounts file (fs_basepath,
// not fs_homepath) but in its own file - one "handle unixtime" line per
// account - since MBIIEZ's Economy page reads economy_accounts.dat expecting
// exactly six fields a line. The whole check-and-record happens under one
// flock(), so logging in on two servers at once can't pay twice.
#define ECONOMY_DAILY_FILE		"economy_daily.dat"
#define ECONOMY_DAILY_SECS		( 24 * 60 * 60 )
#define ECONOMY_DAILY_MAX		ECONOMY_MAX_ACCOUNTS

typedef struct {
	char handle[24];
	long claimed;
} economyDaily_t;

// Claims today's bonus for handle (or, with recordOnly, just marks today as
// claimed). Returns seconds until the next one is due if it's too soon,
// else 0 - with *paid set when a bonus should actually be paid.
static int SV_EconomyDailyClaim( const char *handle, qboolean recordOnly, qboolean *paid ) {
	static economyDaily_t entries[ECONOMY_DAILY_MAX];
	char path[MAX_OSPATH];
	char line[128];
	int count = 0, found = -1, wait = 0, fd, i;
	const long now = (long)time( NULL );
	FILE *f;

	*paid = qfalse;
	Com_sprintf( path, sizeof( path ), "%s/%s/%s",
		Cvar_VariableString( "fs_basepath" ), Cvar_VariableString( "fs_game" ), ECONOMY_DAILY_FILE );

	fd = open( path, O_RDWR | O_CREAT, 0600 );
	if ( fd < 0 ) {
		Com_Printf( "SV_EconomyDailyClaim: failed to open %s\n", path );
		return 0;
	}
	if ( flock( fd, LOCK_EX ) != 0 || !( f = fdopen( fd, "r+" ) ) ) {
		close( fd );
		return 0;
	}

	while ( count < ECONOMY_DAILY_MAX && fgets( line, sizeof( line ), f ) ) {
		if ( sscanf( line, "%23s %ld", entries[count].handle, &entries[count].claimed ) == 2 ) {
			if ( !Q_stricmp( entries[count].handle, handle ) ) {
				found = count;
			}
			count++;
		}
	}

	if ( found >= 0 && now - entries[found].claimed < ECONOMY_DAILY_SECS ) {
		wait = (int)( ECONOMY_DAILY_SECS - ( now - entries[found].claimed ) );
	} else {
		if ( found < 0 && count < ECONOMY_DAILY_MAX ) {
			found = count++;
			Q_strncpyz( entries[found].handle, handle, sizeof( entries[found].handle ) );
		}
		if ( found >= 0 ) {
			entries[found].claimed = now;
			*paid = recordOnly ? qfalse : qtrue;
			rewind( f );
			if ( ftruncate( fd, 0 ) != 0 ) { /* best-effort; nothing else to do here */ }
			for ( i = 0; i < count; i++ ) {
				fprintf( f, "%s %ld\n", entries[i].handle, entries[i].claimed );
			}
			fflush( f );
		}
	}

	fclose( f ); // also releases the lock
	return wait;
}

static void SV_EconomyAccountsPath( char *out, int outSize ) {
	Com_sprintf( out, outSize, "%s/%s/%s",
		Cvar_VariableString( "fs_basepath" ), Cvar_VariableString( "fs_game" ), ECONOMY_ACCOUNTS_FILE );
}

// --- Account transactions ---------------------------------------------------
//
// Every server process shares economy_accounts.dat, and the web panel writes
// it too. Loading under a shared lock and saving under a separate exclusive
// one left a gap between the two where another process's change could land
// and then be overwritten by our whole-file save - a lost registration, or
// credits. Anything that changes an account instead runs between
// SV_EconomyBegin and SV_EconomyEnd: one exclusive flock() held from the
// re-read to the write, which Load and Save use instead of locking for
// themselves. Nests, so a helper that opens its own transaction can be
// called from inside another.
static int svEconomyTxnFd = -1;
static int svEconomyTxnDepth = 0;

static void SV_EconomyBegin( void ) {
	char filepath[MAX_OSPATH];

	if ( svEconomyTxnDepth++ > 0 ) {
		return;
	}
	SV_EconomyAccountsPath( filepath, sizeof( filepath ) );
	svEconomyTxnFd = open( filepath, O_RDWR | O_CREAT, 0600 );
	if ( svEconomyTxnFd < 0 ) {
		Com_Printf( "Economy: failed to open %s\n", filepath );
	} else if ( flock( svEconomyTxnFd, LOCK_EX ) != 0 ) {
		Com_Printf( "Economy: failed to lock %s\n", filepath );
		close( svEconomyTxnFd );
		svEconomyTxnFd = -1;
	}
	// On failure Load and Save fall back to locking on their own.
}

static void SV_EconomyEnd( void ) {
	if ( svEconomyTxnDepth <= 0 || --svEconomyTxnDepth > 0 ) {
		return;
	}
	if ( svEconomyTxnFd >= 0 ) {
		flock( svEconomyTxnFd, LOCK_UN );
		close( svEconomyTxnFd );
		svEconomyTxnFd = -1;
	}
}

static void SV_EconomyAccountsLoad( void ) {
	char filepath[MAX_OSPATH];
	int fd;
	off_t filelen;
	char *buf, *line, *nextline;
	const qboolean inTxn = ( svEconomyTxnFd >= 0 ) ? qtrue : qfalse;

	svEconomyAccountCount = 0;

	if ( inTxn ) {
		fd = svEconomyTxnFd;
	} else {
		SV_EconomyAccountsPath( filepath, sizeof( filepath ) );

		fd = open( filepath, O_RDONLY );
		if ( fd < 0 ) {
			return;	// doesn't exist yet - nobody's registered anywhere yet
		}

		if ( flock( fd, LOCK_SH ) != 0 ) {
			Com_Printf( "Economy: failed to lock %s for reading\n", filepath );
			close( fd );
			return;
		}
	}

	filelen = lseek( fd, 0, SEEK_END );
	lseek( fd, 0, SEEK_SET );

	if ( filelen <= 0 ) {
		if ( !inTxn ) {
			flock( fd, LOCK_UN );
			close( fd );
		}
		return;
	}

	buf = (char *)Z_Malloc( (int)filelen + 1, TAG_TEMP_WORKSPACE );
	filelen = read( fd, buf, (size_t)filelen );
	if ( !inTxn ) {
		flock( fd, LOCK_UN );
		close( fd );
	}

	if ( filelen <= 0 ) {
		Z_Free( buf );
		return;
	}
	buf[filelen] = '\0';

	line = buf;
	while ( line && *line && svEconomyAccountCount < ECONOMY_MAX_ACCOUNTS ) {
		char handleBuf[ECONOMY_HANDLE_SIZE];
		char saltHex[ECONOMY_SALT_SIZE * 2 + 1];
		char hashHex[ECONOMY_HASH_SIZE * 2 + 1];
		int credits, failedAttempts, lockoutUntil;

		nextline = strchr( line, '\n' );
		if ( nextline ) {
			*nextline = '\0';
		}

		if ( sscanf( line, "%23s %32s %32s %d %d %d",
				handleBuf, saltHex, hashHex, &credits, &failedAttempts, &lockoutUntil ) == 6 ) {
			economyAccount_t *acct = &svEconomyAccounts[svEconomyAccountCount++];
			Com_Memset( acct, 0, sizeof( *acct ) );
			Q_strncpyz( acct->handle, handleBuf, sizeof( acct->handle ) );
			SV_EconomyHexToBytes( saltHex, acct->salt, ECONOMY_SALT_SIZE );
			SV_EconomyHexToBytes( hashHex, acct->hash, ECONOMY_HASH_SIZE );
			acct->credits = credits;
			acct->failedAttempts = failedAttempts;
			acct->lockoutUntil = lockoutUntil;
		}

		line = nextline ? nextline + 1 : NULL;
	}

	Z_Free( buf );
}

// Always re-reads rather than caching: this file can be written by up to
// five OTHER server processes between one call and the next, so a load-once
// cache (what this used to be, back when the file was per-instance and
// nothing else could ever write it) would silently go stale the moment any
// other instance's player registered, logged in, or earned a credit.
static void SV_EconomyAccountsEnsureLoaded( void ) {
	SV_EconomyAccountsLoad();
}

static void SV_EconomyAccountsSave( void ) {
	char filepath[MAX_OSPATH];
	int fd;
	int i;
	const qboolean inTxn = ( svEconomyTxnFd >= 0 ) ? qtrue : qfalse;

	if ( inTxn ) {
		fd = svEconomyTxnFd;
	} else {
		SV_EconomyAccountsPath( filepath, sizeof( filepath ) );

		fd = open( filepath, O_WRONLY | O_CREAT, 0600 );
		if ( fd < 0 ) {
			Com_Printf( "SV_EconomyAccountsSave: failed to open %s for writing\n", filepath );
			return;
		}

		if ( flock( fd, LOCK_EX ) != 0 ) {
			Com_Printf( "SV_EconomyAccountsSave: failed to lock %s for writing\n", filepath );
			close( fd );
			return;
		}
	}

	// Truncate under the lock rather than via O_TRUNC on open, so a
	// concurrent LOCK_SH reader (SV_EconomyAccountsLoad, from any of the
	// other server processes) can never observe a momentarily-empty file.
	if ( ftruncate( fd, 0 ) != 0 ) { /* best-effort; nothing else to do here */ }
	lseek( fd, 0, SEEK_SET );

	for ( i = 0; i < svEconomyAccountCount; i++ ) {
		economyAccount_t *acct = &svEconomyAccounts[i];
		char saltHex[ECONOMY_SALT_SIZE * 2 + 1];
		char hashHex[ECONOMY_HASH_SIZE * 2 + 1];
		char line[256];
		int len;

		SV_EconomyBytesToHex( acct->salt, ECONOMY_SALT_SIZE, saltHex );
		SV_EconomyBytesToHex( acct->hash, ECONOMY_HASH_SIZE, hashHex );

		len = Com_sprintf( line, sizeof( line ), "%s %s %s %d %d %d\n",
			acct->handle, saltHex, hashHex, acct->credits, acct->failedAttempts, acct->lockoutUntil );
		if ( write( fd, line, (size_t)len ) != len ) { /* best-effort; nothing else to do here */ }
	}

	if ( !inTxn ) {
		flock( fd, LOCK_UN );
		close( fd );
	}
}

static economyAccount_t *SV_EconomyFindAccount( const char *handle ) {
	int i;

	SV_EconomyAccountsEnsureLoaded();

	for ( i = 0; i < svEconomyAccountCount; i++ ) {
		if ( !Q_stricmp( svEconomyAccounts[i].handle, handle ) ) {
			return &svEconomyAccounts[i];
		}
	}
	return NULL;
}

static qboolean SV_EconomyValidateHandle( const char *handle ) {
	int len = (int)strlen( handle );
	int i;

	if ( len < 3 || len >= ECONOMY_HANDLE_SIZE ) {
		return qfalse;
	}
	for ( i = 0; i < len; i++ ) {
		if ( !isalnum( (unsigned char)handle[i] ) && handle[i] != '_' ) {
			return qfalse;
		}
	}
	return qtrue;
}

static qboolean SV_EconomyValidatePin( const char *pin ) {
	int i;

	if ( (int)strlen( pin ) != ECONOMY_PIN_LEN ) {
		return qfalse;
	}
	for ( i = 0; i < ECONOMY_PIN_LEN; i++ ) {
		if ( !isdigit( (unsigned char)pin[i] ) ) {
			return qfalse;
		}
	}
	return qtrue;
}

static void SV_EconomyHashPin( const byte *salt, const char *pin, byte *outHash ) {
	hmacMD5Context_t ctx;
	HMAC_MD5_Init( &ctx, salt, ECONOMY_SALT_SIZE );
	HMAC_MD5_Update( &ctx, (const byte *)pin, (unsigned int)strlen( pin ) );
	HMAC_MD5_Final( &ctx, outHash );
}

// Constant-time comparison to avoid leaking hash-match progress via timing.
static qboolean SV_EconomySecureCompare( const byte *a, const byte *b, int len ) {
	byte diff = 0;
	int i;
	for ( i = 0; i < len; i++ ) {
		diff |= (byte)( a[i] ^ b[i] );
	}
	return diff == 0 ? qtrue : qfalse;
}

// Writes a logged-in client's current credits back to their persisted account.
// Safe to call for clients that aren't logged into an account (no-op).
// The stored balance can change under a logged-in session: the web panel's
// Give Credits edits economy_accounts.dat directly, and it's shared by
// every instance. Anything that moved it since this session last read or
// wrote it is folded into the in-memory balance here, instead of being
// overwritten the next time the session saves. acct must be freshly
// loaded (SV_EconomyFindAccount reloads the file).
static void SV_EconomyMergeExternal( client_t *cl, economyAccount_t *acct ) {
	const int delta = acct->credits - cl->economyCreditsSynced;

	if ( !delta ) {
		return;
	}
	cl->economyCredits += delta;
	cl->economyCreditsSynced = acct->credits;
	SV_EconomyPrint( cl, va( "Your balance was %s by %d credits. Balance: %d",
		delta > 0 ? "topped up" : "reduced", delta > 0 ? delta : -delta, cl->economyCredits ) );
}

// Picks up any outside change to a logged-in session's balance.
static void SV_EconomySyncCredits( client_t *cl ) {
	economyAccount_t *acct;

	if ( !cl->economyHandle[0] ) {
		return;
	}
	acct = SV_EconomyFindAccount( cl->economyHandle );
	if ( acct ) {
		SV_EconomyMergeExternal( cl, acct );
	}
}

// Lower-case letters and digits only: colours, spaces and clan-tag
// punctuation dropped, so "CA[212]CE-Ricks" becomes "ca212cericks".
static void SV_FuzzyNormalize( const char *in, char *out, int outSize ) {
	char clean[MAX_STRING_CHARS];
	int n = 0;

	Q_strncpyz( clean, in, sizeof( clean ) );
	Q_CleanStr( clean );
	for ( const char *c = clean; *c && n < outSize - 1; c++ ) {
		if ( isalnum( (unsigned char)*c ) ) {
			out[n++] = tolower( (unsigned char)*c );
		}
	}
	out[n] = '\0';
}

// How well what someone typed matches a name, ignoring case, colours and
// punctuation: 4 exact, 3 start of the name, 2 anywhere in it, 1 its letters
// in order with gaps ("cdy" for Cody), 0 no match.
int SV_FuzzyNameScore( const char *name, const char *query ) {
	char n[MAX_STRING_CHARS], q[MAX_STRING_CHARS];
	const char *hit;
	int k = 0;

	SV_FuzzyNormalize( name, n, sizeof( n ) );
	SV_FuzzyNormalize( query, q, sizeof( q ) );
	if ( !q[0] || !n[0] ) {
		return 0;
	}
	if ( !strcmp( n, q ) ) {
		return 4;
	}
	hit = strstr( n, q );
	if ( hit ) {
		return ( hit == n ) ? 3 : 2;
	}
	for ( const char *c = n; *c && q[k]; c++ ) {
		if ( *c == q[k] ) {
			k++;
		}
	}
	return q[k] ? 0 : 1;
}

// Finds the player a chat command means: a slot number, or the best fuzzy
// match on name (SV_FuzzyNameScore). A tie for the best match lists them
// for the asker and finds nobody; no match says so.
client_t *SV_EconomyFindPlayer( client_t *asker, const char *query ) {
	client_t *found = NULL;
	char names[512] = "";
	int best = 0, ties = 0;
	int i;
	const char *p = query;

	while ( *p >= '0' && *p <= '9' ) {
		p++;
	}
	if ( p != query && !*p ) {
		i = atoi( query );
		if ( i >= 0 && i < sv_maxclients->integer && svs.clients[i].state == CS_ACTIVE ) {
			return &svs.clients[i];
		}
	}

	for ( i = 0; i < sv_maxclients->integer; i++ ) {
		client_t *c = &svs.clients[i];
		int score;

		if ( c->state != CS_ACTIVE ) {
			continue;
		}
		score = SV_FuzzyNameScore( c->name, query );
		if ( !score || score < best ) {
			continue;
		}
		if ( score > best ) {
			best = score;
			ties = 0;
			names[0] = '\0';
		}
		ties++;
		found = c;
		if ( ties <= 5 ) {
			Q_strcat( names, sizeof( names ), va( "%s%s^7", ties > 1 ? ", " : "", c->name ) );
		}
	}

	if ( ties == 1 ) {
		return found;
	}
	if ( ties > 1 ) {
		SV_EconomyPrint( asker, va( "\"%s\" matches %d players: %s%s - type more of the name.",
			query, ties, names, ties > 5 ? ", ..." : "" ) );
	} else {
		SV_EconomyPrint( asker, va( "No player found matching \"%s\".", query ) );
	}
	return NULL;
}

// Adds to an account's stored balance directly (raffle winnings, Pazaak
// payouts to someone who's left). A logged-in session picks the change up
// through SV_EconomyMergeExternal like any other outside change.
qboolean SV_EconomyAddCreditsToAccount( const char *handle, int amount ) {
	economyAccount_t *acct;

	SV_EconomyBegin();
	acct = SV_EconomyFindAccount( handle );
	if ( acct ) {
		acct->credits += amount;
		SV_EconomyAccountsSave();
	}
	SV_EconomyEnd();
	return acct ? qtrue : qfalse;
}

void SV_EconomyPersistCredits( client_t *cl ) {
	economyAccount_t *acct;

	if ( !cl->economyHandle[0] ) {
		return;
	}

	SV_EconomyBegin();
	acct = SV_EconomyFindAccount( cl->economyHandle );
	if ( acct ) {
		SV_EconomyMergeExternal( cl, acct );
		acct->credits = cl->economyCredits;
		cl->economyCreditsSynced = acct->credits;
		SV_EconomyAccountsSave();
	}
	SV_EconomyEnd();
}

// Clears a bounty that's been collected (or refunded).
static void SV_EconomyBountyClear( client_t *target ) {
	target->economyBounty = 0;
	target->economyBountyPlacerName[0] = '\0';
	Com_Memset( target->economyBountyStakes, 0, sizeof( target->economyBountyStakes ) );
}

// Gives every backer of target's bounty their share back: straight into the
// balance of a backer on this server, else into their account (which their
// session on another server picks up by itself).
void SV_EconomyBountyRefund( client_t *target, const char *why ) {
	int total = 0;

	if ( target->economyBounty <= 0 ) {
		return;
	}

	for ( int s = 0; s < ECONOMY_BOUNTY_BACKERS; s++ ) {
		const bountyStake_t *stake = &target->economyBountyStakes[s];
		client_t *backer = NULL;

		if ( !stake->handle[0] || stake->amount <= 0 ) {
			continue;
		}
		for ( int i = 0; i < sv_maxclients->integer; i++ ) {
			client_t *c = &svs.clients[i];
			if ( c != target && c->state >= CS_CONNECTED && !Q_stricmp( c->economyHandle, stake->handle ) ) {
				backer = c;
				break;
			}
		}
		if ( backer ) {
			backer->economyCredits += stake->amount;
			SV_EconomyPersistCredits( backer );
			SV_EconomyPrint( backer, va( "Your %d credit bounty on %s ^7was refunded (%s). Balance: %d",
				stake->amount, target->name, why, backer->economyCredits ) );
		} else {
			SV_EconomyAddCreditsToAccount( stake->handle, stake->amount );
		}
		total += stake->amount;
	}

	if ( total > 0 ) {
		Com_Printf( "Economy: refunded %d credits of bounty on %s (%s)\n", total, target->name, why );
	}
	SV_EconomyBountyClear( target );
}

// Every open bounty, before the server shuts down and forgets them.
void SV_EconomyRefundAllBounties( const char *why ) {
	if ( !svs.clients || !sv_maxclients ) {
		return;
	}
	for ( int i = 0; i < sv_maxclients->integer; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			SV_EconomyBountyRefund( &svs.clients[i], why );
		}
	}
}

static qboolean SV_EconomyEnabled( void ) {
	return (Cvar_VariableIntegerValue("g_creditSystemEnable") == 1) ? qtrue : qfalse;
}

// Shop and bounty are independent sub-toggles on top of the master economy
// switch above - a server can run the credit system for kill rewards and
// !balance while keeping only one of !buy / !bounty (or neither) turned on.
static qboolean SV_EconomyShopEnabled( void ) {
	return (g_economyShopEnable && g_economyShopEnable->integer == 1) ? qtrue : qfalse;
}

static qboolean SV_EconomyBountyEnabled( void ) {
	return (g_economyBountyEnable && g_economyBountyEnable->integer == 1) ? qtrue : qfalse;
}

// A game's result, large in the middle of the player's screen: net is what
// they came away with - positive won, negative lost, 0 stake returned.
void SV_EconomyResultBanner( client_t *cl, const char *game, int net ) {
	if ( !cl || cl->state < CS_CONNECTED ) {
		return;
	}
	if ( net > 0 ) {
		SV_SendServerCommand( cl, "cp \"^3%s\n^2You win +%d credits!\"\n", game, net );
	} else if ( net < 0 ) {
		SV_SendServerCommand( cl, "cp \"^3%s\n^1You lose %d credits\"\n", game, -net );
	} else {
		SV_SendServerCommand( cl, "cp \"^3%s\n^7Your credits are back\"\n", game );
	}
}

void SV_EconomyPrint( client_t *cl, const char *text ) {
	SV_SendServerCommand( cl, "chat \"^2[Economy]^7 %s\"\n", text );
}

// --- Chat flood control -----------------------------------------------------
//
// More than CHAT_FLOOD_MAX_MESSAGES say/say_team messages within
// CHAT_FLOOD_WINDOW_MS mutes the sender's chat for CHAT_FLOOD_MUTE_MS,
// counted from the message that tripped the limit (not from window start) -
// so muting a client resets what "10 seconds of continuing to flood" even
// means, rather than the mute silently expiring mid-flood at the original
// window's end. Distinct from sv_floodProtect, which throttles the reliable
// command channel generically (any command, one per floodTime) - this is
// chat-specific and counts actual messages in a real window, not a fixed
// per-command cooldown.
#define CHAT_FLOOD_MAX_MESSAGES	5
#define CHAT_FLOOD_WINDOW_MS		10000
#define CHAT_FLOOD_MUTE_MS			10000

// Returns qtrue if this message should be dropped (client is flood-limited).
// Called once per say/say_team command, before it's parsed as an economy
// command or forwarded to the game module - a muted client's messages never
// reach either, so flooding !buy/!bounty spam is throttled the same as
// ordinary chat spam.
static qboolean SV_ChatFloodCheck( client_t *cl ) {
	if ( !g_chatFloodEnable || !g_chatFloodEnable->integer ) {
		return qfalse;
	}

	if ( cl->chatMutedUntil > svs.time ) {
		return qtrue;
	}

	if ( cl->chatWindowStart == 0 || svs.time - cl->chatWindowStart >= CHAT_FLOOD_WINDOW_MS ) {
		cl->chatWindowStart = svs.time;
		cl->chatMsgCount = 0;
	}

	cl->chatMsgCount++;

	if ( cl->chatMsgCount > CHAT_FLOOD_MAX_MESSAGES ) {
		const qboolean justTripped = (cl->chatMutedUntil <= svs.time) ? qtrue : qfalse;
		cl->chatMutedUntil = svs.time + CHAT_FLOOD_MUTE_MS;
		if ( justTripped ) {
			SV_SendServerCommand( cl, "cp \"^1Slow down!^7 You're chatting too fast - muted for 10 seconds.\"\n" );
		}
		return qtrue;
	}

	return qfalse;
}

// --- Nute Gunray class-selection block --------------------------------------
//
// SV_GunrayCheckFrame (below, in this file) can detect and react to a
// player already playing as Gunray, but every Siege-mode self-service way
// of getting them back out is a dead end, live-tested: "team s" only
// calls SetTeam() if the round hasn't begun yet or the player is already
// out of respawns for this round (Cmd_Team_f, g_cmds.c - otherwise it's a
// deliberate no-op, by design, so players can't abandon a losing team
// mid-round), and "kill" doesn't even reach Cmd_Kill_f in Siege gametype
// at all (same file - it just sets an unrelated holocron-touch flag).
// Both were confirmed to silently do nothing, repeatedly, for a player
// still holding respawns.
//
// Rather than keep fighting those restrictions after the fact, block the
// class pick at the source: "siegeclass v7_NuteG ..." is the literal
// client command a real pick sends (captured live in legends-engine.log -
// see SV_GunrayCheckFrame's header for the full line), intercepted here
// and never forwarded to the game module, so they never actually spawn as
// Gunray in the first place. Private message only (not a server-wide
// broadcast like SV_GunrayCheckFrame's) - this can fire on every click if
// someone keeps trying, and broadcasting each attempt would spam chat for
// no added benefit once the pick is already blocked.
// Siege classes that break the server, by class token (the "sc" key a pick
// sends and CS_PLAYERS carries), with a name for the messages. Nute Gunray
// crashes it; R2-D2 (h5_AstroR2, Legends) is broken too. socialOnly ones
// are only blocked on social servers (g_socialMode).
static const struct { const char *cls; const char *name; qboolean socialOnly; } kBlockedClasses[] = {
	{ "v7_NuteG",     "Nute Gunray",    qfalse },
	{ "h5_AstroR2",   "R2-D2",          qfalse },
	{ "h9_Yarael",    "Yarael Poof",    qtrue },
	{ "v2_Hondo",     "Hondo Ohnaka",   qtrue },
	{ "v7_Tarkin",    "Tarkin",         qtrue },
	{ "v7_Veers",     "Veers",          qtrue },
	{ "v7_Enoch",     "Captain Enoch",  qtrue },
	{ "h7_RebEngie",  "Rebel Engineer", qtrue },
};

static const char *SV_BlockedClassName( const char *cls ) {
	const qboolean social = ( g_socialMode && g_socialMode->integer ) ? qtrue : qfalse;
	for ( size_t i = 0; i < ARRAY_LEN( kBlockedClasses ); i++ ) {
		if ( kBlockedClasses[i].socialOnly && !social ) {
			continue;
		}
		if ( !Q_stricmp( cls, kBlockedClasses[i].cls ) ) {
			return kBlockedClasses[i].name;
		}
	}
	return NULL;
}

static qboolean SV_GunrayClassBlockCheck( client_t *cl ) {
	if ( Q_stricmp( Cmd_Argv( 0 ), "siegeclass" ) ) {
		return qfalse;
	}
	const char *blocked = SV_BlockedClassName( Cmd_Argv( 1 ) );
	if ( !blocked ) {
		return qfalse;
	}

	Com_Printf( "[GunrayDebug] %s blocked from picking %s (siegeclass command intercepted)\n", cl->name, blocked );
	SV_SendServerCommand( cl, "cp \"^1%s is disabled on this server^7 - please pick another class.\"\n", blocked );
	return qtrue;
}

// --- Paced multi-line menu delivery ("!buy" listings) ----------------------
//
// A shop category can run to 18 items; sending all of it (plus header/
// footer) as one "chat" servercommand is technically one message, but the
// chat overlay only keeps a handful of lines on screen before older ones
// scroll off, so most of a long listing was gone before it could be read.
// This queues each line on the client and SV_EconomyFrame below drips them
// out one per ECONOMY_MENU_LINE_DELAY_MS instead.

void SV_EconomyMenuBegin( client_t *cl ) {
	cl->economyMenuLineCount = 0;
	cl->economyMenuNextLine = 0;
	cl->economyMenuNextSendTime = 0;	// 0 = send the first line on the next pump, whenever that is
}

void SV_EconomyMenuAddLine( client_t *cl, const char *line ) {
	if ( cl->economyMenuLineCount >= ECONOMY_MENU_LINES_MAX ) {
		return;
	}
	Q_strncpyz( cl->economyMenuLines[cl->economyMenuLineCount], line, ECONOMY_MENU_LINE_SIZE );
	cl->economyMenuLineCount++;
}

// Sends the next queued line, if any and if enough time has passed since the
// last one. Safe to call every frame for every client - it's a no-op unless
// there's something due.
void SV_EconomyMenuPump( client_t *cl ) {
	if ( cl->economyMenuNextLine >= cl->economyMenuLineCount ) {
		return;
	}
	if ( cl->economyMenuNextSendTime != 0 && svs.time < cl->economyMenuNextSendTime ) {
		return;
	}
	SV_SendServerCommand( cl, "chat \"%s\"\n", cl->economyMenuLines[cl->economyMenuNextLine] );
	cl->economyMenuNextLine++;
	cl->economyMenuNextSendTime = svs.time + ECONOMY_MENU_LINE_DELAY_MS;
}

static void SV_EconomyGiveAmmoRefill( client_t *cl ) {
	const qboolean cheatsWereEnabled = Cvar_VariableIntegerValue( "sv_cheats" ) ? qtrue : qfalse;

	if ( !cheatsWereEnabled ) {
		Cvar_Set( "sv_cheats", "1" );
		GVM_RunFrame( sv.time );
	}

	SV_ExecuteClientCommand( cl, "give ammo_all", qtrue );

	if ( !cheatsWereEnabled ) {
		Cvar_Set( "sv_cheats", "0" );
		GVM_RunFrame( sv.time );
	}
}

static qboolean SV_ParseEconomyChat( char *out, int outSize ) {
	const char *raw = Cmd_Args();
	int i;
	int len;

	if ( !raw || !raw[0] ) {
		return qfalse;
	}

	while ( *raw == ' ' ) {
		raw++;
	}

	Q_strncpyz( out, raw, outSize );
	len = strlen( out );

	if ( len >= 2 && out[0] == '"' && out[len - 1] == '"' ) {
		for ( i = 0; i < len - 1; i++ ) {
			out[i] = out[i + 1];
		}
		out[len - 2] = '\0';
	}

	len = strlen( out );
	while ( len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t') ) {
		out[len - 1] = '\0';
		len--;
	}

	return out[0] ? qtrue : qfalse;
}

static qboolean SV_HandleEconomyChatCommand( client_t *cl ) {
	char commandName[MAX_TOKEN_CHARS];
	char chatText[MAX_STRING_CHARS];
	char firstArg[MAX_TOKEN_CHARS];
	char secondArg[MAX_TOKEN_CHARS];
	const char *clientCmd = Cmd_Argv( 0 );
	const char *chatCursor;
	int commandLen = 0;
	int i;

	if ( Q_stricmp( clientCmd, "say" ) && Q_stricmp( clientCmd, "say_team" ) ) {
		return qfalse;
	}

	if ( !SV_ParseEconomyChat( chatText, sizeof( chatText ) ) ) {
		return qfalse;
	}

	chatCursor = chatText;
	while ( *chatCursor == ' ' ) {
		chatCursor++;
	}

	if ( chatCursor[0] != '!' ) {
		// A bare "help" is what new players type; answer it like !help.
		if ( Q_stricmp( chatCursor, "help" ) ) {
			return qfalse;
		}
		chatCursor = "!help";
	}

	chatCursor++;
	while ( *chatCursor == ' ' ) {
		chatCursor++;
	}

	while ( chatCursor[commandLen] && chatCursor[commandLen] != ' ' && chatCursor[commandLen] != '\t' ) {
		commandLen++;
	}

	if ( commandLen <= 0 || commandLen >= (int)sizeof( commandName ) ) {
		return qfalse;
	}

	for ( i = 0; i < commandLen; i++ ) {
		commandName[i] = tolower( (unsigned char)chatCursor[i] );
	}
	commandName[commandLen] = '\0';

	chatCursor += commandLen;
	while ( *chatCursor == ' ' || *chatCursor == '\t' ) {
		chatCursor++;
	}

	// "!stats" is its own independent feature (own g_statsEnable cvar) and
	// "!help" always responds with whatever's actually enabled here (see
	// below) - neither belongs behind the economy master gate the rest of
	// these commands sit behind.
	if ( !Q_stricmp( commandName, "stats" ) ) {
		if ( !g_statsEnable || !g_statsEnable->integer ) {
			return qfalse;	// feature off - let it pass through as ordinary chat
		}
		SV_StatsShowCommand( cl );
		return qtrue;
	}

	// Emotes (social servers) aren't part of the economy either.
	if ( !Q_stricmp( commandName, "barfight" ) && g_socialMode && g_socialMode->integer ) {
		return SV_SocialBarFightCommand( cl, chatCursor );
	}
	if ( !Q_stricmp( commandName, "ht" ) && g_holotable && g_holotable->integer ) {
		return SV_SocialHoloCommand( cl, chatCursor );
	}
	if ( !Q_stricmp( commandName, "wp" ) && g_socialMode && g_socialMode->integer ) {
		return SV_SocialWaypointCommand( cl, chatCursor );
	}
	if ( !Q_stricmp( commandName, "where" ) && g_socialMode && g_socialMode->integer ) {
		return SV_SocialWhereCommand( cl );
	}
	if ( !Q_stricmp( commandName, "playsound" ) && g_socialMode && g_socialMode->integer ) {
		return SV_SocialPlaySoundCommand( cl, chatCursor );
	}

	if ( SV_SocialSpawnCommand( cl, commandName ) ) {
		return qtrue;
	}

	if ( SV_EmoteCommand( cl, commandName ) ) {
		return qtrue;
	}

	if ( !SV_EconomyEnabled() &&
		( !Q_stricmp( commandName, "balance" ) ||
		  !Q_stricmp( commandName, "buy" ) ||
		  !Q_stricmp( commandName, "bar" ) ||
		  !Q_stricmp( commandName, "jukebox" ) ||
		  !Q_stricmp( commandName, "pazaak" ) ||
		  !Q_stricmp( commandName, "pz" ) ||
		  !Q_stricmp( commandName, "raffle" ) ||
		  !Q_stricmp( commandName, "gift" ) ||
		  !Q_stricmp( commandName, "chance" ) ||
		  !Q_stricmp( commandName, "blackjack" ) ||
		  !Q_stricmp( commandName, "bj" ) ||
		  !Q_stricmp( commandName, "bartender" ) ||
		  !Q_stricmp( commandName, "barkeep" ) ||
		  !Q_stricmp( commandName, "bet" ) ||
		  !Q_stricmp( commandName, "bets" ) ||
		  !Q_stricmp( commandName, "bounty" ) ||
		  !Q_stricmp( commandName, "bountry" ) ||
		  !Q_stricmp( commandName, "register" ) ||
		  !Q_stricmp( commandName, "login" ) ) ) {
		SV_EconomyPrint( cl, "Credit system is disabled." );
		return qtrue;
	}

	// Balances, spending and bounties all live on an account - without one
	// there's nothing to show but a confusing 0, so say how to get one.
	if ( SV_EconomyEnabled() && !cl->economyHandle[0] &&
		( !Q_stricmp( commandName, "balance" ) ||
		  !Q_stricmp( commandName, "buy" ) ||
		  !Q_stricmp( commandName, "bar" ) ||
		  !Q_stricmp( commandName, "jukebox" ) ||
		  !Q_stricmp( commandName, "pazaak" ) ||
		  !Q_stricmp( commandName, "pz" ) ||
		  !Q_stricmp( commandName, "raffle" ) ||
		  !Q_stricmp( commandName, "gift" ) ||
		  !Q_stricmp( commandName, "chance" ) ||
		  !Q_stricmp( commandName, "blackjack" ) ||
		  !Q_stricmp( commandName, "bj" ) ||
		  !Q_stricmp( commandName, "bartender" ) ||
		  !Q_stricmp( commandName, "barkeep" ) ||
		  !Q_stricmp( commandName, "bet" ) ||
		  !Q_stricmp( commandName, "bets" ) ||
		  !Q_stricmp( commandName, "bounty" ) ||
		  !Q_stricmp( commandName, "bountry" ) ) ) {
		SV_EconomyPrint( cl, "You need to be logged in to use this. "
			"^5!login <handle> <pin> ^7if you have an account, or "
			"^5!register <handle> <pin> ^7to make one (with free welcome credits!)." );
		return qtrue;
	}

	if ( SV_EconomyEnabled() ) {
		SV_EconomySyncCredits( cl );
	}

	if ( !Q_stricmp( commandName, "balance" ) ) {
		char balBuf[256];
		Com_sprintf( balBuf, sizeof(balBuf),
			"^3=== BALANCE ===\n"
			"^7Credits: ^2%d\n"
			"^7Bounty on you: ^1%d",
			cl->economyCredits, cl->economyBounty );
		SV_SendServerCommand( cl, "chat \"%s\"\n", balBuf );
		return qtrue;
	}

	if ( !Q_stricmp( commandName, "bar" ) ) {
		return SV_BarCommand( cl, chatCursor );
	}

	if ( !Q_stricmp( commandName, "jukebox" ) ) {
		return SV_JukeboxCommand( cl, chatCursor );
	}

	if ( !Q_stricmp( commandName, "pazaak" ) || !Q_stricmp( commandName, "pz" ) ) {
		return SV_PazaakCommand( cl, chatCursor );
	}

	if ( !Q_stricmp( commandName, "raffle" ) ) {
		return SV_RaffleCommand( cl, chatCursor );
	}

	if ( !Q_stricmp( commandName, "chance" ) ) {
		return SV_ChanceCommand( cl, chatCursor );
	}

	if ( !Q_stricmp( commandName, "blackjack" ) || !Q_stricmp( commandName, "bj" ) ) {
		return SV_BlackjackCommand( cl, chatCursor );
	}

	if ( !Q_stricmp( commandName, "bartender" ) || !Q_stricmp( commandName, "barkeep" ) ) {
		return SV_BartenderCommand( cl, chatCursor );
	}

	if ( !Q_stricmp( commandName, "bet" ) || !Q_stricmp( commandName, "bets" ) ) {
		return SV_BetCommand( cl, chatCursor );
	}

	if ( !Q_stricmp( commandName, "buy" ) ) {
		if ( !SV_EconomyEnabled() ) {
			SV_EconomyPrint( cl, "Credit system is disabled." );
			return qtrue;
		}

		if ( !SV_EconomyShopEnabled() ) {
			SV_EconomyPrint( cl, "The shop is disabled on this server, but you're still earning credits here - "
				"spend them with !buy on a server where the shop is on." );
			return qtrue;
		}

		if ( sscanf( chatCursor, "%31s", firstArg ) != 1 ) {
			char line[ECONOMY_MENU_LINE_SIZE];
			int  c;

			SV_EconomyMenuBegin( cl );

			Com_sprintf( line, sizeof(line), "^3=== SHOP === Balance: ^2%d ^3credits ===", cl->economyCredits );
			SV_EconomyMenuAddLine( cl, line );
			SV_EconomyMenuAddLine( cl, "^7Categories - type ^5!buy <category> ^7to browse one, e.g. ^5!buy pistols^7:" );

			for ( c = 0; c < (int)ARRAY_LEN( svEconomyShopCategories ); c++ ) {
				if ( !SV_EconomyCategoryExists( svEconomyShopCategories[c] ) ) {
					continue;
				}
				Com_sprintf( line, sizeof(line), "^7 - ^5%s", svEconomyShopCategories[c] );
				SV_EconomyMenuAddLine( cl, line );
			}

			SV_EconomyMenuAddLine( cl, "^7Then ^5!buy <item name> ^7to purchase, e.g. ^5!buy bryar" );
			SV_EconomyMenuPump( cl );
			return qtrue;
		}

		// An item's exact name wins over a category of the same name: "ammo"
		// is both, and !buy ammo should buy ammo, not list the category.
		if ( SV_EconomyFindItemByName( firstArg ) < 0 && SV_EconomyCategoryExists( firstArg ) ) {
			char line[ECONOMY_MENU_LINE_SIZE];

			SV_EconomyMenuBegin( cl );

			Com_sprintf( line, sizeof(line), "^3=== SHOP: %s === Balance: ^2%d ^3===", firstArg, cl->economyCredits );
			SV_EconomyMenuAddLine( cl, line );

			for ( i = 0; i < (int)ARRAY_LEN( svEconomyItemDefs ); i++ ) {
				if ( Q_stricmp( svEconomyItemDefs[i].category, firstArg ) ) {
					continue;
				}
				if ( !SV_EconomyItemEnabled( i ) ) {
					continue;
				}
				Com_sprintf( line, sizeof(line), "^7%s ^7- ^2%d ^7cr", svEconomyItemDefs[i].name, SV_EconomyItemCost( i ) );
				SV_EconomyMenuAddLine( cl, line );
			}

			SV_EconomyMenuAddLine( cl, "^3Type !buy <name> to purchase, e.g. !buy bryar" );
			SV_EconomyMenuPump( cl );
			return qtrue;
		}

		i = SV_EconomyFindItemByName( firstArg );
		if ( i < 0 ) {
			SV_EconomyPrint( cl, "Unknown item or category. Type !buy to list categories." );
			return qtrue;
		}

		if ( !SV_EconomyItemEnabled( i ) ) {
			SV_EconomyPrint( cl, "That item is currently disabled." );
			return qtrue;
		}

		if ( cl->economyCredits < SV_EconomyItemCost( i ) ) {
			SV_EconomyPrint( cl, va( "Not enough credits. Need %d, have %d.", SV_EconomyItemCost( i ), cl->economyCredits ) );
			return qtrue;
		}

		if ( !cl->gentity || !cl->gentity->playerState ||
			cl->gentity->playerState->persistant[PERS_TEAM] == TEAM_SPECTATOR ||
			cl->gentity->playerState->stats[STAT_HEALTH] <= 0 ) {
			SV_EconomyPrint( cl, "You must be alive to buy items." );
			return qtrue;
		}

		cl->economyCredits -= SV_EconomyItemCost( i );

		if ( svEconomyItemDefs[i].winIndex >= 0 ) {
			SV_SpinForceGiveWin( cl, svEconomyItemDefs[i].winIndex );
		} else {
			SV_EconomyGiveAmmoRefill( cl );
		}

		SV_EconomyPersistCredits( cl );
		SV_EconomyPrint( cl, va( "Purchased %s. New balance: %d", svEconomyItemDefs[i].name, cl->economyCredits ) );
		return qtrue;
	}

	// "!bounty <player> <credits>" places one directly in a single command -
	// <player> is resolved with the same SV_BetterGetPlayerByHandle every
	// other economy/admin command already uses to target a player (client
	// number, exact name, or name with color codes stripped - see
	// sv_ccmds.cpp), so there's no separately-typed list position and no
	// window between listing and placing where a reused client slot could
	// end up bountied instead of who was actually meant.
	//
	// "!bounty" alone (or with an unresolvable target/amount) falls back to
	// a quick reference instead: your own numbers plus anyone currently
	// carrying a bounty.
	// "!gift <player> <credits>": straight from one account to another. Both
	// have to be logged in - the gate above covers the giver.
	if ( !Q_stricmp( commandName, "gift" ) ) {
		client_t *target;
		int amount;

		if ( sscanf( chatCursor, "%63s %63s", firstArg, secondArg ) != 2 ) {
			SV_EconomyPrint( cl, "Usage: ^5!gift <player> <credits>^7, e.g. ^5!gift Ricks 50" );
			return qtrue;
		}
		target = SV_EconomyFindPlayer( cl, firstArg );
		amount = atoi( secondArg );

		if ( !target ) {
			return qtrue;
		}
		if ( target == cl ) {
			SV_EconomyPrint( cl, "You can't gift credits to yourself." );
			return qtrue;
		}
		if ( !target->economyHandle[0] ) {
			SV_EconomyPrint( cl, va( "%s ^7isn't logged in, so can't receive credits.", target->name ) );
			return qtrue;
		}
		if ( !Q_stricmp( target->economyHandle, cl->economyHandle ) ) {
			SV_EconomyPrint( cl, "That's your own account." );
			return qtrue;
		}
		if ( amount <= 0 ) {
			SV_EconomyPrint( cl, "Gift at least 1 credit." );
			return qtrue;
		}
		SV_EconomySyncCredits( target );
		if ( cl->economyCredits < amount ) {
			SV_EconomyPrint( cl, va( "You only have %d credits.", cl->economyCredits ) );
			return qtrue;
		}

		cl->economyCredits -= amount;
		SV_EconomyPersistCredits( cl );
		target->economyCredits += amount;
		SV_EconomyPersistCredits( target );

		SV_EconomyPrint( cl, va( "You gifted %d credits to %s^7. New balance: %d", amount, target->name, cl->economyCredits ) );
		SV_EconomyPrint( target, va( "%s ^7gifted you ^2%d ^7credits! New balance: %d", cl->name, amount, target->economyCredits ) );
		return qtrue;
	}

	if ( !Q_stricmp( commandName, "bounty" ) || !Q_stricmp( commandName, "bountry" ) ) {
		if ( !SV_EconomyEnabled() ) {
			SV_EconomyPrint( cl, "Credit system is disabled." );
			return qtrue;
		}

		if ( !SV_EconomyBountyEnabled() ) {
			SV_EconomyPrint( cl, "The bounty system is currently disabled on this server." );
			return qtrue;
		}

		if ( sscanf( chatCursor, "%63s %63s", firstArg, secondArg ) == 2 ) {
			client_t *target = SV_EconomyFindPlayer( cl, firstArg );
			int amount = atoi( secondArg );

			if ( !target ) {
				return qtrue;
			}

			if ( target == cl ) {
				SV_EconomyPrint( cl, "You can't place a bounty on yourself." );
				return qtrue;
			}

			if ( amount <= 0 ) {
				SV_EconomyPrint( cl, "Bounty must be greater than 0." );
				return qtrue;
			}

			if ( cl->economyCredits < amount ) {
				SV_EconomyPrint( cl, va( "Not enough credits. You have %d.", cl->economyCredits ) );
				return qtrue;
			}

			// Each backer's share, so it can go back to them if the bounty
			// is never collected.
			{
				bountyStake_t *stake = NULL;
				for ( int s = 0; s < ECONOMY_BOUNTY_BACKERS && !stake; s++ ) {
					if ( !Q_stricmp( target->economyBountyStakes[s].handle, cl->economyHandle ) ) {
						stake = &target->economyBountyStakes[s];
					}
				}
				for ( int s = 0; s < ECONOMY_BOUNTY_BACKERS && !stake; s++ ) {
					if ( !target->economyBountyStakes[s].handle[0] ) {
						stake = &target->economyBountyStakes[s];
						Q_strncpyz( stake->handle, cl->economyHandle, sizeof( stake->handle ) );
					}
				}
				if ( !stake ) {
					SV_EconomyPrint( cl, va( "%s ^7already has %d people backing their bounty - add to it another time.",
						target->name, ECONOMY_BOUNTY_BACKERS ) );
					return qtrue;
				}
				stake->amount += amount;
			}

			cl->economyCredits -= amount;
			target->economyBounty += amount;
			target->economyBountyPlacerNum = (int)( cl - svs.clients );
			Q_strncpyz( target->economyBountyPlacerName, cl->name, sizeof( target->economyBountyPlacerName ) );
			SV_EconomyPersistCredits( cl );

			// Public, so a bounty puts pressure on the target and gives
			// everyone else a reason to go hunt it - plus a personal tell to
			// the target directly, since "on you" lands harder than reading
			// their own name in a message meant for everyone.
			SV_SendServerCommand( NULL, "chat \"" SVSAY_PREFIX "^3%s^7 put a bounty of ^1%d^7 credits on ^3%s^7!\"\n",
				cl->name, amount, target->name );
			SV_EconomyPrint( target, va( "%s put a bounty on you for %d credits!", cl->name, amount ) );
			return qtrue;
		}

		{
			char bBuf[1024];
			int  bLen = 0;
			qboolean anyBounties = qfalse;

			bLen += Com_sprintf( bBuf + bLen, sizeof(bBuf) - bLen,
				"^3=== BOUNTY === ^7Your credits: ^2%d ^7| Bounty on you: ^1%d ^3===\n",
				cl->economyCredits, cl->economyBounty );

			for ( i = 0; i < sv_maxclients->integer; i++ ) {
				client_t *target = &svs.clients[i];

				if ( target->state < CS_CONNECTED || target == cl || target->economyBounty <= 0 ) {
					continue;
				}

				anyBounties = qtrue;
				bLen += Com_sprintf( bBuf + bLen, sizeof(bBuf) - bLen,
					"^7%s ^7- bounty: ^1%d\n", target->name, target->economyBounty );
			}

			if ( !anyBounties ) {
				bLen += Com_sprintf( bBuf + bLen, sizeof(bBuf) - bLen, "^7No active bounties right now.\n" );
			}

			bLen += Com_sprintf( bBuf + bLen, sizeof(bBuf) - bLen,
				"^3Usage: ^7!bounty <player> <credits> ^3e.g. ^7!bounty Bob 50\n" );

			SV_SendServerCommand( cl, "chat \"%s\"\n", bBuf );
		}
		return qtrue;
	}

	if ( !Q_stricmp( commandName, "register" ) ) {
		int argCount = sscanf( chatCursor, "%23s %15s", firstArg, secondArg );

		// A new account starts from nothing but the welcome bonus - never
		// from whatever balance the session is carrying, which is what let
		// registering while logged in copy a whole balance into each new
		// account.
		if ( cl->economyHandle[0] ) {
			SV_EconomyPrint( cl, va( "You're already logged in as '%s' - one account each.", cl->economyHandle ) );
			return qtrue;
		}

		if ( argCount < 1 ) {
			SV_EconomyPrint( cl, "Usage: !register <handle>, then !register <handle> <4-digit pin>" );
			return qtrue;
		}

		if ( !SV_EconomyValidateHandle( firstArg ) ) {
			SV_EconomyPrint( cl, "Handle must be 3-23 letters, numbers, or underscores." );
			return qtrue;
		}

		if ( argCount == 1 ) {
			if ( SV_EconomyFindAccount( firstArg ) ) {
				SV_EconomyPrint( cl, "That handle is taken. Choose another." );
			} else {
				SV_EconomyPrint( cl, va( "Handle '%s' is available. Type !register %s <4-digit pin> to finish.", firstArg, firstArg ) );
			}
			return qtrue;
		}

		if ( !SV_EconomyValidatePin( secondArg ) ) {
			SV_EconomyPrint( cl, "PIN must be exactly 4 digits." );
			return qtrue;
		}

		{
			const int bonus = g_economyRegisterBonus ? Q_max( 0, g_economyRegisterBonus->integer ) : 0;
			economyAccount_t *acct = NULL;
			const char *problem = NULL;
			byte salt[ECONOMY_SALT_SIZE];

			if ( !Sys_RandomBytes( salt, ECONOMY_SALT_SIZE ) ) {
				SV_EconomyPrint( cl, "Registration failed (RNG error). Try again." );
				return qtrue;
			}

			SV_EconomyBegin();
			if ( SV_EconomyFindAccount( firstArg ) ) {
				problem = "That handle is taken. Choose another.";
			} else if ( svEconomyAccountCount >= ECONOMY_MAX_ACCOUNTS ) {
				problem = "Account storage is full. Contact an admin.";
			} else {
				acct = &svEconomyAccounts[svEconomyAccountCount++];
				Com_Memset( acct, 0, sizeof( *acct ) );
				Com_Memcpy( acct->salt, salt, ECONOMY_SALT_SIZE );
				Q_strncpyz( acct->handle, firstArg, sizeof( acct->handle ) );
				SV_EconomyHashPin( acct->salt, secondArg, acct->hash );
				acct->credits = bonus;
				SV_EconomyAccountsSave();

				Q_strncpyz( cl->economyHandle, acct->handle, sizeof( cl->economyHandle ) );
				cl->economyCredits = bonus;
				cl->economyCreditsSynced = bonus;
			}
			SV_EconomyEnd();

			if ( problem ) {
				SV_EconomyPrint( cl, problem );
				return qtrue;
			}

			SV_EconomyPrint( cl, va( "Registered! Logged in as '%s'. Use !login %s <pin> on future connects.", cl->economyHandle, cl->economyHandle ) );
			{
				// The welcome bonus is today's; the daily one starts tomorrow.
				qboolean paid;
				SV_EconomyDailyClaim( cl->economyHandle, qtrue, &paid );
			}
			if ( bonus > 0 ) {
				SV_EconomyPrint( cl, va( "Welcome bonus: +%d credits! Balance: %d", bonus, cl->economyCredits ) );
			}
		}
		return qtrue;
	}

	if ( !Q_stricmp( commandName, "login" ) ) {
		economyAccount_t *acct;
		byte candidateHash[ECONOMY_HASH_SIZE];
		const int now = (int)time( NULL );
		char result[256] = "";
		qboolean ok = qfalse;
		int c;

		if ( cl->economyHandle[0] ) {
			SV_EconomyPrint( cl, va( "You are already logged in as '%s'.", cl->economyHandle ) );
			return qtrue;
		}

		if ( sscanf( chatCursor, "%23s %15s", firstArg, secondArg ) != 2 ) {
			SV_EconomyPrint( cl, "Usage: !login <handle> <pin>" );
			return qtrue;
		}

		// Lockouts are wall-clock seconds (time()), since the file is shared
		// by every server: they used to be svs.time, each process's own
		// uptime in milliseconds, so a lockout meant hours on one server and
		// nothing on another. Old svs.time values read as long past.
		// failedAttempts keeps counting through lockouts until a good PIN,
		// and each lockout doubles, up to an hour: a 4-digit PIN would
		// otherwise fall to a patient script in under a day.
		SV_EconomyBegin();
		acct = SV_EconomyFindAccount( firstArg );
		if ( !acct ) {
			Q_strncpyz( result, "No account with that handle.", sizeof( result ) );
		} else if ( acct->lockoutUntil > now ) {
			Com_sprintf( result, sizeof( result ), "Too many failed attempts. Try again in %d seconds.", acct->lockoutUntil - now );
		} else {
			if ( SV_EconomyValidatePin( secondArg ) ) {
				SV_EconomyHashPin( acct->salt, secondArg, candidateHash );
				ok = SV_EconomySecureCompare( acct->hash, candidateHash, ECONOMY_HASH_SIZE );
			}
			if ( !ok ) {
				acct->failedAttempts++;
				if ( acct->failedAttempts % ECONOMY_LOGIN_MAX_ATTEMPTS == 0 ) {
					const int lockouts = acct->failedAttempts / ECONOMY_LOGIN_MAX_ATTEMPTS;
					const int secs = Q_min( ECONOMY_LOGIN_LOCKOUT_MAX_SECS,
						( ECONOMY_LOGIN_LOCKOUT_MS / 1000 ) << Q_min( lockouts - 1, 10 ) );
					acct->lockoutUntil = now + secs;
					Com_sprintf( result, sizeof( result ), "Too many failed attempts. Account locked for %d seconds.", secs );
				} else {
					Q_strncpyz( result, "Incorrect PIN.", sizeof( result ) );
				}
			} else {
				acct->failedAttempts = 0;
				acct->lockoutUntil = 0;
				Q_strncpyz( cl->economyHandle, acct->handle, sizeof( cl->economyHandle ) );
				cl->economyCredits = acct->credits;
				cl->economyCreditsSynced = acct->credits;
			}
			SV_EconomyAccountsSave();
		}
		SV_EconomyEnd();

		if ( !ok ) {
			SV_EconomyPrint( cl, result );
			return qtrue;
		}

		// Log out any other session on this server already on this handle.
		for ( c = 0; c < sv_maxclients->integer; c++ ) {
			client_t *other = &svs.clients[c];
			if ( other != cl && other->state >= CS_CONNECTED && other->economyHandle[0] &&
				!Q_stricmp( other->economyHandle, cl->economyHandle ) ) {
				SV_EconomyPrint( other, "You were logged out because your account logged in elsewhere." );
				other->economyHandle[0] = '\0';
				// Clearing the handle alone leaves economyCredits holding a
				// stale spendable copy of the balance this session had a
				// moment ago - !buy/bounty-placement only check that number,
				// not login state, so without this a still-connected kicked
				// session could keep spending (or losing to a bounty
				// payout) credits that are also live on whichever session
				// just logged in.
				other->economyCredits = 0;
			}
		}

		SV_EconomyPrint( cl, va( "Logged in as '%s'. Balance: %d credits.", cl->economyHandle, cl->economyCredits ) );

		if ( g_economyDailyBonus && g_economyDailyBonus->integer > 0 ) {
			qboolean paid;
			const int wait = SV_EconomyDailyClaim( cl->economyHandle, qfalse, &paid );
			if ( paid ) {
				cl->economyCredits += g_economyDailyBonus->integer;
				SV_EconomyPersistCredits( cl );
				SV_EconomyPrint( cl, va( "Daily login bonus: ^2+%d ^7credits! Balance: %d. Next one in 24 hours, on any of our servers.",
					g_economyDailyBonus->integer, cl->economyCredits ) );
			} else if ( wait > 0 ) {
				SV_EconomyPrint( cl, va( "Next daily login bonus in %dh %dm.", wait / 3600, ( wait % 3600 ) / 60 ) );
			}
		}
		return qtrue;
	}

	if ( !Q_stricmp( commandName, "help" ) ) {
		// Reflects whatever's actually turned on for THIS instance - shown
		// nothing about !buy/!bounty if that particular sub-feature (or the
		// whole economy) is off here, rather than describing commands that
		// wouldn't work. Paced the same way "!buy" listings are (see
		// SV_EconomyMenuBegin above) since this can run to a similar number
		// of lines.
		qboolean anySection = qfalse;

		SV_EconomyMenuBegin( cl );
		SV_EconomyMenuAddLine( cl, "^3=== SERVER HELP ===" );

		if ( SV_EconomyEnabled() ) {
			anySection = qtrue;
			SV_EconomyMenuAddLine( cl, "^2!balance ^7- show your credits and any bounty on you." );

			if ( SV_EconomyShopEnabled() ) {
				SV_EconomyMenuAddLine( cl, "^2!buy ^7- list shop categories, ^5!buy <category> ^7to browse, ^5!buy <name> ^7to purchase." );
			}

			if ( g_economyBarEnable && g_economyBarEnable->integer ) {
				SV_EconomyMenuAddLine( cl, "^2!bar ^7- the drinks menu. ^5!bar <number> ^7to order one." );
			}

			if ( g_economyJukeboxEnable && g_economyJukeboxEnable->integer ) {
				SV_EconomyMenuAddLine( cl, "^2!jukebox ^7- list the tracks. ^5!jukebox <number> ^7to play one for everyone." );
			}

			if ( g_economyPazaakEnable && g_economyPazaakEnable->integer ) {
				SV_EconomyMenuAddLine( cl, "^2!pazaak <player> <credits> ^7- challenge someone to Pazaak. ^5!pazaak ^7for the rules." );
			}

			if ( g_economyChanceEnable && g_economyChanceEnable->integer ) {
				SV_EconomyMenuAddLine( cl, "^2!chance <player> <credits> ^7- they pick red or blue, the server rolls, winning colour takes the pot." );
			}

			if ( g_economyBlackjackEnable && g_economyBlackjackEnable->integer ) {
				SV_EconomyMenuAddLine( cl, va( "^2!blackjack <credits> ^7- a hand against the dealer (up to %d). ^5!bj hit^7, ^5!bj stand^7, ^5!bj double^7.",
					g_blackjackMaxBet ? g_blackjackMaxBet->integer : 50 ) );
			}

			if ( g_economyBartenderEnable && g_economyBartenderEnable->integer ) {
				SV_EconomyMenuAddLine( cl, va( "^2!bartender <question> ^7- ask the bartender anything (%d credits).",
					g_bartenderCost ? g_bartenderCost->integer : 0 ) );
			}

			if ( g_economyBetEnable && g_economyBetEnable->integer ) {
				SV_EconomyMenuAddLine( cl, "^2!bet ^7- the fight taking bets. ^5!bet <fighter> <credits> ^7to back one. In a duel: ^5!bet start ^7to take bets." );
			}

			if ( g_economyRaffleEnable && g_economyRaffleEnable->integer ) {
				SV_EconomyMenuAddLine( cl, "^2!raffle ^7- this raffle's pool and time left. ^5!raffle <count> ^7to buy tickets." );
			}

			if ( SV_EconomyBountyEnabled() ) {
				SV_EconomyMenuAddLine( cl, "^2!bounty <player> <credits> ^7- place a bounty. ^5!bounty ^7alone shows active bounties." );
			}

			SV_EconomyMenuAddLine( cl, "^2!gift <player> <credits> ^7- give some of your credits to another player." );
			SV_EconomyMenuAddLine( cl, "^2!register <handle> <pin> ^7- new account. ^2!login <handle> <pin> ^7- returning." );
			SV_EconomyMenuAddLine( cl, "^3Credits are earned from kills while logged in." );
		}

		if ( g_socialMode && g_socialMode->integer ) {
			anySection = qtrue;
			SV_EconomyMenuAddLine( cl, "^2!emotes ^7- !sit, !dance, !taunt, !victory, !hug, !sleep, !rage and more. Move to stop." );
			SV_EconomyMenuAddLine( cl, "^2!spawn ^7- stuck in spectator? This gets you into the game. ^2!kill ^7- die and respawn." );
		}

		if ( g_statsEnable && g_statsEnable->integer ) {
			anySection = qtrue;
			SV_EconomyMenuAddLine( cl, "^2!stats ^7- your kills/deaths/suicides/playtime across all our servers." );
		}

		if ( !anySection ) {
			SV_EconomyMenuAddLine( cl, "^7No special commands are currently enabled on this server." );
		}

		SV_EconomyMenuPump( cl );
		return qtrue;
	}

	return qfalse;
}

/*
==================
SV_ExecuteClientCommand

Also called by bot code
==================
*/
void SV_ExecuteClientCommand( client_t *cl, const char *s, qboolean clientOK ) {
	ucmd_t	*u;
	qboolean bProcessed = qfalse;

	Cmd_TokenizeString( s );

	//Com_Printf("clientCommand: %s\n", Cmd_Argv(0));

	// see if it is a server level command
	for (u=ucmds ; u->name ; u++) {
		if (!strcmp (Cmd_Argv(0), u->name) ) {
			u->func( cl );
			bProcessed = qtrue;
			break;
		}
	}

	if (clientOK) {
		// Flood check runs first and covers both plain chat and economy chat
		// commands (!buy, !bounty, etc.) - a muted client's messages are
		// dropped before either path sees them.
		if ( ( !Q_stricmp( Cmd_Argv(0), "say" ) || !Q_stricmp( Cmd_Argv(0), "say_team" ) ) &&
			SV_ChatFloodCheck( cl ) ) {
			return;
		}

		if ( SV_GunrayClassBlockCheck( cl ) ) {
			return;
		}

		if ( SV_HandleEconomyChatCommand( cl ) ) {
			return;
		}

		// pass unknown strings to the game
		if (!u->name && sv.state == SS_GAME && (cl->state == CS_ACTIVE || cl->state == CS_PRIMED)) {
			// strip \r \n and ;
			if ( sv_filterCommands->integer ) {
				Cmd_Args_Sanitize( MAX_CVAR_VALUE_STRING, "\n\r", "  " );
				if ( sv_filterCommands->integer == 2 ) {
					// also strip ';' for callvote
					Cmd_Args_Sanitize( MAX_CVAR_VALUE_STRING, ";", " " );
				}
			}
			SV_SocialClientCommand( cl );
			GVM_ClientCommand( cl - svs.clients );
		}
	}
	else if (!bProcessed)
		Com_DPrintf( "client text ignored for %s: %s\n", cl->name, Cmd_Argv(0) );
}

/*
===============
SV_ClientCommand
===============
*/
static qboolean SV_ClientCommand( client_t *cl, msg_t *msg ) {
	int		seq;
	const char	*s;
	qboolean clientOk = qtrue;

	seq = MSG_ReadLong( msg );
	s = MSG_ReadString( msg );

	// see if we have already executed it
	if ( cl->lastClientCommand >= seq ) {
		return qtrue;
	}

	Com_DPrintf( "clientCommand: %s : %i : %s\n", cl->name, seq, s );

	// drop the connection if we have somehow lost commands
	if ( seq > cl->lastClientCommand + 1 ) {
		Com_Printf( "Client %s lost %i clientCommands\n", cl->name,
			seq - cl->lastClientCommand + 1 );
		SV_DropClient( cl, "Lost reliable commands" );
		return qfalse;
	}

	// malicious users may try using too many string commands
	// to lag other players.  If we decide that we want to stall
	// the command, we will stop processing the rest of the packet,
	// including the usercmd.  This causes flooders to lag themselves
	// but not other people
	// We don't do this when the client hasn't been active yet since its
	// normal to spam a lot of commands when downloading
	if ( !com_cl_running->integer &&
		cl->state >= CS_ACTIVE &&
		sv_floodProtect->integer )
	{
		const int floodTime = (sv_floodProtect->integer == 1) ? 1000 : sv_floodProtect->integer;
		if ( svs.time < (cl->lastReliableTime + floodTime) ) {
			// ignore any other text messages from this client but let them keep playing
			// TTimo - moved the ignored verbose to the actual processing in SV_ExecuteClientCommand, only printing if the core doesn't intercept
			clientOk = qfalse;
		}
		else {
			cl->lastReliableTime = svs.time;
		}
		if ( sv_floodProtectSlow->integer ) {
			cl->lastReliableTime = svs.time;
		}
	}

	SV_ExecuteClientCommand( cl, s, clientOk );

	cl->lastClientCommand = seq;
	Com_sprintf(cl->lastClientCommandString, sizeof(cl->lastClientCommandString), "%s", s);

	// Don't leak !register/!login PINs to the server console/log.
	if ( Q_stristr( s, "!register" ) || Q_stristr( s, "!login" ) ) {
		Com_Printf( "(economy account command redacted)\n" );
	} else {
		Com_Printf("%s \n", s);
	}


	return qtrue;		// continue procesing
}


//==================================================================================


// Handles both deferred vehicle actions client_t carries (see
// vehiclePendingSpawnName/vehicleForceUseNextCmd in server.h) - each one
// only ever acts on a single usercmd tick, one full SV_ClientThink call
// per action, never both in the same call. That staggering is load-
// bearing: firing the spawn command synchronously right after the
// teleport that precedes it (both in the same call stack, no frame in
// between) crashed the server outright in live testing, with no crash
// dump or log line to explain why; giving the teleport's effects a full
// frame to settle before the spawn fires - and the spawn a full frame to
// settle before boarding - did not.
void SV_VehicleClientThinkHook( client_t *cl, usercmd_t *cmd ) {
	if ( cl->vehiclePendingSpawnName[0] ) {
		char cmdBuf[96];
		Com_sprintf( cmdBuf, sizeof( cmdBuf ), "npc spawn vehicle %s", cl->vehiclePendingSpawnName );
		cl->vehiclePendingSpawnName[0] = '\0';
		SV_ExecuteClientCommand( cl, cmdBuf, qtrue );
		cl->vehicleForceUseNextCmd = qtrue; // board on a LATER tick, not this one
		return;
	}

	if ( cl->vehicleForceUseNextCmd ) {
		cmd->buttons |= BUTTON_USE;
		cl->vehicleForceUseNextCmd = qfalse;
	}
}

/*
==================
SV_ClientThink

Also called by bot code
==================
*/
void SV_ClientThink (client_t *cl, usercmd_t *cmd) {
	// GVM_ClientThink below is called with a NULL usercmd pointer - the
	// game module retrieves the current command from cl->lastUsercmd
	// itself (via its own syscall), not from a parameter. So the clamp has
	// to land before this assignment, not after it, or the stored copy the
	// game module actually reads still has the player's original request.
	SV_GunGameClampWeaponSelect( cl, cmd );
	SV_BarClientThink( cl, cmd );
	SV_BetClientThink( cl, cmd );
	SV_VehicleClientThinkHook( cl, cmd );

	cl->lastUsercmd = *cmd;

	if ( cl->state != CS_ACTIVE ) {
		return;		// may have been kicked during the last usercmd
	}

	GVM_ClientThink( cl - svs.clients, NULL );

	SV_SocialClientThink( cl );
}

/*
==================
SV_UserMove

The message usually contains all the movement commands
that were in the last three packets, so that the information
in dropped packets can be recovered.

On very fast clients, there may be multiple usercmd packed into
each of the backup packets.
==================
*/
static void SV_UserMove( client_t *cl, msg_t *msg, qboolean delta ) {
	int			i, key;
	int			cmdCount;
	usercmd_t	nullcmd;
	usercmd_t	cmds[MAX_PACKET_USERCMDS];
	usercmd_t	*cmd, *oldcmd;

	if ( delta ) {
		cl->deltaMessage = cl->messageAcknowledge;
	} else {
		cl->deltaMessage = -1;
	}

	cmdCount = MSG_ReadByte( msg );

	if ( cmdCount < 1 ) {
		Com_Printf( "cmdCount < 1\n" );
		return;
	}

	if ( cmdCount > MAX_PACKET_USERCMDS ) {
		Com_Printf( "cmdCount > MAX_PACKET_USERCMDS\n" );
		return;
	}

	// use the checksum feed in the key
	key = sv.checksumFeed;
	// also use the message acknowledge
	key ^= cl->messageAcknowledge;
	// also use the last acknowledged server command in the key
	key ^= Com_HashKey(cl->reliableCommands[ cl->reliableAcknowledge & (MAX_RELIABLE_COMMANDS-1) ], 32);

	Com_Memset( &nullcmd, 0, sizeof(nullcmd) );
	oldcmd = &nullcmd;
	for ( i = 0 ; i < cmdCount ; i++ ) {
		cmd = &cmds[i];
		MSG_ReadDeltaUsercmdKey( msg, key, oldcmd, cmd );
		if ( sv_legacyFixes->integer ) {
			// block "charge jump" and other nonsense
			if ( cmd->forcesel == FP_LEVITATION || cmd->forcesel >= NUM_FORCE_POWERS ) {
				cmd->forcesel = 0xFFu;
			}

			// affects speed calculation
			cmd->angles[ROLL] = 0;


			
		}
		oldcmd = cmd;
	}

	// save time for ping calculation
	cl->frames[ cl->messageAcknowledge & PACKET_MASK ].messageAcked = svs.time;

	// TTimo
	// catch the no-cp-yet situation before SV_ClientEnterWorld
	// if CS_ACTIVE, then it's time to trigger a new gamestate emission
	// if not, then we are getting remaining parasite usermove commands, which we should ignore
	if (sv_pure->integer != 0 && cl->pureAuthentic == 0 && !cl->gotCP) {
		if (cl->state == CS_ACTIVE)
		{
			// we didn't get a cp yet, don't assume anything and just send the gamestate all over again
			Com_DPrintf( "%s: didn't get cp command, resending gamestate\n", cl->name);
			SV_SendClientGameState( cl );
		}
		return;
	}

	// if this is the first usercmd we have received
	// this gamestate, put the client into the world
	if ( cl->state == CS_PRIMED ) {
		SV_ClientEnterWorld( cl, &cmds[0] );
		// the moves can be processed normaly
	}

	// a bad cp command was sent, drop the client
	if (sv_pure->integer != 0 && cl->pureAuthentic == 0) {
		SV_DropClient( cl, "Cannot validate pure client!");
		return;
	}

	if ( cl->state != CS_ACTIVE ) {
		cl->deltaMessage = -1;
		return;
	}

	// usually, the first couple commands will be duplicates
	// of ones we have previously received, but the servertimes
	// in the commands will cause them to be immediately discarded
	for ( i =  0 ; i < cmdCount ; i++ ) {
		// if this is a cmd from before a map_restart ignore it
		if ( cmds[i].serverTime > cmds[cmdCount-1].serverTime ) {
			continue;
		}
		// extremely lagged or cmd from before a map_restart
		//if ( cmds[i].serverTime > svs.time + 3000 ) {
		//	continue;
		//}
		// don't execute if this is an old cmd which is already executed
		// these old cmds are included when cl_packetdup > 0
		if ( cmds[i].serverTime <= cl->lastUsercmd.serverTime ) {
			continue;
		}
		SV_ClientThink (cl, &cmds[ i ]);
	}
}

/*
===================
SV_EconomyRoundRestart

Called from SV_InitGame( qtrue ) in sv_gameapi.cpp - qtrue there means a
routine round-to-round restart (SV_RestartGame, via the "map_restart 0"
MBII's own ExitLevel() queues once a round concludes), not a fresh map load
(SV_InitGame( qfalse ), from SV_InitGameProgs/SV_SpawnServer). Engine-owned
signal we control and have directly confirmed fires on every real round
restart - see the comment where this replaced the old CS_SIEGE_STATE-polling
approach in SV_EconomyFrame below for why that one didn't work.
===================
*/
void SV_EconomyRoundRestart( void ) {
	int i;

	if ( !SV_EconomyEnabled() ) {
		return;
	}

	for ( i = 0; i < sv_maxclients->integer; i++ ) {
		client_t *cl = &svs.clients[i];

		if ( cl->state >= CS_ACTIVE && cl->economyHandle[0] ) {
			cl->economyCredits += kEconomyRoundReward;
			SV_EconomyPrint( cl, va( "Round credit: +%d credits (balance: %d)", kEconomyRoundReward, cl->economyCredits ) );
			SV_EconomyPersistCredits( cl );
		}
	}
}

/*
===================
SV_EconomyFrame

Runs once per server frame. Detects death transitions and awards the
attacker (PERS_ATTACKER) the kill reward + any bounty on the victim.
Must run once per frame — not per client packet — otherwise the first
client packet of the frame consumes the death transition and later
attackers get nothing.
===================
*/
// For anyone not logged in, while g_economyLoginReminder is on: a centre
// screen banner shortly after they first spawn in ("!login to use the
// Cantina / or !register for 100 free credits"), then, once they've been
// in g_economyLoginReminder seconds, a chat reminder naming what credits
// are for here (only the features switched on). Each once per connection:
// map changes keep the flags.
static void SV_EconomyLoginReminders( void ) {
	const int delayMs = ( g_economyLoginReminder ? g_economyLoginReminder->integer : 0 ) * 1000;

	if ( delayMs <= 0 ) {
		return;
	}
	for ( int i = 0; i < sv_maxclients->integer; i++ ) {
		client_t *cl = &svs.clients[i];

		if ( cl->state != CS_ACTIVE || ( cl->economyReminded && cl->economySpawnBannerShown ) ||
			cl->netchan.remoteAddress.type == NA_BOT ) {
			continue;
		}
		if ( cl->economyHandle[0] ) {
			cl->economyReminded = qtrue;	// logged in - nothing to remind
			cl->economySpawnBannerShown = qtrue;
			continue;
		}

		const int bonus = g_economyRegisterBonus ? Q_max( 0, g_economyRegisterBonus->integer ) : 0;
		if ( !cl->economySpawnBannerShown ) {
			if ( !cl->economySpawnBannerAt ) {
				if ( SV_ClientIsSpawned( cl ) ) {
					cl->economySpawnBannerAt = svs.time + 1500;	// after MBII's own spawn messages
				}
			} else if ( svs.time >= cl->economySpawnBannerAt ) {
				const char *use = ( g_socialMode && g_socialMode->integer ) ? "use the Cantina" : "earn and spend credits";
				cl->economySpawnBannerShown = qtrue;
				if ( bonus > 0 ) {
					SV_SendServerCommand( cl, "cp \"^2!login ^7to %s\n^7or ^2!register ^7for ^2%d free credits\"\n", use, bonus );
				} else {
					SV_SendServerCommand( cl, "cp \"^2!login ^7to %s\n^7or ^2!register ^7to join in\"\n", use );
				}
			}
		}
		if ( cl->economyReminded ) {
			continue;
		}
		if ( !cl->economyReminderAt ) {
			cl->economyReminderAt = svs.time + delayMs;
			continue;
		}
		if ( svs.time < cl->economyReminderAt ) {
			continue;
		}
		cl->economyReminded = qtrue;

		if ( bonus > 0 ) {
			SV_SendServerCommand( cl, "chat \"^5Hey %s^7! ^2!register <name> <pin> ^7for ^2%d free credits^7 - or ^2!login ^7if you've played before.\"\n", cl->name, bonus );
		} else {
			SV_SendServerCommand( cl, "chat \"^5Hey %s^7! ^2!register <name> <pin> ^7to start earning credits - or ^2!login ^7if you've played before.\"\n", cl->name );
		}

		// What they're for, from whatever's on here, most eye-catching first.
		static const struct { cvar_t **enable; const char *what; } kUses[] = {
			{ &g_economyBarEnable, "^5!bar ^7drinks" },
			{ &g_economyBlackjackEnable, "^5!blackjack" },
			{ &g_economyBetEnable, "^5!bet ^7on duels" },
			{ &g_economyPazaakEnable, "^5!pazaak" },
			{ &g_economyJukeboxEnable, "^5!jukebox" },
			{ &g_economyChanceEnable, "^5!chance" },
			{ &g_economyRaffleEnable, "^5!raffle" },
			{ &g_economyShopEnable, "^5!buy ^7gear" },
			{ &g_economyBountyEnable, "^5!bounty" },
		};
		char uses[160] = "";
		int count = 0;
		for ( size_t u = 0; u < ARRAY_LEN( kUses ) && count < 4; u++ ) {
			if ( *kUses[u].enable && ( *kUses[u].enable )->integer ) {
				Q_strcat( uses, sizeof( uses ), va( "%s%s", count ? "^7, " : "", kUses[u].what ) );
				count++;
			}
		}
		if ( count ) {
			SV_SendServerCommand( cl, "chat \"^7Spend them on %s^7 and more - ^5!help ^7shows everything.\"\n", uses );
		}
	}
}

void SV_EconomyFrame( void ) {
	int i;

	// Drip out any queued menu lines that are due (see SV_EconomyMenuPump
	// above) - unconditionally, BEFORE the economy-enabled check below, and
	// not just for CS_ACTIVE clients. "!help" now uses this same paced
	// queue and works even when the economy system is fully disabled (see
	// its handler) - if this pump loop were gated behind SV_EconomyEnabled()
	// like the rest of this function, a disabled instance would send only
	// !help's first line (from the explicit pump in its handler) and then
	// silently never drain the rest, since nothing would ever call
	// SV_EconomyMenuPump again for that client.
	for ( i = 0; i < sv_maxclients->integer; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			SV_EconomyMenuPump( &svs.clients[i] );
		}
	}

	if ( !SV_EconomyEnabled() ) {
		return;
	}

	SV_EconomyLoginReminders();

	// Pick up outside balance changes (web panel gifts, other servers)
	// every few seconds - one fresh load of the shared file, then a merge
	// for each logged-in session.
	static int nextCreditSync = 0;
	if ( svs.time >= nextCreditSync ) {
		nextCreditSync = svs.time + 5000;
		SV_EconomyAccountsEnsureLoaded();
		for ( i = 0; i < sv_maxclients->integer; i++ ) {
			client_t *cl = &svs.clients[i];
			int a;

			if ( cl->state < CS_ACTIVE || !cl->economyHandle[0] ) {
				continue;
			}
			for ( a = 0; a < svEconomyAccountCount; a++ ) {
				if ( !Q_stricmp( svEconomyAccounts[a].handle, cl->economyHandle ) ) {
					SV_EconomyMergeExternal( cl, &svEconomyAccounts[a] );
					break;
				}
			}
		}
	}

	// Broadcast economy mode announcement every 3 minutes
	static int nextAnnounce = 0;
	if (svs.time >= nextAnnounce) {
		nextAnnounce = svs.time + 180000;
		SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "^3This server uses our Economy Credit System^7, type ^3!help^7 in chat for more info\"\n");
	}

	// Round-restart credit used to live here, polled per-frame off
	// CS_SIEGE_STATE. Live-tested bug, found 2026-09-16: added debug logging
	// that printed on every observed CS_SIEGE_STATE change, across multiple
	// real rounds (including a full populated legends match with a genuine
	// objective-completion round end) - it never printed once. The
	// configstring approach was verified against the moviebattles-2 source
	// dump, but that dump is explicitly not guaranteed to match the actual
	// compiled qagame module this server loads, and live evidence says it
	// doesn't: the value never changes, at all, ever, even at round start.
	// Replaced with SV_EconomyRoundRestart() (below), called directly from
	// SV_InitGame( qtrue ) in sv_gameapi.cpp - engine-owned state we control
	// and have directly confirmed fires on every real round-restart, rather
	// than polling a game-module-owned value that isn't behaving as its own
	// source describes.

	// Detection history: this used to award a kill reward (and consume any
	// bounty) purely off stats[STAT_HEALTH] crossing zero, cross-referenced
	// against persistant[PERS_ATTACKER]. Live-tested bug: MBII's round
	// transition can produce a health readout that crosses zero on its own,
	// with PERS_ATTACKER still holding a stale value from the *previous*
	// round - so a round restart could hand out a free kill reward (and
	// silently consume a bounty) with no actual kill involved. An interim
	// fix gated the award on the attacker's own PERS_SCORE having also gone
	// up that frame; this version replaces that with a signal verified
	// directly against the real MBII source (moviebattles-2, not compiled
	// into this build but available to read): G_Damage()/player_die() set
	// persistant[PERS_ATTACKER] and pm_type together, synchronously, on
	// every genuine death, and PERS_ATTACKER is never set to a mere
	// assister - only whoever actually dealt the damage. Requiring
	// pm_type == PM_DEAD (this project's own symbol - see killstreak.cpp's
	// header for why moviebattles-2's own pmtype_t numbering isn't trusted
	// here, only its logic) alongside the health check rules out the round-
	// transition case directly, without needing to track or compare the
	// attacker's own score at all - which also means no cross-client
	// baseline dependency, so a single pass over each client's own previous
	// health is enough; there's no ordering hazard to design around the way
	// there would be if this needed another client's state to already be
	// up to date.
	for ( i = 0; i < sv_maxclients->integer; i++ ) {
		client_t *victim = &svs.clients[i];
		playerState_t *vps;
		int health;

		if ( victim->state < CS_ACTIVE || !victim->gentity || !victim->gentity->playerState ) {
			continue;
		}

		vps = victim->gentity->playerState;
		health = vps->stats[STAT_HEALTH];

		if ( !victim->economyHealthInitialized ) {
			victim->economyLastHealth = health;
			victim->economyHealthInitialized = qtrue;
			continue;
		}

		if ( victim->economyLastHealth > 0 && health <= 0 && vps->pm_type == MB2_PM_DEAD ) {
			const int attackerNum = vps->persistant[PERS_ATTACKER];

			if ( attackerNum >= 0 && attackerNum < sv_maxclients->integer && attackerNum != i ) {
				client_t *attacker = &svs.clients[attackerNum];

				// Credits are only ever earned by a logged-in account (economyHandle
				// set via !register/!login) - an unregistered attacker's kill simply
				// isn't rewarded, and any bounty on the victim is left intact rather
				// than being consumed by a kill nobody could actually collect it from.
				// No kill rewards on a social server: the only kills there are
				// duels, and two friends could duel each other for credits
				// all night.
				if ( attacker->state >= CS_ACTIVE && attacker->gentity && attacker->gentity->playerState &&
					!( g_socialMode && g_socialMode->integer ) ) {
					if ( attacker->economyHandle[0] ) {
						attacker->economyCredits += kEconomyKillReward;
						SV_EconomyPrint( attacker, va( "Kill reward: +%d credits (balance: %d)", kEconomyKillReward, attacker->economyCredits ) );

						if ( victim->economyBounty > 0 ) {
							const int payout = victim->economyBounty;
							const int placerNum = victim->economyBountyPlacerNum;
							char placerName[MAX_NAME_LENGTH];
							Q_strncpyz( placerName, victim->economyBountyPlacerName, sizeof( placerName ) );

							SV_EconomyBountyClear( victim );
							attacker->economyCredits += payout;
							SV_EconomyPrint( attacker, va( "You won %s's bounty of %d credits!", victim->name, payout ) );
							SV_EconomyResultBanner( attacker, "Bounty claimed", payout );
							SV_EconomyPrint( victim, "Your bounty was claimed." );

							// Let whoever placed it know it paid off, if
							// they're still around - reverify identity the
							// same way every other slot-number lookup in
							// this file does, since the slot could have been
							// reused by someone else entirely since they
							// placed it. Skipped if the placer is the same
							// person who just got the kill - they already
							// got the "You won" message above, so a second
							// "claimed your own bounty" message would just
							// be a confusing duplicate.
							if ( placerNum >= 0 && placerNum < sv_maxclients->integer && placerNum != attackerNum &&
								placerName[0] && svs.clients[placerNum].state >= CS_CONNECTED &&
								!Q_stricmp( svs.clients[placerNum].name, placerName ) ) {
								SV_EconomyPrint( &svs.clients[placerNum],
									va( "%s claimed your bounty on %s for %d credits.", attacker->name, victim->name, payout ) );
							}
						}

						SV_EconomyPersistCredits( attacker );
					} else if ( victim->economyBounty > 0 ) {
						// Bounty is left uncollected (see comment above) - but
						// silently losing out on it with no explanation is a
						// bad surprise, so tell them what they missed and how
						// to actually get it next time.
						SV_EconomyPrint( attacker, va( "You won %s's bounty (%d credits) but aren't logged in to receive it! Use !register or !login.",
							victim->name, victim->economyBounty ) );
					}
				}
			}
		}

		victim->economyLastHealth = health;
	}
}

/*
===================
SV_GunrayCheckFrame

The Nute Gunray siege class (model "gunray/default", siege class token
"v7_NuteG" - confirmed live from a real player's MB2_CS_PLAYERS broadcast
string, e.g. legends-engine.log: cs 1231 "n\...\t\3\m\gunray/default\
c1\0\c2\0\sc\v7_NuteG\...") reportedly causes server crashes. This scans
every active client's own MB2_CS_PLAYERS configstring (engine-owned, the
same broadcast string every client's HUD/scoreboard already depends on)
once per frame.

On the transition into that model: broadcasts a server-wide warning
(everyone sees it, not just the offending player - people nearby should
know why someone's about to vanish from the fight) and starts a 5-second
grace timer (client_t::gunraySpecTime) rather than yanking them
immediately, so the warning has time to actually be read. If they switch
off the class themselves before the timer elapses, the pending force is
cancelled (see the isGunray-false branch below) rather than firing anyway
against a class they're no longer playing.

The actual force, once the timer expires, injects "team s" (MBII's single-letter spectator command - see below) as if
the client had typed it themselves (SV_ExecuteClientCommand) - the same
real client-command path this always goes through when a player types it, not a
novel injection like the vehicle-spawn feature's admin-only "npc spawn
vehicle", so no reason to expect the same kind of same-frame-combination
crash that one had.

Checks the "m" (model) key specifically via Info_ValueForKey, not a raw
substring search over the whole configstring - that string also embeds the
player's own name (the "n" key), and a player named e.g. "xXgunrayXx"
would false-positive a plain text search.

Live-tested bug #1: forcing "team spectator" changes the "t" (team) key in
this same broadcast string, but leaves "m" holding the stale last-selected
model - it doesn't go back to empty until they actually pick a new class
next time they join a team. Checking "m" alone meant wasGunray[i] never
went back to false once forced to spectator, so re-picking Gunray a second
time produced no new edge to detect (isGunray was already true both
before and after - nothing to transition into). Now also requires "t" !=
TEAM_SPECTATOR (see bg_public.h team_t - identical between this engine and
the real MBII source, unlike pm_type/CS_PLAYERS, so no MB2_ prefix needed
here) - being in spectator always counts as "not currently Gunray"
regardless of what the stale model field says, which correctly resets the
edge detector for the next pick.

Live-tested bug #2: the single injection at T+5s was silently rejected,
every attempt, indefinitely (still failing after 8+ retries a full 8+
seconds later, which ruled out a mere timing/cooldown race). Actual root
cause, found by reading Cmd_Team_f in the real MBII source (g_cmds.c):
this mod's "team" command only accepts the literal single letter "s" for
spectator (`if (strcmp(s, "s")) return;`) - not the standard "spectator"
this code was originally sending, which failed that exact-match check and
returned immediately every time, with zero feedback. Fixed by sending
"team s" instead.

The retry-every-second loop (rather than firing once) is kept regardless -
Cmd_Team_f's own switchTeamTime cooldown (5s after any real team switch,
same file) is a real, separate reason a single well-formed attempt could
still land inside an active cooldown and need a second try, even with the
right argument now.
===================
*/
void SV_GunrayCheckFrame( void ) {
	static qboolean wasGunray[MAX_CLIENTS];
	int i;

	for ( i = 0; i < sv_maxclients->integer; i++ ) {
		client_t *cl = &svs.clients[i];
		char csBuf[MAX_STRING_CHARS];
		char model[128];
		char team[128];
		qboolean isGunray;

		if ( cl->state < CS_ACTIVE ) {
			wasGunray[i] = qfalse;
			cl->gunraySpecTime = 0;
			continue;
		}

		SV_GetConfigstring( MB2_CS_PLAYERS + i, csBuf, sizeof( csBuf ) );
		// Info_ValueForKey returns a pointer into its own small pool of
		// static buffers, reused (round-robin) on every call - copying out
		// immediately avoids a later call in this same scope (even just
		// another Info_ValueForKey for a different key) silently
		// overwriting a value this code is still holding a pointer to.
		Q_strncpyz( model, csBuf[0] ? Info_ValueForKey( csBuf, "m" ) : "", sizeof( model ) );
		Q_strncpyz( team, csBuf[0] ? Info_ValueForKey( csBuf, "t" ) : "", sizeof( team ) );
		// Any blocked class (kBlockedClasses), by its "sc" class token -
		// Gunray used to be recognised by his model alone.
		char sc[128];
		Q_strncpyz( sc, csBuf[0] ? Info_ValueForKey( csBuf, "sc" ) : "", sizeof( sc ) );
		const char *blockedName = SV_BlockedClassName( sc );
		if ( !blockedName && !Q_stricmp( model, "gunray/default" ) ) {
			blockedName = "Nute Gunray";
		}
		isGunray = ( blockedName && atoi( team ) != TEAM_SPECTATOR ) ? qtrue : qfalse;

		if ( isGunray && !wasGunray[i] ) {
			Com_Printf( "[GunrayDebug] %s (slot %d) picked %s (model=%s, sc=%s) - forcing to spectator in 5s\n",
				cl->name, i, blockedName, model, sc );
			SV_SendServerCommand( NULL, "chat \"^1%s^7 %s is disabled on this server, please choose another class.\"\n", cl->name, blockedName );
			cl->gunraySpecTime = svs.time + 5000;
		} else if ( !isGunray && wasGunray[i] ) {
			Com_Printf( "[GunrayDebug] %s (slot %d) is no longer on a blocked class - cancelling pending spectator force\n", cl->name, i );
			cl->gunraySpecTime = 0;
		}

		if ( isGunray && cl->gunraySpecTime && svs.time >= cl->gunraySpecTime ) {
			Com_Printf( "[GunrayDebug] %s (slot %d) - grace period elapsed, forcing to spectator (retry)\n", cl->name, i );
			// "team s" alone is not enough: Cmd_Team_f (g_cmds.c) only
			// calls SetTeam() if the round hasn't begun yet OR the player
			// is already out of respawns for this round
			// (pers.iRespawnsLeftInRound < 1) - otherwise it deliberately
			// no-ops (you can't abandon your team mid-round while you still
			// have lives, by design). Live-tested: a real player still
			// holding respawns gets silently ignored exactly like our
			// injection did. "kill" (Cmd_Kill_f) has no such gate - only
			// blocked if already dead or already spectator - so inject
			// that too: it removes the live crash-risk window immediately
			// (a dead player isn't actively playing as Gunray) and, once
			// enough kills exhaust their round respawns, "team s" will
			// finally succeed on its own on a later retry.
			SV_ExecuteClientCommand( cl, "kill", qtrue );
			SV_ExecuteClientCommand( cl, "team s", qtrue );
			// Retry in 1s rather than clearing to 0 - keep going (killing
			// them again each retry if they respawn as Gunray again, and
			// re-attempting the team switch) until isGunray actually reads
			// false, which is what really clears gunraySpecTime (see the
			// isGunray-false branch above).
			cl->gunraySpecTime = svs.time + 1000;
		}

		wasGunray[i] = isGunray;
	}
}


/*
===========================================================================

USER CMD EXECUTION

===========================================================================
*/

/*
===================
SV_ExecuteClientMessage

Parse a client packet
===================
*/
void SV_ExecuteClientMessage( client_t *cl, msg_t *msg ) {
	int			c;
	int			serverId;

	MSG_Bitstream(msg);

	serverId = MSG_ReadLong( msg );
	cl->messageAcknowledge = MSG_ReadLong( msg );

	if (cl->messageAcknowledge < 0) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
		//SV_DropClient( cl, "illegible client message" );
		return;
	}

	cl->reliableAcknowledge = MSG_ReadLong( msg );

	// NOTE: when the client message is fux0red the acknowledgement numbers
	// can be out of range, this could cause the server to send thousands of server
	// commands which the server thinks are not yet acknowledged in SV_UpdateServerCommandsToClient
	if (cl->reliableAcknowledge < cl->reliableSequence - MAX_RELIABLE_COMMANDS) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
		//SV_DropClient( cl, "illegible client message" );
		cl->reliableAcknowledge = cl->reliableSequence;
		return;
	}
	// if this is a usercmd from a previous gamestate,
	// ignore it or retransmit the current gamestate
	//
	// if the client was downloading, let it stay at whatever serverId and
	// gamestate it was at.  This allows it to keep downloading even when
	// the gamestate changes.  After the download is finished, we'll
	// notice and send it a new game state
	//
	// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=536
	// don't drop as long as previous command was a nextdl, after a dl is done, downloadName is set back to ""
	// but we still need to read the next message to move to next download or send gamestate
	// I don't like this hack though, it must have been working fine at some point, suspecting the fix is somewhere else
	if ( serverId != sv.serverId && !*cl->downloadName && !strstr(cl->lastClientCommandString, "nextdl") ) {
		if ( serverId >= sv.restartedServerId && serverId < sv.serverId ) { // TTimo - use a comparison here to catch multiple map_restart
			// they just haven't caught the map_restart yet
			Com_DPrintf("%s : ignoring pre map_restart / outdated client message\n", cl->name);
			return;
		}
		// if we can tell that the client has dropped the last
		// gamestate we sent them, resend it
		// Fix for https://bugzilla.icculus.org/show_bug.cgi?id=6324
		if ( cl->state != CS_ACTIVE && cl->messageAcknowledge > cl->gamestateMessageNum ) {
			Com_DPrintf( "%s : dropped gamestate, resending\n", cl->name );
			SV_SendClientGameState( cl );
		}
		return;
	}

	// this client has acknowledged the new gamestate so it's
	// safe to start sending it the real time again
	if( cl->oldServerTime && serverId == sv.serverId ) {
		Com_DPrintf( "%s acknowledged gamestate\n", cl->name );
		cl->oldServerTime = 0;
	}

	// read optional clientCommand strings
	do {
		c = MSG_ReadByte( msg );

		if ( c == clc_EOF ) {
			break;
		}
		if ( c != clc_clientCommand ) {
			break;
		}
		if ( !SV_ClientCommand( cl, msg ) ) {
			return;	// we couldn't execute it because of the flood protection
		}
		if (cl->state == CS_ZOMBIE) {
			return;	// disconnect command
		}
	} while ( 1 );
	
	// read the usercmd_t
	if ( c == clc_move ) {
		SV_UserMove( cl, msg, qtrue );
	} else if ( c == clc_moveNoDelta ) {
		SV_UserMove( cl, msg, qfalse );
	} else if ( c != clc_EOF ) {
		Com_Printf( "WARNING: bad command byte for client %i\n", cl - svs.clients );
	}
//	if ( msg->readcount != msg->cursize ) {
//		Com_Printf( "WARNING: Junk at end of packet for client %i\n", cl - svs.clients );
//	}
}

