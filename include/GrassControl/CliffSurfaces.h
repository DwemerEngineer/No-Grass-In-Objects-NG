#pragma once

namespace GrassControl::CliffSurfaces
{
	struct Surface
	{
		RE::NiPoint3 center;
		float radius;
		RE::FormID object;
		RE::TESLandTexture* landTexture;
	};

	inline std::mutex mutex;
	inline std::unordered_map<RE::FormID, std::vector<Surface>> cells;

	inline constexpr std::uint32_t MaxNodesPerCliff = 4096;

	inline std::mutex unmatchedMutex;
	inline std::unordered_set<std::string> unmatched;

	inline std::string NormalizeTexturePath(std::string_view path)
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

	inline std::string_view FileStem(std::string_view normalized)
	{
		const auto slash = normalized.rfind('\\');
		return slash == std::string_view::npos ? normalized : normalized.substr(slash + 1);
	}

	struct TextureIndex
	{
		std::unordered_map<std::string, RE::TESLandTexture*> byPath;
		std::unordered_map<std::string, RE::TESLandTexture*> byStem;
	};

	inline const TextureIndex& GetTextureIndex()
	{
		static const TextureIndex index = [] {
			TextureIndex result;
			auto* data = RE::TESDataHandler::GetSingleton();
			std::unordered_map<const RE::TESFile*, std::size_t> loadOrder;
			for (const auto* file : data->files)
				loadOrder.emplace(file, loadOrder.size() + 1);

			const auto priority = [&](const RE::TESLandTexture* texture) {
				// GetFile() returns the winning override's plugin.
				const auto it = loadOrder.find(texture->GetFile());
				return std::pair{ !texture->textureGrassList.empty(), it != loadOrder.end() ? it->second : 0 };
			};
			const auto add = [&](auto& entries, std::string key, RE::TESLandTexture* texture) {
				auto& match = entries[std::move(key)];
				if (!match || priority(texture) > priority(match))
					match = texture;
			};

			for (auto* landTexture : data->GetFormArray<RE::TESLandTexture>()) {
				if (!landTexture || landTexture->IsDeleted() || !landTexture->textureSet)
					continue;

				const char* diffuse = landTexture->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
				if (!diffuse || !*diffuse)
					continue;

				auto path = NormalizeTexturePath(diffuse);
				add(result.byStem, std::string(FileStem(path)), landTexture);
				add(result.byPath, std::move(path), landTexture);
			}
			logger::info("Indexed {} landscape textures for cliff tops", result.byPath.size());
			return result;
		}();
		return index;
	}

	// Cache results only after every eligible cliff has loaded its 3D.
	std::vector<Surface> Get(RE::TESObjectCELL* cell);

	void Forget(RE::FormID cell);

	void Clear();
}
