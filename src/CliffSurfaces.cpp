#include "GrassControl/CliffSurfaces.h"

#include "GrassControl/CollisionGrid.h"
#include "GrassControl/Main.h"
#include "GrassControl/RaycastHelper.h"
#include "GrassControl/Util.h"

namespace GrassControl::CliffSurfaces
{
	static RE::TESLandTexture* LandTextureFor(std::string_view texturePath)
	{
		if (auto* texture = GetTextureIndex().Find(texturePath))
			return texture;

		const auto path = NormalizeTexturePath(texturePath);
		std::scoped_lock lock(unmatchedMutex);
		if (unmatched.insert(path).second)
			logger::info("Cliff texture {} matches no landscape texture", path);

		return nullptr;
	}

	static void CollectShapes(RE::NiAVObject* object, const char* root, RE::FormID formID, const CliffObject* rule, std::vector<Surface>& surfaces, std::uint32_t& budget, std::size_t depth = 0)
	{
		if (!object || depth >= 128 || budget == 0 || object->GetFlags().any(RE::NiAVObject::Flag::kHidden))
			return;

		--budget;
		if (auto* node = object->AsNode()) {
			for (auto& child : node->GetChildren())
				CollectShapes(child.get(), root, formID, rule, surfaces, budget, depth + 1);
			return;
		}

		auto* geometry = object->AsGeometry();
		if (!geometry || !(geometry->worldBound.radius > 0.0f))
			return;

		auto* property = GetSurfaceShader(*geometry);
		if (!property)
			return;

		if (rule && !Util::IsShapeAllowed(geometry->name.c_str(), root, rule->allowedShapes, rule->blockedShapes))
			return;

		const auto* texture = property->GetBaseTexture();
		if (!texture || texture->name.empty())
			return;

		if (auto* landTexture = LandTextureFor(texture->name.c_str()); landTexture && !landTexture->textureGrassList.empty())
			surfaces.push_back({ geometry->worldBound.center, geometry->worldBound.radius, formID, landTexture });
	}

	static std::vector<Surface> CollectCell(RE::TESObjectCELL* cell, bool& complete)
	{
		complete = true;
		std::vector<Surface> surfaces;
		const auto settings = Plugin::GetRaycastSettings();
		const auto* coordinates = cell->GetCoordinates();

		if (!settings || !coordinates || cell->IsInteriorCell())
			return surfaces;

		std::vector<RE::NiPointer<RE::TESObjectREFR>> references;
		const auto retain = [&](RE::TESObjectREFR* reference) {
			const auto* base = reference->GetBaseObject();

			if (base && settings->IsCliff(base->GetFormID()) && !reference->IsDisabled())
				references.emplace_back(reference);

			return RE::BSContainer::ForEachResult::kContinue;
		};

		cell->ForEachReference(retain);

		const auto* world = cell->GetRuntimeData().worldSpace;
		if (const auto* persistent = world ? world->persistentCell : nullptr) {

			persistent->ForEachReference([&](RE::TESObjectREFR* reference) {
				const auto position = reference->GetPosition();
				if (std::floor(position.x / 4096.0f) != static_cast<float>(coordinates->cellX) || std::floor(position.y / 4096.0f) != static_cast<float>(coordinates->cellY))
					return RE::BSContainer::ForEachResult::kContinue;

				return retain(reference);
			});
		}

		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			complete = false;
			return surfaces;
		}

		std::uint32_t without3D = 0;
		{
			renderer->Lock();
			const stl::scope_exit rendererLock([&] { renderer->Unlock(); });
			for (const auto& reference : references) {
				RE::NiPointer<RE::NiAVObject> root(reference->Get3D());
				if (!root) {
					++without3D;
					continue;
				}

				const auto* base = reference->GetBaseObject();
				if (!base)
					continue;

				const auto formID = base->GetFormID();
				const auto rule = settings->cliffObjects.find(formID);
				std::uint32_t budget = MaxNodesPerCliff;

				CollectShapes(root.get(), root->name.c_str(), formID, rule != settings->cliffObjects.end() ? &rule->second : nullptr, surfaces, budget);
			}
		}

		complete = without3D == 0;
		if (!references.empty())
			logger::debug("Cliff surfaces in cell ({}, {}): {} cliffs ({} without 3D), {} shapes", coordinates->cellX, coordinates->cellY, references.size(), without3D, surfaces.size());

		return surfaces;
	}

	static std::vector<Surface> CollectPreloaded(RE::TESObjectCELL* cell, bool& complete)
	{
		std::vector<Surface> surfaces;
		const auto geometry = CollisionGrid::Find(*cell);
		const auto settings = Plugin::GetRaycastSettings();
		complete = geometry != nullptr;

		if (!geometry || !settings)
			return surfaces;

		std::uint32_t cliffs = 0;
		for (const auto& object : geometry->Objects()) {
			if (!settings->IsCliff(object.base))
				continue;

			++cliffs;

			const auto rule = settings->cliffObjects.find(object.base);
			const auto& root = object.model->rootName;
			const auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(object.base);
			const auto* swap = base ? base->As<RE::TESModelTextureSwap>() : nullptr;

			for (const auto& surface : object.model->Surfaces()) {
				if (rule != settings->cliffObjects.end() && !Util::IsShapeAllowed(surface.shape, root, rule->second.allowedShapes, rule->second.blockedShapes))
					continue;

				// Alternate textures override the model's diffuse texture
				std::string_view texture = surface.texture;
				for (std::uint32_t i = 0; swap && i < swap->numAlternateTextures; ++i) {
					const auto& alternate = swap->alternateTextures[i];

					if (alternate.index3D == surface.index && alternate.textureSet) {
						const char* path = alternate.textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
						texture = path ? path : "";
						break;
					}
				}

				if (auto* landTexture = texture.empty() ? nullptr : LandTextureFor(texture); landTexture && !landTexture->textureGrassList.empty())
					surfaces.push_back({ object.transform * surface.center, surface.radius * object.transform.scale, object.base, landTexture });
			}
		}

		if (const auto* coordinates = cliffs ? cell->GetCoordinates() : nullptr)
			logger::debug("Preloaded cliff surfaces in cell ({}, {}): {} cliffs, {} shapes", coordinates->cellX, coordinates->cellY, cliffs, surfaces.size());

		return surfaces;
	}

	std::vector<Surface> Get(RE::TESObjectCELL* cell)
	{
		{
			std::scoped_lock lock(mutex);
			if (const auto it = cells.find(cell->GetFormID()); it != cells.end())
				return it->second;
		}

		bool complete = false;
		auto surfaces = cell->IsAttached() ? CollectCell(cell, complete) : CollectPreloaded(cell, complete);

		if (complete) {
			std::scoped_lock lock(mutex);
			cells.insert_or_assign(cell->GetFormID(), surfaces);
		}

		return surfaces;
	}

	void Forget(RE::FormID cell)
	{
		std::scoped_lock lock(mutex);
		cells.erase(cell);
	}

	void Clear()
	{
		std::scoped_lock lock(mutex);
		cells.clear();
	}
}
