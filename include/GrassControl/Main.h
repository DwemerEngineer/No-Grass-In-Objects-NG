#pragma once

#include "CasualLibrary.hpp"
#include "GrassControl/Config.h"
#include "GrassControl/DistantGrass.h"
#include "GrassControl/Logging.h"
#include "GrassControl/Profiler.h"
#include "GrassControl/RaycastHelper.h"

namespace GrassControl
{

	class GrassControlPlugin final
	{
		inline static Profiler* profiler = new Profiler();

		inline static std::intptr_t addr_MaxGrassPerTexture = RELOCATION_ID(501615, 360443).address();

		inline static int _did_mainMenu = 0;

	public:
		inline static std::unique_ptr<RaycastHelper> RaycastSettingsCache;

		static void init();

		static void warn_extend_without_cache();

		static void InstallHooks()
		{
			Hooks::Install();
		}

		static void OnMainMenuOpen();

		static void Update();

		static RE::TESObjectREFR* PlaceAtMe(RE::TESObjectREFR* self, RE::TESForm* a_form, std::uint32_t count, bool forcePersist, bool initiallyDisabled)
		{
			using func_t = RE::TESObjectREFR*(RE::BSScript::Internal::VirtualMachine*, RE::VMStackID, RE::TESObjectREFR*, RE::TESForm*, std::uint32_t, bool, bool);
			RE::VMStackID frame = 0;

			REL::Relocation<func_t> func{ RELOCATION_ID(55672, 56203) };
			auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();

			return func(vm, frame, self, a_form, count, forcePersist, initiallyDisabled);
		};

	protected:
		struct Hooks
		{
			struct MainUpdate_Nullsub
			{
				static void thunk()
				{
					func();
					Update();
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			// Track when console is opened.
			struct ConsoleOpen
			{
				static void thunk(int64_t a1, char a2)
				{
					func(a1, a2);
					profiler->Report();
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			// Track the time taken in create grass function.
			// 4D10
			struct GrassCreationStart
			{
				static uint64_t thunk(uint64_t a1, uint64_t a2, char* a3)
				{
					profiler->Begin();

					return func(a1, a2, a3);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};
			// 5923
			struct GrassCreationEnd
			{
				static DWORD thunk()
				{
					// func(); This is GetCurrentThreadId() but has an access error in SE;
					profiler->End();
					return GetCurrentThreadId();
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			static void Install()
			{
				bool marketplace = REL::Relocate(false, REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_6_1130);

				if (Config::UseGrassCache && exists(std::filesystem::path(Util::GetProgressFilePath()))) {
					if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99)
					{
						stl::write_thunk_call<MainUpdate_Nullsub>(RELOCATION_ID(35565, 36564).address() + 0xC3D);
					}
					else
					{
						stl::write_thunk_call<MainUpdate_Nullsub>(RELOCATION_ID(35565, 36564).address() + REL::Relocate(0x748, (marketplace ? 0xC2b : 0xC26), 0x7EE));
					}
				}

				if (Config::ProfilerReport) {
					if (marketplace) {
						stl::write_thunk_call<ConsoleOpen>(REL::ID(442669).address() + 0x15D);
					} else {
						stl::write_thunk_call<ConsoleOpen>(RELOCATION_ID(50155, 51082).address() + REL::Relocate(0x142, 0x15C, 0x14F));
					}

					stl::write_thunk_call<GrassCreationStart>(RELOCATION_ID(13148, 13288).address() + REL::Relocate(0x905, 0xb29));
					stl::write_thunk_jump<GrassCreationStart>(RELOCATION_ID(13138, 13278).address() + REL::Relocate(0xF, 0xF));
					stl::write_thunk_call<GrassCreationEnd, 6>(RELOCATION_ID(15204, 15372).address() + REL::Relocate(0xBDD, 0xbd9));
				}
			}
		};
	};
}
