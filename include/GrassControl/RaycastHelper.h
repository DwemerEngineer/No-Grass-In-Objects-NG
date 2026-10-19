#pragma once

#include "GrassControl/Config.h"
#include "GrassControl/ObjectHandler.h"
#include "GrassControl/Util.h"

namespace GrassControl
{
	class RaycastHelper final
	{
	public:
		RaycastHelper(float rayHeight, float rayDepth, const std::string& layers, std::unique_ptr<Util::CachedFormList> ignored, std::unique_ptr<Util::CachedFormList> textures, std::unique_ptr<Util::CachedFormList> cliffs, std::unique_ptr<Util::CachedFormList> grassTypes);

		const float RayHeight;
		const float RayDepth;
		const int Mode = Config::RayCastMode;
		const double Width = Config::RayCastWidth;
		const double WidthMultiplier = Config::RayCastWidthMult;
		const double TextureWidth = Config::RayCastTextureWidth;

		bool Valid = false;
		std::uint64_t RaycastMask = 0;

		std::unique_ptr<Util::CachedFormList> const Ignore;
		std::unique_ptr<Util::CachedFormList> const Textures;
		std::unique_ptr<Util::CachedFormList> const Cliffs;
		std::unique_ptr<Util::CachedFormList> const Grasses;

		std::unordered_map<RE::FormID, CliffObject> cliffObjects;
		std::unordered_map<RE::FormID, PartIgnoredObject> partIgnoredObjects;

		// Exempt candidates bypass both texture and collision filtering.
		bool IsExempt(RE::TESObjectLAND* land, float x, float y, float z, const RE::GrassParam* param) const;

		// Reads the loaded-cell grid; do not call from placement workers.
		bool OnBlockedTexture(float x, float y, float z) const;

		bool IsCliff(RE::FormID formID) const;

		bool HasCliffShapeRule(RE::FormID formID) const;

		bool IsCliffAt(RE::FormID formID, RE::ObjectRefHandle reference, const RE::NiPoint3& point) const;

		// Checks collision only; callers apply texture filtering separately.
		bool CanPlaceGrass(RE::TESObjectLAND* land, float x, float y, float z, RE::GrassParam* param, bool ignoreCliffs) const;
	};
}
