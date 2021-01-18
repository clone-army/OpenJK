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
#include "game/bg_mb2.h"
#include "qcommon/stringed_ingame.h"
#include "qcommon/game_version.h"

/*
===============================================
SMOD Defines and Defs
===============================================
*/

typedef struct smod_function_s
{
	struct smod_function_s* next;
	char* name;
	char* description;
	int   level;
	int	  args;
	xcommand_t				function;
	completionFunc_t		complete;
} smod_function_t;

static	smod_function_t* smod_functions; // possible commands to execute

typedef std::vector<const smod_function_t*> SmodFuncVector;

/*
===============================================
SMOD Server Functions
===============================================
*/

/* Main SMOD Extensions Caller. Called by sv_client return qtrue to continue executing with legacy SMOD */
qboolean SV_Smod(client_t* cl, const char* s) {

	// Turns our string into args 
	Cmd_TokenizeString(s);

	// If logging in, allow to login, then return to legacy
	if (!strcmp(Cmd_Argv(1), "login")) {
		return SV_SmodLogin(cl);
	}

	// If logging out, allow to logout, then return to legacy
	if (!strcmp(Cmd_Argv(1), "logout")) {
		return SV_SmodLogout(cl);
	}

	// If still not logged in just send to legacy
	if (cl->smod == qfalse) {
		return qtrue;
	}
	else {

		// Load all the commands
		SV_SmodAddCmds();

		// Just SMOD was given, thus all we want is a list of commands
		if (Cmd_Argc() == 1) {
			std::thread(SV_SmodPrintCommands, cl).detach();

			// Continue SMOD Legacy
			return qtrue;
		}

		// Now executing a command
		smod_function_t* cmd, ** prev;

		for (prev = &smod_functions; *prev; prev = &cmd->next) {
			cmd = *prev;
			if (!Q_stricmp(Cmd_Argv(1), cmd->name)) {
				*prev = cmd->next;
				cmd->next = smod_functions;
				smod_functions = cmd;

				// perform the action
				if (!cmd->function) {
					// let the legacy SMOD handle it
					return qtrue;
				}
				else {
					if (cmd->level >= cl->smodLevel) {

						// Count includes SMOD and the Command
						if (Cmd_Argc() != (cmd->args + 2)) {
							SV_SendServerCommand(cl, "print \"%s\n\"\n", cmd->description);
						}
						else {
							cmd->function();
						}
					}
					else {
						SV_SendServerCommand(cl, "print \"%s\n\"\n", "You do not have permission to perform this command");
					}
					
				}
				// have client command stop execution
				return qfalse;
			}
		}
	}

	return qtrue;

}

/* Logs out the calling client */
qboolean SV_SmodLogout(client_t* cl) {

	cl->smod = qfalse;
	cl->smodLevel = 0;

	return qtrue;

}

/* Logs in the calling client */
qboolean SV_SmodLogin(client_t* cl) {

	// Return if already logged in
	if (cl->smod == qtrue) {
		return qtrue;
	}

	// If we are logging in, handle our own login 
	// Before handing back to legacy smod to also login
	if (!strcmp(Cmd_Argv(1), "login")) {

		if (Cmd_Argc() != 4) {
			return qtrue;
		}

		// Logging into smod 1
		if (!strcmp(Cmd_Argv(2), "1")) {
			if (!strcmp(Cmd_Argv(3), g_smodAdminPassword_1->string)) {
				SV_SendServerCommand(cl, "print \"^3Enabled SMOD Extensions Level 1^7\n\"\n");
				cl->smod = qtrue;
				cl->smodLevel = 1;
				return qtrue;
			}
		}

		// Logging into smod 2
		if (!strcmp(Cmd_Argv(2), "2")) {
			if (!strcmp(Cmd_Argv(3), g_smodAdminPassword_2->string)) {
				SV_SendServerCommand(cl, "print \"^3Enabled SMOD Extensions Level 2^7\n\"\n");
				cl->smod = qtrue;
				cl->smodLevel = 2;
				Com_Printf("You logged into SMOD Extensions as SMOD2");
				return qtrue;
			}
		}

		// Logging into smod 3
		if (!strcmp(Cmd_Argv(2), "3")) {
			if (!strcmp(Cmd_Argv(3), g_smodAdminPassword_3->string)) {
				SV_SendServerCommand(cl, "print \"^3Enabled SMOD Extensions Level 3^7\n\"\n");
				cl->smod = qtrue;
				cl->smodLevel = 3;
				Com_Printf("You logged into SMOD Extensions as SMOD3");
				return qtrue;
			}
		}

		// Return to legacy SMOD to give the bad news.
		return qtrue;

	}
	else {

		return qtrue;

	}

}

/* Prints all available SMOD commands for a logged in client */
void SV_SmodPrintCommands(client_t* cl)
{

	/* Why this? so when this command finishes, and SMOD prints it's legcy commands, this prints after not before */
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	const smod_function_t* cmd = NULL;
	int				i, j;
	char* match = NULL;
	SmodFuncVector	cmds;
	cmds.clear();

	if (Cmd_Argc() > 1) {
		match = Cmd_Argv(1);
	}

	for (cmd = smod_functions, i = 0, j = 0; cmd; cmd = cmd->next, i++)
	{
		if (!cmd->name || (match && !Com_Filter(match, cmd->name, qfalse)))
			continue;

		cmds.push_back(cmd);
		j++;
	}

	SV_SendServerCommand(cl, "print \"%s\n\"\n", "^3Avaliable Extension commands:\n------------------------^7\n");

	SmodFuncVector::const_iterator itr;
	for (itr = cmds.begin(); itr != cmds.end(); ++itr)
	{
		cmd = (*itr);
		if (VALIDSTRING(cmd->description)) {
			SV_SendServerCommand(cl, "print \"%s\n\"\n", cmd->description);
		}else {
			SV_SendServerCommand(cl, "print \"%s\n\"\n", cmd->description);
		}

	}

	return;
}

/* Finds a function with a given name */
smod_function_t* SV_SmodFindCommand(const char* cmd_name)
{
	smod_function_t* cmd;
	for (cmd = smod_functions; cmd; cmd = cmd->next)
		if (!Q_stricmp(cmd_name, cmd->name))
			return cmd;
	return NULL;
}

/* Adds a command so it can be executed */
void SV_SmodAddCmd(char* cmd_name, int level, xcommand_t function, int args, char* cmd_desc = "") {

	smod_function_t* cmd;

	/* Function already exists */
	if (SV_SmodFindCommand(cmd_name))
	{
		return;
	}

	cmd = (struct smod_function_s*)S_Malloc(sizeof(smod_function_t));
	cmd->name = CopyString(cmd_name);
	if (VALIDSTRING(cmd_desc))
		cmd->description = CopyString(cmd_desc);
	else
		cmd->description = NULL;
	cmd->function = function;
	cmd->complete = NULL;
	cmd->next = smod_functions;
	cmd->level = level;
	cmd->args = args;

	smod_functions = cmd;

}

/*
===============================================
SMOD Commands
===============================================
*/

/*
==================
SM_Slap - Slap a player as a punishment inflicting damage
==================
*/
static void SM_Slap(void) {

	client_t* cl = SV_BetterGetPlayerByHandle(Cmd_Argv(2));
	if (!cl) return;

	if (cl->gentity->playerState->persistant[PERS_TEAM] == TEAM_SPECTATOR) {
		return;
	}

	int damage = atoi(Cmd_Argv(3));
	char* reason = Cmd_Argv(4);

	// Pushes the player forward and up
	cl->gentity->playerState->velocity[0] = -300;
	cl->gentity->playerState->velocity[2] = 600;

	cl->gentity->playerState->forceHandExtend = HANDEXTEND_FORCE_HOLD;
	cl->gentity->playerState->forceDodgeAnim = 1;
	cl->gentity->playerState->forceHandExtendTime = sv.time + 1100;
	cl->gentity->playerState->quickerGetup = qfalse;

	// If we were to kill, set health to 1
	if (cl->gentity->health - damage <= 0) {
		cl->gentity->health = 1;
	}
	else {
		cl->gentity->health = cl->gentity->health - damage;
	}

	SV_SendServerCommand(cl, "chat \"" S_COLOR_RED "You was slapped by an admin because: %s" S_COLOR_WHITE "\"\n", reason);
	SV_ExecuteClientCommand(cl, "voice_cmd *reply_comp", qtrue);

}

/*
==================
SM_Slay - Slay a player
==================
*/
static void SM_Slay(void) {

	client_t* cl = SV_BetterGetPlayerByHandle(Cmd_Argv(2));
	if (!cl) return;

	if (cl->gentity->playerState->persistant[PERS_TEAM] == TEAM_SPECTATOR) {
		return;
	}
	
	int damage = atoi(Cmd_Argv(3));

	char* reason = Cmd_Argv(3);
	SV_ExecuteClientCommand(cl, "kill", qtrue);
	SV_SendServerCommand(cl, "chat \"" S_COLOR_RED "You was slayed by an admin because: %s" S_COLOR_WHITE "\"\n", reason);

}

/*
==================
SM_SetHealth - Set a players health
==================
*/
static void SM_SetHealth(void) {

	client_t* cl = SV_BetterGetPlayerByHandle(Cmd_Argv(2));
	if (!cl) return;

	if (cl->gentity->playerState->persistant[PERS_TEAM] == TEAM_SPECTATOR) {
		return;
	}

	int health = atoi(Cmd_Argv(3));

	if(health > cl->gentity->playerState->stats[STAT_MAX_HEALTH])
		cl->gentity->playerState->stats[STAT_MAX_HEALTH] = health;

	cl->gentity->health = health;
	cl->gentity->playerState->stats[STAT_HEALTH] = health;

	SV_SendServerCommand(cl, "chat \"" S_COLOR_YELLOW "Your health was changed by an admin" S_COLOR_WHITE "\"\n");

}

/*
==================
SM_SetArmor - Set a players armor
==================
*/
static void SM_SetArmor(void) {

	client_t* cl = SV_BetterGetPlayerByHandle(Cmd_Argv(2));
	if (!cl) return;

	if (cl->gentity->playerState->persistant[PERS_TEAM] == TEAM_SPECTATOR) {
		return;
	}

	int armor = atoi(Cmd_Argv(3));
	cl->gentity->playerState->stats[STAT_ARMOR] = armor;

	SV_SendServerCommand(cl, "chat \"" S_COLOR_YELLOW "Your armor was changed by an admin" S_COLOR_WHITE "\"\n");

}

/*
==================
SM_GiveForce - Give a player a force power
==================
*/
static void SM_GiveForce(void) {

	client_t* cl = SV_BetterGetPlayerByHandle(Cmd_Argv(2));
	if (!cl) return;

	if (cl->gentity->playerState->persistant[PERS_TEAM] == TEAM_SPECTATOR) {
		return;
	}

	int power = atoi(Cmd_Argv(3));
	int level = atoi(Cmd_Argv(4));

	cl->gentity->playerState->fd.forcePowersKnown |= (1 << power);
	cl->gentity->playerState->fd.forcePowerLevel[power] = level;
	cl->gentity->playerState->fd.forcePower = 100;
	cl->gentity->playerState->fd.forcePowerMax = 100;

	SV_SendServerCommand(cl, "chat \"" S_COLOR_YELLOW "Your force powers were changed by an admin" S_COLOR_WHITE "\"\n");

}

/*
==================
SM_GiveWeapon - Give a player a weapon
==================
*/
static void SM_GiveWeapon(void) {

	client_t* cl = SV_BetterGetPlayerByHandle(Cmd_Argv(2));
	if (!cl) return;

	if (cl->gentity->playerState->persistant[PERS_TEAM] == TEAM_SPECTATOR) {
		return;
	}

	int power = atoi(Cmd_Argv(3));
	int level = atoi(Cmd_Argv(4));

	cl->gentity->playerState->fd.forcePowersKnown |= (1 << power);
	cl->gentity->playerState->fd.forcePowerLevel[power] = level;
	cl->gentity->playerState->fd.forcePower = 100;
	cl->gentity->playerState->fd.forcePowerMax = 100;

	SV_SendServerCommand(cl, "chat \"" S_COLOR_YELLOW "Your force powers were changed by an admin" S_COLOR_WHITE "\"\n");

}


// All Commands for Extended SMOD have to be loaded here 
void SV_SmodAddCmds() {

	// command / smod level / function / arguments needed after command / description

	SV_SmodAddCmd("slap", 3, SM_Slap, 3, "slap <clientid or name> <damage> <reason>");
	SV_SmodAddCmd("slay", 2, SM_Slay, 2, "slay <clientid or name> <reason>");
	SV_SmodAddCmd("sethealth", 3, SM_SetHealth, 2, "sethealth <clientid or name> <health>");
	SV_SmodAddCmd("setarmor", 3, SM_SetArmor, 2, "setarmor <clientid or name> <armor>");
	SV_SmodAddCmd("giveforce", 3, SM_GiveForce, 3, "giveforce <clientid or name> <forcepower number> <level 1-3>");
}

