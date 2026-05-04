#pragma once

// List all Possible Wins — updated for MB2 new weapon system

#define SPIN_VERSION "2.000"

typedef enum {
	// ── Pistols & Light Sidearms ──
	WIN_BRYAR,              // Bryar Pistol
	WIN_CLONE_PISTOL,       // DC-17 Pistol
	WIN_MANDO_PISTOL,       // Westar 34 / Mandalorian Pistol
	WIN_HEAVY_PISTOL,       // Heavy Pistol
	WIN_BRYAR_OLD,          // Classic Bryar Pistol
	WIN_EE3,                // EE-3 Blaster Carbine
	// ── Blasters & Carbines ──
	WIN_BLASTER,            // E-11 Blaster Rifle
	WIN_DC_CARBINE,         // DC-15 Carbine
	WIN_CR2,                // CR-2 Sub-Blaster
	WIN_E22,                // E-22 Blaster Rifle
	WIN_DLT19,              // DLT-19 Heavy Blaster
	WIN_TRAD_BOWCASTER,     // Traditional Wookiee Bowcaster
	WIN_DISRUPTOR,          // Disruptor Rifle
	WIN_BOWCASTER,          // Advanced Wookiee Bowcaster
	WIN_REPEATER,           // Imperial Heavy Repeater
	WIN_CLONE_RIFLE,        // DC-15A Clone Rifle
	WIN_A280,               // A280 Rebel Rifle
	WIN_DLT20A,             // DLT-20A Laser Rifle
	WIN_M5,                 // M5 Rifle
	WIN_T21,                // T-21 Repeating Blaster
	WIN_EE4,                // EE-4 Blaster Carbine
	WIN_AMBAN,              // Amban Phase-Pulse Rifle
	WIN_PROJ,               // Projectile Rifle
	WIN_SBD,                // SBD Wrist Blaster
	// ── Special Weapons ──
	WIN_DEMP2,              // DEMP-2 Ionic Disruption Rifle
	WIN_FLECHETTE,          // Golan Arms FC1 Flechette Cannon
	WIN_CONCUSSION,         // Concussion Rifle
	WIN_THROWER,            // Flamethrower / Cryojet
	WIN_MINIGUN,            // Minigun
	WIN_SHOTGUN,            // Shotgun
	// ── Heavy Launchers ──
	WIN_ROCKET_LAUNCHER,    // Merr-Sonn Rocket Launcher
	WIN_PLX1,               // PLX-1 Portable Missile Launcher
	// ── Grenades & Explosives ──
	WIN_FRAG_NADE,          // Frag Grenades
	WIN_PULSE_NADE,         // Pulse Grenades
	WIN_THERMAL,            // Thermal Detonator
	WIN_REAL_TD,            // Proximity Detonator
	WIN_FIRE_NADE,          // Incendiary Grenade
	WIN_SONIC_NADE,         // Sonic Grenade
	WIN_CRYO_NADE,          // Cryo Grenade
	WIN_CONC_NADE,          // Concussion Grenade
	WIN_TRIP_MINE,          // Trip Mine
	WIN_DET_PACK,           // Det Pack
	WIN_UGL,                // Universal Grenade Launcher
	WIN_MGL,                // Micro Grenade Launcher
	// ── Melee ──
	WIN_SABER,              // Lightsaber (with random style)
	// ── Equipment ──
	WIN_100_ARMOR,
	WIN_250_ARMOR,
	WIN_JETPACK,
	WIN_CLOAK,
	WIN_EWEB,
	WIN_SENTRY,
	WIN_SEEKER,
	WIN_BACTA,
	WIN_FORCEFIELD,
	// ── Vehicles ──
	WIN_TAUN_TAUN,
	WIN_SWOOP,
	WIN_SPEEDER,
	WIN_DEWBACK,
	WIN_MECH,
	// ── Fun / Size ──
	WIN_SIZE_XS,           // 50% scale
	WIN_SIZE_S,            // 80% scale
	WIN_SIZE_L,            // 130% scale
	WIN_SIZE_XL,           // 175% scale
	WIN_SIZE_HUGE,         // 250% scale
	// ── Force Powers ──
	WIN_FORCE_SENSITIVITY, // Bundle: jump + push + pull + speed + sense + saber L2
	WIN_FORCE_SPEED,
	WIN_FORCE_PUSH,
	WIN_FORCE_PULL,
	WIN_FORCE_LIGHTNING,
	WIN_FORCE_GRIP,
	WIN_FORCE_MINDTRICK,
	WIN_FORCE_HEAL,
	WIN_FORCE_JUMP,
	WIN_FORCE_DESTRUCTION,
	WIN_FORCE_PROTECT,
	WIN_FORCE_ABSORB,
	WIN_FORCE_DRAIN,
	WIN_FORCE_SENSE,
	WIN_FORCE_SABER_THROW,
	WIN_FORCE_TEAM_HEAL,
	WIN_FORCE_TEAM_ENERGIZE,
	WIN_FORCE_MASTER,      // All 18 force powers at level 3
	WIN_PHASING,           // 30-second phasing powerup
	WIN_INVINCIBLE,
	// ── Health ──
	WIN_100_HEALTH,
	WIN_250_HEALTH,
	WIN_NUM_WINS // MUST BE LAST AND IS NOT A REAL WIN
} spin_wins_t;


// Default weights — higher value = more common spin result
// Spin_GeneratePrices may zero-out entries depending on player state

int weights[WIN_NUM_WINS] = {
	// Pistols & Light Sidearms
	30, // WIN_BRYAR
	40, // WIN_CLONE_PISTOL
	40, // WIN_MANDO_PISTOL
	30, // WIN_HEAVY_PISTOL
	30, // WIN_BRYAR_OLD
	35, // WIN_EE3
	// Blasters & Carbines
	40, // WIN_BLASTER
	40, // WIN_DC_CARBINE
	35, // WIN_CR2
	35, // WIN_E22
	30, // WIN_DLT19
	30, // WIN_TRAD_BOWCASTER
	40, // WIN_DISRUPTOR
	40, // WIN_BOWCASTER
	35, // WIN_REPEATER
	35, // WIN_CLONE_RIFLE
	35, // WIN_A280
	35, // WIN_DLT20A
	35, // WIN_M5
	30, // WIN_T21
	35, // WIN_EE4
	25, // WIN_AMBAN
	25, // WIN_PROJ
	30, // WIN_SBD
	// Special Weapons
	30, // WIN_DEMP2
	30, // WIN_FLECHETTE
	25, // WIN_CONCUSSION
	30, // WIN_THROWER
	20, // WIN_MINIGUN
	35, // WIN_SHOTGUN
	// Heavy Launchers
	20, // WIN_ROCKET_LAUNCHER
	20, // WIN_PLX1
	// Grenades & Explosives
	40, // WIN_FRAG_NADE
	40, // WIN_PULSE_NADE
	35, // WIN_THERMAL
	30, // WIN_REAL_TD
	35, // WIN_FIRE_NADE
	35, // WIN_SONIC_NADE
	35, // WIN_CRYO_NADE
	35, // WIN_CONC_NADE
	30, // WIN_TRIP_MINE
	30, // WIN_DET_PACK
	25, // WIN_UGL
	25, // WIN_MGL
	// Melee
	40, // WIN_SABER
	// Equipment
	50, // WIN_100_ARMOR
	30, // WIN_250_ARMOR
	40, // WIN_JETPACK
	35, // WIN_CLOAK
	35, // WIN_EWEB
	35, // WIN_SENTRY
	35, // WIN_SEEKER
	35, // WIN_BACTA
	35, // WIN_FORCEFIELD
	// Vehicles
	25, // WIN_TAUN_TAUN
	25, // WIN_SWOOP
	20, // WIN_SPEEDER
	25, // WIN_DEWBACK
	 5, // WIN_MECH
	// Fun / Size
	25, // WIN_SIZE_XS
	40, // WIN_SIZE_S
	20, // WIN_SIZE_L
	15, // WIN_SIZE_XL
	 8, // WIN_SIZE_HUGE
	// Force Powers
	40, // WIN_FORCE_SENSITIVITY
	40, // WIN_FORCE_SPEED
	40, // WIN_FORCE_PUSH
	40, // WIN_FORCE_PULL
	35, // WIN_FORCE_LIGHTNING
	35, // WIN_FORCE_GRIP
	35, // WIN_FORCE_MINDTRICK
	35, // WIN_FORCE_HEAL
	40, // WIN_FORCE_JUMP
	25, // WIN_FORCE_DESTRUCTION
	30, // WIN_FORCE_PROTECT
	30, // WIN_FORCE_ABSORB
	25, // WIN_FORCE_DRAIN
	35, // WIN_FORCE_SENSE
	30, // WIN_FORCE_SABER_THROW
	20, // WIN_FORCE_TEAM_HEAL
	20, // WIN_FORCE_TEAM_ENERGIZE
	 5, // WIN_FORCE_MASTER
	15, // WIN_PHASING
	 8, // WIN_INVINCIBLE
	// Health
	50, // WIN_100_HEALTH
	30, // WIN_250_HEALTH
};
