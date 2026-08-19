
## This Fork

This fork of OpenJK contains a customised dedicated server for spin and is used on "New Republic Clan" spin server. Changes to this fork include

## Spin Mode

This fork has spin mode available. 
If enabled any client doing `!spin` in the chat will be given a random perk. 

Spin mode can be enabled using the following CVARS
`sv_spin <1/0>` enable or disable spin
`sv_spinCoolDown X` cooldown between spins

The weights for various perks are coded into `spin.h` with conditional exclusions coded in `sv_ccmds` along with the code for awarding the spin. 

**Perks include:**
- Weapons + Ammo
- Big Model, 
- Small Model, 
- Items, 
- Vehicles
- God Mode (30 seconds)


### Notes
- Giving a saber will automatically give BP (which is also jet fuel)
and saber defence 3. Which can then be removed using removeforce if not needed
- Giving a jetpack will automatically give 100 fuel
- Removing force will remove saber defence as well

## Economy / Credit System

This fork adds an optional server-side credit and bounty economy. It is disabled by default and controlled with:

`g_creditSystemEnable <1/0>` enable or disable the credit system

When enabled, players earn credits for kills and can spend them in a chat-driven shop, all via `say`/`say_team` chat commands (nothing is broadcast to other players or the server console):

- `!balance` - show your current credits and any bounty on your head
- `!buy` - show shop categories and balance; `!buy <category>` to list items in a category; `!buy <name>` to purchase (e.g. `!buy bryar`, `!buy jetpack`)
- `!bounty` - list players and active bounties; `!bounty <clientnum> <credits>` to place a bounty (paid to whoever gets the kill)
- `!help` - summary of the above

Credits are awarded automatically: killing another player grants a fixed kill reward plus any bounty on the victim.

### Shop catalog

The shop is built from the same "spin" prize system used by `!spin`/Chaos Mode (see Spin Mode above), so every purchasable item reuses the exact same granting logic as a random spin win - no separate gameplay code path to keep in sync. Vehicles, NPC spawns, and the debug-only "all skills" win are intentionally excluded from the shop.

Categories (`!buy <category>`):

| Category | Contents |
| --- | --- |
| `pistols` | Bryar, DC-17, Westar-34, Heavy Pistol, classic Bryar, EE-3 |
| `rifles` | E-11, DC-15, CR-2, E-22, DLT-19, bowcasters, Disruptor, repeaters, A280, DLT-20A, M5, T-21, EE-4, Amban, Projectile Rifle, SBD wrist blaster |
| `special` | DEMP2, Flechette, Concussion Rifle, Thrower, Minigun, Shotgun |
| `launchers` | Rocket Launcher, PLX-1 |
| `nades` | All grenade/explosive types (frag, pulse, thermal, proximity, fire, sonic, cryo, concussion, trip mine, det pack) |
| `melee` | Lightsaber (random style) |
| `gadgets` | Armor, cloak, EWEB, sentry, seeker, bacta, forcefield, spawner, stimpack, jetpack, shockfield, protocol droid |
| `size` | Size-change fun perks (XS/S/L/XL) |
| `ammo` | Ammo refill for your current loadout |

Each item's cost is its own server cvar named `g_shopCost_<name>` (e.g. `g_shopCost_bryar`, `g_shopCost_rocket_launcher`), registered with a sensible default the first time the server starts. **Setting a cost to `0` disables that item** - no separate enable flag exists, and a whole category is automatically hidden from `!buy` once every item in it is set to 0. Changes take effect immediately (`set g_shopCost_bryar 0`), no reload needed, and being `CVAR_ARCHIVE` they persist in the server config across restarts.

Full list of shop cvars and their default costs:

| Category | Cvar | Default |
| --- | --- | --- |
| pistols | `g_shopCost_bryar` | 8 |
| pistols | `g_shopCost_clone_pistol` | 8 |
| pistols | `g_shopCost_bryar_old` | 8 |
| pistols | `g_shopCost_mando_pistol` | 10 |
| pistols | `g_shopCost_heavy_pistol` | 10 |
| pistols | `g_shopCost_ee3` | 10 |
| rifles | `g_shopCost_blaster` | 12 |
| rifles | `g_shopCost_dc_carbine` | 15 |
| rifles | `g_shopCost_cr2` | 15 |
| rifles | `g_shopCost_e22` | 15 |
| rifles | `g_shopCost_trad_bowcaster` | 15 |
| rifles | `g_shopCost_t21` | 15 |
| rifles | `g_shopCost_dlt19` | 18 |
| rifles | `g_shopCost_clone_rifle` | 18 |
| rifles | `g_shopCost_a280` | 18 |
| rifles | `g_shopCost_dlt20a` | 18 |
| rifles | `g_shopCost_m5` | 18 |
| rifles | `g_shopCost_ee4` | 18 |
| rifles | `g_shopCost_bowcaster` | 20 |
| rifles | `g_shopCost_repeater` | 20 |
| rifles | `g_shopCost_sbd` | 20 |
| rifles | `g_shopCost_disruptor` | 22 |
| rifles | `g_shopCost_proj` | 22 |
| rifles | `g_shopCost_amban` | 25 |
| special | `g_shopCost_shotgun` | 18 |
| special | `g_shopCost_thrower` | 20 |
| special | `g_shopCost_flechette` | 22 |
| special | `g_shopCost_concussion` | 22 |
| special | `g_shopCost_minigun` | 30 |
| launchers | `g_shopCost_rocket_launcher` | 35 |
| launchers | `g_shopCost_plx1` | 35 |
| nades | `g_shopCost_frag_nade` | 8 |
| nades | `g_shopCost_pulse_nade` | 8 |
| nades | `g_shopCost_thermal` | 10 |
| nades | `g_shopCost_real_td` | 10 |
| nades | `g_shopCost_fire_nade` | 10 |
| nades | `g_shopCost_sonic_nade` | 10 |
| nades | `g_shopCost_cryo_nade` | 10 |
| nades | `g_shopCost_conc_nade` | 10 |
| nades | `g_shopCost_trip_mine` | 12 |
| nades | `g_shopCost_det_pack` | 15 |
| melee | `g_shopCost_saber` | 30 |
| gadgets | `g_shopCost_bacta` | 5 |
| gadgets | `g_shopCost_stimpack` | 8 |
| gadgets | `g_shopCost_100_armor` | 10 |
| gadgets | `g_shopCost_seeker` | 10 |
| gadgets | `g_shopCost_sentry` | 15 |
| gadgets | `g_shopCost_protocol` | 15 |
| gadgets | `g_shopCost_250_armor` | 20 |
| gadgets | `g_shopCost_cloak` | 20 |
| gadgets | `g_shopCost_forcefield` | 20 |
| gadgets | `g_shopCost_shockfield` | 20 |
| gadgets | `g_shopCost_jetpack` | 22 |
| gadgets | `g_shopCost_eweb` | 25 |
| gadgets | `g_shopCost_spawner` | 25 |
| size | `g_shopCost_size_s` | 8 |
| size | `g_shopCost_size_xs` | 10 |
| size | `g_shopCost_size_l` | 12 |
| size | `g_shopCost_size_xl` | 18 |
| ammo | `g_shopCost_ammo` | 6 |

### Persistent accounts

Credits can optionally be saved across reconnects/restarts using a lightweight handle + PIN account system, also driven entirely through chat commands:

- `!register <handle>` - check whether a handle (3-23 letters/numbers/underscores) is available
- `!register <handle> <pin>` - create the account with a 4-digit PIN, saving your current session's credits to it and logging you in
- `!login <handle> <pin>` - log into an existing account on a new connection, restoring its saved credit balance

Accounts are stored server-side in `economy_accounts.dat`. PINs are never stored in plaintext - each account has a random salt and only a salted HMAC-MD5 hash of the PIN is saved. Repeated failed `!login` attempts lock that account out for 60 seconds. Logging in from a new connection will kick any other session currently logged into the same handle.

Admins can also grant credits directly from the server console/rcon with:

`givecredits <player> <amount>`

## License

OpenJK is licensed under GPLv2 as free software. You are free to use, modify and redistribute OpenJK following the terms in LICENSE.txt.


## For Developers


### Building OpenJK

* [Compilation guide](https://github.com/JACoders/OpenJK/wiki/Compilation-guide)
* [Debugging guide](https://github.com/JACoders/OpenJK/wiki/Debugging)


### Contributing to OpenJK

* [Fork](https://github.com/JACoders/OpenJK/fork) the project on GitHub
* Create a new branch and make your changes
* Send a [pull request](https://help.github.com/articles/creating-a-pull-request) to upstream (JACoders/OpenJK)


### Using OpenJK as a base for a new mod

* [Fork](https://github.com/JACoders/OpenJK/fork) the project on GitHub
* Change the GAMEVERSION define in codemp/game/g_local.h from "OpenJK" to your project name
* If you make a nice change, please consider back-porting to upstream via pull request as described above. This is so everyone benefits without having to reinvent the wheel for every project.


## Maintainers (in alphabetical order)

* Ensiform
* Razish
* Xycaleth


## Significant contributors (in alphabetical order)

* eezstreet
* exidl
* ImperatorPrime
* mrwonko
* redsaurus
* Scooper
* Sil
* smcv
