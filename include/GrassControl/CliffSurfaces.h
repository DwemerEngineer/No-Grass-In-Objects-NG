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

	inline RE::BSLightingShaderProperty* GetSurfaceShader(RE::BSGeometry& geometry)
	{
		if (!geometry.AsTriShape() || geometry.GetFlags().any(RE::NiAVObject::Flag::kHidden))
			return nullptr;

		const auto& runtime = geometry.GetGeometryRuntimeData();
		auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.shaderProperty.get());
		using enum RE::BSShaderProperty::EShaderPropertyFlag;
		if (!property || property->flags.none(kZBufferWrite) || property->flags.any(kSkinned, kDecal, kDynamicDecal, kTreeAnim, kRefraction, kTempRefraction))
			return nullptr;

		// Alpha-tested cliff edges can also use alpha blending
		const auto* alpha = runtime.alphaProperty.get();
		return alpha && alpha->GetAlphaBlending() && !alpha->GetAlphaTesting() ? nullptr : property;
	}

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

	inline bool IsNoGrassVariant(RE::TESLandTexture* texture)
	{
		const auto* retainedID = texture->GetFormEditorID();
		std::string editorID = retainedID ? retainedID : "";
		// LTEX does not retain its editor ID, so read EDID from the plugin
		if (editorID.empty()) {
			for (auto* source : { texture->GetFile(), texture->GetFile(0) }) {
				if (!source)
					continue;
				auto* file = source->Duplicate();
				if (!file || !file->SeekForm(texture) || !file->SeekNextSubrecordType(0x44494445))  // EDID
					continue;
				editorID.resize(file->GetCurrentSubRecordSize());
				if (!file->ReadData(editorID.data(), static_cast<std::uint32_t>(editorID.size())))
					editorID.clear();
				if (!editorID.empty())
					break;
			}
		}
		std::ranges::transform(editorID, editorID.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return editorID.find("nograss") != std::string::npos;
	}

	struct TextureIndex
	{
		std::unordered_map<std::string, RE::TESLandTexture*> byPath;
		std::unordered_map<std::string, RE::TESLandTexture*> byStem;

		RE::TESLandTexture* Find(std::string_view texturePath) const
		{
			const auto path = NormalizeTexturePath(texturePath);
			const auto exact = byPath.find(path);
			if (exact != byPath.end() && !exact->second->textureGrassList.empty())
				return exact->second;
			if (const auto it = byStem.find(std::string(FileStem(path))); it != byStem.end())
				return it->second;
			return exact != byPath.end() ? exact->second : nullptr;
		}
	};

	inline const TextureIndex& GetTextureIndex()
	{
		static const TextureIndex index = [] {
			TextureIndex result;
			auto* data = RE::TESDataHandler::GetSingleton();
			std::unordered_map<const RE::TESFile*, std::size_t> loadOrder;
			std::unordered_map<RE::TESLandTexture*, bool> noGrassVariants;
			for (const auto* file : data->files)
				loadOrder.emplace(file, loadOrder.size() + 1);

			const auto priority = [&](RE::TESLandTexture* texture) {
				const auto it = loadOrder.find(texture->GetFile());
				const auto [variant, inserted] = noGrassVariants.try_emplace(texture);
				if (inserted)
					variant->second = IsNoGrassVariant(texture);
				// Overrides can add grass to a NoGrass variant
				return std::tuple{ !texture->textureGrassList.empty(), !variant->second, it != loadOrder.end() ? it->second : 0 };
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

	std::vector<Surface> Get(RE::TESObjectCELL* cell);

	void Forget(RE::FormID cell);

	void Clear();
}
