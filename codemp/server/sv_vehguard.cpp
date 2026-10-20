/*
===========================================================================
Vehicle field guard

The Legends summoners - Palpatine (h7_Palp) and Nute Gunray (v7_NuteG), the
only classes with FP_SUMMON - crash MBII's game module, and both Palpatine
crashes we have dumps for were the same thing: an entity that isn't on a
vehicle, on a map without any, holding garbage in its vehicle fields.

  - a corpse (bodyque) with m_pVehicle = 5. A blaster bolt hit it, G_Damage
    -> differentEntity -> realEntityNumber followed the "vehicle" to find
    its pilot, and faulted reading 0x2d5.
  - a spectator with s.m_iVehicleNum = 14000. vmMain took g_entities[14000]
    as their vehicle (there are 1024), read a garbage client pointer from
    far past the array, and faulted.

Whatever writes them is in MBII; this can't stop that. What it can do is put
the fields right before the game next reads them: before every game frame
(every entity) and before each player's think (theirs). Only values nothing
valid could hold are touched - a vehicle number outside the entity array, a
vehicle pointer in the first 64K or not 4-byte aligned (MBII allocates them
4-aligned, from its own heap) - so a real vehicle and its riders are never
changed. Each fix goes in vehguard.log (SV_SharedLog), so when it fires we
know which entity, what value and when, for MBII's developers.

If the bad value is written and read inside the same game frame there's no
gap to get into, and it still crashes. sv_vehicleGuard 0 turns it off.
===========================================================================
*/

#include "server.h"

#define VEHGUARD_MIN_PTR	0x10000	// no heap pointer is ever this low
#define VEHGUARD_MAX_LOGS	50		// lines a map, so a bad loop can't flood the log

static cvar_t	*sv_vehicleGuard;
static int		vehGuardServerId = -1;
static int		vehGuardLogged;

static void VehGuard_Log( const char *where, const sharedEntity_t *ent, const char *field, unsigned int bad ) {
	if ( sv.serverId != vehGuardServerId ) {
		vehGuardServerId = sv.serverId;
		vehGuardLogged = 0;
	}
	if ( vehGuardLogged >= VEHGUARD_MAX_LOGS ) {
		return;
	}
	vehGuardLogged++;

	// eType and number only - with the entity's memory already suspect, its
	// classname pointer isn't safe to follow.
	const int num = ent->s.number;
	const char *player = ( num >= 0 && num < sv_maxclients->integer && svs.clients[num].state >= CS_CONNECTED ) ? svs.clients[num].name : "";
	Com_Printf( "[VehGuard] %s: entity %d (eType %d%s%s) had %s = 0x%x - cleared\n",
		where, num, ent->s.eType, player[0] ? ", " : "", player, field, bad );
	SV_SharedLog( "vehguard.log", va( "%s\t%d\t%s\t%d\t%d\t%s\t%s\t0x%x",
		sv_mapname->string, sv.time, where, num, ent->s.eType, player, field, bad ) );
}

static void VehGuard_Check( const char *where, sharedEntity_t *ent ) {
	if ( ent->s.m_iVehicleNum < 0 || ent->s.m_iVehicleNum >= MAX_GENTITIES ) {
		VehGuard_Log( where, ent, "s.m_iVehicleNum", (unsigned int)ent->s.m_iVehicleNum );
		ent->s.m_iVehicleNum = 0;
	}

	const uintptr_t veh = (uintptr_t)ent->m_pVehicle;
	if ( veh && ( veh < VEHGUARD_MIN_PTR || ( veh & 3 ) ) ) {
		VehGuard_Log( where, ent, "m_pVehicle", (unsigned int)veh );
		ent->m_pVehicle = NULL;
	}
}

static qboolean VehGuard_On( void ) {
	if ( !sv_vehicleGuard ) {
		sv_vehicleGuard = Cvar_Get( "sv_vehicleGuard", "1", CVAR_ARCHIVE, "Clear impossible vehicle fields on entities before the game reads them (vehguard.log)" );
	}
	return ( sv_vehicleGuard->integer && sv.state == SS_GAME && sv.gentities ) ? qtrue : qfalse;
}

// Every entity, before a game frame.
void SV_VehicleGuardFrame( void ) {
	if ( !VehGuard_On() ) {
		return;
	}
	for ( int i = 0; i < sv.num_entities; i++ ) {
		VehGuard_Check( "frame", SV_GentityNum( i ) );
	}
}

// One player's entity, before their think.
void SV_VehicleGuardClient( client_t *cl ) {
	if ( !VehGuard_On() || !cl->gentity ) {
		return;
	}
	VehGuard_Check( "think", cl->gentity );
}
