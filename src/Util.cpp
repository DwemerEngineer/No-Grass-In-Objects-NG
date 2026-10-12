#include "GrassControl/Util.h"

#include <set>

namespace
{
	struct FileHandle
	{
		HANDLE value;

		explicit FileHandle(HANDLE handle) : value(handle) {}

		FileHandle(const FileHandle&) = delete;

		FileHandle& operator=(const FileHandle&) = delete;

		~FileHandle()
		{
			if (value != INVALID_HANDLE_VALUE) {
				CloseHandle(value);
			}
		}
	};

	bool LastError(std::error_code& error)
	{
		error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
		return false;
	}

	bool ReadHandle(HANDLE file, std::string& text, std::error_code& error)
	{
		constexpr std::size_t MaxTextFileBytes = 16 * 1024 * 1024;
		std::string contents;
		char buffer[65536];
		DWORD count = 0;
		while (true) {
			if (!ReadFile(file, buffer, sizeof(buffer), &count, nullptr)) {
				return LastError(error);
			}

			if (count == 0) {
				text = std::move(contents);
				return true;
			}

			if (contents.size() + count > MaxTextFileBytes) {
				error = std::make_error_code(std::errc::file_too_large);
				return false;
			}

			contents.append(buffer, count);
		}
	}

	bool WriteHandle(HANDLE file, std::string_view text, std::error_code& error)
	{
		LARGE_INTEGER start{};
		if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) {
			return LastError(error);
		}

		while (!text.empty()) {
			DWORD count = 0;
			const auto chunk = static_cast<DWORD>(std::min<std::size_t>(text.size(), 65536));
			if (!WriteFile(file, text.data(), chunk, &count, nullptr)) {
				return LastError(error);
			}

			if (count == 0) {
				error = std::error_code(ERROR_WRITE_FAULT, std::system_category());
				return false;
			}

			text.remove_prefix(count);
		}

		if (!SetEndOfFile(file) || !FlushFileBuffers(file)) {
			return LastError(error);
		}

		return true;
	}

	struct CaseInsensitivePathLess
	{
		bool operator()(const std::filesystem::path& left, const std::filesystem::path& right) const
		{
			return CompareStringOrdinal(left.c_str(), static_cast<int>(left.native().size()), right.c_str(), static_cast<int>(right.native().size()), TRUE) == CSTR_LESS_THAN;
		}
	};
}

namespace GrassControl::Util::Files
{
	std::filesystem::path GamePath(const std::filesystem::path& relative)
	{
		static const auto root = [] {
			std::wstring executable(260, L'\0');

			while (true) {
				const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
				if (length == 0) {
					throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot locate the game executable");
				}

				if (length < executable.size()) {
					executable.resize(length);
					return std::filesystem::path(executable).parent_path();
				}

				if (executable.size() >= 32768) {
					throw std::system_error(std::make_error_code(std::errc::filename_too_long), "Game executable path is too long");
				}

				executable.resize(std::min<std::size_t>(executable.size() * 2, 32768));
			}
		}();
		return (root / relative).lexically_normal().make_preferred();
	}

	std::string PathForLog(const std::filesystem::path& path)
	{
		const auto utf8 = path.u8string();
		return { reinterpret_cast<const char*>(utf8.data()), utf8.size() };
	}

	bool IsMissing(const std::error_code& error)
	{
		return error == std::errc::no_such_file_or_directory ||
		       (error.category() == std::system_category() && (error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND));
	}

	bool ReadText(const std::filesystem::path& path, std::string& text, std::error_code& error)
	{
		error.clear();
		const FileHandle file{ CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
		if (file.value == INVALID_HANDLE_VALUE) {
			return LastError(error);
		}

		return ReadHandle(file.value, text, error);
	}

	bool CreateText(const std::filesystem::path& path, std::string_view text, std::error_code& error)
	{
		error.clear();
		const FileHandle file{ CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) };
		if (file.value == INVALID_HANDLE_VALUE) {
			return LastError(error);
		}

		return WriteHandle(file.value, text, error);
	}

	bool UpdateText(const std::filesystem::path& path, const std::function<bool(std::string&)>& update, std::error_code& error)
	{
		error.clear();

		// Let the VFS route an existing-file open, then retain that handle for the update.
		const FileHandle file{ CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
		if (file.value == INVALID_HANDLE_VALUE) {
			return LastError(error);
		}

		std::string contents;
		if (!ReadHandle(file.value, contents, error)) {
			return false;
		}

		const auto original = contents;
		if (!update(contents)) {
			error = std::make_error_code(std::errc::invalid_argument);
			return false;
		}

		return contents == original || WriteHandle(file.value, contents, error);
	}

	void SortUniquePaths(std::vector<std::filesystem::path>& paths)
	{
		for (auto& path : paths) {
			path = GamePath(path);
		}

		// File order determines precedence when object configs repeat a form.
		std::ranges::sort(paths);
		std::set<std::filesystem::path, CaseInsensitivePathLess> seen;
		std::erase_if(paths, [&](const auto& path) { return !seen.insert(path).second; });
	}

	std::vector<std::filesystem::path> ObjectConfigs()
	{
		std::vector<std::filesystem::path> paths;
		std::error_code error;
		const auto data = GamePath(L"Data");

		std::filesystem::directory_iterator entry(data, error), end;

		while (!error && entry != end) {
			const auto& path = entry->path();
			const auto extension = path.extension().native();
			auto name = path.stem().native();
			for (auto& c : name) {
				if (c >= L'A' && c <= L'Z') {
					c += L'a' - L'A';
				}
			}

			if (CompareStringOrdinal(extension.c_str(), static_cast<int>(extension.size()), L".ini", 4, TRUE) == CSTR_EQUAL && name.find(L"_ngio") != std::wstring::npos) {
				std::error_code statusError;
				if (entry->is_regular_file(statusError)) {
					paths.push_back(path);
				} else if (statusError) {
					logger::error("Failed to inspect object config: {}", statusError.message());
				}
			}

			if (!error) {
				entry.increment(error);
			}
		}

		if (error) {
			logger::error("Failed to enumerate object configs: {}", error.message());
		}

		SortUniquePaths(paths);
		return paths;
	}
}

namespace GrassControl::Util
{
	void ReportAndFailTimed(const std::string& a_message)
	{
		logger::error("{}", a_message);
		MessageBoxTimeoutA(nullptr, a_message.c_str(), a_message.c_str(), MB_SYSTEMMODAL, 0, 7000);
		TerminateProcess(GetCurrentProcess(), 1);
	}

	std::filesystem::path GetProgressFilePath()
	{
		static const auto path = [] {
			const auto normal = Files::GamePath(L"PrecacheGrass.txt");
			const auto doubled = Files::GamePath(L"PrecacheGrass.txt.txt");
			std::error_code error;
			if (std::filesystem::exists(normal, error)) {
				return normal;
			}

			if (!error && std::filesystem::exists(doubled, error)) {
				return doubled;
			}

			if (error) {
				logger::error("Failed to locate the progress file: {}", error.message());
			}

			return normal;
		}();
		return path;
	}
}

namespace GrassControl::Util
{
	std::unique_ptr<CachedFormList> CachedFormList::TryParse(const std::string& input, const std::string& settingNameForLog)
	{
		auto list = std::make_unique<CachedFormList>();
		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		for (const auto& entry : StringHelpers::split(input, ';', true)) {
			const auto spec = ParseFormSpec(entry);
			if (!spec) {
				logger::warn("Failed to parse form for {}: {}", settingNameForLog, entry);
				continue;
			}

			const auto* file = dataHandler->LookupLoadedModByName(spec->filename);
			bool light = false;
			if (!file) {
				file = dataHandler->LookupLoadedLightModByName(spec->filename);
				light = true;
			}

			if (!file) {
				logger::warn("Failed to find plugin {} for {}", spec->filename, settingNameForLog);
				continue;
			}

			const auto localID = spec->id & (light ? 0xFFFu : 0xFFFFFFu);
			auto* form = dataHandler->LookupForm(localID, spec->filename);
			if (!form) {
				logger::warn("Failed to find form 0x{:08x} in {} for {}", localID, spec->filename, settingNameForLog);
				continue;
			}

			list->Ids.insert(form->formID);
		}

		return list;
	}

	void CachedFormList::printList(const std::string& settingNameForLog) const
	{
		std::vector<std::string> ids;
		for (const auto id : Ids) {
			ids.push_back(fmt::format("0x{:08x}", id));
		}

		logger::info("Forms {} were successfully added to {}", StringHelpers::Join(ids, ", "), settingNameForLog);
	}
}

namespace GrassControl::Util
{
	static constexpr std::size_t MaxSceneObjects = 4096;
	static constexpr std::size_t MaxSceneVertices = 262144;

	static bool CollectSceneObjects(RE::NiAVObject* object, std::set<RE::NiAVObject*>& objects)
	{
		if (!object)
			return false;

		std::vector<RE::NiAVObject*> pending{ object };
		while (!pending.empty()) {
			auto* current = pending.back();
			pending.pop_back();
			if (!current || !objects.insert(current).second)
				continue;

			if (objects.size() > MaxSceneObjects || pending.size() > MaxSceneObjects) {
				objects.clear();
				return false;
			}

			if (auto* node = current->AsNode()) {
				for (const auto& child : node->GetChildren()) {
					if (pending.size() >= MaxSceneObjects) {
						objects.clear();
						return false;
					}

					if (child.get())
						pending.push_back(child.get());
				}
			}
		}

		return true;
	}

	std::optional<std::string> ClosestShape(RE::NiAVObject* root, const RE::NiPoint3& position)
	{
		if (!root)
			return std::nullopt;

		std::set<RE::NiAVObject*> objects;
		if (!CollectSceneObjects(root, objects))
			return std::nullopt;

		float closestDistanceSquared = std::numeric_limits<float>::max();
		RE::NiAVObject* closestShape = nullptr;
		std::size_t totalVertices = 0;
		for (auto* object : objects) {
			const auto& transform = object->world;
			if (!std::isfinite(transform.scale) || transform.scale == 0)
				continue;

			const auto considerVertex = [&](const RE::NiPoint3& local) {
				const auto world = transform.rotate * (local * transform.scale) + transform.translate;
				const auto distanceSquared = world.GetSquaredDistance(position);
				if (std::isfinite(distanceSquared) && distanceSquared < closestDistanceSquared) {
					closestDistanceSquared = distanceSquared;
					closestShape = object;
				}
			};

			if (auto* legacy = object->AsNiTriShape()) {
				auto modelData = legacy->GetRuntimeData().spModelData;
				if (!modelData || !modelData->vertex)
					continue;

				const auto count = static_cast<std::size_t>(modelData->vertices);
				if (count > MaxSceneVertices - totalVertices)
					return {};

				totalVertices += count;
				for (std::size_t i = 0; i < count; ++i) {
					considerVertex(modelData->vertex[i]);
				}
			} else if (auto* modern = object->AsTriShape()) {
				// Dynamic and skinned buffers require their own synchronization.
				if (modern->GetType().get() != RE::BSGeometry::Type::kTriShape ||
					modern->GetGeometryRuntimeData().skinInstance)
					continue;

				auto* rendererData = modern->GetGeometryRuntimeData().rendererData;
				if (!rendererData || !rendererData->rawVertexData)
					continue;

				const auto stride = rendererData->vertexDesc.GetSize();
				const auto positionOffset = rendererData->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
				if (stride < sizeof(RE::NiPoint3) || stride > 1024 || positionOffset > stride - sizeof(RE::NiPoint3))
					continue;

				const auto count = static_cast<std::size_t>(modern->GetTrishapeRuntimeData().vertexCount);
				if (count > MaxSceneVertices - totalVertices)
					return {};

				totalVertices += count;
				for (std::size_t i = 0; i < count; ++i) {
					RE::NiPoint3 vertex;
					std::memcpy(&vertex, rendererData->rawVertexData + stride * i + positionOffset, sizeof(vertex));
					considerVertex(vertex);
				}
			}
		}

		return closestShape ? std::optional<std::string>(closestShape->name.c_str()) : std::nullopt;
	}
}
