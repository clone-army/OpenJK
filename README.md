
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
- `!buy` - list purchasable items (seeker, sentry, cloak, eweb, tripmine, jetpack, bacta, ammo, cryo); `!buy <id>` to purchase
- `!bounty` - list players and active bounties; `!bounty <clientnum> <credits>` to place a bounty (paid to whoever gets the kill)
- `!help` - summary of the above

Credits are awarded automatically: killing another player grants a fixed kill reward plus any bounty on the victim.

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
