#pragma once

#include <cstdint>

#include <RE/B/BSCoreTypes.h>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace GrassControl
{
	struct CliffObject
	{
		const bool isSteep;
		const std::unordered_set<std::string> allowedShapes;
		const std::unordered_set<std::string> blockedShapes;
	};

	struct PartIgnoredObject
	{
		const std::unordered_set<std::string> ignoredShapes;
	};
}

namespace GrassControl::ObjectHandler
{
	void LoadObjectConfigs(std::unordered_map<RE::FormID, CliffObject>& cliffObjects, std::unordered_map<RE::FormID, PartIgnoredObject>& partIgnoredObjects);
}
