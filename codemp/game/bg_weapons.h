/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
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

#pragma once

// Filename:-	bg_weapons.h
// Updated to match MB2 game DLL (NEW_WEAPON_NETWORKING, GCJ_AMMO_NETCODE)

typedef enum {
	WP_NONE,

	// Start player useable weapons
	WP_STUN_BATON,
	WP_MELEE,
	WP_SABER,
	WP_BRYAR_PISTOL,
	WP_CLONE_PISTOL,
	WP_MANDO_PISTOL,
	WP_BLASTER,
	WP_DC_CARBINE,
	WP_CR2,
	WP_E_22,
	WP_HEAVY_PISTOL,
	WP_DLT19,
	WP_TRAD_BOWCASTER,
	WP_DISRUPTOR,
	WP_BOWCASTER,
	WP_REPEATER,
	WP_CLONE_RIFLE,
	WP_THROWER,
	WP_MINIGUN,
	WP_DEMP2,
	WP_SHOTGUN,
	WP_FLECHETTE,
	WP_A280,
	WP_DLT20A,
	WP_M5,
	WP_T21,
	WP_ROCKET_LAUNCHER,
	WP_PLX1,
	WP_THERMAL,
	WP_FRAG_NADE,
	WP_REAL_TD,
	WP_TRIP_MINE,
	WP_PULSE_NADE,
	WP_FIRE_NADE,
	WP_SONIC_NADE,
	WP_CRYO_NADE,
	WP_CONC_NADE,
	WP_DET_PACK,
	WP_CONCUSSION,
	WP_SBD,
	WP_WELD_PULSE,
	WP_WELD_BEAM,
	WP_BRYAR_OLD,
	WP_EE3,
	WP_EE4,
	WP_AMBAN,
	WP_PROJ,
#ifdef GCJ_KHORNE_NADES
	WP_IMPACT_NADE,
	WP_BACTA_BOMB,
	WP_GAS_NADE,
	WP_FLASH_NADE,
	WP_GLOP_NADE,
	WP_IMPLODER_NADE,
	WP_SMOKE_NADE,
	WP_SHIELD_NADE,
	WP_PROTON_NADE,
#endif
	WP_UGL,
	WP_MGL,
	// End player useable weapons

	WP_EMPLACED_GUN,
	WP_TURRET,

	WP_NUM_WEAPONS
} weapon_t;

#define FIRST_USEABLE_WEAPON	(WP_NONE + 1)
#define LAST_USEABLE_WEAPON		(WP_EMPLACED_GUN - 1)

// Should not exceed MAX_AMMO_SLOTS
typedef enum {
	AMMO_NONE,
	AMMO_FORCE,
	AMMO_BLASTER,
	AMMO_POWERCELL,
	AMMO_METAL_BOLTS,
	AMMO_ROCKETS,
	AMMO_EMPLACED,
	AMMO_FRAG_NADE,
	AMMO_PULSE_NADE,
	AMMO_T21,
	AMMO_PISTOL,
	AMMO_WESTAR,
	AMMO_CLONEPISTOL,
	AMMO_PROJECTILE,
	AMMO_FIRE_NADE,
	AMMO_SONIC_NADE,
	AMMO_CONC_NADE,
	AMMO_CRYO_NADE,
	AMMO_REAL_TD,
	AMMO_THERMAL,
	AMMO_TRIPMINE,
	AMMO_DETPACK,
	AMMO_HOMING,
	AMMO_EE3,
	AMMO_EE4,
	AMMO_AMBAN,
	AMMO_WELDING,
	// Networking-aligned slots (always present for enum layout consistency)
	AMMO_IMPACT_NADE,
	AMMO_BACTA_BOMB,
	AMMO_GAS_NADE,
	AMMO_FLASH_NADE,
	AMMO_GLOP_NADE,
	AMMO_IMPLODER_NADE,
	AMMO_NON_NETWORK_1,
	AMMO_SMOKE_NADE,
	AMMO_NON_NETWORK_2,
	AMMO_SHIELD_NADE,
	AMMO_NON_NETWORK_3,
	AMMO_PROTON_NADE,
	AMMO_NON_NETWORK_4,
	AMMO_STICKY_BOMBS,
	AMMO_NON_NETWORK_5,
	AMMO_SHOTGUN,
	AMMO_NON_NETWORK_6,
	AMMO_TRAD_CASTER,
	AMMO_NON_NETWORK_7,
	AMMO_CONCUSSION,
	AMMO_NON_NETWORK_8,
	AMMO_FLECHETTE,
	AMMO_NON_NETWORK_9,
	AMMO_DEMP2,
	AMMO_NON_NETWORK_10,
	AMMO_THROWER,
	AMMO_NON_NETWORK_11,
	AMMO_MINIGUN,
	AMMO_NON_NETWORK_12,
	AMMO_BOWCASTER,
	AMMO_NON_NETWORK_13,
	AMMO_CLONERIFLE,
	AMMO_NON_NETWORK_14,
	AMMO_CR2,
	AMMO_NON_NETWORK_15,
	AMMO_E22,
	AMMO_MAX
} ammo_t;


typedef struct weaponData_s
{
//	char	classname[32];		// Spawning name

	int		ammoIndex;			// Index to proper ammo slot
	int		ammoLow;			// Count when ammo is low

	int		energyPerShot;		// Amount of energy used per shot
	int		fireTime;			// Amount of time between firings
	int		range;				// Range of weapon

	int		altEnergyPerShot;	// Amount of energy used for alt-fire
	int		altFireTime;		// Amount of time between alt-firings
	int		altRange;			// Range of alt-fire

	int		chargeSubTime;		// ms interval for subtracting ammo during charge
	int		altChargeSubTime;	// above for secondary

	int		chargeSub;			// amount to subtract during charge on each interval
	int		altChargeSub;		// above for secondary

	int		maxCharge;			// stop subtracting once charged for this many ms
	int		altMaxCharge;		// above for secondary
} weaponData_t;


typedef struct  ammoData_s
{
//	char	icon[32];	// Name of ammo icon file
	int		max;		// Max amount player can hold of ammo
} ammoData_t;


extern weaponData_t weaponData[WP_NUM_WEAPONS];
extern ammoData_t ammoData[AMMO_MAX];


// Specific weapon information

#define FIRST_WEAPON		WP_BRYAR_PISTOL		// this is the first weapon for next and prev weapon switching
#define MAX_PLAYER_WEAPONS	WP_NUM_WEAPONS-1	// this is the max you can switch to and get with the give all.


#define DEFAULT_SHOTGUN_SPREAD	700
#define DEFAULT_SHOTGUN_COUNT	11

#define	LIGHTNING_RANGE		768
