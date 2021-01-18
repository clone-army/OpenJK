
## This Fork

This fork of OpenJK contains a customised dedicated server for use on "New Republic Clan" servers. Changes to this fork include

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
-  Invisible
- God Mode (30 seconds)

## SMOD Extensions

SMOD extensions is additional code to allow the inserting of additional SMOD functions and commands using the serverside. 
SMOD itself lives on the GAME not the SERVER ENGINE however using some clever coding. We can add new commands. 

### Commands (need to include what level of SMOD is required)

`slay` Kill a given player with a given reason

`slap` Damages a player and throws them some distance with a given reason

`sethealth` Change a players health

`setarmor` Change a players armor



### Still todo

`setteamhealth` sets an entire teams health

`setteamarmor` sets an entire teams armor

`model` change a players model

`teammodel` change a team to a given model

`giveweapon` give a player a weapon and ammo

`giveteamweapon` give an entire team a given weapon and ammo

`giveitem` give a player an item

`giveteamitem` give an entire team an item

`removeforce` removes a players force powers

`removeteamforce` removes a teams force powers

`giveforce` give a player a force power

`giveteamforce` give an entire team a force power

`removeweapons` remove a players weapons

`removeteamsweapons` remove an entire teams weapons

`npc spawn` spawn an NPC in front of a given player

`vehicle spawn` spawn a vehicle in front of a given player

`freezetime` freeze the round time

### Notes
- Giving a saber will automatically give BP (which is also jet fuel)
and saber defence 3. Which can then be removed using removeforce if not needed
- Giving a jetpack will automatically give 100 fuel
- Removing force will remove saber defence as well
- Some commands require the player to be alive
- Slay takes 3 seconds to have an effect


### Weapon Numbers
still to add

### Item Numbers
still to add



## Adding and Contributing to SMOD Extensions (NR Members)

To add new SMOD Commands, 
- Open the `/server/sv_smod.cpp` file in visual studio
- Create a new static void function using the format `static void SM_MyCommand(void)`
- If you are asking the calling admin for a client, you can retreve this with a simple `client_t* cl = SV_BetterGetPlayerByHandle(Cmd_Argv(2));`
- You can check if the `cl->gentity->playerState->persistant[PERS_TEAM] == TEAM_SPECTATOR` if your command should not run on dead players
- Make any changes you wish. 
- Add a line to the `SV_SmodAddCmds` command in the following format given in the function description
- Your command should now show when running `smod` to list available commands
- If there is an error, you command CAN crash the server so due to checks needed to ensure this wont happen
- Once tested on a local server you can commit and push back to this repo
- If Someone has made a change since you last pulled, you will need to copy your changes (to notepad), discard your change on github desktop, so you can then pull the latest version
- Re-add and commit your new changes, and push. 

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
