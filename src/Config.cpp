#include "GrassControl/Config.h"

#include "GrassControl/Util.h"

namespace GrassControl::Config
{
	static bool PopulateDefaults(CSimpleIniA& ini)
	{
		ini.SetUnicode();
		ini.SetMultiLine();
		bool valid = true;
		for (const auto& entry : entries) {
			valid &= std::visit([&](auto* value) {
				using T = std::remove_pointer_t<decltype(value)>;
				if constexpr (std::is_same_v<T, bool>)
					return ini.SetBoolValue(entry.section, entry.key, *value, entry.comment) >= 0;
				else if constexpr (std::is_same_v<T, int>)
					return ini.SetLongValue(entry.section, entry.key, *value, entry.comment) >= 0;
				else if constexpr (std::is_same_v<T, double>)
					return ini.SetDoubleValue(entry.section, entry.key, *value, entry.comment) >= 0;
				else
					return ini.SetValue(entry.section, entry.key, value->c_str(), entry.comment) >= 0;
			},
				entry.value);
		}

		return valid;
	}

	static void ReadSetting(const CSimpleIniA& ini, const Entry& entry)
	{
		if (!ini.GetValue(entry.section, entry.key))
			return;

		std::visit([&](auto* value) {
			using T = std::remove_pointer_t<decltype(value)>;
			if constexpr (std::is_same_v<T, bool>)
				*value = ini.GetBoolValue(entry.section, entry.key);
			else if constexpr (std::is_same_v<T, int>)
				*value = static_cast<int>(ini.GetLongValue(entry.section, entry.key));
			else if constexpr (std::is_same_v<T, double>)
				*value = ini.GetDoubleValue(entry.section, entry.key);
			else
				*value = Util::StringHelpers::Trim(ini.GetValue(entry.section, entry.key));
		},
			entry.value);
	}

	static SI_Error AddMissingSettings(CSimpleIniA& ini, const CSimpleIniA& defaults)
	{
		CSimpleIniA::TNamesDepend sections;
		defaults.GetAllSections(sections);
		sections.sort(CSimpleIniA::Entry::LoadOrder());
		SI_Error result = SI_OK;

		for (const auto& section : sections) {
			CSimpleIniA::TNamesDepend keys;
			defaults.GetAllKeys(section.pItem, keys);
			keys.sort(CSimpleIniA::Entry::LoadOrder());

			for (const auto& key : keys) {
				if (ini.GetValue(section.pItem, key.pItem)) {
					continue;
				}

				result = ini.SetValue(section.pItem, key.pItem, defaults.GetValue(section.pItem, key.pItem), key.pComment);
				if (result < 0) {
					return result;
				}
			}
		}

		return result;
	}

	static void ReadIni()
	{
		try {
			std::string contents;
			std::error_code error;
			const auto path = Util::Files::GamePath(iniPath);
			if (!Util::Files::ReadText(path, contents, error)) {
				if (!Util::Files::IsMissing(error)) {
					logger::error("Failed to read GrassControl.ini; keeping defaults: {}", error.message());
					return;
				}

				// CREATE_NEW cannot truncate a file supplied by the VFS or a competing writer.
				if (!WriteSettings(error) && error != std::errc::file_exists) {
					return;
				}

				if (!Util::Files::ReadText(path, contents, error)) {
					logger::error("Failed to read the new GrassControl.ini; keeping defaults: {}", error.message());
					return;
				}
			}

			logger::info("Reading .ini...");
			CSimpleIniA ini;
			ini.SetUnicode();
			ini.SetMultiLine();
			if (contents.find('\0') != std::string::npos) {
				logger::error("GrassControl.ini contains NUL bytes; keeping defaults");
				return;
			}

			if (const auto result = ini.LoadData(contents); result < 0) {
				logger::error("Failed to parse GrassControl.ini (code: {}); keeping defaults", result);
				return;
			}

			CSimpleIniA defaults;
			const auto result = PopulateDefaults(defaults) ? AddMissingSettings(ini, defaults) : SI_FAIL;
			if (result < 0) {
				logger::error("Failed to populate missing GrassControl.ini settings");
				return;
			}

			if (result == SI_INSERTED) {
				const std::lock_guard lock(iniMutex);
				const auto saved = Util::Files::UpdateText(path, [&](std::string& current) {
					CSimpleIniA updated;
					updated.SetUnicode();
					updated.SetMultiLine();
					if (current.find('\0') != std::string::npos || updated.LoadData(current) < 0) {
						return false;
					}

					const auto added = AddMissingSettings(updated, defaults);
					if (added != SI_INSERTED) {
						return added >= 0;
					}

					current.clear();
					return updated.Save(current, true) >= 0; }, error);

				if (!saved) {
					logger::error("Failed to save missing GrassControl.ini settings: {}", error.message());
				}
			}

			for (const auto& entry : entries)
				ReadSetting(ini, entry);

			logger::info("...success");

		} catch (const std::exception& e) {
			logger::error("GrassControl.ini operation failed: {}", e.what());
		}
	}

	void ReadSettings()
	{
		ReadIni();
		std::error_code error;
		GeneratingCache = UseGrassCache && std::filesystem::is_regular_file(Util::GetProgressFilePath(), error);
		if (error && !Util::Files::IsMissing(error))
			logger::error("Failed to inspect the grass generation progress file: {}", error.message());
	}

	bool WriteSettings(std::error_code& error)
	{
		error.clear();
		try {
			const std::lock_guard lock(iniMutex);

			logger::info("Creating default GrassControl.ini...");

			CSimpleIniA ini;
			const auto valid = PopulateDefaults(ini);

			std::string contents;
			if (!valid || ini.Save(contents, true) < 0) {
				logger::error("Failed to serialize default GrassControl.ini");
				error = std::make_error_code(std::errc::invalid_argument);
				return false;
			}

			if (!Util::Files::CreateText(Util::Files::GamePath(iniPath), contents, error)) {
				if (error != std::errc::file_exists) {
					logger::error("Failed to create GrassControl.ini: {}", error.message());
				}

				return false;
			}

			logger::info("...success");
			return true;

		} catch (const std::exception& e) {
			logger::error("GrassControl.ini operation failed: {}", e.what());
			error = std::make_error_code(std::errc::io_error);
			return false;
		}
	}

	bool WriteBoolSetting(const char* section, const char* key, bool value)
	{
		const std::lock_guard lock(iniMutex);

		try {
			std::error_code error;
			const auto saved = Util::Files::UpdateText(Util::Files::GamePath(iniPath), [&](std::string& contents) {
				CSimpleIniA ini;
				ini.SetUnicode();
				ini.SetMultiLine();
				if (contents.find('\0') != std::string::npos || ini.LoadData(contents) < 0 || ini.SetBoolValue(section, key, value) < 0) {
					return false;
				}

				std::string replacement;
				if (ini.Save(replacement, true) < 0) {
					return false;
				}

				contents = std::move(replacement);
				return true; }, error);

			if (!saved) {
				logger::error("Failed to save [{}] {} in GrassControl.ini: {}", section, key, error.message());
			}

			return saved;
		} catch (const std::exception& e) {
			logger::error("Failed to save [{}] {} in GrassControl.ini: {}", section, key, e.what());
			return false;
		}
	}
}
