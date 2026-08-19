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

#ifdef USE_INTERNAL_ZLIB
#include "zlib/zlib.h"
#else
#include <zlib.h>
#endif

#include "server/sv_gameapi.h"

static const int kEconomyKillReward = 5;


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

#define ECONOMY_ACCOUNTS_FILE		"economy_accounts.dat"
#define ECONOMY_MAX_ACCOUNTS		1024
#define ECONOMY_HANDLE_SIZE			24	// must match client_t::economyHandle
#define ECONOMY_PIN_LEN				4
#define ECONOMY_SALT_SIZE			16
#define ECONOMY_HASH_SIZE			MD5_DIGEST_SIZE
#define ECONOMY_LOGIN_MAX_ATTEMPTS	5
#define ECONOMY_LOGIN_LOCKOUT_MS	60000

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
static qboolean svEconomyAccountsLoaded = qfalse;

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

static void SV_EconomyAccountsLoad( void ) {
	int filelen;
	fileHandle_t f;
	char *buf, *line, *nextline;
	char filepath[MAX_QPATH];

	svEconomyAccountCount = 0;
	svEconomyAccountsLoaded = qtrue;

	Com_sprintf( filepath, sizeof( filepath ), "%s/%s", FS_GetCurrentGameDir(), ECONOMY_ACCOUNTS_FILE );

	filelen = FS_SV_FOpenFileRead( filepath, &f );
	if ( filelen <= 0 ) {
		if ( f ) {
			FS_FCloseFile( f );
		}
		return;
	}

	buf = (char *)Z_Malloc( filelen + 1, TAG_TEMP_WORKSPACE );
	filelen = FS_Read( buf, filelen, f );
	FS_FCloseFile( f );
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

static void SV_EconomyAccountsEnsureLoaded( void ) {
	if ( !svEconomyAccountsLoaded ) {
		SV_EconomyAccountsLoad();
	}
}

static void SV_EconomyAccountsSave( void ) {
	fileHandle_t f;
	char filepath[MAX_QPATH];
	int i;

	Com_sprintf( filepath, sizeof( filepath ), "%s/%s", FS_GetCurrentGameDir(), ECONOMY_ACCOUNTS_FILE );

	f = FS_SV_FOpenFileWrite( filepath );
	if ( !f ) {
		Com_Printf( "SV_EconomyAccountsSave: failed to open %s for writing\n", filepath );
		return;
	}

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
		FS_Write( line, len, f );
	}

	FS_FCloseFile( f );
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
void SV_EconomyPersistCredits( client_t *cl ) {
	economyAccount_t *acct;

	if ( !cl->economyHandle[0] ) {
		return;
	}

	acct = SV_EconomyFindAccount( cl->economyHandle );
	if ( !acct ) {
		return;
	}

	acct->credits = cl->economyCredits;
	SV_EconomyAccountsSave();
}

static qboolean SV_EconomyEnabled( void ) {
	return (Cvar_VariableIntegerValue("g_creditSystemEnable") == 1) ? qtrue : qfalse;
}

static void SV_EconomyPrint( client_t *cl, const char *text ) {
	SV_SendServerCommand( cl, "chat \"^2[Economy]^7 %s\"\n", text );
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
		return qfalse;
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

	if ( !SV_EconomyEnabled() &&
		( !Q_stricmp( commandName, "balance" ) ||
		  !Q_stricmp( commandName, "buy" ) ||
		  !Q_stricmp( commandName, "bounty" ) ||
		  !Q_stricmp( commandName, "bountry" ) ||
		  !Q_stricmp( commandName, "register" ) ||
		  !Q_stricmp( commandName, "login" ) ||
		  !Q_stricmp( commandName, "help" ) ) ) {
		SV_EconomyPrint( cl, "Credit system is disabled." );
		return qtrue;
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

	if ( !Q_stricmp( commandName, "buy" ) ) {
		if ( !SV_EconomyEnabled() ) {
			SV_EconomyPrint( cl, "Credit system is disabled." );
			return qtrue;
		}

		if ( sscanf( chatCursor, "%31s", firstArg ) != 1 ) {
			char menuBuf[1024];
			int  menuLen = 0;
			int  c;
			qboolean firstShown = qtrue;

			menuLen += Com_sprintf( menuBuf + menuLen, sizeof(menuBuf) - menuLen,
				"^3=== SHOP === Balance: ^2%d ^3credits ===\n^7Categories: ", cl->economyCredits );
			for ( c = 0; c < (int)ARRAY_LEN( svEconomyShopCategories ); c++ ) {
				if ( !SV_EconomyCategoryExists( svEconomyShopCategories[c] ) ) {
					continue;
				}
				menuLen += Com_sprintf( menuBuf + menuLen, sizeof(menuBuf) - menuLen,
					"%s^5%s", firstShown ? "" : "^7, ", svEconomyShopCategories[c] );
				firstShown = qfalse;
			}
			menuLen += Com_sprintf( menuBuf + menuLen, sizeof(menuBuf) - menuLen,
				"\n^7Type ^5!buy <category> ^7to view items, ^5!buy <name> ^7to purchase." );
			SV_SendServerCommand( cl, "chat \"%s\"\n", menuBuf );
			return qtrue;
		}

		if ( SV_EconomyCategoryExists( firstArg ) ) {
			char catBuf[1024];
			int  catLen = 0;

			catLen += Com_sprintf( catBuf + catLen, sizeof(catBuf) - catLen,
				"^3=== SHOP: %s === Balance: ^2%d ^3===\n", firstArg, cl->economyCredits );

			for ( i = 0; i < (int)ARRAY_LEN( svEconomyItemDefs ); i++ ) {
				if ( Q_stricmp( svEconomyItemDefs[i].category, firstArg ) ) {
					continue;
				}
				if ( !SV_EconomyItemEnabled( i ) ) {
					continue;
				}
				catLen += Com_sprintf( catBuf + catLen, sizeof(catBuf) - catLen,
					"^7%s ^7- ^2%d ^7cr\n", svEconomyItemDefs[i].name, SV_EconomyItemCost( i ) );
			}

			catLen += Com_sprintf( catBuf + catLen, sizeof(catBuf) - catLen,
				"^3Type !buy <name> to purchase" );
			SV_SendServerCommand( cl, "chat \"%s\"\n", catBuf );
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

	if ( !Q_stricmp( commandName, "bounty" ) || !Q_stricmp( commandName, "bountry" ) ) {
		int targetNum;
		int amount;

		if ( !SV_EconomyEnabled() ) {
			SV_EconomyPrint( cl, "Credit system is disabled." );
			return qtrue;
		}

		if ( sscanf( chatCursor, "%63s %63s", firstArg, secondArg ) != 2 ) {
			char bBuf[1024];
			int  bLen = 0;
			bLen += Com_sprintf( bBuf + bLen, sizeof(bBuf) - bLen,
				"^3=== BOUNTIES === Your bounty: ^1%d ^3credits ===\n", cl->economyBounty );
			bLen += Com_sprintf( bBuf + bLen, sizeof(bBuf) - bLen,
				"^3Usage: ^7!bounty <id> <credits>\n" );
			for ( i = 0; i < sv_maxclients->integer; i++ ) {
				client_t *target = &svs.clients[i];
				if ( target->state >= CS_CONNECTED && target != cl ) {
					if ( target->economyBounty > 0 ) {
						bLen += Com_sprintf( bBuf + bLen, sizeof(bBuf) - bLen,
							"^7%d) ^5%s ^7- bounty: ^1%d\n", i, target->name, target->economyBounty );
					} else {
						bLen += Com_sprintf( bBuf + bLen, sizeof(bBuf) - bLen,
							"^7%d) ^5%s\n", i, target->name );
					}
				}
			}
			SV_SendServerCommand( cl, "chat \"%s\"\n", bBuf );
			return qtrue;
		}

		targetNum = atoi( firstArg );
		amount = atoi( secondArg );

		if ( targetNum < 0 || targetNum >= sv_maxclients->integer ) {
			SV_EconomyPrint( cl, "Invalid target clientnum." );
			return qtrue;
		}

		if ( amount <= 0 ) {
			SV_EconomyPrint( cl, "Bounty must be greater than 0." );
			return qtrue;
		}

		if ( &svs.clients[targetNum] == cl ) {
			SV_EconomyPrint( cl, "You cannot place a bounty on yourself." );
			return qtrue;
		}

		if ( svs.clients[targetNum].state < CS_CONNECTED ) {
			SV_EconomyPrint( cl, "Target player is not connected." );
			return qtrue;
		}

		if ( cl->economyCredits < amount ) {
			SV_EconomyPrint( cl, va( "Not enough credits. You have %d.", cl->economyCredits ) );
			return qtrue;
		}

		cl->economyCredits -= amount;
		svs.clients[targetNum].economyBounty += amount;
		SV_EconomyPersistCredits( cl );
		SV_EconomyPrint( cl, va( "Placed %d credit bounty on %s.", amount, svs.clients[targetNum].name ) );
		SV_EconomyPrint( &svs.clients[targetNum], va( "Someone placed a %d credit bounty on you.", amount ) );
		return qtrue;
	}

	if ( !Q_stricmp( commandName, "register" ) ) {
		int argCount = sscanf( chatCursor, "%23s %15s", firstArg, secondArg );

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

		if ( SV_EconomyFindAccount( firstArg ) ) {
			SV_EconomyPrint( cl, "That handle is taken. Choose another." );
			return qtrue;
		}

		if ( svEconomyAccountCount >= ECONOMY_MAX_ACCOUNTS ) {
			SV_EconomyPrint( cl, "Account storage is full. Contact an admin." );
			return qtrue;
		}

		{
			economyAccount_t *acct = &svEconomyAccounts[svEconomyAccountCount];
			Com_Memset( acct, 0, sizeof( *acct ) );

			if ( !Sys_RandomBytes( acct->salt, ECONOMY_SALT_SIZE ) ) {
				SV_EconomyPrint( cl, "Registration failed (RNG error). Try again." );
				return qtrue;
			}

			svEconomyAccountCount++;
			Q_strncpyz( acct->handle, firstArg, sizeof( acct->handle ) );
			SV_EconomyHashPin( acct->salt, secondArg, acct->hash );
			acct->credits = cl->economyCredits;

			Q_strncpyz( cl->economyHandle, acct->handle, sizeof( cl->economyHandle ) );
			SV_EconomyAccountsSave();

			SV_EconomyPrint( cl, va( "Registered! Logged in as '%s'. Use !login %s <pin> on future connects.", acct->handle, acct->handle ) );
		}
		return qtrue;
	}

	if ( !Q_stricmp( commandName, "login" ) ) {
		economyAccount_t *acct;
		byte candidateHash[ECONOMY_HASH_SIZE];
		int c;

		if ( sscanf( chatCursor, "%23s %15s", firstArg, secondArg ) != 2 ) {
			SV_EconomyPrint( cl, "Usage: !login <handle> <pin>" );
			return qtrue;
		}

		acct = SV_EconomyFindAccount( firstArg );
		if ( !acct ) {
			SV_EconomyPrint( cl, "No account with that handle." );
			return qtrue;
		}

		if ( acct->lockoutUntil > 0 && svs.time < acct->lockoutUntil ) {
			SV_EconomyPrint( cl, va( "Too many failed attempts. Try again in %d seconds.",
				( acct->lockoutUntil - svs.time + 999 ) / 1000 ) );
			return qtrue;
		}

		if ( !SV_EconomyValidatePin( secondArg ) ) {
			SV_EconomyPrint( cl, "Incorrect PIN." );
			return qtrue;
		}

		SV_EconomyHashPin( acct->salt, secondArg, candidateHash );

		if ( !SV_EconomySecureCompare( acct->hash, candidateHash, ECONOMY_HASH_SIZE ) ) {
			acct->failedAttempts++;
			if ( acct->failedAttempts >= ECONOMY_LOGIN_MAX_ATTEMPTS ) {
				acct->lockoutUntil = svs.time + ECONOMY_LOGIN_LOCKOUT_MS;
				acct->failedAttempts = 0;
				SV_EconomyAccountsSave();
				SV_EconomyPrint( cl, "Too many failed attempts. Account locked for 60 seconds." );
			} else {
				SV_EconomyAccountsSave();
				SV_EconomyPrint( cl, "Incorrect PIN." );
			}
			return qtrue;
		}

		// success: clear any lockout state and kick any other session already logged into this handle
		acct->failedAttempts = 0;
		acct->lockoutUntil = 0;

		for ( c = 0; c < sv_maxclients->integer; c++ ) {
			client_t *other = &svs.clients[c];
			if ( other != cl && other->state >= CS_CONNECTED && other->economyHandle[0] &&
				!Q_stricmp( other->economyHandle, acct->handle ) ) {
				SV_EconomyPrint( other, "You were logged out because your account logged in elsewhere." );
				other->economyHandle[0] = '\0';
			}
		}

		Q_strncpyz( cl->economyHandle, acct->handle, sizeof( cl->economyHandle ) );
		cl->economyCredits = acct->credits;
		SV_EconomyAccountsSave();

		SV_EconomyPrint( cl, va( "Logged in as '%s'. Balance: %d credits.", acct->handle, cl->economyCredits ) );
		return qtrue;
	}

	if ( !Q_stricmp( commandName, "help" ) ) {
		SV_SendServerCommand( cl, "chat \""
			"^3=== CREDIT SYSTEM HELP ===\n"
			"^2!balance\n"
			"^7  Show your credits and your current bounty.\n"
			"^2!buy\n"
			"^7  Type ^5!buy ^7to list shop categories, ^5!buy <category> ^7to view items,\n"
			"^7  and ^5!buy <name> ^7to purchase (e.g. ^5!buy bryar^7, ^5!buy jetpack^7).\n"
			"^2!bounty\n"
			"^7  List all players and active bounties.\n"
			"^7  Type ^5!bounty <id> <credits> ^7to place a bounty.\n"
			"^7  The player who kills them earns the bounty.\n"
			"^2!register / !login\n"
			"^7  Type ^5!register <handle> ^7to reserve a handle, then\n"
			"^7  ^5!register <handle> <pin> ^7to set a 4-digit PIN and save your credits.\n"
			"^7  Type ^5!login <handle> <pin> ^7on a new connection to restore your balance.\n"
			"^3Credits are earned by getting kills.^7\"\n"
		);
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


/*
==================
SV_ClientThink

Also called by bot code
==================
*/
void SV_ClientThink (client_t *cl, usercmd_t *cmd) {
	cl->lastUsercmd = *cmd;

	if ( cl->state != CS_ACTIVE ) {
		return;		// may have been kicked during the last usercmd
	}

	GVM_ClientThink( cl - svs.clients, NULL );
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
SV_EconomyFrame

Runs once per server frame. Detects death transitions and awards the
attacker (PERS_ATTACKER) the kill reward + any bounty on the victim.
Must run once per frame — not per client packet — otherwise the first
client packet of the frame consumes the death transition and later
attackers get nothing.
===================
*/
void SV_EconomyFrame( void ) {
	int i;

	if ( !SV_EconomyEnabled() ) {
		return;
	}

	// Broadcast economy mode announcement every 3 minutes
	static int nextAnnounce = 0;
	if (svs.time >= nextAnnounce) {
		nextAnnounce = svs.time + 180000;
		SV_SendServerCommand(NULL, "chat \"" SVSAY_PREFIX "^3This server uses our Economy Credit System^7, type ^3!help^7 in chat for more info\"\n");
	}

	for ( i = 0; i < sv_maxclients->integer; i++ ) {
		client_t *victim = &svs.clients[i];
		playerState_t *vps;
		int health;
		int attackerNum;

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

		if ( victim->economyLastHealth > 0 && health <= 0 ) {
			attackerNum = vps->persistant[PERS_ATTACKER];

			if ( attackerNum >= 0 && attackerNum < sv_maxclients->integer && attackerNum != i ) {
				client_t *attacker = &svs.clients[attackerNum];

				if ( attacker->state >= CS_ACTIVE && attacker->gentity && attacker->gentity->playerState ) {
					attacker->economyCredits += kEconomyKillReward;
					SV_EconomyPrint( attacker, va( "Kill reward: +%d credits (balance: %d)", kEconomyKillReward, attacker->economyCredits ) );

					if ( victim->economyBounty > 0 ) {
						const int payout = victim->economyBounty;
						victim->economyBounty = 0;
						attacker->economyCredits += payout;
						SV_EconomyPrint( attacker, va( "Bounty payout: +%d credits for %s", payout, victim->name ) );
						SV_EconomyPrint( victim, "Your bounty was claimed." );
					}

					SV_EconomyPersistCredits( attacker );
				}
			}
		}

		victim->economyLastHealth = health;
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

