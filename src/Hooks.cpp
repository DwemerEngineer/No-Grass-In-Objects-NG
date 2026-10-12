#include "CasualLibrary.hpp"
#include "GrassControl/Config.h"
#include "GrassControl/DistantGrass.h"
#include "GrassControl/GidFileCache.h"
#include "GrassControl/Main.h"
#include "GrassControl/PhysicsRaycast.h"
#include "GrassControl/Profiler.h"
#include "GrassControl/RaycastHelper.h"
#include "GrassControl/Util.h"
#include <array>
#include <bit>
#include <new>

using namespace Xbyak;

namespace
{
	using namespace GrassControl;

	Profiler profiler;
	bool cacheHelperInstalled = false;

	namespace PluginHooks
	{
		struct MainUpdate_Nullsub
		{
			static void thunk()
			{
				func();
				Plugin::Update();
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ConsoleOpen
		{
			static void thunk(int64_t a1, char a2)
			{
				func(a1, a2);
				profiler.Report();
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct GrassCreationStart
		{
			static void thunk(RE::BGSGrassManager* a_manager, RE::TESObjectCELL* a_cell, bool* a_abort)
			{
				profiler.Begin();

				func(a_manager, a_cell, a_abort);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct GrassCreationQueuedStart
		{
			static void thunk(RE::BGSGrassManager* a_manager, RE::TESObjectCELL* a_cell, bool* a_abort)
			{
				profiler.Begin();

				func(a_manager, a_cell, a_abort);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct GrassCreationEnd
		{
			static DWORD thunk()
			{
				profiler.End();
				return GetCurrentThreadId();
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		void Install()
		{
			bool marketplace = REL::Relocate(false, REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_6_1130);

			if (Config::RayCast || Config::GrassCliffs || (Config::UseGrassCache && exists(std::filesystem::path(Util::GetProgressFilePath())))) {
				if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99) {
					stl::write_thunk_call<MainUpdate_Nullsub>(RELOCATION_ID(35565, 36564).address() + 0xC3D);
				} else {
					stl::write_thunk_call<MainUpdate_Nullsub>(RELOCATION_ID(35565, 36564).address() + REL::Relocate(0x748, (marketplace ? 0xC2b : 0xC26), 0x7EE));
				}
			}

			if (Config::ProfilerReport) {
				if (marketplace) {
					const auto offset = REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99 ? 0x16B : 0x15D;
					stl::write_thunk_call<ConsoleOpen>(REL::ID(442669).address() + offset);
				} else {
					stl::write_thunk_call<ConsoleOpen>(RELOCATION_ID(50155, 51082).address() + REL::Relocate(0x142, 0x15C, 0x14F));
				}

				const bool updatedRuntime = REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99;
				stl::write_thunk_call<GrassCreationStart>(RELOCATION_ID(13148, 13288).address() + (updatedRuntime ? 0x9D9 : REL::Relocate(0x905, 0xB29)));
				stl::write_thunk_jump<GrassCreationQueuedStart>(RELOCATION_ID(13138, 13278).address() + REL::Relocate(0xF, 0xF));
				stl::write_thunk_call<GrassCreationEnd, 6>(RELOCATION_ID(15204, 15372).address() + (updatedRuntime ? 0xBE9 : REL::Relocate(0xBDD, 0xBD9)));
			}
		}
	}
}

namespace GrassControl
{
	void Plugin::InstallHooks()
	{
		PluginHooks::Install();
	}
}

namespace GrassControl
{
	static bool CanPlaceGrassWrapper(RE::TESObjectLAND* land, const float x, const float y, const float z, uintptr_t param, bool& hitCliff, bool& falseCliff)
	{
		hitCliff = false;
		try {
			if (!Config::RayCast)
				return true;

			RE::GrassParam* grassParam;
			if (REL::Module::IsSE()) {
				auto paramPtr = reinterpret_cast<RE::GrassParam**>(param);
				grassParam = paramPtr ? *paramPtr : nullptr;
			} else {
				grassParam = reinterpret_cast<RE::GrassParam*>(param);
			}

			if (auto settings = Plugin::GetRaycastSettings(); settings && !settings->CanPlaceGrass(land, x, y, z, grassParam, hitCliff, falseCliff)) {
				return false;
			}
		} catch (const std::bad_alloc&) {
			hitCliff = false;
			falseCliff = false;
			PhysicsRaycast::GetSingleton().ReportAllocationFailure();
		}
		return true;
	}

	static float GrassCliffHelper(glm::vec3& normal, const float x, const float y, const float z, uintptr_t param, bool& falseCliff, RE::TESObjectLAND* land)
	{
		try {
			if (!Config::GrassCliffs)
				return z;

			if (auto settings = Plugin::GetRaycastSettings()) {
				RE::GrassParam* grassParam;
				if (REL::Module::IsSE()) {
					auto paramPtr = reinterpret_cast<RE::GrassParam**>(param);
					grassParam = paramPtr ? *paramPtr : nullptr;
				} else {
					grassParam = reinterpret_cast<RE::GrassParam*>(param);
				}

				auto ret = settings->CreateGrassCliff(land, x, y, z, normal, grassParam, falseCliff);
				return ret;
			}
		} catch (const std::bad_alloc&) {
			falseCliff = true;
			PhysicsRaycast::GetSingleton().ReportAllocationFailure();
		}

		return z;
	}

	void Plugin::Initialize()
	{
		auto fi = std::filesystem::path(Util::GetProgressFilePath());
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

			Generation::ApplyPatches();

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
			auto rspPosOffset = REL::Relocate(0x40, 0x50, 0x50);
			auto rspNormalOffset = REL::Relocate(0x30, 0x40, 0x40);
			auto rbpColorOffset = REL::Relocate(0x48, 0x68, 0x38);

			// Re-enter the rejection path before it restores the loop's XMM values and clears edx.
			auto jumpOffset = REL::Relocate(0x5 + (0x661 - 0x23F), -0x172, 0x5 + 0x510);
			if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99) {
				addr = RELOCATION_ID(15212, 15381).address() + 0x698;
				rspPosOffset = 0x68;
				rspNormalOffset = 0x38;
				rbpColorOffset = 0x60;
				jumpOffset = -0x16F;
			}

			struct GrassPlacementPatch : CodeGenerator
			{
				GrassPlacementPatch(std::uintptr_t a_canPlaceGrassFunc, std::uintptr_t a_grassCliffHelperFunc, std::uintptr_t a_target,
					std::uintptr_t a_rspPosOffset,
					std::uintptr_t a_rspNormalOffset,
					Reg a_rcxSource,
					Xmm a_z,
					std::uintptr_t a_rbpColorOffset,
					Reg64 a_grassParamReg,
					std::uintptr_t a_targetJumpOffset)
				{
					Label canPlaceGrassFunc;
					Label grassCliffHelper;
					Label retnLabel;

					Label jump;
					Label canPlace;
					Label PossibleCliff;

					Label isCliff;
					Label notCliff;
					Label recheckCliff;

					sub(rsp, 0x70);            // Reserve helper arguments, flags and the cliff normal.
					mov(byte[rsp + 0x48], 0);  // falseCliff = false;

					L(recheckCliff);

					movss(xmm1, ptr[rsp + 0x70 + a_rspPosOffset]);        // x
					movss(xmm2, ptr[rsp + 0x70 + a_rspPosOffset + 0x4]);  // y
					movss(xmm3, a_z);                                     // z
					mov(rcx, a_rcxSource);                                // land

					mov(rax, a_grassParamReg);
					mov(ptr[rsp + 0x20], rax);  // grassParam

					lea(rax, ptr[rsp + 0x40]);
					mov(ptr[rsp + 0x28], rax);  // &hitCliff

					lea(rax, ptr[rsp + 0x48]);
					mov(ptr[rsp + 0x30], rax);           // &falseCliff
					call(ptr[rip + canPlaceGrassFunc]);  // canPlace = CanPlaceGrassWrapper(land, x, y, z, grassParam, hitCliff, falseCliff);

					mov(cl, al);               // canPlace
					mov(al, ptr[rsp + 0x40]);  // hitCliff
					test(al, al);
					jne(PossibleCliff);  // if (hitCliff) goto PossibleCliff;

					test(cl, cl);
					jne(canPlace);  // if (canPlace) goto canPlace;

					add(rsp, 0x70);

					jmp(ptr[rip + jump]);  // Reject the candidate.

					L(canPlace);
					add(rsp, 0x70);

					movss(xmm6, ptr[rbp - a_rbpColorOffset]);  // Vertex color must remain separate from the cliff orientation normal.

					jmp(ptr[rip + retnLabel]);

					L(jump);
					dq(a_target + a_targetJumpOffset);  // Candidate rejection path.

					L(PossibleCliff);
					movss(xmm1, ptr[rsp + 0x70 + a_rspPosOffset]);        // x
					movss(xmm2, ptr[rsp + 0x70 + a_rspPosOffset + 0x4]);  // y
					movss(xmm3, a_z);                                     // z
					lea(rcx, ptr[rsp + 0x50]);                            // &cliffNormal

					mov(rax, a_grassParamReg);
					mov(ptr[rsp + 0x20], rax);  // grassParam

					lea(rax, ptr[rsp + 0x48]);
					mov(ptr[rsp + 0x28], rax);  // &falseCliff

					mov(rax, a_rcxSource);
					mov(ptr[rsp + 0x30], rax);          // land
					call(ptr[rip + grassCliffHelper]);  // cliffZ = GrassCliffHelper(cliffNormal, x, y, z, grassParam, falseCliff, land);

					ucomiss(a_z, xmm0);  // Compare z with cliffZ.
					jp(isCliff);         // if (unordered(z, cliffZ)) goto isCliff;
					je(notCliff);        // if (z == cliffZ) goto notCliff;

					L(isCliff);
					movss(a_z, xmm0);  // z = cliffZ;

					lea(rcx, ptr[rsp + 0x50]);  // &cliffNormal

					movss(xmm0, ptr[rcx + 0x8]);
					movss(ptr[rsp + 0x70 + a_rspNormalOffset + 0x8], xmm0);  // normal.z = cliffNormal.z;
					movss(xmm0, ptr[rcx + 0x4]);
					movss(ptr[rsp + 0x70 + a_rspNormalOffset + 0x4], xmm0);  // normal.y = cliffNormal.y;
					movss(xmm0, ptr[rcx]);
					movss(ptr[rsp + 0x70 + a_rspNormalOffset], xmm0);  // normal.x = cliffNormal.x;

					jmp(canPlace);  // Accept the cliff position and normal.

					L(notCliff);
					mov(al, 1);
					mov(byte[rsp + 0x48], al);  // falseCliff = true;

					jmp(recheckCliff);  // Recheck placement without the rejected cliff.

					L(canPlaceGrassFunc);
					dq(a_canPlaceGrassFunc);

					L(grassCliffHelper);
					dq(a_grassCliffHelperFunc);

					L(retnLabel);
					dq(a_target + 0x5);
				}
			};

			GrassPlacementPatch patch(reinterpret_cast<uintptr_t>(CanPlaceGrassWrapper), reinterpret_cast<uintptr_t>(GrassCliffHelper), addr,
				rspPosOffset,
				rspNormalOffset,
				Reg64(REL::Relocate(Reg::RSI, Reg::RDI, Reg::RBX)),
				Xmm(REL::Relocate(7, 7, 14)),
				rbpColorOffset,
				Reg64(REL::Relocate(Reg64::RBP, Reg64::RBX, Reg64::R13)),
				jumpOffset);
			patch.ready();

			auto& trampoline = SKSE::GetTrampoline();
			trampoline.write_branch<5>(addr, trampoline.allocate(patch));
		}

		if (Config::SuperDenseGrass) {
			auto addr = RELOCATION_ID(15202, 15370).address() + REL::Relocate(0xAE5 - 0x890, 0x258);
			int mode = std::max(0, std::min(12, Config::SuperDenseMode));
			if (mode != 7) {
				Memory::Internal::write<uint8_t>(addr + 2, static_cast<unsigned char>(mode), true);
			}
		}

		if (Config::ExtendGrassCount) {
			auto addr = RELOCATION_ID(15220, 15385).address() + REL::Relocate(0x433 - 0x3C0, 0x68);
			REL::safe_write(addr, REL::NOP6, 6);

			addr = RELOCATION_ID(15214, 15383).address() + REL::Relocate(0x960 - 0x830, 0x129);
			struct GrassShapeCapacityPatch : CodeGenerator
			{
				GrassShapeCapacityPatch(uintptr_t a_target)
				{
					Label continueLookup;
					Label retnLabel;
					Label reuseShape;

					cmp(ptr[rax], rbx);   // entry->key == typeKey
					jne(continueLookup);  // if (entry->key != typeKey) goto continueLookup;

					mov(rdx, ptr[rax + 8]);  // value = entry->value;
					cmp(rdx, 0);
					je(continueLookup);  // if (!value) goto continueLookup;

					mov(rdx, ptr[rdx]);  // shape = value->typeShape;
					cmp(rdx, 0);
					je(continueLookup);  // if (!shape) goto continueLookup;

					mov(ecx, ptr[rdx + 0x190]);   // bytes = shape->instanceCount;
					imul(ecx, ptr[rdx + 0x194]);  // bytes *= shape->instanceSize;
					shl(ecx, 1);                  // bytes *= 2;
					cmp(ecx, 0x20000);
					jge(continueLookup);  // if (int32_t(bytes) >= 0x20000) goto continueLookup;

					jmp(ptr[rip + reuseShape]);  // Use the existing shape.

					L(continueLookup);
					jmp(ptr[rip + retnLabel]);  // Continue the hash lookup when the shape is missing or full.

					L(retnLabel);
					dq(a_target + 0x5);

					L(reuseShape);
					dq(a_target + 0xF);
				}
			};

			GrassShapeCapacityPatch patch(addr);

			patch.ready();

			auto& trampoline = SKSE::GetTrampoline();
			trampoline.write_branch<5>(addr, trampoline.allocate(patch));
		}

		if (Config::UseGrassCache) {
			GidFileCache::FixFileFormat(Config::OnlyLoadFromCache);
		}

		if (Config::ExtendGrassDistance) {
			DistantGrass::ReplaceGrassGrid(Config::OnlyLoadFromCache);
			DistantGrass::InstallHooks(Config::OnlyLoadFromCache);
		}

		if (Config::EnsureMaxGrassTypesPerTextureSetting > 0) {
			auto addr = RELOCATION_ID(18342, 18758).address() + REL::Relocate(0xD63 - 0xCF0, 0x68);

			uint32_t max = std::max(Config::EnsureMaxGrassTypesPerTextureSetting, Memory::Internal::read<int>(RELOCATION_ID(501615, 360443).address() + 8));

			struct MaxGrassTypesPatch : CodeGenerator
			{
				MaxGrassTypesPatch(uint32_t max, std::uintptr_t a_target)
				{
					Label retnLabel;

					// maxGrassTypes = max;
					if (REL::Module::IsAE()) {
						mov(edi, max);
					} else {
						mov(r12d, max);
					}

					jmp(ptr[rip + retnLabel]);

					L(retnLabel);
					dq(a_target + REL::Relocate(0x7, 0x6));
				}
			};

			MaxGrassTypesPatch patch(max, addr);

			patch.ready();

			auto& trampoline = SKSE::GetTrampoline();
			REL::safe_fill(addr + 5, REL::NOP, REL::Relocate(2, 1));
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
			if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99) {
				addr = RELOCATION_ID(15212, 15381).address() + 0xAE8;
			}

			struct GrassScalePatch : CodeGenerator
			{
				GrassScalePatch(uintptr_t a_target, double a_scale,
					Xmm a_randomNormalization,
					Xmm a_one,
					Reg a_baseReg)
				{
					Label scaleLabel;
					Label retnLabel;

					mulss(xmm0, a_randomNormalization);  // delta = randomValue * 4.656613e-10f;
					subss(xmm0, a_one);                  // delta -= 1.0f;

					mulss(xmm0, ptr[a_baseReg + 0x10]);  // delta *= scaleVariation;
					addss(xmm0, a_one);                  // finalScale = delta + 1.0f;

					cvtss2sd(xmm0, xmm0);                // scaled = double(finalScale);
					mulsd(xmm0, ptr[rip + scaleLabel]);  // scaled *= a_scale;
					cvtsd2ss(xmm0, xmm0);                // finalScale = float(scaled);

					subss(xmm0, a_one);  // xmm0 = finalScale - 1.0f;

					jmp(ptr[rip + retnLabel]);  // The continuation consumes a float delta, not the final scale.

					L(scaleLabel);
					dq(std::bit_cast<std::uint64_t>(a_scale));

					L(retnLabel);
					dq(a_target);
				}
			};

			GrassScalePatch patch(addr + REL::Relocate(0xf, 0xf, 0x10), Config::GlobalGrassScale, Xmm(REL::Relocate(14, 14, 12)), Xmm(REL::Relocate(11, 10, 11)), Reg64(REL::Relocate(Reg::RSI, Reg::RBX, Reg::R13)));

			patch.ready();

			auto& trampoline = SKSE::GetTrampoline();
			trampoline.write_branch<5>(addr, trampoline.allocate(patch));
		}
	}
}

namespace
{
	using namespace GrassControl;

	namespace DistantHooks
	{
		RE::TESObjectCELL* CellFromExtraList(const RE::ExtraDataList* a_extraList)
		{
			return reinterpret_cast<RE::TESObjectCELL*>(reinterpret_cast<std::uintptr_t>(a_extraList) - offsetof(RE::TESObjectCELL, extraList));
		}

		struct CellDestructor
		{
			static void* thunk(RE::ExtraDataList* a_extraList, std::uint32_t)
			{
				DistantGrass::Handle_RemoveGrassFromCell_Call(CellFromExtraList(a_extraList));
				REL::Relocation<void (*)(RE::ExtraDataList*)> clear{ RELOCATION_ID(11931, 12070) };
				clear(a_extraList);

				// Returning null prevents the caller from removing grass a second time.
				return nullptr;
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		void RemoveGrassAfterLock(RE::BSSpinLock* a_lock)
		{
			auto* cell = reinterpret_cast<RE::TESObjectCELL*>(reinterpret_cast<std::uintptr_t>(a_lock) - offsetof(RE::TESObjectCELL, grassCreateLock));
			DistantGrass::RemoveGrassHook(cell, reinterpret_cast<std::uintptr_t>(RE::BGSGrassManager::GetSingleton()));
		}

		struct RemoveBeforeReadSE
		{
			static void thunk(RE::BSSpinLock* a_lock, std::uint32_t a_pauseAttempts)
			{
				func(a_lock, a_pauseAttempts);
				RemoveGrassAfterLock(a_lock);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct RemoveBeforeReadAE
		{
			static void thunk(RE::BSSpinLockGuard* a_guard, RE::BSSpinLock* a_lock)
			{
				func(a_guard, a_lock);
				RemoveGrassAfterLock(a_lock);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AllowMissingLandMesh
		{
			static std::uintptr_t thunk() { return 1; }

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ClearUnloadTask
		{
			static void thunk(const RE::ExtraDataList& a_extraList, void** a_task)
			{
				auto* cell = CellFromExtraList(std::addressof(a_extraList));
				const bool cleared = DistantGrass::ClearCellAddGrassTask(cell);
				*a_task = nullptr;
				DistantGrass::CellUnloadHook(cleared, cell);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct WriteProgress
		{
			static void thunk(RE::BGSGrassManager* GrassMgr, RE::TESObjectCELL* cell, uintptr_t unk)
			{
				DistantGrass::OnGrassCreated(GrassMgr, cell, unk);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CellSelection
		{
			static RE::BSMultiStreamInstanceTriShape* thunk(RE::BGSGrassManager* a_GrassManager, RE::GrassParam* a_grassParam, std::uint32_t a_CellXDivided, std::uint32_t a_CellYDivided, std::uint64_t* a_typeKey, RE::BSFixedString* a_name)
			{
				return func(a_GrassManager, a_grassParam, a_CellXDivided * 12, a_CellYDivided * 12, a_typeKey, a_name);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct UpdateGrassGridAddRemove
		{
			static void thunk(RE::TES* a_TES)
			{
				DistantGrass::UpdateGrassGridNow(a_TES, 0, 0, 1);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct UpdateGrassGridAddRemove2
		{
			static void thunk(RE::TES* a_TES)
			{
				DistantGrass::UpdateGrassGridNow(a_TES, 0, 0, -1);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CallUpdateGrassGridEnsureLoad
		{
			static void thunk(RE::TES* a_TES, std::int32_t arg_2)
			{
				func(a_TES, arg_2);
				DistantGrass::UpdateGrassGridEnsureLoad(a_TES->GetRuntimeData2().worldSpace, a_TES->currentGridX, a_TES->currentGridY);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		void Install(bool loadOnly)
		{
			if (Config::ExtendGrassDistance) {
				stl::write_thunk_jump<WriteProgress>(RELOCATION_ID(13138, 13278).address() + REL::Relocate(0xF, 0xF));

				stl::write_thunk_call<CellSelection>(RELOCATION_ID(15206, 15374).address() + REL::Relocate(0x645C - 0x6200, 0x645C - 0x6200));
				const auto loadTypeOffset = REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99 ? 0x590 : REL::Relocate(0x62B, 0x597);
				stl::write_thunk_call<CellSelection>(RELOCATION_ID(15205, 15373).address() + loadTypeOffset);
				if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99) {
					stl::write_thunk_call<CellSelection>(RELOCATION_ID(15204, 15372).address() + 0x305);
				} else {
					stl::write_thunk_call<CellSelection>(RELOCATION_ID(15204, 15372).address() + REL::Relocate(0x2F5, 0x2F5));
				}

				stl::write_thunk_jump<UpdateGrassGridAddRemove>(RELOCATION_ID(13190, 13335).address());
				stl::write_thunk_jump<UpdateGrassGridAddRemove2>(RELOCATION_ID(13191, 13336).address());

				if (!loadOnly) {
					if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99) {
						stl::write_thunk_call<CallUpdateGrassGridEnsureLoad>(RELOCATION_ID(13148, 13288).address() + 0x85A);
					} else {
						stl::write_thunk_call<CallUpdateGrassGridEnsureLoad>(RELOCATION_ID(13148, 13288).address() + REL::Relocate(0x29AF - 0x2220, 0x9A9));
					}
				}
			}
		}
	}
}

namespace GrassControl::DistantGrass
{
	static void ThrowOurMethodException(const uint8_t ourMethod)
	{
		stl::report_and_fail("GrassControl.dll: unexpected ourMethod: " + std::to_string(ourMethod));
	}

	void InstallHooks(bool loadOnly)
	{
		DistantHooks::Install(loadOnly);
	}

	void ReplaceGrassGrid(const bool loadOnly)
	{
		Initialize(loadOnly);

		if (!loadOnly) {
			auto addr = RELOCATION_ID(38141, 39098).address();
			Memory::Internal::write<uint8_t>(addr, 0xC3, true);

			try {
				if (addr = (RELOCATION_ID(13240, 13391).address() + (0x55E - 0x480)); REL::make_pattern<"E8">().match(addr)) {
					REL::safe_write(addr, REL::NOP5, 5);
				}
			} catch (...) {
			}
		}

		if (!loadOnly) {
			int radius = getChosenGrassGridRadius();
			auto setting = RE::INISettingCollection::GetSingleton()->GetSetting("iGrassCellRadius:Grass");
			setting->data.i = radius;
		}

		auto addr = RELOCATION_ID(13148, 13288).address() + REL::Relocate(0xA06 - 0x220, 0xA1D);
		auto retnOffset = REL::Relocate(0x159, 0x17D);
		if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99) {
			addr = RELOCATION_ID(13148, 13288).address() + 0x8B3;
			retnOffset = 0x177;
		}

		struct UpdateGrassGridPatch : CodeGenerator
		{
			UpdateGrassGridPatch(std::uintptr_t a_func, uintptr_t a_target, Reg a_Y, Reg a_X)
			{
				Label funcLabel;
				Label retnLabel;

				push(rdx);  // Preserve the caller's live registers across the helper.
				push(rcx);

				mov(r9d, 0);    // addType = 0;
				mov(r8d, a_Y);  // movedY = int32_t(a_Y);

				if (REL::Module::IsAE() && REL::Module::get().version() < SKSE::RUNTIME_SSE_1_7_99) {
					mov(edx, ptr[rsp + 0x10 + 0x44]);  // movedX = int32At(entrySP + 0x44);
				} else {
					mov(edx, a_X);  // movedX = int32_t(a_X);
				}

				mov(rcx, rbx);  // tes

				sub(rsp, 0x20);
				call(ptr[rip + funcLabel]);  // UpdateGrassGridNow(tes, movedX, movedY, 0);
				add(rsp, 0x20);

				pop(rcx);
				pop(rdx);

				jmp(ptr[rip + retnLabel]);  // The helper handles grid addition and removal.

				L(funcLabel);
				dq(a_func);

				L(retnLabel);
				dq(a_target);
			}
		};

		const bool updatedRuntime = REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99;
		UpdateGrassGridPatch patch(reinterpret_cast<uintptr_t>(UpdateGrassGridNow), addr + retnOffset,
			Reg32(updatedRuntime ? Reg::EBP : REL::Relocate(Reg::R14D, Reg::EDI)),
			Reg32(updatedRuntime ? Reg::R15D : REL::Relocate(Reg::EBP, Reg::R14D)));
		patch.ready();

		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_branch<6>(addr, trampoline.allocate(patch));

		if (!loadOnly) {
			stl::write_thunk_call<DistantHooks::CellDestructor>(RELOCATION_ID(18446, 18877).address() + 0x6E);
		}

		if (!loadOnly) {
			addr = RELOCATION_ID(13623, 13721).address() + REL::Relocate(0xC0F8 - 0xBF90, 0x1E8);
			REL::safe_write(addr, REL::NOP5, 5);
		}

		if (!loadOnly) {
			addr = RELOCATION_ID(15204, 15372).address() + REL::Relocate(0x7F, 0x7D);
			if (REL::Module::IsAE()) {
				stl::write_thunk_call<DistantHooks::RemoveBeforeReadAE>(addr);
			} else {
				stl::write_thunk_call<DistantHooks::RemoveBeforeReadSE>(addr);
			}
		}

		addr = RELOCATION_ID(15214, 15383).address() + REL::Relocate(0x78B7 - 0x7830, 0x7E);

		struct PackGrassCoordinatesPatch : CodeGenerator
		{
			explicit PackGrassCoordinatesPatch(uintptr_t a_target, Reg a_X, uintptr_t rsp_offset, uintptr_t rsp_stackOffset)
			{
				Label retnLabel;

				mov(ptr[rsp + rsp_offset + 4], a_X.cvt16());  // packedCoordinates.x = int16_t(a_X);
				mov(ptr[rsp + rsp_offset + 6], bx);           // packedCoordinates.y = int16_t(ebx);

				movsx(eax, a_X.cvt16());  // x = int16_t(a_X);
				mov(a_X, ecx);            // savedEcx = ecx;

				cdq();
				mov(ecx, 12);
				idiv(ecx);  // gridX = x / 12; signed division truncates toward zero.

				mov(ecx, a_X);          // ecx = savedEcx;
				mov(a_X.cvt32(), eax);  // a_X = gridX;

				movsx(eax, bx);  // y = int16_t(ebx);
				mov(ebx, ecx);   // savedEcx = ecx;

				cdq();
				mov(ecx, 12);
				idiv(ecx);      // gridY = y / 12;
				mov(ecx, ebx);  // ecx = savedEcx;

				mov(ebx, eax);  // ebx = gridY;

				mov(ptr[rsp + rsp_stackOffset + 0x58], a_X.cvt32());  // stackArgumentX = gridX;
				mov(ptr[rsp + rsp_stackOffset + 0x60], ebx);          // stackArgumentY = gridY;

				jmp(ptr[rip + retnLabel]);

				L(retnLabel);
				dq(a_target + 0xB);
			}
		};

		PackGrassCoordinatesPatch coordinatesPatch(addr, Reg32(REL::Relocate(Reg::R15, Reg::R14)), REL::Relocate(0x48, 0x50), REL::Relocate(0x258, 0x288));

		coordinatesPatch.ready();

		REL::safe_fill(addr, REL::NOP, 0xB);
		trampoline.write_branch<5>(addr, trampoline.allocate(coordinatesPatch));

		if (!loadOnly) {
			addr = RELOCATION_ID(13233, 13384).address() + (0xB2 - 0x60);
			const int uGridWidth = GetUGrids();

			const int retainedGrassRadius = getChosenGrassGridRadius() + 1;
			const int grassGridWidth = retainedGrassRadius * 2 + 1;
			const int exteriorBufferWidth = std::max(uGridWidth, grassGridWidth);

			struct ExteriorBufferPatch : CodeGenerator
			{
				explicit ExteriorBufferPatch(uintptr_t a_target, int a_bufferWidth)
				{
					Label retnLabel;

					mov(rax, a_bufferWidth);  // exteriorBufferWidth includes the grass retention ring.

					jmp(ptr[rip + retnLabel]);

					L(retnLabel);
					dq(a_target + 0x6);
				}
			};

			ExteriorBufferPatch bufferWidthPatch(addr, exteriorBufferWidth);

			bufferWidthPatch.ready();
			trampoline.write_branch<6>(addr, trampoline.allocate(bufferWidthPatch));
		}

		// Allow grass distance to extend beyond uGrids * 4096 units (20480).
		addr = RELOCATION_ID(15202, 15370).address() + REL::Relocate(0xB06 - 0x890, 0x279);
		Memory::Internal::write<uint8_t>(addr + 2, 16, true);

		addr = RELOCATION_ID(528751, 15379).address() + REL::Relocate(0xFE - 0xE0, 0x1E);
		Memory::Internal::write<uint8_t>(addr + 2, 16, true);

		// The caller only tests this geometry result for null.
		if (!loadOnly) {
			stl::write_thunk_call<DistantHooks::AllowMissingLandMesh>(RELOCATION_ID(15204, 15372).address() + 0x1C2);
		}

		if (!loadOnly) {
			stl::write_thunk_call<DistantHooks::ClearUnloadTask>(RELOCATION_ID(18655, 19130).address() + REL::Relocate(0xC8, 0xCA));
		}

		if (!loadOnly) {
			addr = RELOCATION_ID(18137, 18527).address() + REL::Relocate(0x17, 0x25);

			struct CellLoadDispatchPatch : CodeGenerator
			{
				CellLoadDispatchPatch(std::uintptr_t a_func, std::uintptr_t b_func, uintptr_t a_rbxWorldSpaceOffset, uintptr_t a_targetReturn, uintptr_t a_targetSkip)
				{
					Label loadCustomCell;
					Label invalidMethod;
					Label retnLabel;
					Label completeTask;
					Label resolveNormalCell;

					Label checkNormalLoad;
					Label normalLoad;
					Label resolveCell;

					mov(al, ptr[rbx + a_rbxWorldSpaceOffset + 0x1E]);  // ourMethod = task->ourMethod;
					cmp(al, 1);
					jne(checkNormalLoad);  // if (ourMethod != 1) goto checkNormalLoad;

					mov(rcx, ptr[rbx + a_rbxWorldSpaceOffset]);         // worldSpace = task->worldSpace;
					mov(edx, ptr[rbx + a_rbxWorldSpaceOffset + 0x10]);  // x = task->x;
					mov(r8d, ptr[rbx + a_rbxWorldSpaceOffset + 0x14]);  // y = task->y;

					sub(rsp, 0x20);
					call(ptr[rip + loadCustomCell]);  // CellLoadNow_Our(worldSpace, x, y);
					add(rsp, 0x20);

					jmp(ptr[rip + completeTask]);  // Finish the task without the normal load or its cell call.

					L(checkNormalLoad);
					cmp(al, 0);
					je(normalLoad);  // if (ourMethod == 0) goto normalLoad;

					mov(cl, al);

					sub(rsp, 0x20);
					call(ptr[rip + invalidMethod]);  // ThrowOurMethodException(ourMethod);
					add(rsp, 0x20);

					jmp(resolveCell);

					L(normalLoad);
					jmp(resolveCell);

					L(resolveCell);
					if (REL::Module::IsAE()) {
						call(ptr[rip + resolveNormalCell]);
					} else {
						mov(rcx, ptr[rbx + a_rbxWorldSpaceOffset - 0x8]);  // loadTask
						lea(rdx, ptr[rbx + a_rbxWorldSpaceOffset]);        // &task->worldSpace
					}

					jmp(ptr[rip + retnLabel]);

					L(loadCustomCell);
					dq(a_func);

					L(invalidMethod);
					dq(b_func);

					L(retnLabel);
					dq(a_targetReturn);

					L(resolveNormalCell);
					dq(RELOCATION_ID(18637, 19111).address());

					L(completeTask);
					dq(a_targetSkip);
				}
			};

			// Custom loads have no cell pointer for the call at +0x166; resume at the task completion store.
			CellLoadDispatchPatch cellLoadDispatchPatch(reinterpret_cast<uintptr_t>(CellLoadNow_Our), reinterpret_cast<uintptr_t>(ThrowOurMethodException), REL::Relocate(0x20, 0x20, 0x28), addr + REL::Relocate(0x8, 0x5), addr + REL::Relocate(0x8 + (0xA9 - 0x8F), 0x146));

			cellLoadDispatchPatch.ready();

			if (!REL::Module::IsAE()) {
				REL::safe_write(addr + 5, REL::NOP3, 3);
			}

			trampoline.write_branch<5>(addr, trampoline.allocate(cellLoadDispatchPatch));

			addr = RELOCATION_ID(18150, 18541).address() + REL::Relocate(0xB094 - 0xAF20, 0x1CA, 0x177);
			if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99) {
				addr = RELOCATION_ID(18150, 18541).address() + 0x1E4;
			}

			struct CellLoadSelectionPatch : CodeGenerator
			{
				CellLoadSelectionPatch(uintptr_t a_func, uintptr_t a_target, uintptr_t rbx_offset)
				{
					Label funcLabel;
					Label retnLabel;

					if (REL::Module::IsAE())
						movzx(eax, ptr[rsp + 0xA0]);  // originalFlag = byteAt(entrySP + 0xA0);
					else
						mov(esi, ptr[rsp + 0x30]);  // originalFlag is already in al; esi is live in the continuation.

					mov(ptr[rbx + rbx_offset + 0xd], al);  // task->originalFlag = originalFlag;

					mov(ecx, ptr[rbx + rbx_offset]);        // x = task->x;
					mov(edx, ptr[rbx + rbx_offset + 0x4]);  // y = task->y;

					sub(rsp, 0x20);
					call(ptr[rip + funcLabel]);  // ourMethod = CellLoadHook(x, y);
					add(rsp, 0x20);

					mov(byte[rbx + rbx_offset + 0xe], al);  // task->ourMethod = ourMethod;

					jmp(ptr[rip + retnLabel]);

					L(funcLabel);
					dq(a_func);

					L(retnLabel);
					dq(a_target);
				}
			};

			CellLoadSelectionPatch cellLoadSelectionPatch(reinterpret_cast<uintptr_t>(CellLoadHook), addr + REL::Relocate(0x7, 0xB), REL::Relocate(0x30, 0x30, 0x38));

			cellLoadSelectionPatch.ready();
			if (!REL::Module::IsAE()) {
				REL::safe_write(addr + 5, REL::NOP2, 2);
			} else {
				REL::safe_write(addr + 5, REL::NOP6, 6);
			}

			trampoline.write_branch<5>(addr, trampoline.allocate(cellLoadSelectionPatch));

			addr = RELOCATION_ID(18149, 18540).address() + REL::Relocate(0xE1B - 0xCC0, 0x167, 0x15E);

			struct CellLoadFlagsPatch : CodeGenerator
			{
				explicit CellLoadFlagsPatch(uintptr_t a_target, uintptr_t rbx_offset, Reg a_source)
				{
					Label retnLabel;

					mov(byte[rbx + rbx_offset + 0x2], 0);  // task->ourMethod = 0;

					mov(byte[rbx + rbx_offset], 1);  // loadFlags[0] = 1;

					mov(ptr[rbx + rbx_offset + 0x1], a_source);  // loadFlags[1] = uint8_t(a_source);

					jmp(ptr[rip + retnLabel]);

					L(retnLabel);
					dq(a_target + 0x8);
				}
			};

			CellLoadFlagsPatch cellLoadFlagsPatch(addr, REL::Relocate(0x3c, 0x3c, 0x44), Reg8(REL::Relocate(Reg::R15B, Reg::BPL)));

			cellLoadFlagsPatch.ready();

			REL::safe_write(addr + 5, REL::NOP3, 3);
			trampoline.write_branch<5>(addr, trampoline.allocate(cellLoadFlagsPatch));

			addr = RELOCATION_ID(13148, 13288).address() + REL::Relocate(0x2630 - 0x2220, 0x4D0);
			if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99) {
				addr = RELOCATION_ID(13148, 13288).address() + 0x4C0;
			}

			struct QueueGrassGridPatch : CodeGenerator
			{
				QueueGrassGridPatch(std::uintptr_t a_func, uintptr_t a_target, Reg a_movedY, uintptr_t a_retn_offset)
				{
					Label funcLabel;
					Label retnLabel;
					Label jump;
					Label jumpAd;
					Label restore;

					push(r9);  // Preserve caller state; entrySP is rsp before these pushes.
					push(rdx);
					push(rax);
					push(rsi);

					if (REL::Module::IsAE()) {
						movsx(r8d, r12w);  // movedX = int16_t(r12w);
					} else {
						movsx(r8d, word[rsp + 0x20 + 0x90]);  // movedX = int16At(entrySP + 0x90);
					}

					movsx(r9d, a_movedY);  // movedY = int16_t(a_movedY);
					mov(esi, r8d);         // Preserve movedX across the helper.

					const auto worldSpaceOffset = REL::Module::IsAE() && REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_6_1130 ? 0x148 : 0x140;
					mov(rax, ptr[rbx + worldSpaceOffset]);  // worldSpace = tes->worldSpace;
					test(rax, rax);
					je(restore);  // if (!worldSpace) goto restore;

					mov(ecx, ptr[rbx + 0xB8]);  // previousX = tes->previousX;
					mov(edx, ptr[rbx + 0xBC]);  // previousY = tes->previousY;

					sub(rsp, 0x20);
					call(ptr[rip + funcLabel]);  // UpdateGrassGridQueue(previousX, previousY, movedX, movedY);
					add(rsp, 0x20);

					L(restore);
					pop(rsi);
					pop(rax);
					pop(rdx);
					pop(r9);

					if (REL::Module::IsAE()) {
						test(r12w, r12w);
						jz(jump);  // if (movedX == 0) goto jump;
					} else
						cmp(word[rsp + 0x90], 0);  // Preserve comparison flags for the continuation.

					jmp(ptr[rip + retnLabel]);

					L(funcLabel);
					dq(a_func);

					L(jumpAd);
					dq(a_target + 0x2C);  // Zero movement in X follows a separate engine branch.

					L(jump);
					jmp(ptr[rip + jumpAd]);

					L(retnLabel);
					dq(a_target + a_retn_offset);
				}
			};

			QueueGrassGridPatch queueGrassGridPatch(reinterpret_cast<uintptr_t>(UpdateGrassGridQueue), addr, Reg16(REL::Relocate(Reg16::R13W, Reg16::R15W)), REL::Relocate(0x9, 0x6));

			queueGrassGridPatch.ready();

			if (REL::Module::IsAE()) {
				trampoline.write_branch<6>(addr, trampoline.allocate(queueGrassGridPatch));
			} else {
				REL::safe_write(addr + 5, REL::NOP4, 4);
				trampoline.write_branch<5>(addr, trampoline.allocate(queueGrassGridPatch));
			}
		}
	}
}

namespace
{
	using namespace GrassControl;

	struct SaveGrassGroup
	{
		static std::uint32_t thunk(RE::BSMultiStreamInstanceTriShape* a_shape, std::uint32_t a_groupIndex,
			std::uintptr_t a_stream, std::uint32_t a_numShortsPerInstance, bool a_swapEndian)
		{
			const auto* group = a_shape->GetMultiStreamTrishapeRuntimeData().instanceGroups[a_groupIndex];
			RE::BSMultiStreamInstanceTriShape::GroupHeader header{
				group->center, group->size, group->triCount, group->instanceCount, a_numShortsPerInstance
			};

			if (a_swapEndian) {
				auto words = std::bit_cast<std::array<std::uint32_t, 9>>(header);
				for (auto& word : words) {
					word = std::byteswap(word);
				}

				header = std::bit_cast<decltype(header)>(words);
			}

			REL::Relocation<std::uint32_t (*)(std::uintptr_t, const void*, std::uint32_t)> write{ RELOCATION_ID(74621, 76352) };
			write(a_stream + 0x8, std::addressof(header), static_cast<std::uint32_t>(sizeof(header)));

			return write(a_stream + 0x8, group->vertexBuffer->m_data, static_cast<std::uint32_t>(group->vertexBuffer->byteWidth));
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};
}

namespace GrassControl
{
	void GidFileCache::SetHelperInstalled(bool installed)
	{
		cacheHelperInstalled = installed;
	}

	void GidFileCache::FixFileFormat(const bool only_load)
	{
		if (exists(std::filesystem::path(Util::GetProgressFilePath()))) {
			try {
				auto dir = Util::Files::GamePath(L"Data/Grass");
				if (!exists(dir)) {
					create_directories(dir);
				}
			} catch (...) {
				stl::report_and_fail("Error occured while trying to create or access data/grass folder");
			}
		}

		stl::write_thunk_jump<SaveGrassGroup>(RELOCATION_ID(74601, 76329).address());

		auto setting = RE::INISettingCollection::GetSingleton()->GetSetting("bAllowLoadGrass:Grass");
		setting->data.b = true;

		setting = RE::INISettingCollection::GetSingleton()->GetSetting("bAllowCreateGrass:Grass");
		if (!only_load || Config::Updating) {
			setting->data.b = true;
		} else {
			setting->data.b = false;
		}

		if (Config::Updating) {
			logger::info("Cache Updating Mode Enabled.");
			GidFileCache::EnableFileGeneration();
		}

		std::uintptr_t addr;

		if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99) {
			addr = RELOCATION_ID(15204, 15372).address() + 0x8A6;
		} else {
			addr = RELOCATION_ID(15204, 15372).address() + 0x896;
		}

		struct SkipConsoleGrassPatch : CodeGenerator
		{
			SkipConsoleGrassPatch(const std::uintptr_t a_target)
			{
				Label retnLabel;

				mov(r15, 0);

				jmp(ptr[rip + retnLabel]);  // Skip GrassConsole serialization; the PC cache stream is already closed.

				L(retnLabel);
				dq(a_target + 0x7 + 0x2D5);
			}
		};

		SkipConsoleGrassPatch skipConsoleGrassPatch(addr);

		skipConsoleGrassPatch.ready();

		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_branch<5>(addr, trampoline.allocate(skipConsoleGrassPatch));
		REL::safe_write(addr + 5, REL::NOP2, 2);
	}
}

namespace
{
	using namespace GrassControl;

	namespace GenerationHooks
	{
		struct GrassGridRadius
		{
			static int thunk() { return Generation::GetGrassGridRadius(); }

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct PathFileNameSave
		{
			static int thunk(char* buffer, std::size_t capacity, const char*, const char* world, int x, int y)
			{
				// Deletion and creation must use the same name to prevent appending to an old cache.
				return func(buffer, capacity, "Grass\\%sx%04dy%04d.cgid", world, x, y);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct PathFileNameLoad
		{
			static int thunk(char* buffer, std::size_t capacity, const char*, const char* world, int x, int y)
			{
				// Avoid loading vanilla .gid files from archives.
				return func(buffer, capacity, "Grass\\%sx%04dy%04d.cgid", world, x, y);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct GrassCountIncrement
		{
			static void thunk(RE::TESObjectCELL* cell)
			{
				Generation::OnGrassQueued();
				func(cell);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct WriteProgress
		{
			static void thunk(uintptr_t GrassMgr, RE::TESObjectCELL* cell, uintptr_t unk)
			{
				func(GrassMgr, cell, unk);
				Generation::OnGrassCreated(cell);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		void Install()
		{
			if (Config::UseGrassCache) {
				if (!cacheHelperInstalled) {
					stl::write_thunk_call<PathFileNameLoad>(RELOCATION_ID(15206, 15374).address() + 0xD4);
				}

				if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99) {
					stl::write_thunk_call<PathFileNameSave>(RELOCATION_ID(15204, 15372).address() + 0x666);
				} else {
					stl::write_thunk_call<PathFileNameSave>(RELOCATION_ID(15204, 15372).address() + REL::Relocate(0x65A, 0x656));
				}

				if (exists(std::filesystem::path(Util::GetProgressFilePath()))) {
					stl::write_thunk_call<GrassCountIncrement>(RELOCATION_ID(13190, 13335).address() + REL::Relocate(0xD40 - 0xC70, 0xD0));
					stl::write_thunk_jump<WriteProgress>(RELOCATION_ID(13138, 13278).address() + REL::Relocate(0xF, 0xF));
				}
			}
		}
	}
}

namespace GrassControl
{
	void Generation::InstallHooks()
	{
		GenerationHooks::Install();
	}

	void Generation::ApplyPatches()
	{
		unsigned long long addr;

		const auto createOffset = REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99 ? 0x9D9 : REL::Relocate(0x2B25 - 0x2220, 0xB29);
		if (addr = (RELOCATION_ID(13148, 13288).address() + createOffset); REL::make_pattern<"E8">().match(addr)) {
			REL::safe_write(addr, REL::NOP5, 5);
		}

		addr = RELOCATION_ID(13190, 13335).address() + REL::Relocate(0x106, 0x106);

		struct GrassDispatchPatch : CodeGenerator
		{
			GrassDispatchPatch(uintptr_t a_onGrassDispatch, const uintptr_t a_target)
			{
				Label retnLabel;
				Label onGrassDispatch;

				mov(rdi, ptr[rsp + 0x50]);  // rdi = savedRdi;

				sub(rsp, 0x20);
				call(ptr[rip + onGrassDispatch]);  // Generation::OnGrassDispatch();
				add(rsp, 0x20);

				jmp(ptr[rip + retnLabel]);

				L(retnLabel);
				dq(a_target + 0x5);

				L(onGrassDispatch);
				dq(a_onGrassDispatch);
			}
		};

		GrassDispatchPatch patch(reinterpret_cast<uintptr_t>(&Generation::OnGrassDispatch), addr);

		patch.ready();

		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_branch<5>(addr, trampoline.allocate(patch));

		stl::write_thunk_call<GenerationHooks::GrassGridRadius, 6>(RELOCATION_ID(15202, 15370).address() + REL::Relocate(0x17E, 0x17D));

		GidFileCache::EnableFileGeneration();

		Generation::Enable();

		// Cache generation must continue while the game is unfocused.
		addr = RELOCATION_ID(35565, 36564).address() + REL::Relocate(0x216 - 0x1E0, 0x51, 0x4b);
		Memory::Internal::write<uint8_t>(addr, 0xEB, true);
	}
}
