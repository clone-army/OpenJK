
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
