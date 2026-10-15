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

	inline constexpr std::string_view iniPath = "Data/SKSE/Plugins/GrassControl.ini";
}
