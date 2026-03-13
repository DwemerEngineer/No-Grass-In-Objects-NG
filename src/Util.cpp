#include "GrassControl/Util.h"

namespace Util
{
	void report_and_fail_timed(const std::string& a_message)
	{
		logger::error(fmt::runtime(a_message));
		MessageBoxTimeoutA(nullptr, a_message.c_str(), a_message.c_str(), MB_SYSTEMMODAL, 0, 7000);
		TerminateProcess(GetCurrentProcess(), 1);
	}

	std::string Util::getProgressFilePath()
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
		} catch (...) {
		}

		if (_ovFilePath.empty()) {
			_ovFilePath = _progressFilePath;
		}

		return _ovFilePath;
	}

	void Util::nopBlock(uintptr_t addr, int size, int offset)
	{
		DWORD flOldProtect = 0;
		BOOL change_protection = VirtualProtect(reinterpret_cast<LPVOID>(addr), 0x13, PAGE_EXECUTE_READWRITE, &flOldProtect);
		if (change_protection) {
			memset(reinterpret_cast<void*>(addr + offset), 0x90, size);
		}
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

	std::unordered_set<RE::TESForm*> CachedFormList::getAll() const
	{
		return this->Forms;
	}
}
