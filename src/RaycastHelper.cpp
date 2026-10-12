#include "GrassControl/RaycastHelper.h"
#include "GrassControl/PhysicsRaycast.h"

namespace GrassControl
{
	RaycastHelper::RaycastHelper(float rayHeight, float rayDepth, const std::string& layers, std::unique_ptr<Util::CachedFormList> ignored, std::unique_ptr<Util::CachedFormList> textures, std::unique_ptr<Util::CachedFormList> cliffs, std::unique_ptr<Util::CachedFormList> grassTypes) :
		RayHeight(rayHeight), RayDepth(rayDepth), Ignore(std::move(ignored)), Textures(std::move(textures)), Cliffs(std::move(cliffs)), Grasses(std::move(grassTypes))
	{
		for (const auto& token : Util::StringHelpers::Split_at_any(layers, { ' ', ',', '\t', '+' }, true)) {
			if (const auto layer = Util::ParseCollisionLayer(token))
				RaycastMask |= 1ULL << *layer;
			else
				logger::warn("Invalid raycast collision layer: {}", token);
		}

		Valid = PhysicsRaycast::ValidateSettings(*this);
		if (!Valid)
			logger::error("Invalid raycast settings: mode {}, height {}, depth {}, width {}, multiplier {}, texture width {}. Physics filtering will be skipped.", Mode, RayHeight, RayDepth, Width, WidthMultiplier, TextureWidth);
	}

	bool RaycastHelper::CanPlaceGrass(RE::TESObjectLAND* land, float x, float y, float z, RE::GrassParam* param, bool& hitCliff, bool& falseCliff) const
	{
		const bool ignoreCliff = falseCliff;
		hitCliff = false;
		falseCliff = false;
		if (!Valid || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
			return true;

		if (std::abs(x) > 100000000.0f || std::abs(y) > 100000000.0f || std::abs(z) > 100000000.0f)
			return true;

		auto* cell = land ? land->GetSaveParentCell() : nullptr;
		if (!cell || cell->IsInteriorCell() || !cell->IsAttached())
			return true;

		if (param && Grasses && Grasses->Contains(param->grassFormID))
			return true;

		if (Textures) {
			if (auto* tes = RE::TES::GetSingleton()) {
				const auto width = static_cast<float>(TextureWidth);
				const std::array points = { RE::NiPoint3{ x, y, z }, RE::NiPoint3{ x + width, y, z }, RE::NiPoint3{ x - width, y, z }, RE::NiPoint3{ x, y + width, z }, RE::NiPoint3{ x, y - width, z } };
				const std::size_t count = width > 0 ? points.size() : 1;
				for (std::size_t i = 0; i < count; ++i) {
					if (auto* texture = tes->GetLandTexture(points[i]); texture && Textures->Contains(texture->GetFormID()))
						return false;
				}
			}
		}

		auto& physics = PhysicsRaycast::GetSingleton();
		const glm::vec4 begin(x, y, z - RayDepth, 0), end(x, y, z + RayHeight, 0);

		const auto result = Mode >= 1 ? physics.CastVolume(begin, end, cell, param, *this, ignoreCliff) : physics.CastRay(begin, end, cell, *this, ignoreCliff);

		if (result.status != PhysicsRaycast::QueryStatus::Success)
			return true;

		bool blocked = false;
		for (const auto& hit : result.hits) {
			if (const auto rule = partIgnoredObjects.find(hit.formID); rule != partIgnoredObjects.end()) {
				const auto point = Mode >= 1 ? glm::vec4(x, y, z, 0) : begin + (end - begin) * hit.hitFraction;
				const auto names = physics.GetShapeNames(hit.reference, RE::NiPoint3{ point.x, point.y, point.z });
				if (names && Util::MatchesShape(names->shape, names->root, rule->second.ignoredShapes))
					continue;
			}

			blocked = true;
			hitCliff |= hit.hitCliff && !ignoreCliff;
		}

		return !blocked;
	}

	float RaycastHelper::CreateGrassCliff(RE::TESObjectLAND* land, float x, float y, float z, glm::vec3& normal, RE::GrassParam* param, bool& falseCliff) const
	{
		falseCliff = true;
		if (!Valid || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
			return z;

		auto* cell = land ? land->GetSaveParentCell() : nullptr;
		if (!cell || cell->IsInteriorCell() || !cell->IsAttached() || (!Cliffs && cliffObjects.empty()))
			return z;

		auto* grass = param ? RE::TESForm::LookupByID<RE::TESGrass>(param->grassFormID) : nullptr;
		if (grass && (grass->GetUnderwaterState() == RE::TESGrass::GRASS_WATER_STATE::kBelowOnlyAtLeast || grass->GetUnderwaterState() == RE::TESGrass::GRASS_WATER_STATE::kBelowOnlyAtMost))
			return z;

		auto& physics = PhysicsRaycast::GetSingleton();
		const glm::vec4 begin(x, y, z, 0), end(x, y, z + 600, 0);

		const auto result = physics.CastRay(begin, end, cell, *this);
		if (result.status != PhysicsRaycast::QueryStatus::Success)
			return z;

		float height = z;
		glm::vec3 selectedNormal{};
		const PhysicsRaycast::HitResult* selected = nullptr;
		glm::vec4 selectedPosition = begin;
		for (const auto& hit : result.hits) {
			if (!hit.hitCliff || !hit.formID || hit.hitFraction >= 1)
				continue;

			const auto point = begin + (end - begin) * hit.hitFraction;
			if (point.z <= height)
				continue;

			const auto candidateNormal = -hit.normal;
			if (grass) {
				const auto slope = std::acos(std::clamp(candidateNormal.z, -1.0f, 1.0f));
				if (!std::isfinite(slope) || slope > grass->GetMaxSlope() || slope < grass->GetMinSlope())
					continue;
			}

			height = point.z;
			selectedNormal = candidateNormal;
			selected = &hit;
			selectedPosition = point;
		}

		if (!selected)
			return z;

		float maximumHeightDifference = 30;
		if (const auto rule = cliffObjects.find(selected->formID); rule != cliffObjects.end()) {
			const auto& info = rule->second;
			if (!info.allowedShapes.empty() || !info.blockedShapes.empty()) {
				const auto names = physics.GetShapeNames(selected->reference, RE::NiPoint3{ selectedPosition.x, selectedPosition.y, selectedPosition.z });
				if (!names || !Util::IsShapeAllowed(names->shape, names->root, info.allowedShapes, info.blockedShapes))
					return z;
			}

			if (info.isSteep)
				maximumHeightDifference = 80;
		}

		float width = 80;
		if (grass && grass->boundData.boundMax.x != 0 && grass->boundData.boundMin.x != 0) {
			width = std::max(std::abs(float(grass->boundData.boundMax.x - grass->boundData.boundMin.x) / 2),
						std::abs(float(grass->boundData.boundMax.y - grass->boundData.boundMin.y) / 2)) +
			        40;
		}

		const std::array offsets = { glm::vec2(width, 0), glm::vec2(-width, 0), glm::vec2(0, width), glm::vec2(0, -width) };
		for (const auto& offset : offsets) {
			const glm::vec4 supportBegin(x + offset.x, y + offset.y, height - maximumHeightDifference, 0);

			const glm::vec4 supportEnd(x + offset.x, y + offset.y, height + maximumHeightDifference, 0);

			const auto support = physics.CastRay(supportBegin, supportEnd, cell, *this);
			if (support.status != PhysicsRaycast::QueryStatus::Success || support.hits.empty())
				return z;

			const auto& closest = *std::ranges::min_element(support.hits, {}, &PhysicsRaycast::HitResult::hitFraction);
			const auto position = supportBegin + (supportEnd - supportBegin) * closest.hitFraction;
			if (!closest.formID || (!closest.hitCliff && std::abs(position.z - height) > 20))
				return z;
		}

		normal = selectedNormal;
		if (param)
			param->fitsToSlope = true;
		falseCliff = false;
		return height;
	}
}
