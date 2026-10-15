#include "GrassControl/CliffSurfaces.h"

#include "GrassControl/Main.h"
#include "GrassControl/RaycastHelper.h"
#include "GrassControl/Util.h"

namespace GrassControl::CliffSurfaces
{
	static std::mutex mutex;
	static std::unordered_map<RE::FormID, std::vector<Surface>> cells;

	static constexpr std::uint32_t MaxNodesPerCliff = 4096;

	static std::string NormalizeTexturePath(std::string_view path)
	{
		std::string result(path);
		std::ranges::transform(result, result.begin(), [](unsigned char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(c)); });
		for (const std::string_view prefix : { "data\\", "textures\\" }) {
			if (result.starts_with(prefix))
				result.erase(0, prefix.size());
		}
		if (const auto dot = result.rfind('.'); dot != std::string::npos && result.find('\\', dot) == std::string::npos)
			result.resize(dot);
		return result;
	}

	static std::string_view FileStem(std::string_view normalized)
	{
		const auto slash = normalized.rfind('\\');
		return slash == std::string_view::npos ? normalized : normalized.substr(slash + 1);
	}

	// Match copied textures by file name when their directory differs from the landscape texture.
	static RE::TESLandTexture* LandTextureFor(std::string_view texturePath)
	{
		struct Index
		{
			std::unordered_map<std::string, RE::TESLandTexture*> byPath;
			std::unordered_map<std::string, RE::TESLandTexture*> byStem;
		};
		static const Index index = [] {
			Index result;
			for (auto* landTexture : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESLandTexture>()) {
				if (!landTexture || !landTexture->textureSet || landTexture->textureGrassList.empty())
					continue;

				const char* diffuse = landTexture->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
				if (!diffuse || !*diffuse)
					continue;

				auto path = NormalizeTexturePath(diffuse);
				result.byStem.try_emplace(std::string(FileStem(path)), landTexture);
				result.byPath.try_emplace(std::move(path), landTexture);
			}
			logger::info("Indexed {} landscape textures with grass for cliff tops", result.byPath.size());
			return result;
		}();

		const auto path = NormalizeTexturePath(texturePath);
		if (const auto it = index.byPath.find(path); it != index.byPath.end())
			return it->second;
		if (const auto it = index.byStem.find(std::string(FileStem(path))); it != index.byStem.end())
			return it->second;

		static std::mutex unmatchedMutex;
		static std::unordered_set<std::string> unmatched;
		std::scoped_lock lock(unmatchedMutex);
		if (unmatched.insert(path).second)
			logger::info("Cliff texture {} matches no landscape texture with grass", path);
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
		if (!geometry || !geometry->AsTriShape() || !(geometry->worldBound.radius > 0.0f))
			return;

		const auto& runtime = geometry->GetGeometryRuntimeData();
		auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.shaderProperty.get());
		using enum RE::BSShaderProperty::EShaderPropertyFlag;
		if (!property || property->flags.any(kSkinned, kDecal, kDynamicDecal, kTreeAnim))
			return;

		// Cliffs blend into the landscape with vertex alpha and alpha-tested edges, so only see-through blended shapes are skipped.
		if (const auto* alpha = runtime.alphaProperty.get(); alpha && alpha->GetAlphaBlending() && !alpha->GetAlphaTesting())
			return;

		if (rule && !Util::IsShapeAllowed(geometry->name.c_str(), root, rule->allowedShapes, rule->blockedShapes))
			return;

		const auto* texture = property->GetBaseTexture();
		if (!texture || texture->name.empty())
			return;

		if (auto* landTexture = LandTextureFor(texture->name.c_str()))
			surfaces.push_back({ geometry->worldBound.center, geometry->worldBound.radius, formID, landTexture });
	}

	// Missing 3D leaves the result uncached so a later build can retry.
	static std::vector<Surface> CollectCell(RE::TESObjectCELL* cell, bool& complete)
	{
		std::vector<Surface> surfaces;
		complete = true;
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

		// Persistent references live in the worldspace's persistent cell, so the grid cell only finds them by position.
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

	std::vector<Surface> Get(RE::TESObjectCELL* cell)
	{
		{
			std::scoped_lock lock(mutex);
			if (const auto it = cells.find(cell->GetFormID()); it != cells.end())
				return it->second;
		}

		bool complete = false;
		auto surfaces = CollectCell(cell, complete);
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
