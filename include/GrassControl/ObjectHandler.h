#pragma once

namespace GrassControl
{
	struct CliffObject
	{
		const std::unordered_set<std::string> allowedShapes;
		const std::unordered_set<std::string> blockedShapes;
	};

	struct PartIgnoredObject
	{
		const std::unordered_set<std::string> ignoredShapes;
	};

	namespace ObjectHandler
	{
		void LoadObjectConfigs(std::unordered_map<RE::FormID, CliffObject>& cliffObjects, std::unordered_map<RE::FormID, PartIgnoredObject>& partIgnoredObjects);
	}
}
