#include "GrassControl/Main.h"
#include "GrassControl/Config.h"
#include "GrassControl/DistantGrass.h"
#include "GrassControl/Engine.h"
#include "GrassControl/Events.h"
#include "GrassControl/GidFileCache.h"
#include "GrassControl/Globals.h"
#include "GrassControl/PhysicsRaycast.h"
#include "GrassControl/RaycastHelper.h"
#include "GrassControl/Util.h"

namespace GrassControl::Plugin
{
	using namespace SKSE;
	using namespace SKSE::log;
	using namespace SKSE::stl;

	static void InitializeHooking()
	{
		log::trace("Initializing trampoline...");
		auto& trampoline = GetTrampoline();
		trampoline.create(2048);
		log::trace("Trampoline initialized.");
		GrassControl::Generation::InstallHooks();
		InstallHooks();
	}

	static std::unique_ptr<Util::CachedFormList> LoadFormList(const std::string& forms, const std::string& settingName)
	{
		auto list = Util::CachedFormList::TryParse(forms, settingName);
		if (list->Empty())
			return nullptr;

		list->PrintList(settingName);
		return list;
	}

	static void InitializeRaycastSettings()
	{
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
	}

	static void InitializeMessaging()
	{
		if (!GetMessagingInterface()->RegisterListener([](MessagingInterface::Message* message) {
				switch (message->type) {
				case MessagingInterface::kPreLoadGame:
					PhysicsRaycast::GetSingleton().SetQueriesDisabled(true);
					break;

				case MessagingInterface::kPostLoadGame:
				case MessagingInterface::kNewGame:
					{
						auto* ui = RE::UI::GetSingleton();
						PhysicsRaycast::GetSingleton().SetQueriesDisabled(!ui || ui->IsMenuOpen(RE::MainMenu::MENU_NAME));
						break;
					}

				case MessagingInterface::kInputLoaded:
					Initialize();
					break;

				case MessagingInterface::kDataLoaded:
					InitializeRaycastSettings();
					Events::Register();
					if (Config::UseGrassCache && !Config::loadFromBSA && !Config::GeneratingCache && DistantGrass::gridState.cacheIndex.Empty()) {
						const auto* message = Config::OnlyLoadFromCache ?
					                              "Grass cache files are missing. You will see no grass unless you generate a cache by creating a new text file named PrecacheGrass next to SkyrimSE.exe or SkyrimVR.exe, or download a pre-generated cache from the Nexus." :
					                              "Grass cache files are missing, so all grass is placed while you play. To use a cache, create a file named PrecacheGrass next to the game executable to generate one, or install a pre-generated cache from the Nexus.";
						logger::info("{}", message);
						RE::DebugMessageBox(message);
					}

					break;

				default:
					break;
				}
			})) {
			report_and_fail("Unable to register message listener.");
		}
	}

	static void InitializeLog()
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

	std::shared_ptr<const RaycastHelper> GetRaycastSettings()
	{
		return raycastSettings.load();
	}

	void Update()
	{
		PhysicsRaycast::GetSingleton().Update();
		Generation::Update();
		DistantGrass::Update();

		// Generation starts on the second frame after the main menu opens.
		if ((mainMenuUpdates == 1 || mainMenuUpdates == 2) && ++mainMenuUpdates == 3)
			Generation::Start();
	}

	void OnMainMenuOpen()
	{
		PhysicsRaycast::GetSingleton().SetQueriesDisabled(true);
		if (Config::UseGrassCache && Config::Updating) {
			GidFileCache::EnableFileGeneration();
		}

		if (Config::GeneratingCache && mainMenuUpdates == 0) {
			mainMenuUpdates = 1;
			if (auto* alwaysActive = globals::FindSetting("bAlwaysActive:General"))
				alwaysActive->data.b = true;
			if (auto* allowLoadGrass = globals::settings::allowLoadGrass)
				allowLoadGrass->data.b = false;
		}

		if (!Config::AskedRaycastWarning && Config::RayCast && !Config::UseGrassCache) {
			Config::AskedRaycastWarning = true;

			Config::WriteBoolSetting("Debug", "Asked-Enable-Raycast-Warning", Config::AskedRaycastWarning);

			auto callback = [](std::uint8_t a_idx) {
				if (a_idx == 1) {
					Config::RayCastError = false;

					Config::WriteBoolSetting("RayCastConfig", "Ray-cast-error-message", Config::RayCastError);
				}
			};

			auto settings = RE::GameSettingCollection::GetSingleton();

			RE::MessageBoxMenu::Create("Do you want NGIO to display a warning when repeated physics queries fail in a cell? Failed queries use fallback placement, so grass may remain within objects. You can change this preference using Ray-cast-error-message in GrassControl.ini.", callback, 0, 0x19, 10, settings->GetSetting("sYes")->GetString(), settings->GetSetting("sNo")->GetString());
		}
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

	GrassControl::Plugin::InitializeLog();
	GrassControl::Config::ReadSettings();
	if (GrassControl::Config::DebugLogEnable) {
		spdlog::default_logger()->set_level(spdlog::level::trace);
		spdlog::default_logger()->flush_on(spdlog::level::trace);
	}

	logger::info("Game version : {}", a_skse->RuntimeVersion().string());

	if (LoadLibraryW(GrassControl::Util::Files::GamePath(L"Data/SKSE/Plugins/GrassCacheHelperNG.dll").c_str())) {
		GrassControl::GidFileCache::SetHelperInstalled(true);
		logger::info("Grass Cache Helper NG detected.");
	}

	GrassControl::globals::OnInit();
	GrassControl::Plugin::InitializeHooking();
	GrassControl::Plugin::InitializeMessaging();

	return true;
}
