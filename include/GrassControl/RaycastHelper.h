#pragma once

#include "GrassControl/Config.h"
#include "GrassControl/ObjectHandler.h"
#include "GrassControl/PhysicsRaycast.h"
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

		// Exempt candidates skip texture and collision checks
		bool IsExempt(RE::TESObjectLAND* land, float x, float y, float z, const RE::GrassParam* param) const;

		// Loaded-cell grid and land access must stay on the caller
		bool OnBlockedTexture(RE::TESObjectLAND* land, float x, float y, float z) const;

		bool IsCliff(RE::FormID formID) const;

		bool HasCliffShapeRule(RE::FormID formID) const;

		bool IsCliffAt(const PhysicsRaycast::HitResult& hit, const RE::NiPoint3& point) const;

		// Collision only, texture filtering belongs to the caller
		bool CanPlaceGrass(RE::TESObjectLAND* land, float x, float y, float z, RE::GrassParam* param, bool ignoreCliffs) const;
	};
}
