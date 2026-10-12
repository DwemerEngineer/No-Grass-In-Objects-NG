## No Grass in Objects NG

This is a port of [No Grass in Objects](https://www.nexusmods.com/skyrimspecialedition/mods/42161) rewritten using Commonlib. The config is roughly the same with a change to ini that should be created while the mod is first loaded, minor renaming of settings and an option for debug logs. To my knowledge all features should be working in SE/VR, the same goes for AE.

---

## MO2 Installation Instructions:

1. Download and install the Mod Organizer 2 plugin from the original mod into your MO2 plugins directory, located here: [`Grass Generation MO2 Plugin v1 (fix)`](https://www.nexusmods.com/skyrimspecialedition/mods/42161?tab=files).

- You will need to edit lines 56 and 57 to reflect `grassPluginPath = "SKSE/Plugins/"` and `grassPlugin = "*NGIO-NG.dll"`, as shown below.

  ![Screenshot 2024-04-20 080244](https://github.com/AllstaRawR/No-Grass-In-Objects-NG/assets/164591926/af9cced5-e1f9-49b8-8014-863e6195d87f)

2. You will then need to install the latest version of NGIO-NG located in the releases section [here on Github](https://github.com/SaneEngineer/No-Grass-In-Objects-NG/releases/). This needs to be installed to the `\Data\SKSE\Plugins\...` directory of your Skyrim installation.
3. You will now click on the `Tools -> Plugins -> Precache Grass` option in your MO2 toolbar, which will launch (and re-launch) Skyrim to generate grass cache in your `Overwrites` folder per the settings in `GrassControl.ini`.
4. After you have waited, you should get a pop up message that says, `Grass Caching complete`, at which point you can copy the `\Grass\` folder from `Overwrites` into a new mod in MO2.

---

## Vortex/Manual Installation Instructions:

1. Install the latest version of NGIO-NG located in the releases section [here on Github](https://github.com/SaneEngineer/No-Grass-In-Objects-NG/releases/). This needs to be installed with the `Install from file` option in Vortex or installed manually to the `\Data\SKSE\Plugins\...` directory of your Skyrim installation.
2. In order to start the Grass Cache, set `Use-grass-cache = true, Only-load-from-cache = true` in `GrassControl.ini`
3. Create a new text document called `PrecacheGrass.txt` in the root folder of Skyrim Next to SkyrimSE.exe.
   This can be found by opening Steam and locating Skyrim SE in your game library, right-click it and navigate to Manage > Browse Local Files
4. The next time you open the game, caching should begin. Caching is expected to crash repeately, just reopen the game and it should continue.

---

## Common Issues:

- Multiple `GrassControl.ini` copies under a mod manager
  - NGIO reads the INI visible through the game's virtual Data directory. Warning preferences update that existing file; defaults are created only when it is missing. Read or save failures are logged rather than creating a replacement. Existing copies in other mods or Overwrite remain under your mod manager's precedence rules; edit the visible winner.
- Instant completion of Caching or completion of caching for only some worldspaces.
  - Check `Skip-pregenerate-world-spaces` and `Only-pregenerate-world-spaces`, make sure that both of these options are only one line.
- Crashing while Caching
  - Check `NGIO-NG/log`, `PrecacheGrass.txt`, and `Data\Grass\` to see if any progress is being done. If no progress is being made check any crash logs as the crash is likely caused by an issue with a mod affecting the area. This can be a texture, mesh, patch, or mod editing the area. It should be mentioned by the crash log.
  - Consider using a process restarter (e.g., [Restart on Crash](https://w-shadow.com/blog/2009/03/04/restart-on-crash/)) for unattended generation.
  - I recommend using [Crash Logger SSE AE VR](https://www.nexusmods.com/skyrimspecialedition/mods/59818?tab=description) over [Trainwreck](https://www.nexusmods.com/skyrimspecialedition/mods/106440). As it provides much more helpful and readable crash logs.
- Raycasting not working
  - Raycasting when used without a grass cache `Use-grass-cache = false` does not apply without loading a save twice. Raycasting will always take effect when a cache is generated with it enabled `Use-grass-cache = true`.
  ***

## User Requirements

- [Address Library for SKSE](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
  - Needed for SSE/AE
- [VR Address Library for SKSEVR](https://www.nexusmods.com/skyrimspecialedition/mods/58101)
  - Needed for VR

## License

[GPL-3.0-or-later](LICENSE) WITH [Modding Exception AND GPL-3.0 Linking Exception (with Corresponding Source)](EXCEPTIONS.md).

The Modded Code is Skyrim and its variants. The Modding Libraries are [SKSE](https://skse.silverlock.org/) and Windows.

The original [MIT license](licenses/LICENSE-MIT) is preserved for code previously released under those terms. Third-party components retain their own licenses; the historical [CommonLibSSE MIT notice](licenses/LICENSE-CommonLibSSE-MIT) and the bundled [Stopwatch license](licenses/LICENSE-Stopwatch) are also retained.
