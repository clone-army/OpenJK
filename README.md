# OpenJK - Clone Army fork (`caded.i386`)

This is the Clone Army fork of [OpenJK](https://github.com/JACoders/OpenJK), used to run our Movie Battles II
servers. It builds a customised multiplayer **dedicated server**, installed as **`/usr/bin/caded.i386`**, that
adds server-side features on top of MBII without touching MBII itself:

| Feature | Switched on by | Player commands |
|---|---|---|
| [Economy](#economy--credit-system): credits, shop, bounties, accounts | `g_creditSystemEnable`, `g_economyShopEnable`, `g_economyBountyEnable` | `!balance` `!gift` `!buy` `!bounty` `!register` `!login` `!help` |
| [Chaos Mode](#chaos-mode): a random prize for everyone every few seconds | `g_chaosEnable`, `g_chaosCooldown` | |
| [Gun Game](#gun-game): climb a weapon ladder one kill at a time | `g_gungame`, `g_gungameAnnounce` | |
| [Social Mode](#social-mode): no damage, spawn any time, duels; bots that pick Legends classes | `g_socialMode`, `g_socialRespawnTime`, `g_socialDuels`, `g_socialRoundTime`, `g_socialBots` | |
| [Kill streaks](#kill-streaks): server-wide streak callouts | `g_killstreakEnable` | |
| [Stats](#stats): kills, deaths, suicides and playtime across all servers | `g_statsEnable` | `!stats` |
| [Chat flood control](#chat-flood-control) | always on | |
| [Nute Gunray block](#nute-gunray-class-block) | always on | |

**Every feature is compiled into the one binary and off by default.** Each is switched on with cvars, so any
server can run any combination of them.

### Using it with MBIIEZ

These servers are managed with **[MBIIEZ](https://github.com/clone-army/mbiiez)**. An MBIIEZ instance uses
this engine when its config says `"engine": "caded.i386"`, and each feature has an MBIIEZ plugin that sets
its cvars: `creditsystem`, `chaos`, `gungame`, `killstreak`, `social` and `stats`. **Those plugins do nothing on any
other engine** (`mbiided.i386`, `openjkded.i386`), because the cvars only exist here. The plugins also re-apply
their cvars every minute, so a manual `rcon set` won't stick. Change the instance's settings in MBIIEZ
instead.

You can also use the cvars directly in a server config (`seta g_chaosEnable 1`) or on the command line
(`+set g_chaosEnable 1`) without MBIIEZ.

---

## Building and installing

On the server (Linux, 32-bit build like MBII itself):

```bash
git clone https://github.com/clone-army/OpenJK
cd OpenJK
./build.sh --install   # first time only: installs build dependencies, then builds
./build.sh             # every later build
```

`build.sh`:

1. does a clean CMake build of the dedicated server only (a few minutes on a 2-core VPS)
2. installs it to `/usr/bin/caded.i386`, swapping the file in atomically, so running servers aren't affected
3. finds every MBIIEZ instance (`/root/mbiiez/configs/*.json`) whose engine is `caded.i386` and restarts the
   ones that are **empty**. Instances with players on keep running the old build and pick up the new one the
   next time they restart (their daily scheduled restart, or a later `build.sh` run while they're empty).

For building on other platforms, see the upstream [compilation guide](https://github.com/JACoders/OpenJK/wiki/Compilation-guide).

---

## Economy / Credit System

A server-side economy driven entirely by chat commands. Replies go only to the player who typed the
command: nothing is broadcast, and the commands never show up as chat.

### Switches

| Cvar | Default | Meaning |
|---|---|---|
| `g_creditSystemEnable` | `0` | Master switch: accounts, earning credits, `!balance`, `!register`, `!login`, `!help` |
| `g_economyShopEnable` | `0` | The `!buy` shop (needs the master switch too) |
| `g_economyBountyEnable` | `0` | Bounties (needs the master switch too) |
| `g_economyBarEnable` | `0` | The `!bar` drinks menu (needs the master switch too) |
| `g_economyJukeboxEnable` | `0` | The `!jukebox` (needs the master switch too): `g_jukeboxCost` (10) per track, `g_jukeboxCooldown` (60) seconds before it can change again |
| `g_economyPazaakEnable` | `0` | `!pazaak` challenges for credits (needs the master switch too) |
| `g_economyChanceEnable` | `0` | `!chance` red/blue challenges for credits (needs the master switch too) |
| `g_economyRaffleEnable` | `0` | The `!raffle` (needs the master switch too): `g_raffleIntervalMinutes` (60), `g_raffleOpenMinutes` (10), `g_raffleTicketPrice` (5), `g_raffleMinEntrants` (5) |
| `g_economyRegisterBonus` | `100` | Credits given once when a player `!register`s a new account |
| `g_shopCost_<item>` | per item | Price of one shop item; `0` removes it (see [Shop catalog](#shop-catalog)) |

### Earning credits

**Credits are only earned while logged into an account** (`!register` or `!login`, below). Unregistered
players can see the commands but don't earn.

- **5 credits per kill**, plus any bounty on the victim.
- **1 credit per round** for every logged-in player on the server when a round ends.
- A bounty is only paid (and removed) by a kill that could actually be paid out, so a kill by an
  unregistered player doesn't use up the bounty.

### Commands

| Command | What it does |
|---|---|
| `!help` | Summary of the commands |
| `!balance` | Your credits, and any bounty on your head |
| `!gift <player> <credits>` | Give some of your credits to another logged-in player |
| `!buy` | Shop categories and your balance |
| `!buy <category>` | Items in a category, e.g. `!buy rifles` |
| `!buy <item>` | Buy something, e.g. `!buy bryar`, `!buy jetpack` |
| `!bounty` | A numbered list of players (up to 10) and any bounties on them |
| `!<n> <credits>` | Put a bounty on player `n` from your last `!bounty` list, e.g. `!2 50`. Paid to whoever kills them. |
| `!register <handle>` | Check whether a handle is free (3-23 letters, numbers or `_`) |
| `!register <handle> <pin>` | Create the account with a 4-digit PIN. Your current credits go into it and you're logged in. |
| `!login <handle> <pin>` | Log in on a new connection and get your balance back |

`!<n> <credits>` only counts as a bounty command when `n` is a valid number from your own latest `!bounty`
list. Anything else (like typing `!1 lol`) is left alone as normal chat. Long `!buy` listings are sent a line
at a time so they don't scroll off the chat overlay before you can read them.

### The bar

`!bar` lists the drinks and `!bar <number>` orders one; every order is announced in chat. There's no drinking
animation in the game, and the bar is meant for the no-damage social server, so drinks change how you look, move and
steer - nothing that matters in a fight - and most play one of MBII's own effects on you (smoke, a confusion swirl,
bubbles, frost, dust, flames, sparks) that everyone sees, you included:

| # | Drink | Does | Lasts |
|---|---|---|---|
| 1 | Jawa Juice | Tiny | 2 min |
| 2 | Gungan Grog | You keep tripping over | 1 min |
| 3 | Corellian Whiskey | Drunk: your view sways hard - worse with every extra one - and you choke on it | 90 s |
| 4 | Tatooine Twister | Your view spins | 20 s |
| 5 | Bubble Brew | Hiccups: you hop every few seconds | 1 min |
| 6 | Moon Milk | Low gravity | 1 min |
| 7 | Sugar Rush | Super speed | 30 s |
| 8 | Bantha Sludge | Quarter speed | 1 min |
| 9 | Backwards Brandy | Controls reversed | 1 min |
| 10 | Runaway Rum | You can't stop running forward | 30 s |
| 11 | Low-Ceiling Lager | Stuck crouching | 1 min |
| 12 | Spotchka | Shimmer nearly invisible | 45 s |
| 13 | Hoth Chiller | Frost forms all over you | 1 min |
| 14 | Mustafar Magma | On fire (looks only) | 1 min |
| 15 | Ion Fizz | Crackling with electricity (looks only) | 1 min |
| 16 | Death Stick | Super speed and twitchy hops, crackling | 30 s |
| 17 | Spice | Low gravity, a slowly spinning view and a grey haze (you're slower while it lasts) | 45 s |

Drink too much within `g_barTabMinutes` (5) and it catches up with you: the `g_barPassOutDrinks`-th drink (8) knocks you out cold for a few seconds, the `g_barPoisoningDrinks`-th (10) is alcohol poisoning and the `g_barSpiceOverdose`-th Spice (3) an overdose - both kill you (MBII's usual 5-second `/kill` countdown) and everyone is told what did it.

Every order burps (Jawa Juice adds a Jawa line, spice and death sticks a cough), and the 8th order in 3 minutes knocks you out cold for a few seconds.

On social servers, `!emotes` lists chat emotes played with MBII's own animations: `!sit`, `!slump`, `!handsup`, `!cower`, `!playdead`, `!hug`, `!sleep` and `!dance` (a shuffled chain of taunts, a spin and victory flourishes) hold until you move; `!taunt` and `!victory` (a random one each time), `!rage`, `!choke`, `!lookaround`, `!salute`, `!nod`, `!shakehead` and `!talk` play once. A Corellian Whiskey makes you choke (`codemp/server/emotes.cpp`).

Each price is its own cvar, `g_barCost_<drink>` (the drink's name in lower case with underscores, e.g.
`g_barCost_jawa_juice`); `0` takes a drink off the menu. Effects only last for the life they were bought in. See the
header of `codemp/server/bar.cpp` for how each effect works.

### The jukebox

`!jukebox` shows 21 featured tracks (Cantina Band, Nightclub, Jabba's Sail Barge, Duel of the Fates, Benny Hill, Crazy
Train, TMNT, Halo, Portal, Mortal Kombat, Lord of the Rings and more). Every one of the ~200 music tracks in MBII's own
files is in the catalogue (`codemp/server/jukebox_tracks.h`), each with a friendly name and a category: `!jukebox <words>`
searches names and categories (e.g. `!jukebox cloud city`, `!jukebox duels`) and lists up to 20 matches with their
numbers, `!jukebox <number>` plays any track for everyone and `!jukebox random` plays a random one. Picks are announced
in chat; the map's own music comes back next round. See `codemp/server/jukebox.cpp`.

### Pazaak

`!pazaak <player> <credits>` challenges someone to KOTOR's card game; they `!pazaak accept` or `!pazaak decline` within 60 seconds. Both stakes are taken on accept and the winner gets the pot. The server deals from a 1-10 deck and each player gets four side cards (+1..+6 or -1..-6). On your turn you're dealt a card, may `!pz play <n>` one side card, then `!pz end` or `!pz stand`. Over 20 is a bust; closest to 20 wins the set, first to two sets wins. Turns time out after 30 seconds (you stand); leaving or `!pz forfeit` loses. See `codemp/server/pazaak.cpp`.

### Chance

`!chance <player> <credits>` challenges someone; they answer `!chance red` or `!chance blue` (or `!chance decline`) within 60 seconds and the challenger gets the other colour. The server rolls red or blue, 50/50, and whoever's colour comes up takes both stakes, announced to everyone. See `codemp/server/chance.cpp`.

### The raffle

A draw every `g_raffleIntervalMinutes` (on the hour by default). Tickets go on sale `g_raffleOpenMinutes` before each draw, announced with the price and how to buy, with reminders at 5 minutes and 1 minute. `!raffle <count>` buys tickets, `!raffle` shows the pool and time left. One ticket wins the whole pool, paid into the account. Fewer than `g_raffleMinEntrants` different players and everyone is refunded instead. Tickets survive a restart. See `codemp/server/raffle.cpp`.

### Shop catalog

Everything in the shop is granted through the same prize code as Chaos Mode, so a bought item behaves
exactly like the same item won from a prize. Vehicles and NPC spawns aren't for sale.

| Category (`!buy <category>`) | Contents |
| --- | --- |
| `pistols` | Bryar, DC-17, Westar-34, Heavy Pistol, classic Bryar, EE-3 |
| `rifles` | E-11, DC-15, CR-2, E-22, DLT-19, bowcasters, Disruptor, repeaters, A280, DLT-20A, M5, T-21, EE-4, Amban, Projectile Rifle, SBD wrist blaster |
| `special` | DEMP2, Flechette, Concussion Rifle, Thrower, Minigun, Shotgun |
| `launchers` | Rocket Launcher, PLX-1 |
| `nades` | All grenades and explosives (frag, pulse, thermal, proximity, fire, sonic, cryo, concussion, trip mine, det pack) |
| `melee` | Lightsaber (random style) |
| `gadgets` | Armor, cloak, E-Web, sentry, seeker, bacta, forcefield, spawner, stimpack, jetpack, shockfield, protocol droid |
| `size` | Size changes (XS / S / L / XL) |
| `ammo` | Ammo refill for your current loadout |

Each item's price is its own cvar, `g_shopCost_<name>`, created with the default below when the server first
starts. **A price of `0` removes the item**, and a category disappears from `!buy` once all its items are at
`0`. Price changes apply immediately (`set g_shopCost_bryar 0`, no restart) and, being `CVAR_ARCHIVE`, persist
across restarts. MBIIEZ's `creditsystem` plugin can set any of them from the instance config's `cvars`.

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
| special | `g_shopCost_demp2` | 25 |
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

### Accounts

- A handle plus a 4-digit PIN. PINs are never stored: each account has a random salt and only a salted
  HMAC-MD5 hash of the PIN is kept.
- **5 wrong PINs lock the account for 60 seconds.**
- Logging in from a new connection kicks any other session logged into the same handle.
- Accounts live in `economy_accounts.dat` in the game folder (`fs_basepath/fs_game`, e.g.
  `/opt/openjk/MBII/`). Every server on the machine shares that folder, so **accounts and balances are shared
  across all your servers**. The file is locked while it's being written, so several servers can use it at
  once.

### Admin commands

| Command | What it does |
|---|---|
| `givecredits <player> <amount>` | Give (or with a negative amount, take) credits from a connected player, by slot or name |

MBIIEZ's web panel has an **Economy** page for managing balances by account handle, including players who
are offline.

---

## Chaos Mode

Every `g_chaosCooldown` seconds, **every player gets a random prize**: a weapon with ammo, an item, armor, a
size change, a vehicle, god mode and so on, from the same prize pool the shop uses.

| Cvar | Default | Meaning |
|---|---|---|
| `g_chaosEnable` | `0` | Turn Chaos Mode on |
| `g_chaosCooldown` | `20` | Seconds between prizes, per player |

- Each player's timer starts when they spawn: the first prize comes 2 seconds in, then one every cooldown.
  Timers reset every round.
- Spectators and Droidekas are skipped.
- A reminder ("Chaos Mode enabled! Prizes for everyone every N seconds!") is broadcast every 3 minutes.

---

## Gun Game

Every player, whatever class they pick, has **one weapon (plus melee)** and moves up a fixed ladder with each
kill:

> Bryar pistol → E-11 blaster → DC carbine → CR-2 → E-22 → clone rifle → A280 → DLT-19 → repeater → bowcaster →
> disruptor → shotgun → flechette → DEMP2 → concussion rifle → rocket launcher → **lightsaber**

| Cvar | Default | Meaning |
|---|---|---|
| `g_gungame` | `0` | Turn Gun Game on |
| `g_gungameAnnounce` | `1` | Broadcast "X advanced to weapon n/17!" and "X WINS Gun Game!" |

- **Dying doesn't cost you your place**: you only go up. Your place carries over between rounds and resets
  when you disconnect, or when Gun Game is switched off and on again.
- Switching to any other weapon is blocked, so you can't get around the ladder.
- Reaching the lightsaber wins.
- MBIIEZ's `gungame` plugin can also limit which classes can be picked while Gun Game is on
  (`gungame_restrict_classes`).

---

## Social Mode

A hang-out mode on top of whatever MBII mode the server runs (e.g. Legends, with all its classes):

- **Nobody takes damage**, players or NPCs (including vehicles). Death pits, lava/water, out-of-bounds, doors, `/kill` and team/class switches still
  work as normal.
- **Spawn in any time.** Dying or joining mid-round puts you on a short respawn timer instead of sitting out the
  round. Players who pick a class but somehow don't spawn are automatically re-joined for them.
- **Duels.** Bow at someone to challenge them; they bow back to accept. Any class, any weapon, as in MBII's
  Duel mode. The two duelists can hurt each other (and only each other) until one dies, then the loser respawns as normal.
- **No team-kill points.** A teammate killing you (say, in a duel) counts as a suicide: no TK points, no
  punish/forgive prompt, no TK respawn penalty.
- **Optional round length** that replaces the map's own round timer, with the on-screen clock kept in step.

| Cvar | Default | Meaning |
|---|---|---|
| `g_socialMode` | `0` | Turn Social Mode on |
| `g_socialRespawnTime` | `3` | Respawn wait in seconds |
| `g_socialDuels` | `1` | Allow duels (bow to challenge / accept) |
| `g_socialRoundTime` | `0` | Round length in seconds; `0` keeps the map's own |
| `g_socialBots` | `1` | Bots pick a random Legends class so they actually spawn (they never pick one themselves). On by default on every Legends server, social mode or not; only affects bots |

Only servers with `g_socialMode 1` have MBII's damage function hooked; everywhere else it runs untouched.

MBII isn't modified: the engine finds MBII's own respawn-mode, duel and round-timer code by name in the loaded
game module and switches it on (see the header of `codemp/server/social.cpp` for exactly how, and which MBII
internals it relies on). If an MBII update renames or changes something it needs, the affected part switches
itself off with a yellow `Social mode:` line in the server console rather than guessing.

---

## Kill streaks

Kills in a row without dying trigger a **server-wide callout**, with a random phrase for each level and a
mention of how the kill was made when it's distinctive (saber, explosives, a sniper shot, bare hands).

| Cvar | Default | Meaning |
|---|---|---|
| `g_killstreakEnable` | `0` | Turn kill streak callouts on |

- Callouts at **3, 5, 7, 10 and 15** kills, then every 5 kills after that (20, 25...).
- Streaks reset every round and on map changes. A player who leaves doesn't pass their streak on to whoever
  joins in their slot next.

---

## Stats

The engine keeps **kills, deaths, suicides and playtime** for every player and answers `!stats`.

| Cvar | Default | Meaning |
|---|---|---|
| `g_statsEnable` | `0` | Turn stats tracking and `!stats` on |

- Stored in `player_stats.dat` in the game folder (e.g. `/opt/openjk/MBII/`), so **stats are shared across
  every server on the machine**. The file is locked while being written.
- Players who are **logged into an economy account** are tracked by account handle, so their stats follow
  them even if they change name. Everyone else is tracked by player name.
- Playtime is saved every minute.
- MBIIEZ's web panel has a **Stats** page showing everyone's stats.

---

## Chat flood control

Always on. A player who sends **more than 5 chat messages (`say` / `say_team`) within 10 seconds is muted for
10 seconds**, counted from the message that set it off. This covers economy commands too, so `!buy` and
`!bounty` spam is limited like any other chat. It's separate from the engine's generic `sv_floodProtect`,
which limits all commands.

## Nute Gunray class block

Always on. Selecting Nute Gunray is blocked at the class-selection step. Once someone is playing as him,
MBII's Siege rules give them no way to switch back out mid-round, so the server stops it from happening
in the first place.

---

## Other server commands

| Command | What it does |
|---|---|
| `spinwin <clientNum> <prize>` | Give a player a specific prize from the Chaos/shop prize pool, by name or index (testing and admin fun) |
| `givecredits <player> <amount>` | See [Economy admin commands](#admin-commands) |

Notes on how prizes are granted:

- A lightsaber also gives BP (which is also jetpack fuel) and saber defence 3. Use `removeforce` to take them
  away if needed.
- A jetpack comes with 100 fuel.
- Removing force also removes saber defence.

---

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
