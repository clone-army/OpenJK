/*
===========================================================================
Vehicle field guard

The Legends summoners - Palpatine (h7_Palp) and Nute Gunray (v7_NuteG), the
only classes with FP_SUMMON - crash MBII's game module. Every dump we have
from them is garbage in the same 12 bytes of an entity, the three shared
fields after playerState:

  0x218 m_pVehicle      a corpse (bodyque) with m_pVehicle = 5. A blaster
                        bolt hit it, G_Damage -> differentEntity ->
                        realEntityNumber followed the "vehicle" to find its
                        pilot, and faulted reading 0x2d5.
  0x21c ghoul2          = 0x5780 at a map_restart; ShutdownGame handed it to
                        G2API_HaveWeGhoul2Models, which faulted on it.
  0x220 localAnimIndex  twice, a player's index named an animation set that
                        was never loaded, and WP_SaberPositionUpdate read
                        bgAllAnims[i].anims (NULL) the frame they respawned.

and s.m_iVehicleNum = 14000 on a spectator: vmMain took g_entities[14000] as
their vehicle (there are 1024), read a garbage client pointer from far past
the array, and faulted.

Whatever writes them is in MBII; this can't stop that. What it can do is put
the fields right before the game next reads them: before every game frame
(every entity), before each player's think (theirs), when the game asks for
saber bolts (the call just before the anim crash) and when it asks whether a
ghoul2 instance has models (the shutdown crash). Only values nothing valid
could hold are touched - a vehicle number outside the entity array, a pointer
in the first 64K or not 4-byte aligned (both are allocated aligned, from a
heap), an anim index naming an empty slot of the game's bgAllAnims - so real
vehicles, models and anim sets are never changed. A bad anim index goes to 0,
the humanoid set every player model uses. Each fix goes in vehguard.log
(SV_SharedLog), so when it fires we know which entity, what value and when,
for MBII's developers.

If the bad value is written and read with no trap call in between there's no
gap to get into, and it still crashes. sv_vehicleGuard 0 turns it off.
===========================================================================
*/

#include "server.h"

#ifdef __linux__
#include <dlfcn.h>
#include <link.h>
#endif

#define VEHGUARD_MIN_PTR	0x10000	// no heap pointer is ever this low
#define VEHGUARD_MAX_LOGS	50		// lines a map, so a bad loop can't flood the log

// MBII's bgLoadedAnim_t: char filename[MAX_QPATH]; animation_t *anims;
#define ANIMFILE_SIZE		0x44
#define ANIMFILE_ANIMS		0x40

static cvar_t	*sv_vehicleGuard;
static int		vehGuardServerId = -1;
static int		vehGuardLogged;

// the game module's bgAllAnims / bgNumAllAnims, looked up per load
static void		*animDll;
static byte		*animTable;
static int		*animCount;
static int		animSlots;

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

static qboolean VehGuard_BadPtr( const void *p ) {
	const uintptr_t v = (uintptr_t)p;
	return ( v && ( v < VEHGUARD_MIN_PTR || ( v & 3 ) ) ) ? qtrue : qfalse;
}

// Find bgAllAnims in the game module; its size gives the slot count (MBII
// never checks it). The module is reloaded each map, so look again when the
// handle changes. Without it, localAnimIndex just isn't checked.
static void VehGuard_FindAnims( void ) {
	void *dll = SV_GameDllHandle();
	if ( dll == animDll ) {
		return;
	}
	animDll = dll;
	animTable = NULL;
	animCount = NULL;
	animSlots = 0;
#ifdef __linux__
	if ( !dll ) {
		return;
	}
	void *table = dlsym( dll, "bgAllAnims" );
	int *count = (int *)dlsym( dll, "bgNumAllAnims" );
	Dl_info info;
	ElfW(Sym) *sym = NULL;
	if ( !table || !count || !dladdr1( table, &info, (void **)&sym, RTLD_DL_SYMENT ) || !sym
		|| !sym->st_size || sym->st_size % ANIMFILE_SIZE ) {
		Com_Printf( "[VehGuard] game module has no usable bgAllAnims - anim indexes not checked\n" );
		return;
	}
	animTable = (byte *)table;
	animCount = count;
	animSlots = sym->st_size / ANIMFILE_SIZE;
#endif
}

static void *VehGuard_AnimSet( int i ) {
	return *(void **)( animTable + i * ANIMFILE_SIZE + ANIMFILE_ANIMS );
}

// Players and NPCs only - nothing else animates from the table.
static void VehGuard_CheckAnim( const char *where, sharedEntity_t *ent ) {
	if ( !animTable || !ent->client || !ent->ghoul2 ) {
		return;
	}
	const int idx = ent->localAnimIndex;
	const int loaded = Q_min( *animCount, animSlots );
	if ( idx >= 0 && idx < loaded && VehGuard_AnimSet( idx ) ) {
		return;
	}
	if ( !VehGuard_AnimSet( 0 ) ) {
		return;	// humanoid set not loaded yet, nothing to put back
	}
	VehGuard_Log( where, ent, "localAnimIndex", (unsigned int)idx );
	ent->localAnimIndex = 0;
}

static void VehGuard_Check( const char *where, sharedEntity_t *ent ) {
	if ( ent->s.m_iVehicleNum < 0 || ent->s.m_iVehicleNum >= MAX_GENTITIES ) {
		VehGuard_Log( where, ent, "s.m_iVehicleNum", (unsigned int)ent->s.m_iVehicleNum );
		ent->s.m_iVehicleNum = 0;
	}
	if ( VehGuard_BadPtr( ent->m_pVehicle ) ) {
		VehGuard_Log( where, ent, "m_pVehicle", (unsigned int)(uintptr_t)ent->m_pVehicle );
		ent->m_pVehicle = NULL;
	}
	// leaks whatever instance it was, if any - better than a fault
	if ( VehGuard_BadPtr( ent->ghoul2 ) ) {
		VehGuard_Log( where, ent, "ghoul2", (unsigned int)(uintptr_t)ent->ghoul2 );
		ent->ghoul2 = NULL;
	}
	VehGuard_CheckAnim( where, ent );
}

static qboolean VehGuard_On( void ) {
	if ( !sv_vehicleGuard ) {
		sv_vehicleGuard = Cvar_Get( "sv_vehicleGuard", "1", CVAR_ARCHIVE, "Clear impossible vehicle/ghoul2/anim fields on entities before the game reads them (vehguard.log)" );
	}
	if ( !sv_vehicleGuard->integer || !sv.gentities ) {
		return qfalse;
	}
	VehGuard_FindAnims();
	return qtrue;
}

// Every entity, before a game frame.
void SV_VehicleGuardFrame( void ) {
	if ( !VehGuard_On() || sv.state != SS_GAME ) {
		return;
	}
	for ( int i = 0; i < sv.num_entities; i++ ) {
		VehGuard_Check( "frame", SV_GentityNum( i ) );
	}
}

// One player's entity, before their think.
void SV_VehicleGuardClient( client_t *cl ) {
	if ( !VehGuard_On() || sv.state != SS_GAME || !cl->gentity ) {
		return;
	}
	VehGuard_Check( "think", cl->gentity );
}

// The game asking for a bolt on this instance: WP_SaberPositionUpdate does it
// for the saber bolts right before it reads the owner's anim set.
void SV_VehicleGuardBolt( void *ghoul2 ) {
	if ( !ghoul2 || VehGuard_BadPtr( ghoul2 ) || !VehGuard_On() ) {
		return;
	}
	for ( int i = 0; i < sv.num_entities; i++ ) {
		sharedEntity_t *ent = SV_GentityNum( i );
		if ( ent->ghoul2 == ghoul2 ) {
			VehGuard_CheckAnim( "bolt", ent );
		}
	}
}

// A ghoul2 pointer the game passed in that can't be one. qtrue tells the
// caller to treat it as no instance; the entity holding it is cleared.
qboolean SV_VehicleGuardGhoul2( const char *where, void *ghoul2 ) {
	if ( !VehGuard_BadPtr( ghoul2 ) ) {
		return qfalse;
	}
	if ( !VehGuard_On() ) {
		return qfalse;
	}
	for ( int i = 0; i < sv.num_entities; i++ ) {
		sharedEntity_t *ent = SV_GentityNum( i );
		if ( ent->ghoul2 == ghoul2 ) {
			VehGuard_Log( where, ent, "ghoul2", (unsigned int)(uintptr_t)ghoul2 );
			ent->ghoul2 = NULL;
		}
	}
	return qtrue;
}
