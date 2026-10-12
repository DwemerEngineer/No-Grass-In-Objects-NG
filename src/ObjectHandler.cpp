#include "GrassControl/ObjectHandler.h"
#include "GrassControl/Util.h"
#include <CLibUtil/String.hpp>

namespace
{
	using namespace GrassControl;

	std::optional<RE::FormID> ParseObjectKey(const char* key, const std::string& settingName, std::vector<std::string>& parts)
	{
		parts = clib_util::string::split(key, "|");
		if (parts.empty())
			return std::nullopt;

		for (auto& part : parts)
			part = Util::StringHelpers::trim(part);

		const auto forms = Util::CachedFormList::TryParse(parts.front(), settingName);
		if (forms->Empty()) {
			logger::warn("Invalid formID {} found, skipping", parts.front());
			return std::nullopt;
		}

		const auto formID = *forms->GetAllFormIDs().begin();
		parts.erase(parts.begin());
		return formID;
	}

	void AddCliffObjects(CSimpleIniA::TNamesDepend& values, std::unordered_map<RE::FormID, CliffObject>& cliffObjects)
	{
		for (const auto& key : values) {
			std::vector<std::string> splitKey;
			const auto formID = ParseObjectKey(key.pItem, "CliffObjects", splitKey);
			if (!formID)
				continue;

			std::unordered_set<std::string> allowedShapes;
			std::unordered_set<std::string> blockedShapes;

			bool steep = false;

			for (auto& str : splitKey) {
				if (str.contains("~")) {
					std::erase(str, '~');
					blockedShapes.insert(str);
				} else if (str.contains("Steep") || str.contains("steep")) {
					steep = true;
				} else {
					allowedShapes.insert(str);
				}
			}

			cliffObjects.try_emplace(*formID, steep, std::move(allowedShapes), std::move(blockedShapes));
		}
	}

	void AddPartIgnoredObjects(CSimpleIniA::TNamesDepend& values, std::unordered_map<RE::FormID, PartIgnoredObject>& partIgnoredObjects)
	{
		for (const auto& key : values) {
			std::vector<std::string> splitKey;
			const auto formID = ParseObjectKey(key.pItem, "IgnoredShapes", splitKey);
			if (!formID)
				continue;

			partIgnoredObjects.try_emplace(*formID, std::unordered_set<std::string>(splitKey.begin(), splitKey.end()));
		}
	}
}

namespace GrassControl::ObjectHandler
{
	void LoadObjectConfigs(std::unordered_map<RE::FormID, CliffObject>& cliffObjects, std::unordered_map<RE::FormID, PartIgnoredObject>& partIgnoredObjects)
	{
		const auto configs = Util::Files::ObjectConfigs();

		if (configs.empty()) {
			logger::warn("No .ini files with _NGIO suffix were found within the Data folder. Overrides will not be loaded");
			return;
		}

		logger::info("{} matching inis found", configs.size());

		for (const auto& config : configs) {
			logger::info("Loading object config {}...", Util::Files::PathForLog(config));

			CSimpleIniA ini;
			ini.SetUnicode();
			ini.SetMultiKey();
			ini.SetAllowKeyOnly();

			std::string contents;
			std::error_code error;
			if (!Util::Files::ReadText(config, contents, error)) {
				logger::error("Failed to read object config: {}", error.message());
				continue;
			}

			if (const auto rc = ini.LoadData(contents); rc < 0) {
				logger::error("Failed to parse object config (code: {})", rc);
				continue;
			}

			CSimpleIniA::TNamesDepend sections;
			ini.GetAllSections(sections);
			sections.sort(CSimpleIniA::Entry::LoadOrder());

			for (const auto& section : sections) {
				const std::string_view name(section.pItem);
				const auto type = name.substr(0, name.find('|'));
				if (type != "CliffObjects" && type != "IgnoredShapes")
					continue;

				CSimpleIniA::TNamesDepend values;
				ini.GetAllKeys(section.pItem, values);
				values.sort(CSimpleIniA::Entry::LoadOrder());
				if (values.empty())
					continue;

				if (type == "CliffObjects") {
					logger::info("{} grass objects found", values.size());
					AddCliffObjects(values, cliffObjects);
				} else {
					logger::info("{} ignored triShapes found", values.size());
					AddPartIgnoredObjects(values, partIgnoredObjects);
				}
			}

			logger::info("... Finished loading object config");
		}

		logger::info("All Object Configs loaded");
	}
}
