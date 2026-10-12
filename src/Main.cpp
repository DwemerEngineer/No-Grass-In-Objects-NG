#include "GrassControl/Main.h"
#include "GrassControl/Config.h"
#include "GrassControl/Events.h"
#include "GrassControl/GidFileCache.h"
#include "GrassControl/PhysicsRaycast.h"
#include "GrassControl/RaycastHelper.h"
#include "GrassControl/Util.h"

using namespace SKSE;
using namespace SKSE::log;
using namespace SKSE::stl;

namespace
{
	using namespace GrassControl;

	std::atomic<std::shared_ptr<const RaycastHelper>> raycastSettings;
	int mainMenuUpdates = 0;

	void InitializeHooking()
	{
		log::trace("Initializing trampoline...");
		auto& trampoline = GetTrampoline();
		trampoline.create(2048);
		log::trace("Trampoline initialized.");
		GrassControl::Generation::InstallHooks();
		GrassControl::Plugin::InstallHooks();
	}

	bool MissingLooseCache()
	{
		using namespace GrassControl::Util;
		std::error_code error;
		const auto directory = Files::GamePath(L"Data/Grass");

		std::filesystem::directory_iterator entry(directory, error), end;

		if (Files::IsMissing(error)) {
			return true;
		}

		while (!error && entry != end) {
			if (entry->is_regular_file(error)) {
				const auto extension = entry->path().extension().native();
				if (CompareStringOrdinal(extension.c_str(), static_cast<int>(extension.size()), L".cgid", 5, TRUE) == CSTR_EQUAL) {
					return false;
				}
			}

			if (!error) {
				entry.increment(error);
			}
		}

		if (error) {
			logger::error("Failed to inspect grass cache directory: {}", error.message());
			return false;
		}

		return true;
	}

	void InitializeMessaging()
	{
		if (!GetMessagingInterface()->RegisterListener([](MessagingInterface::Message* message) {
				switch (message->type) {
				case MessagingInterface::kPreLoadGame:
					GrassControl::PhysicsRaycast::GetSingleton().SetQueriesDisabled(true);
					break;

				case MessagingInterface::kPostLoadGame:
				case MessagingInterface::kNewGame:
					GrassControl::PhysicsRaycast::GetSingleton().SetQueriesDisabled(false);
					break;

				case MessagingInterface::kInputLoaded:
					GrassControl::Plugin::Initialize();
					break;

				case MessagingInterface::kDataLoaded:

					GrassControl::Events::Register();
					if (GrassControl::Config::UseGrassCache && !GrassControl::Config::loadFromBSA && MissingLooseCache() && !std::filesystem::exists(GrassControl::Util::GetProgressFilePath())) {
						logger::info("Grass cache files are missing. You will see no grass unless you generate a Cache by creating a new text file named PrecacheGrass next to SkyrimSE.exe or SkyrimVR.exe or downloading a pre-generated Cache from the Nexus");
						RE::DebugMessageBox("Grass cache files are missing. You will see no grass unless you generate a Cache by creating a new text file named PrecacheGrass next to SkyrimSE.exe or SkyrimVR.exe or downloading a pre-generated Cache from the Nexus");
					}

					break;

				default:
					break;
				}
			})) {
			report_and_fail("Unable to register message listener.");
		}
	}

	void InitializeLog()
	{
		auto path = logger::log_directory();
		if (!path) {
			report_and_fail("Failed to find standard logging directory"sv);
		}

		*path /= PluginDeclaration::GetSingleton()->GetName();
		*path += ".log"sv;
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		spdlog::level::level_enum level = spdlog::level::info;

		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
		log->set_level(level);
		log->flush_on(level);

		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S:%e][%l] %v"s);

		logger::info(FMT_STRING("{} v{}"), PluginDeclaration::GetSingleton()->GetName(), PluginDeclaration::GetSingleton()->GetVersion());
	}

	std::unique_ptr<Util::CachedFormList> LoadFormList(const std::string& forms, const std::string& settingName)
	{
		auto list = Util::CachedFormList::TryParse(forms, settingName);
		if (list->Empty())
			return nullptr;

		list->printList(settingName);
		return list;
	}

	void WarnExtendWithoutCache()
	{
		auto ls = std::vector<std::string>();
		ls.emplace_back("Warning!! You have enabled Extend-Grass-Distance without using pre-generated grass. This could lead to unstable game. Either disable Extend-Grass-Distance or pre-generate grass cache files. In order to use pre-generated grass cache you will need UseGrassCache=True and Only-Load-From-Cache=true");
		ls.emplace_back("Check nexus page of 'No Grass In Objects' mod for more information on how to do this.");
		ls.emplace_back("This warning won't be shown again next time you start game.");

		try {
			const auto path = Util::Files::GamePath(L"Data/SKSE/Plugins/GrassControl.warned.txt");
			std::string contents;
			std::error_code error;
			if (Util::Files::ReadText(path, contents, error)) {
				return;
			}

			if (Util::Files::IsMissing(error)) {
				contents = "Dummy file to track whether the following warning was shown:\r\n\r\n";
				for (const auto& line : ls) {
					contents += line + "\r\n\r\n";
				}

				if (!Util::Files::CreateText(path, contents, error)) {
					logger::warn("Failed to create grass warning marker: {}", error.message());
				}
			} else {
				logger::warn("Failed to read grass warning marker: {}", error.message());
			}
		} catch (const std::exception& e) {
			logger::warn("Failed to persist grass warning: {}", e.what());
		}

		std::stringstream s;
		std::ranges::copy(ls.begin(), ls.end(), std::ostream_iterator<std::string, char>(s, "\n\n"));
		logger::debug("{}", s.str());
		RE::DebugMessageBox(s.str().c_str());
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	REL::Module::reset();

#ifdef NGIO_WAIT_FOR_DEBUGGER
	while (!IsDebuggerPresent()) {
		Sleep(100);
	}
#endif

	SKSE::Init(a_skse);

	InitializeLog();
	GrassControl::Config::ReadSettings();
	if (GrassControl::Config::DebugLogEnable) {
		spdlog::default_logger()->set_level(spdlog::level::trace);
	}

	logger::info("Game version : {}", a_skse->RuntimeVersion().string());

	if (LoadLibraryW(GrassControl::Util::Files::GamePath(L"Data/SKSE/Plugins/GrassCacheHelperNG.dll").c_str())) {
		GrassControl::GidFileCache::SetHelperInstalled(true);
		logger::info("Grass Cache Helper NG detected.");
	}

	InitializeHooking();
	InitializeMessaging();

	return true;
}

namespace GrassControl::Plugin
{
	std::shared_ptr<const RaycastHelper> GetRaycastSettings()
	{
		return raycastSettings.load();
	}

	void Update()
	{
		PhysicsRaycast::GetSingleton().Update();
		Generation::Update();

		if (mainMenuUpdates == 0)
			return;

		if (mainMenuUpdates == 1) {
			mainMenuUpdates++;
		} else if (mainMenuUpdates == 2) {
			mainMenuUpdates++;

			Generation::Start();
		}
	}

	void OnMainMenuOpen()
	{
		PhysicsRaycast::GetSingleton().SetQueriesDisabled(true);
		if (Config::UseGrassCache && Config::Updating) {
			GidFileCache::EnableFileGeneration();
		}

		auto fi = std::filesystem::path(Util::GetProgressFilePath());
		if (Config::UseGrassCache && exists(fi) && mainMenuUpdates == 0) {
			mainMenuUpdates = 1;
			Memory::Internal::write<uint8_t>(RELOCATION_ID(508798, 380767).address() + 8, 1);  // Skyrim.ini [General] bAlwaysActive=1
			Memory::Internal::write<uint8_t>(RELOCATION_ID(501125, 359439).address() + 8, 0);  // Skyrim.ini [Grass] bAllowLoadGrass=0
		}

		if (Config::RayCast || Config::GrassCliffs) {
			auto cachedList = LoadFormList(Config::RayCastIgnoreForms, "Ray-cast-ignore-forms");
			auto cachedTextureList = LoadFormList(Config::RayCastTextureForms, "Ray-cast-texture-forms");
			auto cachedCliffsList = LoadFormList(Config::GrassCliffsForms, "Grass-cliffs-forms");
			auto cachedGrassFormsList = LoadFormList(Config::RayCastIgnoreGrassForms, "Ray-cast-ignore-grass-forms");

			auto settings = std::make_shared<RaycastHelper>(static_cast<float>(Config::RayCastHeight), static_cast<float>(Config::RayCastDepth), Config::RayCastCollisionLayers, std::move(cachedList), std::move(cachedTextureList), std::move(cachedCliffsList), std::move(cachedGrassFormsList));
			logger::info("Created Cache for Raycasting Settings");

			ObjectHandler::LoadObjectConfigs(settings->cliffObjects, settings->partIgnoredObjects);
			raycastSettings.store(std::move(settings));
		}

		if (Config::ExtendGrassDistance) {
			if (!Config::UseGrassCache || !Config::OnlyLoadFromCache) {
				WarnExtendWithoutCache();
			}
		}

		if (!Config::AskedRaycastWarning && Config::RayCast && !GrassControl::Config::UseGrassCache) {
			Config::AskedRaycastWarning = true;

			Config::WriteBoolSetting("Debug", "Asked-Enable-Raycast-Warning", Config::AskedRaycastWarning);

			auto callback = [](std::uint8_t a_idx) {
				if (a_idx == 1) {
					Config::RayCastError = false;

					Config::WriteBoolSetting("RayCastConfig", "Ray-cast-error-message", Config::RayCastError);
				}
			};

			auto settings = RE::GameSettingCollection::GetSingleton();

			static REL::Relocation<void (*)(const char*, void (*a_callback)(std::uint8_t), std::uint32_t, std::uint32_t, std::uint32_t, const char*, const char*, const char*)> func{ RELOCATION_ID(51420, 52269) };
			func("Do you want NGIO to display a warning when repeated physics queries fail in a cell? Failed queries use fallback placement, so grass may remain within objects. You can change this preference using Ray-cast-error-message in GrassControl.ini.", callback, 0, 0x19, 10, settings->GetSetting("sYes")->GetString(), settings->GetSetting("sNo")->GetString(), nullptr);
		}
	}
}
