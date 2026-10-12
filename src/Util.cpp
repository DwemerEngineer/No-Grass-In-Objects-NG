#include "GrassControl/Util.h"
#include "closest_point.h"

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

namespace Util::Files
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

namespace Util
{
	void ReportAndFailTimed(const std::string& a_message)
	{
		logger::error(fmt::runtime(a_message));
		MessageBoxTimeoutA(nullptr, a_message.c_str(), a_message.c_str(), MB_SYSTEMMODAL, 0, 7000);
		TerminateProcess(GetCurrentProcess(), 1);
	}

	std::string GetProgressFilePath()
	{
		std::string n = _ovFilePath;
		if (!n.empty()) {
			return n;
		}

		// Dumb user mode for .txt.txt file name.
		try {
			auto fi = std::filesystem::path(_progressFilePath);
			if (exists(fi)) {
				_ovFilePath = _progressFilePath;
			} else {
				std::string fpath = _progressFilePath + R"(.txt)";
				fi = std::filesystem::path(fpath);
				if (exists(fi)) {
					_ovFilePath = fpath;
				}
			}
		} catch (std::filesystem::filesystem_error e) {
			logger::error(fmt::runtime("Failed to find progress file path! Exception occured: {}"), e.what());
			_ovFilePath = _progressFilePath;
			return _ovFilePath;
		}

		if (_ovFilePath.empty()) {
			_ovFilePath = _progressFilePath;
		}

		return _ovFilePath;
	}

	void NopBlock(uintptr_t addr, int size, int offset)
	{
		DWORD flOldProtect = 0;
		BOOL change_protection = VirtualProtect(reinterpret_cast<LPVOID>(addr), 0x13, PAGE_EXECUTE_READWRITE, &flOldProtect);
		if (change_protection) {
			memset(reinterpret_cast<void*>(addr + offset), 0x90, size);
		}
	}

	RE::NiAVObject* GetAVObject(const RE::hkpCdBody* body)
	{
		typedef RE::NiAVObject* (*_GetUserData)(const RE::hkpCdBody*);
		static auto getAVObject = REL::Relocation<_GetUserData>(RELOCATION_ID(76160, 77988));
		return body ? getAVObject(body) : nullptr;
	}

	RE::TESForm* GetRefrBaseForm(RE::TESObjectREFR* ref)
	{
		RE::TESForm* result = nullptr;

		if (ref != nullptr) {
			auto bound = ref->GetBaseObject();
			if (bound != nullptr) {
				auto baseform = bound->As<RE::TESForm>();
				if (baseform != nullptr) {
					result = baseform;
					return result;
				}
			}
		}

		return result;
	}

	RE::TESForm* GetBodyBaseForm(const RE::hkpCdBody* body)
	{
		RE::TESForm* result = nullptr;

		RE::TESObjectREFR* ref = nullptr;

		auto av = GetAVObject(body);
		if (av) {
			ref = av->GetUserData();
		}
		if (ref != nullptr) {
			auto bound = ref->GetBaseObject();
			if (bound != nullptr) {
				auto baseform = bound->As<RE::TESForm>();
				if (baseform != nullptr) {
					result = baseform;
					return result;
				}
			}
		}

		return result;
	}

	void GetAllObjects(RE::NiAVObject* object, std::set<RE::NiAVObject*>* result)
	{
		if (object && result)
			result->insert(object);
		auto node = object ? object->AsNode() : nullptr;
		if (node)
			for (auto child : node->GetChildren())
				GetAllObjects(child.get(), result);
	}

	struct ObjCache
	{
		struct ShapeVertices
		{
			RE::NiAVObject* object;
			std::vector<RE::NiPoint3> vertices;
		};

		struct CacheEntry
		{
			RE::NiAVObject* baseObj = nullptr;
			RE::BSFixedString name;
			std::vector<ShapeVertices> nodes;
		};

		std::array<CacheEntry, 10> entries;
		int nextEvict = 0;

		CacheEntry* GetOrBuild(RE::NiAVObject* obj)
		{
			for (auto& entry : entries) {
				if (entry.baseObj == obj)
					return &entry;
			}

			CacheEntry& entry = entries[nextEvict];
			nextEvict = (nextEvict + 1) % 10;
			entry.baseObj = obj;
			entry.nodes.clear();

			const RE::BSFixedString& objName = obj->name;

			for (auto& entry : entries) {
				if (entry.name == objName) {
					if (entry.baseObj != obj) {
						entry.baseObj = obj;

						size_t i = 0;
						std::set<RE::NiAVObject*> objects;
						Util::GetAllObjects(obj, &objects);
						for (auto object : objects) {
							if (object->AsTriShape() && i < entry.nodes.size()) {
								const RE::NiTransform& world = object->world;
								entry.nodes[i].object = object;
								i++;
							}
						}
					}
					return &entry;
				}
			}

			std::set<RE::NiAVObject*> objects;
			Util::GetAllObjects(obj, &objects);

			for (auto object : objects) {
				if (auto niTriShape = object->AsNiTriShape()) {
					if (auto modelData = niTriShape->GetRuntimeData().spModelData) {
						ShapeVertices sv;
						sv.object = object;

						sv.vertices.reserve(modelData->vertices);
						for (int i = 0; i < modelData->vertices; i++) {
							auto vertex = modelData->vertex[i];
							sv.vertices.push_back(vertex);
						}
						entry.nodes.push_back(std::move(sv));
					}
				} else if (auto bsTriShape = object->AsTriShape()) {
					if (auto rendererData = bsTriShape->GetGeometryRuntimeData().rendererData) {
						ShapeVertices sv;
						sv.object = object;

						const RE::NiTransform& world = object->world;

						uint32_t vertexSize = rendererData->vertexDesc.GetSize();
						uint32_t offset = rendererData->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
						uint8_t* base = rendererData->rawVertexData + offset;
						auto vertexCount = bsTriShape->GetTrishapeRuntimeData().vertexCount;

						sv.vertices.reserve(vertexCount);
						for (int i = 0; i < vertexCount; i++) {
							sv.vertices.push_back(*reinterpret_cast<RE::NiPoint3*>(base + vertexSize * i));
						}
						entry.nodes.push_back(std::move(sv));
					}
				}
			}

			return &entry;
		}
	};

	static thread_local ObjCache objCache;

	RE::NiAVObject* FindClosestObj(RE::NiAVObject* baseObj, RE::NiPoint3& pos)
	{
		/*
				auto triangleCount = bsTriShape->GetTrishapeRuntimeData().triangleCount;
			uint32_t vertexSize = rendererData->vertexDesc.GetSize();
			uint32_t offset = rendererData->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);

			for (std::uint32_t i = 0; i < (triangleCount * 3u); i += 3) {
				glm::vec3 triangle[3];

				triangle[0] = *reinterpret_cast<glm::vec3*>(&rendererData->rawVertexData[vertexSize * rendererData->rawIndexData[i] + offset]);
				triangle[1] = *reinterpret_cast<glm::vec3*>(&rendererData->rawVertexData[vertexSize * rendererData->rawIndexData[i + 1] + offset]);
				triangle[2] = *reinterpret_cast<glm::vec3*>(&rendererData->rawVertexData[vertexSize * rendererData->rawIndexData[i + 2] + offset]);

				auto closestPt = closest_pt_triangle(triangle, glmPos);
				auto dist = glm::distance2(closestPt, glmPos);

					if (dist < bestDist) {
						bestObj = object;
						bestDist = dist;
					}
			}
		*/

		auto* entry = objCache.GetOrBuild(baseObj);

		float bestDist = std::numeric_limits<float>::max();
		RE::NiAVObject* bestObj = nullptr;

		for (auto& node : entry->nodes) {
			const RE::NiTransform& world = node.object->world;
			RE::NiMatrix3 invRotate = world.rotate.Transpose();
			float invScale = 1.0f / world.scale;
			RE::NiPoint3 localPos = (invRotate * (pos - world.translate)) * invScale;
			RE::NiPoint3 bestVert;
			for (auto& vertex : node.vertices) {
				float dist = vertex.GetSquaredDistance(localPos);
				if (dist < bestDist) {
					bestDist = dist;
					bestObj = node.object;
				}
			}
		}

		return bestObj;
	}

	CachedFormList::CachedFormList() = default;

	std::unique_ptr<CachedFormList> CachedFormList::TryParse(const std::string& input, std::string settingNameForLog, bool warnOnMissingForm, bool dontWriteAnythingToLog)
	{
		if (settingNameForLog.empty()) {
			settingNameForLog = "unknown form list setting";
		}

		auto ls = std::make_unique<CachedFormList>();
		char Char = ';';
		auto spl = StringHelpers::split(input, Char, true);
		for (auto& x : spl) {
			std::string idstr;
			std::string fileName;

			auto ix = x.find(L':');
			if (ix <= 0) {
				if (!dontWriteAnythingToLog) {
					logger::warn(fmt::runtime("Failed to parse form for " + settingNameForLog + "! Invalid input: `" + x + "`."));
				}

				continue;
			}

			idstr = x.substr(0, ix);
			fileName = x.substr(ix + 1);

			if (!std::ranges::all_of(idstr.begin(), idstr.end(), [](wchar_t q) { return (q >= L'0' && q <= L'9') || (q >= L'a' && q <= L'f') || (q >= L'A' && q <= L'F'); })) {
				if (!dontWriteAnythingToLog) {
					logger::warn(fmt::runtime("Failed to parse form for " + settingNameForLog + "! Invalid form ID: `" + idstr + "`."));
				}

				continue;
			}

			if (fileName.empty()) {
				if (!dontWriteAnythingToLog) {
					logger::warn(fmt::runtime("Failed to parse form for " + settingNameForLog + "! Missing file name."));
				}

				continue;
			}

			RE::FormID id = 0;
			bool sucess;
			try {
				auto substr = idstr.substr(0, 2);
				for (auto& c : substr)
					c = static_cast<char>(std::tolower(c));

				if (substr == "fe" || substr == "0x")
					idstr.erase(0, 2);

				id = stoul(idstr, nullptr, 16);
				sucess = true;
			} catch (std::exception&) {
				sucess = false;
			}
			if (!sucess) {
				if (!dontWriteAnythingToLog) {
					logger::warn(fmt::runtime("Failed to parse form for " + settingNameForLog + "! Invalid form ID: `" + idstr + "`."));
				}

				continue;
			}

			auto file = RE::TESDataHandler::GetSingleton()->LookupLoadedModByName(fileName);
			if (!file) {
				file = RE::TESDataHandler::GetSingleton()->LookupLoadedLightModByName(fileName);
				if (file)
					id &= 0x00000FFF;
			} else {
				id &= 0x00FFFFFF;
			}

			if (file) {
				if (!RE::TESDataHandler::GetSingleton()->LookupForm(id, fileName)) {
					if (!dontWriteAnythingToLog && warnOnMissingForm) {
						logger::warn(fmt::runtime("Failed to find form for " + settingNameForLog + "! Form ID was 0x{:08x} and file was " + fileName + "."), id);
					}
					continue;
				}
			} else {
				if (!dontWriteAnythingToLog) {
					logger::warn(fmt::runtime("Failed to find file for " + settingNameForLog + "! File name was `" + fileName + "` and form ID was 0x{:08x}."), id);
				}
				continue;
			}

			auto form = RE::TESDataHandler::GetSingleton()->LookupForm(id, fileName);
			auto formID = RE::TESDataHandler::GetSingleton()->LookupFormID(id, fileName);
			if (!form || !formID) {
				if (!dontWriteAnythingToLog) {
					logger::warn(fmt::runtime("Invalid form detected while adding form to " + settingNameForLog + "! Possible invalid form ID: `0x{:08x}`."), formID);
				}
				continue;
			}

			auto baseFile = form->GetFile(0);
			if (file != baseFile) {
				formID = RE::TESDataHandler::GetSingleton()->LookupFormID(id, baseFile->GetFilename());
			}

			bool skip = false;

			for (auto oldForm : ls->Forms) {
				if (oldForm == nullptr)
					continue;

				auto existingFormID = oldForm->formID & 0x00FFFFFF;
				if (existingFormID == id) {
					if ((formID & 0xFF000000) > (oldForm->formID & 0xFF000000)) {
						if (!dontWriteAnythingToLog) {
							logger::info(fmt::runtime("Replacing form 0x{:08x} in " + settingNameForLog + " with new form 0x{:08x}."), oldForm->formID, formID);
						}
						ls->Forms.erase(std::ranges::find(ls->Forms, oldForm));
					} else if ((formID & 0xFF000000) <= (oldForm->formID & 0xFF000000)) {
						if (!dontWriteAnythingToLog) {
							logger::warn(fmt::runtime("Form 0x{:08x} already exists in " + settingNameForLog + "! Skipping identical or earlier loaded form."), formID);
						}
						skip = true;
						continue;
					}
				}
			}

			if (skip)
				continue;

			if (ls->Ids.insert(formID).second) {
				ls->Forms.emplace(form);
			}
		}
		return ls;
	}

	void CachedFormList::printList(std::string settingNameForLog) const
	{
		std::string formIDs;

		auto it = Forms.begin();

		while (it != Forms.end()) {
			auto form = *it;

			if (form == nullptr)
				continue;

			auto formID = form->formID;

			if (it++ != Forms.end()) {
				formIDs += fmt::format("0x{:08x}, ", formID);
			} else {
				formIDs += fmt::format("and 0x{:08x}", formID);
			}
		}
		logger::info(fmt::runtime("Forms {} were successfully added to {}"), formIDs, settingNameForLog);
	}

	bool CachedFormList::Contains(RE::TESForm* form) const
	{
		if (form == nullptr)
			return false;

		return Contains(form->formID);
	}

	bool CachedFormList::Contains(unsigned int formId) const
	{
		return this->Ids.contains(formId);
	}

	std::unordered_set<RE::TESForm*> CachedFormList::GetAllForms() const
	{
		return this->Forms;
	}

	std::unordered_set<RE::FormID> CachedFormList::GetAllFormIDs() const
	{
		return this->Ids;
	}
}
