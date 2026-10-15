#include "GrassControl/RaycastHelper.h"
#include "GrassControl/PhysicsRaycast.h"

namespace GrassControl
{
	RaycastHelper::RaycastHelper(float rayHeight, float rayDepth, const std::string& layers, std::unique_ptr<Util::CachedFormList> ignored, std::unique_ptr<Util::CachedFormList> textures, std::unique_ptr<Util::CachedFormList> cliffs, std::unique_ptr<Util::CachedFormList> grassTypes) :
		RayHeight(rayHeight), RayDepth(rayDepth), Ignore(std::move(ignored)), Textures(std::move(textures)), Cliffs(std::move(cliffs)), Grasses(std::move(grassTypes))
	{
		for (const auto& token : Util::StringHelpers::SplitAtAny(layers, { ' ', ',', '\t', '+' }, true)) {
			if (const auto layer = Util::ParseCollisionLayer(token))
				RaycastMask |= 1ULL << *layer;
			else
				logger::warn("Invalid raycast collision layer: {}", token);
		}

		Valid = PhysicsRaycast::ValidateSettings(*this);
		if (!Valid)
			logger::error("Invalid raycast settings: mode {}, height {}, depth {}, width {}, multiplier {}, texture width {}. Physics filtering will be skipped.", Mode, RayHeight, RayDepth, Width, WidthMultiplier, TextureWidth);
	}

	bool RaycastHelper::IsExempt(RE::TESObjectLAND* land, float x, float y, float z, const RE::GrassParam* param) const
	{
		if (!Valid || !PhysicsRaycast::ValidPoint(x, y, z))
			return true;

		const auto* cell = land ? land->GetSaveParentCell() : nullptr;
		if (!cell || cell->IsInteriorCell() || !cell->IsAttached())
			return true;

		return param && Grasses && Grasses->Contains(param->grassFormID);
	}

	bool RaycastHelper::OnBlockedTexture(float x, float y, float z) const
	{
		auto* tes = RE::TES::GetSingleton();
		if (!Textures || !tes)
			return false;

		const auto width = static_cast<float>(TextureWidth);
		const std::array points = { RE::NiPoint3{ x, y, z }, RE::NiPoint3{ x + width, y, z }, RE::NiPoint3{ x - width, y, z }, RE::NiPoint3{ x, y + width, z }, RE::NiPoint3{ x, y - width, z } };
		const std::size_t count = width > 0 ? points.size() : 1;
		for (std::size_t i = 0; i < count; ++i) {
			if (auto* texture = tes->GetLandTexture(points[i]); texture && Textures->Contains(texture->GetFormID()))
				return true;
		}

		return false;
	}

	bool RaycastHelper::IsCliff(RE::FormID formID) const
	{
		return (Cliffs && Cliffs->Contains(formID)) || cliffObjects.contains(formID);
	}

	bool RaycastHelper::HasCliffShapeRule(RE::FormID formID) const
	{
		const auto rule = cliffObjects.find(formID);
		return rule != cliffObjects.end() && (!rule->second.allowedShapes.empty() || !rule->second.blockedShapes.empty());
	}

	bool RaycastHelper::IsCliffAt(RE::FormID formID, RE::ObjectRefHandle reference, const RE::NiPoint3& point) const
	{
		if (!IsCliff(formID))
			return false;

		const auto rule = cliffObjects.find(formID);
		if (rule == cliffObjects.end() || (rule->second.allowedShapes.empty() && rule->second.blockedShapes.empty()))
			return true;

		const auto names = PhysicsRaycast::GetSingleton().GetShapeNames(reference, point);
		return names && Util::IsShapeAllowed(names->shape, names->root, rule->second.allowedShapes, rule->second.blockedShapes);
	}

	bool RaycastHelper::CanPlaceGrass(RE::TESObjectLAND* land, float x, float y, float z, RE::GrassParam* param, bool ignoreCliffs) const
	{
		if (IsExempt(land, x, y, z, param))
			return true;

		auto* cell = land->GetSaveParentCell();
		auto& physics = PhysicsRaycast::GetSingleton();
		const glm::vec4 begin(x, y, z - RayDepth, 0), end(x, y, z + RayHeight, 0);

		const auto result = Mode >= 1 ? physics.CastVolume(begin, end, cell, param, *this, ignoreCliffs) : physics.CastRay(begin, end, cell, *this, ignoreCliffs);
		if (result.status != PhysicsRaycast::QueryStatus::Success)
			return true;

		for (const auto& hit : result.hits) {
			const auto at = Mode >= 1 ? glm::vec4(x, y, z, 0) : begin + (end - begin) * hit.hitFraction;
			const RE::NiPoint3 point{ at.x, at.y, at.z };
			if (ignoreCliffs && hit.hitCliff && IsCliffAt(hit.formID, hit.reference, point))
				continue;

			if (const auto rule = partIgnoredObjects.find(hit.formID); rule != partIgnoredObjects.end()) {
				const auto names = physics.GetShapeNames(hit.reference, point);
				if (names && Util::MatchesShape(names->shape, names->root, rule->second.ignoredShapes))
					continue;
			}

			return false;
		}

		return true;
	}
}
