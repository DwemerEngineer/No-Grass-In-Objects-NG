#pragma once

namespace GrassControl::Config
{
	void ReadSettings();

	bool WriteSettings(std::error_code& error);

	bool WriteBoolSetting(const char* section, const char* key, bool value);

	inline bool DebugLogEnable = false;
	inline bool AskedRaycastWarning = false;

	inline bool RayCast = true;
	inline double RayCastHeight = 150.0;
	inline double RayCastDepth = 5.0;
	inline std::string RayCastCollisionLayers = "1 2 13 20 31";
	inline std::string RayCastIgnoreForms = "";
	inline std::string RayCastTextureForms = "";
	inline std::string RayCastIgnoreGrassForms = "";
	inline double RayCastTextureWidth = 5.0;
	inline bool GrassCliffs = true;
	inline std::string GrassCliffsForms = "13303:Skyrim.esm;B8D38:Skyrim.esm;5092D:Skyrim.esm;24E48:Skyrim.esm;B018B:Skyrim.esm;B018C:Skyrim.esm;56AD2:Skyrim.esm;10A7F5:Skyrim.esm;5B82C:Skyrim.esm;55A69:Skyrim.esm;5C164:Skyrim.esm;97065:Skyrim.esm;135E2:Skyrim.esm;BAD90:Skyrim.esm;4D709:Skyrim.esm;B018A:Skyrim.esm;B018D:Skyrim.esm;56AD5:Skyrim.esm;22763:Skyrim.esm;5B7E1:Skyrim.esm;D37C0:Skyrim.esm;55A6B:Skyrim.esm;8976D:Skyrim.esm;897C5:Skyrim.esm;16D5F:Skyrim.esm;50928:Skyrim.esm;3ACD3:Skyrim.esm;39A8E:Skyrim.esm;B0187:Skyrim.esm;B018E:Skyrim.esm;56ACD:Skyrim.esm;1EF4A:Skyrim.esm;5B7E2:Skyrim.esm;61924:Skyrim.esm;55A6D:Skyrim.esm;197C2:Skyrim.esm;5C107:Skyrim.esm;8976E:Skyrim.esm;56ADC:Skyrim.esm;60095:Skyrim.esm;B018F:Skyrim.esm;B0190:Skyrim.esm;58EF5:Skyrim.esm;8B025:Skyrim.esm;60096:Skyrim.esm;D37B7:Skyrim.esm;B8A78:Skyrim.esm;B9528:Skyrim.esm;13976:Skyrim.esm;46186:Skyrim.esm;4D715:Skyrim.esm;B425E:Skyrim.esm;B425F:Skyrim.esm;1EF3F:Skyrim.esm;56AD7:Skyrim.esm;5B887:Skyrim.esm;1EF3C:Skyrim.esm;61925:Skyrim.esm;6A8C8:Skyrim.esm;19845:Skyrim.esm;5C16E:Skyrim.esm;9707C:Skyrim.esm;8976F:Skyrim.esm;5092B:Skyrim.esm;13A5B:Skyrim.esm;BC5CB:Skyrim.esm;6E0DD:Skyrim.esm;56AD8:Skyrim.esm;1EF4B:Skyrim.esm;5B7DE:Skyrim.esm;61923:Skyrim.esm;6A8C9:Skyrim.esm;19856:Skyrim.esm;659EE:Skyrim.esm;50931:Skyrim.esm;B0189:Skyrim.esm;98BE2:Skyrim.esm;998C9:Skyrim.esm;13974:Skyrim.esm;DFA3A:Skyrim.esm;42A88:Skyrim.esm;39B63:Skyrim.esm;B0188:Skyrim.esm;B0191:Skyrim.esm;56AD9:Skyrim.esm;D1EC2:Skyrim.esm;5B7E3:Skyrim.esm;D1EC3:Skyrim.esm;8A03E:Skyrim.esm;19885:Skyrim.esm;43947:Skyrim.esm;8976C:Skyrim.esm;897C6:Skyrim.esm";
	inline int RayCastMode = 1;
	inline double RayCastWidth = 0.0f;
	inline double RayCastWidthMult = 0.3f;
	inline bool RayCastError = true;

	inline bool SuperDenseGrass = false;
	inline int SuperDenseMode = 8;
	inline bool ProfilerReport = false;
	inline bool UseGrassCache = false;
	inline bool ExtendGrassDistance = false;
	inline bool ExtendGrassCount = true;
	inline int EnsureMaxGrassTypesPerTextureSetting = -1;
	inline double OverwriteGrassDistance = -1;
	inline double OverwriteGrassFadeRange = -1;
	inline int OverwriteMinGrassSize = -1;
	inline double OverwriteGrassFadeInTime = 0.0;
	inline double GlobalGrassScale = 1.0;
	inline bool OnlyLoadFromCache = false;
	inline std::string SkipPregenerateWorldSpaces = "DLC2ApocryphaWorld;DLC01Boneyard;WindhelmPitWorldspace";
	inline std::string OnlyPregenerateWorldSpaces = "";
	inline int DynDOLODGrassMode = 0;
	inline int MaxFailures = 2;
	inline bool FreezeCheck = true;
	inline int FreezeTime = 90000;
	inline bool Updating = false;
	inline bool loadFromBSA = false;
	inline bool ThreadedGrassPlacement = false;

	// Derived from the PrecacheGrass marker beside the game executable.
	inline bool GeneratingCache = false;

	inline std::mutex iniMutex;

	using Value = std::variant<bool*, int*, double*, std::string*>;

	struct Entry
	{
		const char* section;
		const char* key;
		Value value;
		const char* comment;
	};

	inline const std::array entries = std::to_array<Entry>({
		{ "Debug", "Debug-Log-Enable", &DebugLogEnable, ";Enable additional debug logging." },
		{ "Debug", "Asked-Enable-Raycast-Warning", &AskedRaycastWarning, nullptr },
		{ "RayCastConfig", "Ray-cast-enabled", &RayCast, ";Use ray casting to detect where grass shouldn't grow (inside rocks or roads)." },
		{ "RayCastConfig", "Ray-cast-height", &RayCastHeight, ";The distance above grass that must be free. 200 is slightly more than player height." },
		{ "RayCastConfig", "Ray-cast-depth", &RayCastDepth, ";The distance below grass that must be free." },
		{ "RayCastConfig", "Ray-cast-collision-layers", &RayCastCollisionLayers, ";Which collision layers to check when raycasting. Not recommended to change unless you know what you're doing. These are collision layer index from CK separated by space." },
		{ "RayCastConfig", "Ray-cast-ignore-forms", &RayCastIgnoreForms, ";Which objects will raycast ignore. This can be useful if you want grass to still grow from some objects (such as roads).The format is formid : formfile separated by; for example \"1812A:Skyrim.esm;1812D:Skyrim.esm\" would add RoadChunkL01 and RoadChunkL02 forms to ignore list. Base forms go here not object references !" },
		{ "RayCastConfig", "Ray-cast-texture-forms", &RayCastTextureForms, ";Which landcape textures raycast will check for. This can be useful if you want to prevent grass from growing on certain landscape textures (such as roads textures from Northern Roads). The format is formid : formfile separated by; for example \"425FE:Northern Roads.esp;2D5520:Northern Roads.esp\" would add COTN_LRoadStone01 and COTN_LRoadDirt01Trail77 forms to the texture list to check. Landscape textures go here not texturesets !" },
		{ "RayCastConfig", "Ray-cast-ignore-grass-forms", &RayCastIgnoreGrassForms, ";Which Grass types to ignore raycasting. Useful if you want certain grass types to be close to objects like rift leaves. The format is formid : formfile separated by; for example \"009988:Folkvangr - Grass and Landscape Overhaul.esp;024AA4:Folkvangr - Grass and Landscape Overhaul.esp\" would add the large and small apsen leaf piles from Folkvangr to the list to check to skip over. Any grass that is of ay of the types specified will not have a raycast check run for them." },
		{ "RayCastConfig", "Ray-cast-texture-width", &RayCastTextureWidth, ";The distance from the grass in each direction that should be checked as well as the point under the grass for the land texture(s) specified in Ray-cast-texture-forms." },
		{ "RayCastConfig", "Grass-cliffs-enabled", &GrassCliffs, ";Grow grass on top of cliff objects. The texture on top of each cliff is matched to the landscape texture with the same diffuse texture, and that landscape texture's grass is used. Grass under cliffs is removed." },
		{ "RayCastConfig", "Grass-cliffs-forms", &GrassCliffsForms, ";The forms of the dirt cliffs in the game that will be converted to grass cliffs." },
		{ "RayCastConfig", "Ray-cast-mode", &RayCastMode, ";Collision query shape: 0 = line, 1 = capsule, 2 = box. Volume queries use Ray-cast-width or the grass model bounds." },
		{ "RayCastConfig", "Ray-cast-width", &RayCastWidth, ";Width in game units for modes 1 and 2. Zero derives the dimensions from each grass model's bounds." },
		{ "RayCastConfig", "Ray-cast-width-multiplier", &RayCastWidthMult, ";Scales dimensions derived from grass model bounds; does not affect an explicit Ray-cast-width." },
		{ "RayCastConfig", "Ray-cast-error-message", &RayCastError, ";Show warnings when repeated physics queries fail. Failed queries allow fallback placement, which can leave grass inside objects." },
		{ "GrassConfig", "Super-dense-grass", &SuperDenseGrass, ";Enable much more grass without having to change mod files." },
		{ "GrassConfig", "Super-dense-mode", &SuperDenseMode, ";How the super dense mode is achieved. Not recommended to change for normal play. This does nothing unless you enable SuperDenseGrass setting.7 is normal game(meaning nothing is actually changed), 6 or less would be much less grass, 8 is dense, 9 is ?, 10 + is probably crash city." },
		{ "GrassConfig", "Profiler-report", &ProfilerReport, ";Whether to track how much time is taken to generate grass. Whenever you open console the result is reported. Try disabling all settings except profiler, go to game and in main menu 'coc riverwood', after loading open console to see normal game time. Then enable settings and check again how it changed. Remember to coc from main menu instead of loading save because it might not be accurate otherwise." },
		{ "GrassConfig", "Use-grass-cache", &UseGrassCache, ";This will generate cache files in /Data/Grass/ and use those when we have them. Any time you change anything with your mod setup you must delete the contents of that directory so new cache can be generated or you will have bugs like floating grass or still grass in objects(that were changed by mods)." },
		{ "GrassConfig", "Extend-grass-distance", &ExtendGrassDistance, ";Set true if you want to enable extended grass distance mode. This will allow grass to appear outside of loaded cells.Relevant ini settings : SkyrimPrefs.ini[Grass] fGrassStartFadeDistance" },
		{ "GrassConfig", "Extend-grass-count", &ExtendGrassCount, ";Allow more grass to be made in total. This is needed if you use very dense grass or have large draw distance. Otherwise it will reach a limit and just stop making grass leaving weird empty squares." },
		{ "GrassConfig", "Ensure-max-grass-types-setting", &EnsureMaxGrassTypesPerTextureSetting, ";Makes sure that the max grass types per texture setting is set to at least this much. Can be useful to make sure your INI isn't being overwritten by anything. Most grass replacer mods require this. Set 0 to disable any verification." },
		{ "GrassConfig", "Overwrite-grass-distance", &OverwriteGrassDistance, ";Overwrite fGrassStartFadeDistance from any INI. If this is zero or higher then the grass distance will always be taken from this configuration instead of any INI.This can be useful if you have a million things overwriting your INI files and don't know what to edit, so you can just set it here. For example 7000 is vanilla highest in-game grass slider. If you want to set higher you need to enable ExtendGrassDistance setting as well or it will not look right in - game. What the setting actually means is that grass will start to fade out at this distance. Actual total grass distance is this value + fade range value." },
		{ "GrassConfig", "Overwrite-grass-fade-range", &OverwriteGrassFadeRange, ";Overwrite fGrassFadeRange from any INI. If this is zero or higher then the grass fade range will always be taken from this configuration instead of any INI. This determines the distance it takes for grass to completely fade out starting from OverwriteGrassDistance(or fGrassStartFadeDistance if you didn't use the overwrite). If you want the grass fade out to not be so sudden then increase this value.Probably recommended to keep this at least half of the other setting." },
		{ "GrassConfig", "Overwrite-min-grass-size", &OverwriteMinGrassSize, ";Overwrite iMinGrassSize from any INI. If this is zero or higher then the grass density setting (iMinGrassSize) will be taken from here instead of INI files. Lower values means more dense grass. 50 or 60 is normal mode with somewhat sparse grass(good performance).Lowering the value increases density !20 is very high density. You should probably not set lower than 20 if you decide to change it. The grass form density setting itself still mostly controls the actual density of grass, think of iMinGrassSize more of as a cap for that setting." },
		{ "GrassConfig", "Overwrite-grass-fade-in-time", &OverwriteGrassFadeInTime, ";Overwrite fGrassFadeInTime from any INI. If this is zero or higher then newly placed grass fades in over this many seconds instead of the INI value (default 1.8). 0 shows replaced grass immediately, so cells switching to full grass do not visibly disappear and fade back in. Set -1 to keep the INI value." },
		{ "GrassConfig", "Global-grass-scale", &GlobalGrassScale, ";Apply this scale to every piece of grass everywhere. For example 0.5 makes all grass pieces half the size it should be." },
		{ "GrassConfig", "Only-load-from-cache", &OnlyLoadFromCache, ";This will change how grass works. It means grass can only ever be loaded from files and not generated during play." },
		{ "GrassConfig", "Skip-pregenerate-world-spaces", &SkipPregenerateWorldSpaces, ";When pre-generating grass then skip worldspaces with this editor IDs. This can greatly speed up generating if we know the worldspace will not need grass at all." },
		{ "GrassConfig", "Only-pregenerate-world-spaces", &OnlyPregenerateWorldSpaces, ";If this is not empty then skip every worldspace that isn't in this list." },
		{ "GrassConfig", "DynDOLOD-Grass-Mode", &DynDOLODGrassMode, ";Enable grass compatibility mode with DynDOLOD Grass LODs. Valid values: 0 = Disabled; 1 = Display grass only in active cells(without fade) and let DynDOLOD handle inactive cell Grass; 2 = Display grass only in active cells and large ref loaded cells(without fade) and let DynDOLOD handle grass outside of large ref cells " },
		{ "GrassConfig", "Max-Failures", &MaxFailures, ";Maximum number of attempts to generate a grass cache before skipping a cell." },
		{ "GrassConfig", "Freeze-check-enabled", &FreezeCheck, ";Enable or disable the freeze check that is run while generating a grass cache. Recommended to leave enabled unless the message repeately appears." },
		{ "GrassConfig", "Freeze-check-time", &FreezeTime, ";Controls how long until the game is considered frozen in ms since the last file was created, default is a minute and a half." },
		{ "GrassConfig", "Updating-Cache", &Updating, ";If you are generating a cache for a worldspace you have not generated yet, set this true. Otherwise, the cache files in your grass folder will be deleted. If you are updating a worldspace, then move all files you do not want to be deleted to a new folder and replace the old files in the folder once you have generated new cache files. Then rename/move the folder back." },
		{ "GrassConfig", "Load-from-BSA", &loadFromBSA, ";Disables the empty grass folder/missing grass cache message displayed in the main menu when the grass folder is empty. If you pack your cache in to a BSA, you will want to enabled this to remove the message as it does not detect the packed cache files." },
		{ "GrassConfig", "Threaded-grass-placement", &ThreadedGrassPlacement, ";Spread grass placement over several threads. Grass placed by NGIO is laid out differently from the game's own placement and from cache files made without NGIO, so blades rearrange when such a cached cell becomes active." },
	});

	inline constexpr std::string_view iniPath = "Data/SKSE/Plugins/GrassControl.ini";
}
