#include "GrassControl/Main.h"
using namespace Xbyak;
namespace GrassControl
{
	static float SetScale(float a_input)
	{
		// we need to only interact with floats so the input/ouput register xmm0 is proper precision
		return static_cast<float>(a_input * Config::GlobalGrassScale);
	}

	static bool CanPlaceGrassWrapper(RE::TESObjectLAND* land, const float x, const float y, const float z, uintptr_t param)
	{
		if (Config::RayCast) {
			RE::GrassParam* grassParam;
			if (REL::Module::IsSE()) {
				auto paramPtr = reinterpret_cast<RE::GrassParam**>(param);
				grassParam = *paramPtr;
			} else {
				grassParam = reinterpret_cast<RE::GrassParam*>(param);
			}

			if (GrassControlPlugin::RaycastSettingsCache != nullptr && !GrassControlPlugin::RaycastSettingsCache->CanPlaceGrass(land, x, y, z, grassParam)) {
				return false;
			}
		}
		return true;
	}

	static float GrassCliffHelper(glm::vec3& normal, const float x, const float y, const float z, uintptr_t param)
	{
		if (Config::GrassCliffs) {
			if (GrassControlPlugin::RaycastSettingsCache != nullptr) {
				RE::GrassParam* grassParam = nullptr;
				if (REL::Module::IsSE()) {
					auto paramPtr = reinterpret_cast<RE::GrassParam**>(param);
					grassParam = *paramPtr;
				} else {
					grassParam = reinterpret_cast<RE::GrassParam*>(param);
				}

				auto ret = GrassControlPlugin::RaycastSettingsCache->CreateGrassCliff(x, y, z, normal, grassParam);
				return ret;
			}
		}
		return z;
	}

	void GrassControlPlugin::Update()
	{
		GidFileGenerationTask::Update();

		if (_did_mainMenu == 0)
			return;

		if (_did_mainMenu == 1) {
			_did_mainMenu++;
		} else if (_did_mainMenu == 2) {
			_did_mainMenu++;

			GidFileGenerationTask::cur_state = 1;

			if (Config::FreezeCheck) {
				GidFileGenerationTask::_lastDidSomething = GetTickCount64();
				std::thread t(GidFileGenerationTask::run_freeze_check);
				t.detach();
			}

			auto gf = std::make_unique<GidFileGenerationTask>();
			GidFileGenerationTask::cur_instance = std::move(gf);
		}

		//if (RaycastSettingsCache->phantomCreated)
			//RaycastSettingsCache->CheckInactivePhantom();
	}

	void GrassControlPlugin::OnMainMenuOpen()
	{
		auto fi = std::filesystem::path(Util::getProgressFilePath());
		if (Config::UseGrassCache && exists(fi)) {
			_did_mainMenu = 1;
			Memory::Internal::write<uint8_t>(RELOCATION_ID(508798, 380767).address() + 8, 1);  // Skyrim.ini [General] bAlwaysActive=1
			Memory::Internal::write<uint8_t>(RELOCATION_ID(501125, 359439).address() + 8, 0);  // Skyrim.ini [Grass] bAllowLoadGrass=0
		}

		if (Config::RayCast || Config::GrassCliffs) {
			std::string formsStr = Config::RayCastIgnoreForms;
			auto cachedList = Util::CachedFormList::TryParse(formsStr, "Ray-cast-ignore-forms", true, false);
			if (cachedList != nullptr && cachedList->getAll().empty()) {
				cachedList = nullptr;
			} else if (cachedList != nullptr) {
				cachedList->printList("Ray-cast-ignore-forms");
			}

			std::string textureFormsStr = Config::RayCastTextureForms;
			auto cachedTextureList = Util::CachedFormList::TryParse(textureFormsStr, "Ray-cast-texture-forms", true, false);
			if (cachedTextureList != nullptr && cachedTextureList->getAll().empty()) {
				cachedTextureList = nullptr;
			} else if (cachedTextureList != nullptr) {
				cachedTextureList->printList("Ray-cast-texture-forms");
			}

			std::string cliffsFormsStr = Config::GrassCliffsForms;
			auto cachedCliffsList = Util::CachedFormList::TryParse(cliffsFormsStr, "Grass-cliffs-forms", true, false);
			if (cachedCliffsList != nullptr && cachedCliffsList->getAll().empty()) {
				cachedCliffsList = nullptr;
			} else if (cachedCliffsList != nullptr) {
				cachedCliffsList->printList("Grass-cliffs-forms");
			}

			std::string grassFormsStr = Config::RayCastIgnoreGrassForms;
			auto cachedGrassFormsList = Util::CachedFormList::TryParse(grassFormsStr, "Ray-cast-ignore-grass-forms", true, false);
			if (cachedGrassFormsList != nullptr && cachedGrassFormsList->getAll().empty()) {
				cachedGrassFormsList = nullptr;
			} else if (cachedGrassFormsList != nullptr) {
				cachedGrassFormsList->printList("Grass-cliffs-forms");
			}

			RaycastSettingsCache = std::make_unique<RaycastHelper>(static_cast<int>(std::stof(SKSE::PluginDeclaration::GetSingleton()->GetVersion().string())), static_cast<float>(Config::RayCastHeight), static_cast<float>(Config::RayCastDepth), Config::RayCastCollisionLayers, std::move(cachedList), std::move(cachedTextureList), std::move(cachedCliffsList), std::move(cachedGrassFormsList));
			logger::info("Created Cache for Raycasting Settings");
		}

		if (Config::ExtendGrassDistance) {
			if (!Config::UseGrassCache || !Config::OnlyLoadFromCache) {
				warn_extend_without_cache();
			}
		}

		if (!Config::AskedRaycastWarning && Config::RayCast && !GrassControl::Config::UseGrassCache) {
			Config::AskedRaycastWarning = true;

			CSimpleIniA ini;
			ini.SetUnicode();

			ini.LoadFile(Config::iniPath.data());
			ini.SetBoolValue("Debug", "Asked-Enable-Raycast-Warning", Config::AskedRaycastWarning);
			ini.SaveFile(Config::iniPath.data());

			auto callback = [](std::uint8_t a_idx) {
				if (a_idx == 1) {
					Config::RayCastError = false;

					CSimpleIniA ini;
					ini.SetUnicode();

					ini.LoadFile(Config::iniPath.data());
					ini.SetBoolValue("RayCastConfig", "Ray-cast-error-message", Config::RayCastError);
					ini.SaveFile(Config::iniPath.data());
				}
			};

			auto settings = RE::GameSettingCollection::GetSingleton();

			static REL::Relocation<void (*)(const char*, void (*a_callback)(std::uint8_t), std::uint32_t, std::uint32_t, std::uint32_t, const char*, const char*, const char*)> func{ RELOCATION_ID(51420, 52269) };
			func("Do you want NGIO to display an error warning when it encounters multiple errors while raycasting within a cell? These errors are signs of possible crashes. When NGIO encounters an error, grass may remain within objects. If you change your mind, you just need to change Ray-cast-error-message in GrassControl.ini", callback, 0, 0x19, 10, settings->GetSetting("sYes")->GetString(), settings->GetSetting("sNo")->GetString(), nullptr);
		}
	}

	void GrassControlPlugin::init()
	{
		std::stringstream s;
		auto list = { std::to_string(static_cast<int>(RE::COL_LAYER::kStatic)), std::to_string(static_cast<int>(RE::COL_LAYER::kAnimStatic)), std::to_string(static_cast<int>(RE::COL_LAYER::kTerrain)), std::to_string(static_cast<int>(RE::COL_LAYER::kDebrisLarge)), std::to_string(static_cast<int>(RE::COL_LAYER::kStairHelper)) };
		std::ranges::copy(list.begin(), list.end(), std::ostream_iterator<std::string>(s, " "));

		Config::RayCastCollisionLayers = s.str();

		auto fi = std::filesystem::path(Util::getProgressFilePath());
		if (Config::UseGrassCache && exists(fi)) {
			if (REL::Module::IsVR()) {
				auto setting = RE::INISettingCollection::GetSingleton()->GetSetting("bLoadVRPlayroom:VR");
				if (!setting) {
					setting = RE::INIPrefSettingCollection::GetSingleton()->GetSetting("bLoadVRPlayroom:VR");
					if (!setting) {
						logger::error("Failed to find bLoadVRPlayroom");
						return;
					}
				}
				setting->data.b = false;
			}

			Config::OnlyLoadFromCache = false;

			GidFileGenerationTask::apply();

			Config::ExtendGrassDistance = false;
			Config::DynDOLODGrassMode = 0;
		} else if (Config::UseGrassCache) {
			logger::info("Grass Cache is Enabled. PrecacheGrass.txt is not detected, assuming normal usage.");
		} else {
			logger::info("Grass Cache is Disabled");
		}

		switch (Config::DynDOLODGrassMode) {
		case 1:
			{
				Config::OverwriteGrassDistance = 999999.0;
				Config::OverwriteGrassFadeRange = 0.0;
				Config::ExtendGrassDistance = false;
			}
			break;

		case 2:
			{
				Config::OverwriteGrassDistance = 999999.0;
				Config::OverwriteGrassFadeRange = 0.0;
				Config::ExtendGrassDistance = true;
			}
			break;
		default:
			break;
		}

		if (Config::RayCast || Config::GrassCliffs) {
			auto addr = RELOCATION_ID(15212, 15381).address() + REL::Relocate((0x723A - 0x6CE0), 0x664, 0x56C);
			struct Patch : CodeGenerator
			{
				Patch(std::uintptr_t a_func, std::uintptr_t b_func, std::uintptr_t a_target,
					std::uintptr_t a_rspOffset,
					std::uintptr_t b_rspOffset,
					Reg a_rcxSource,
					Xmm a_z,
					std::uintptr_t a_rbpOffset,
					Reg64 a_grassParamReg,
					std::uintptr_t a_targetJumpOffset,
					std::uintptr_t a_floatArray)
				{
					Label funcLabel;
					Label funcLabel2;
					Label retnLabel;

					Label isCliff;
					Label notCliff;

					Label jump;
					Label notIf;

					movss(xmm1, ptr[rsp + a_rspOffset]);        // x
					movss(xmm2, ptr[rsp + a_rspOffset + 0x4]);  // y
					movss(xmm3, a_z);                           // z
					mov(rcx, a_floatArray);

					mov(rax, a_grassParamReg);
					sub(rsp, 0x30);
					mov(ptr[rsp + 0x20], rax);
					call(ptr[rip + funcLabel2]);
					add(rsp, 0x30);

					ucomiss(a_z, xmm0);
					jp(isCliff);
					je(notCliff);

					L(isCliff);
					movss(a_z, xmm0);

					//terrain normal -> Cliff normal
					mov(rcx, a_floatArray);
					movss(xmm0, ptr[rcx + 0x8]);
					movss(ptr[rsp + b_rspOffset + 0x8], xmm0);  // z
					movss(xmm0, ptr[rcx + 0x4]);
					movss(ptr[rsp + b_rspOffset + 0x4], xmm0);  // y
					movss(xmm0, ptr[rcx]);
					movss(ptr[rsp + b_rspOffset], xmm0);  // x

					jmp(notIf);
					L(notCliff);
					movss(xmm1, ptr[rsp + a_rspOffset]);        // x
					movss(xmm2, ptr[rsp + a_rspOffset + 0x4]);  // y
					movss(xmm3, a_z);                           // z
					mov(rcx, a_rcxSource);

					mov(rax, a_grassParamReg);
					sub(rsp, 0x30);
					mov(ptr[rsp + 0x20], rax);
					call(ptr[rip + funcLabel]);
					add(rsp, 0x30);

					test(al, al);
					jne(notIf);
					jmp(ptr[rip + jump]);

					L(notIf);
					movss(xmm6, ptr[rbp - a_rbpOffset]);
					jmp(ptr[rip + retnLabel]);

					L(jump);
					dq(a_target + a_targetJumpOffset);

					L(funcLabel);
					dq(a_func);

					L(funcLabel2);
					dq(b_func);

					L(retnLabel);
					dq(a_target + 0x5);
				}
			};
			Patch patch(reinterpret_cast<uintptr_t>(CanPlaceGrassWrapper), reinterpret_cast<uintptr_t>(GrassCliffHelper), addr,
				REL::Relocate(0x40, 0x50, 0x50),
				REL::Relocate(0x30, 0x40, 0x40),
				Reg64(REL::Relocate(Reg::RSI, Reg::RDI, Reg::RBX)),
				Xmm(REL::Relocate(7, 7, 14)),
				REL::Relocate(0x48, 0x68, 0x38),
				Reg64(REL::Relocate(Reg64::RBP, Reg64::RBX, Reg64::R13)),
				REL::Relocate(0x5 + (0x661 - 0x23F), -0x156, 0x5 + 0x510),
				reinterpret_cast<uintptr_t>(&normalBuffer));
			patch.ready();

			auto& trampoline = SKSE::GetTrampoline();
			trampoline.write_branch<5>(addr, trampoline.allocate(patch));
		}

		if (Config::SuperDenseGrass) {
			// Make amount big.
			auto addr = RELOCATION_ID(15202, 15370).address() + REL::Relocate(0xAE5 - 0x890, 0x258);
			int mode = std::max(0, std::min(12, Config::SuperDenseMode));
			if (mode != 7) {
				Memory::Internal::write<uint8_t>(addr + 2, static_cast<unsigned char>(mode), true);
			}
		}

		if (Config::ExtendGrassCount) {
			// Create more grass shapes if one becomes full.
			auto addr = RELOCATION_ID(15220, 15385).address() + REL::Relocate(0x433 - 0x3C0, 0x68);
			REL::safe_write(addr, REL::NOP6, 6);

			addr = RELOCATION_ID(15214, 15383).address() + REL::Relocate(0x960 - 0x830, 0x129);
			struct Patch : CodeGenerator
			{
				Patch(uintptr_t a_target)
				{
					Label notIf;
					Label retnLabel;
					Label jump;

					cmp(ptr[rax], rbx);
					jne(notIf);
					mov(rdx, ptr[rax + 8]);
					cmp(rdx, 0);
					je(notIf);
					mov(rdx, ptr[rdx]);
					cmp(rdx, 0);
					je(notIf);
					mov(ecx, ptr[rdx + 0x190]);
					imul(ecx, ptr[rdx + 0x194]);
					shl(ecx, 1);
					cmp(ecx, 0x20000);
					jge(notIf);
					jmp(ptr[rip + jump]);
					L(notIf);
					jmp(ptr[rip + retnLabel]);

					L(retnLabel);
					dq(a_target + 0x5);

					L(jump);
					dq(a_target + 0xF);
				}
			};
			Patch patch(addr);
			patch.ready();

			auto& trampoline = SKSE::GetTrampoline();
			trampoline.write_branch<5>(addr, trampoline.allocate(patch));
		}

		if (Config::UseGrassCache) {
			GidFileCache::FixFileFormat(Config::OnlyLoadFromCache);
		}

		if (Config::ExtendGrassDistance) {
			DistantGrass::InstallHooks();
			DistantGrass::ReplaceGrassGrid(Config::OnlyLoadFromCache);
		}

		if (Config::EnsureMaxGrassTypesPerTextureSetting > 0) {
			auto addr = RELOCATION_ID(18342, 18758).address() + REL::Relocate(0xD63 - 0xCF0, 0x68);

			uint32_t max = std::max(Config::EnsureMaxGrassTypesPerTextureSetting, Memory::Internal::read<int>(addr_MaxGrassPerTexture + 8));

			struct Patch : CodeGenerator
			{
				Patch(uint32_t max, std::uintptr_t a_target)
				{
					Label retnLabel;

					if (REL::Module::IsAE()) {
						mov(edi, max);
					} else {
						mov(r12d, max);
					}
					jmp(ptr[rip + retnLabel]);

					L(retnLabel);
					dq(a_target + 0x7);
				}
			};
			Patch patch(max, addr);
			patch.ready();

			auto& trampoline = SKSE::GetTrampoline();
			REL::safe_write(addr + 5, REL::NOP2, 2);
			trampoline.write_branch<5>(addr, trampoline.allocate(patch));
		}

		if (Config::OverwriteGrassDistance >= 0.0) {
			auto setting = RE::INISettingCollection::GetSingleton()->GetSetting("fGrassStartFadeDistance:Grass");
			if (!setting) {
				setting = RE::INIPrefSettingCollection::GetSingleton()->GetSetting("fGrassStartFadeDistance:Grass");
				if (!setting) {
					logger::error("Failed to find fGrassStartFadeDistance");
					return;
				}
			}
			setting->data.f = static_cast<float>(Config::OverwriteGrassDistance);
		}

		if (Config::OverwriteGrassFadeRange >= 0.0) {
			auto setting = RE::INISettingCollection::GetSingleton()->GetSetting("fGrassFadeRange:Grass");
			if (!setting) {
				setting = RE::INIPrefSettingCollection::GetSingleton()->GetSetting("fGrassFadeRange:Grass");
				if (!setting) {
					logger::error("Failed to find fGrassFadeRange");
					return;
				}
			}
			setting->data.f = static_cast<float>(Config::OverwriteGrassFadeRange);
		}

		if (Config::OverwriteMinGrassSize >= 0) {
			auto setting = RE::INISettingCollection::GetSingleton()->GetSetting("iMinGrassSize:Grass");
			if (!setting) {
				setting = RE::INIPrefSettingCollection::GetSingleton()->GetSetting("iMinGrassSize:Grass");
				if (!setting) {
					logger::error("Failed to find iMinGrassSize");
					return;
				}
			}
			setting->data.i = Config::OverwriteMinGrassSize;
		}

		if (Config::GlobalGrassScale != 1.0 && Config::GlobalGrassScale > 0.0001) {
			auto addr = RELOCATION_ID(15212, 15381).address() + REL::Relocate(0x93a, 0xab4, 0xa0c);
			struct Patch : CodeGenerator
			{
				Patch(uintptr_t a_target, uintptr_t a_func,
					Xmm a_sourceReg_4656,  // 4.656613e-10
					Xmm a_sourceReg_1,     // 1.0
					Reg a_baseReg)
				{
					Label funcLabel;
					Label retnLabel;

					mulss(xmm0, a_sourceReg_4656);       // finalScale *= 4.656613e-10; // original code
					subss(xmm0, a_sourceReg_1);          // xmm0 -= 1; original code
					mulss(xmm0, ptr[a_baseReg + 0x10]);  // original code
					addss(xmm0, a_sourceReg_1);          // finalScale = xmm0 + 1;

					// function call
					sub(rsp, 0x20);
					mov(al, 1);                  // one float parameter
					call(ptr[rip + funcLabel]);  // finalScale = SetScale(finalScale);
					add(rsp, 0x20);
					subss(xmm0, a_sourceReg_1);  // xmm0 = finalScale - 1; // original code
					jmp(ptr[rip + retnLabel]);

					L(funcLabel);
					dq(a_func);

					L(retnLabel);
					dq(a_target);
				}
			};
			Patch patch(addr + REL::Relocate(0xf, 0xf, 0x10), reinterpret_cast<uintptr_t>(SetScale), Xmm(REL::Relocate(14, 14, 12)), Xmm(REL::Relocate(11, 10, 11)), Reg64(REL::Relocate(Reg::RSI, Reg::RBX, Reg::R13)));
			patch.ready();

			auto& trampoline = SKSE::GetTrampoline();
			trampoline.write_branch<5>(addr, trampoline.allocate(patch));
		}
	}

	void GrassControlPlugin::warn_extend_without_cache()
	{
		auto ls = std::vector<std::string>();
		ls.emplace_back("Warning!! You have enabled Extend-Grass-Distance without using pre-generated grass. This could lead to unstable game. Either disable Extend-Grass-Distance or pre-generate grass cache files. In order to use pre-generated grass cache you will need UseGrassCache=True and Only-Load-From-Cache=true");
		ls.emplace_back("Check nexus page of 'No Grass In Objects' mod for more information on how to do this.");
		ls.emplace_back("This warning won't be shown again next time you start game.");

		try {
			auto fi = std::filesystem::path("Data/SKSE/Plugins/GrassControl.warned.txt");
			if (exists(fi))
				return;

			std::ofstream sw;
			sw.open(fi);
			sw << "Dummy file to track whether the following warning was shown: \n";
			sw << "\n";
			std::stringstream s;
			std::string Newline = "\r\n";
			std::string NewNewLine = "\r\n" + Newline;
			std::ranges::copy(ls.begin(), ls.end(), std::ostream_iterator<std::string, char>(s, NewNewLine.c_str()));
			sw << s.str();
		} catch (...) {
		}
		std::stringstream s;
		std::ranges::copy(ls.begin(), ls.end(), std::ostream_iterator<std::string, char>(s, "\n\n"));
		logger::debug(fmt::runtime(s.str()));
		RE::DebugMessageBox(s.str().c_str());
	}
}
