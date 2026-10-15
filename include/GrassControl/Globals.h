#pragma once

namespace GrassControl::globals
{
	namespace settings
	{
		inline RE::Setting* allowLoadGrass = nullptr;
		inline RE::Setting* gridsToLoad = nullptr;
		inline RE::Setting* largeRefLODGridSize = nullptr;
		inline RE::Setting* grassFadeRange = nullptr;
		inline RE::Setting* grassStartFadeDistance = nullptr;
		inline RE::Setting* minGrassSize = nullptr;
		inline RE::Setting* maxGrassTypesPerTexture = nullptr;
	}

	inline RE::Setting* FindSetting(const char* name)
	{
		if (auto* setting = RE::INISettingCollection::GetSingleton()->GetSetting(name))
			return setting;
		return RE::INIPrefSettingCollection::GetSingleton()->GetSetting(name);
	}

	inline const RE::Setting* WaveOffsetRange()
	{
		static const auto* setting = FindSetting("fWaveOffsetRange:Grass");
		return setting;
	}

	// Initialize before installing hooks that read these settings.
	inline void OnInit()
	{
		settings::allowLoadGrass = FindSetting("bAllowLoadGrass:Grass");
		settings::gridsToLoad = FindSetting("uGridsToLoad:General");
		settings::largeRefLODGridSize = FindSetting("uLargeRefLODGridSize:General");
		settings::grassFadeRange = FindSetting("fGrassFadeRange:Grass");
		settings::grassStartFadeDistance = FindSetting("fGrassStartFadeDistance:Grass");
		settings::minGrassSize = FindSetting("iMinGrassSize:Grass");
		settings::maxGrassTypesPerTexture = FindSetting("iMaxGrassTypesPerTexure:Grass");
	}

	inline bool AllowLoadGrass()
	{
		return settings::allowLoadGrass && settings::allowLoadGrass->GetBool();
	}

	inline int GridsToLoad()
	{
		return settings::gridsToLoad ? settings::gridsToLoad->GetInteger() : 5;
	}

	inline bool IsLoading()
	{
		auto* ui = RE::UI::GetSingleton();
		return !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
	}
}
