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
	// ── Melee ──
	WIN_SABER,              // Lightsaber (with random style)
	// ── Equipment ──
	WIN_100_ARMOR,
	WIN_250_ARMOR,
	WIN_CLOAK,
	WIN_EWEB,
	WIN_SENTRY,
	WIN_SEEKER,
	WIN_BACTA,
	WIN_FORCEFIELD,
	WIN_SPAWNER,            // Support Beacon (spawns a clone trooper)
	WIN_STIMPACK,           // Stimpack
	// ── Vehicles ──
	WIN_TAUN_TAUN,
	WIN_SWOOP,
	WIN_SPEEDER,
	WIN_DEWBACK,
	WIN_MECH,
	WIN_AWING_MINI,
	WIN_TIE_BOMBER_MINI,
	WIN_TIE_FIGHTER_MINI,
	WIN_YWING_MINI,
	WIN_BANTHA,
	// ── NPC Spawns ──
	WIN_NPC_CT_CARBINE,     // Clone Trooper (DC-15 Carbine)
	WIN_NPC_CT_CR,          // Clone Trooper (CR-2)
	WIN_NPC_CT_CR2,         // Clone Trooper (CR-2 Elite)
	WIN_NPC_B1,             // B1 Battle Droid
	WIN_NPC_BX,             // BX Commando Droid
	WIN_NPC_JEDI,           // Jedi NPC
	WIN_NPC_WAMPA,          // Wampa
	WIN_NPC_RANCOR,         // Rancor
	// ── Fun / Size ──
	WIN_SIZE_XS,           // 50% scale
	WIN_SIZE_S,            // 80% scale
	WIN_SIZE_L,            // 130% scale
	WIN_SIZE_XL,           // 175% scale
	WIN_JETPACK,           // Jetpack + fuel refill
	WIN_SHOCKFIELD,        // Shockfield holdable via give item_shockfield
	WIN_PROTOCOL,          // Protocol droid companion
	WIN_NUM_WINS // MUST BE LAST AND IS NOT A REAL WIN
} spin_wins_t;


// Default weights — higher value = more common spin result
// Spin_GeneratePrices may zero-out entries depending on player state

static int weights[WIN_NUM_WINS] = {
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
	// Melee
	50, // WIN_SABER
	// Equipment
	50, // WIN_100_ARMOR
	30, // WIN_250_ARMOR

	35, // WIN_CLOAK
	35, // WIN_EWEB
	35, // WIN_SENTRY
	35, // WIN_SEEKER
	35, // WIN_BACTA
	35, // WIN_FORCEFIELD
	30, // WIN_SPAWNER
	30, // WIN_STIMPACK
	// Vehicles
	25, // WIN_TAUN_TAUN
	25, // WIN_SWOOP
	20, // WIN_SPEEDER
	25, // WIN_DEWBACK
	 5, // WIN_MECH
	 5, // WIN_AWING_MINI
	 5, // WIN_TIE_BOMBER_MINI
	 5, // WIN_TIE_FIGHTER_MINI
	 5, // WIN_YWING_MINI
	 5, // WIN_BANTHA
	// NPC Spawns
	 0, // WIN_NPC_CT_CARBINE (disabled)
	 0, // WIN_NPC_CT_CR (disabled)
	 0, // WIN_NPC_CT_CR2 (disabled)
	 0, // WIN_NPC_B1 (disabled)
	 0, // WIN_NPC_BX (disabled)
	 0, // WIN_NPC_JEDI (disabled)
	10, // WIN_NPC_WAMPA
	 5, // WIN_NPC_RANCOR
	// Fun / Size
	25, // WIN_SIZE_XS
	40, // WIN_SIZE_S
	20, // WIN_SIZE_L
	15, // WIN_SIZE_XL
	30, // WIN_JETPACK
	30, // WIN_SHOCKFIELD
	20, // WIN_PROTOCOL
};

// rcon test command
void SV_SpinWin_f(void);
