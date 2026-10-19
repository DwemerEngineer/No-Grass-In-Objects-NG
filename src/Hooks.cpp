#include "GrassControl/AddCellGrassTask.h"
#include "GrassControl/Config.h"
#include "GrassControl/DistantGrass.h"
#include "GrassControl/Engine.h"
#include "GrassControl/GidFileCache.h"
#include "GrassControl/Globals.h"
#include "GrassControl/GrassCellCreation.h"
#include "GrassControl/GrassPatchCreation.h"
#include "GrassControl/Main.h"
#include "GrassControl/PhysicsRaycast.h"
#include "GrassControl/Profiler.h"
#include "GrassControl/Util.h"

namespace GrassControl
{
	using namespace Xbyak;

	static bool UpdatedRuntime()
	{
		return REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99;
	}

	template <class T>
	static void OverrideSetting(const char* name, T value)
	{
		auto* setting = globals::FindSetting(name);
		if (!setting) {
			logger::error("Failed to find {}", name);
			return;
		}

		if constexpr (std::is_same_v<T, float>)
			setting->data.f = value;
		else if constexpr (std::is_same_v<T, bool>)
			setting->data.b = value;
		else
			setting->data.i = value;
	}

	namespace Plugin
	{
		struct MainUpdate
		{
			static void thunk()
			{
				func();
				Plugin::Update();
			}

			inline static REL::Relocation<decltype(thunk)> func;
		};

		struct ConsoleOpen
		{
			static void thunk(std::int64_t a1, char a2)
			{
				func(a1, a2);
				Profiler::Report();
			}

			inline static REL::Relocation<decltype(thunk)> func;
		};

		struct AddCellGrassNow
		{
			static void thunk(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort)
			{
				GrassPatchCreation::AddCell(manager, cell, abort, func.get());
			}

			inline static REL::Relocation<decltype(thunk)> func;
		};

		struct AddCellGrassTaskRun
		{
			static void thunk(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort)
			{
				if (Config::ExtendGrassDistance) {
					DistantGrass::OnGrassCreated(manager, cell, abort);
					return;
				}

				const auto started = std::chrono::steady_clock::now();
				GrassPatchCreation::AddCell(manager, cell, abort, func.get());
				if (Config::GeneratingCache)
					Generation::OnGrassCreated(cell, abort, std::chrono::steady_clock::now() - started);
			}

			inline static REL::Relocation<decltype(thunk)> func;
		};

		void InstallHooks()
		{
			const bool marketplace = REL::Relocate(false, REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_6_1130);

			if (UpdatedRuntime())
				stl::write_thunk_call<MainUpdate>(RELOCATION_ID(35565, 36564).address() + 0xC3D);
			else
				stl::write_thunk_call<MainUpdate>(RELOCATION_ID(35565, 36564).address() + REL::Relocate(0x748, (marketplace ? 0xC2b : 0xC26), 0x7EE));

			if (Config::ProfilerReport) {
				if (marketplace)
					stl::write_thunk_call<ConsoleOpen>(REL::ID(442669).address() + (UpdatedRuntime() ? 0x16B : 0x15D));
				else
					stl::write_thunk_call<ConsoleOpen>(RELOCATION_ID(50155, 51082).address() + REL::Relocate(0x142, 0x15C, 0x14F));
			}

			stl::write_thunk_call<AddCellGrassNow>(RELOCATION_ID(13148, 13288).address() + (UpdatedRuntime() ? 0x9D9 : REL::Relocate(0x905, 0xB29)));
			stl::write_thunk_jump<AddCellGrassTaskRun>(RELOCATION_ID(13138, 13278).address() + 0xF);
		}

		void Initialize()
		{
			if (Config::GeneratingCache) {
				if (REL::Module::IsVR())
					OverrideSetting("bLoadVRPlayroom:VR", false);

				// Matching uGridsToLoad disables large-reference loading beyond the grid being cached.
				OverrideSetting("uLargeRefLODGridSize:General", globals::GridsToLoad());

				Generation::ApplyPatches();

				Config::OnlyLoadFromCache = false;
				Config::ExtendGrassDistance = false;
				Config::DynDOLODGrassMode = 0;
			} else {
				logger::info("{}", Config::UseGrassCache ? "Grass Cache is Enabled. PrecacheGrass.txt is not detected, assuming normal usage." : "Grass Cache is Disabled");
			}

			if (Config::DynDOLODGrassMode == 1 || Config::DynDOLODGrassMode == 2) {
				Config::OverwriteGrassDistance = 999999.0;
				Config::OverwriteGrassFadeRange = 0.0;
				Config::ExtendGrassDistance = Config::DynDOLODGrassMode == 2;
			}

			GrassPatchCreation::Install();

			// Patched while the game is still single-threaded; the engine's write lock is in constant use once it runs.
			if (Config::RayCast || Config::GrassCliffs)
				PhysicsRaycast::InstallWriterSignal();

			if (Config::SuperDenseGrass) {
				const auto addr = RELOCATION_ID(15202, 15370).address() + REL::Relocate(0xAE5 - 0x890, 0x258);
				const int mode = std::clamp(Config::SuperDenseMode, 0, 12);
				if (mode != 7)
					REL::safe_write(addr + 2, static_cast<std::uint8_t>(mode));
			}

			// Shared placement starts a new instance buffer when a shape fills; engine placement would overrun it.
			if (Config::ExtendGrassCount)
				REL::safe_write(RELOCATION_ID(15220, 15385).address() + REL::Relocate(0x433 - 0x3C0, 0x68), REL::NOP6, 6);

			if (Config::UseGrassCache)
				GidFileCache::FixFileFormat();

			if (Config::ExtendGrassDistance)
				DistantGrass::Install();

			if (Config::EnsureMaxGrassTypesPerTextureSetting > 0) {
				const auto addr = RELOCATION_ID(18342, 18758).address() + REL::Relocate(0xD63 - 0xCF0, 0x68);
				const std::uint32_t max = std::max(Config::EnsureMaxGrassTypesPerTextureSetting, globals::settings::maxGrassTypesPerTexture ? globals::settings::maxGrassTypesPerTexture->GetInteger() : 0);

				struct MaxGrassTypesPatch : CodeGenerator
				{
					MaxGrassTypesPatch(std::uint32_t max, std::uintptr_t a_target)
					{
						Label retnLabel;

						if (REL::Module::IsAE())
							mov(edi, max);  // maxGrassTypes = max;
						else
							mov(r12d, max);  // maxGrassTypes = max;

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

			if (Config::OverwriteGrassDistance >= 0.0)
				OverrideSetting("fGrassStartFadeDistance:Grass", static_cast<float>(Config::OverwriteGrassDistance));

			if (Config::OverwriteGrassFadeRange >= 0.0)
				OverrideSetting("fGrassFadeRange:Grass", static_cast<float>(Config::OverwriteGrassFadeRange));

			if (Config::OverwriteMinGrassSize >= 0)
				OverrideSetting("iMinGrassSize:Grass", Config::OverwriteMinGrassSize);

			// The engine divides the frame time by this value, so zero would produce NaN fade alphas.
			if (Config::OverwriteGrassFadeInTime >= 0.0)
				OverrideSetting("fGrassFadeInTime:Grass", std::max(static_cast<float>(Config::OverwriteGrassFadeInTime), 0.0001f));
		}
	}

	namespace DistantGrass
	{
		static RE::TESObjectCELL* CellFromExtraList(const RE::ExtraDataList* a_extraList)
		{
			return reinterpret_cast<RE::TESObjectCELL*>(reinterpret_cast<std::uintptr_t>(a_extraList) - offsetof(RE::TESObjectCELL, extraList));
		}

		struct CellDestructor
		{
			static void* thunk(RE::ExtraDataList* a_extraList, std::uint32_t)
			{
				RemoveCellGrass(CellFromExtraList(a_extraList));
				RE::ClearGrassHandles(a_extraList);

				// Returning null prevents the caller from removing grass a second time.
				return nullptr;
			}

			inline static REL::Relocation<decltype(thunk)> func;
		};

		// The caller only tests this geometry result for null.
		struct AllowMissingLandMesh
		{
			static std::uintptr_t thunk() { return 1; }

			inline static REL::Relocation<decltype(thunk)> func;
		};

		struct ClearUnloadTask
		{
			static void thunk(const RE::ExtraDataList& a_extraList, void** a_task)
			{
				ClearCellAddGrassTask(CellFromExtraList(std::addressof(a_extraList)));
				*a_task = nullptr;
			}

			inline static REL::Relocation<decltype(thunk)> func;
		};

		struct CellSelection
		{
			static RE::BSMultiStreamInstanceTriShape* thunk(RE::BGSGrassManager* a_GrassManager, RE::GrassParam* a_grassParam, std::uint32_t a_CellXDivided, std::uint32_t a_CellYDivided, std::uint64_t* a_typeKey, RE::BSFixedString* a_name)
			{
				if (auto* cell = GrassPatchCreation::GetCell()) {
					if (const auto* coordinates = cell->GetCoordinates())
						return func(a_GrassManager, a_grassParam, static_cast<std::uint32_t>(coordinates->cellX), static_cast<std::uint32_t>(coordinates->cellY), a_typeKey, a_name);
				}

				return func(a_GrassManager, a_grassParam, a_CellXDivided * 12, a_CellYDivided * 12, a_typeKey, a_name);
			}

			inline static REL::Relocation<decltype(thunk)> func;
		};

		struct UpdateGrassGridEnable
		{
			static void thunk(RE::TES* a_TES)
			{
				UpdateGrassGridNow(a_TES, 0, 0, 1);
			}

			inline static REL::Relocation<decltype(thunk)> func;
		};

		struct UpdateGrassGridDisable
		{
			static void thunk(RE::TES* a_TES)
			{
				UpdateGrassGridNow(a_TES, 0, 0, -1);
			}

			inline static REL::Relocation<decltype(thunk)> func;
		};

		static void ThrowOurMethodException(const std::uint8_t ourMethod)
		{
			stl::report_and_fail("GrassControl.dll: unexpected ourMethod: " + std::to_string(ourMethod));
		}

		static void InstallCellGridPatches()
		{
			auto& trampoline = SKSE::GetTrampoline();

			REL::safe_write(RELOCATION_ID(38141, 39098).address(), REL::RET);
			REL::safe_write(RELOCATION_ID(13240, 13391).address() + (0x55E - 0x480), REL::NOP5, 5);

			OverrideSetting("iGrassCellRadius:Grass", GetGrassGridRadius());

			stl::write_thunk_call<CellDestructor>(RELOCATION_ID(18446, 18877).address() + 0x6E);
			REL::safe_write(RELOCATION_ID(13623, 13721).address() + REL::Relocate(0xC0F8 - 0xBF90, 0x1E8), REL::NOP5, 5);

			stl::write_thunk_call<AllowMissingLandMesh>(RELOCATION_ID(15204, 15372).address() + 0x1C2);
			stl::write_thunk_call<ClearUnloadTask>(RELOCATION_ID(18655, 19130).address() + REL::Relocate(0xC8, 0xCA));

			{
				const auto worker = RELOCATION_ID(18137, 18527).address();
				// AE gates coordinate tasks to a separate branch before the cell call, so the hook replaces that gate.
				const auto addr = worker + REL::Relocate(0x17, 0x1B);

				struct CellLoadDispatchPatch : CodeGenerator
				{
					CellLoadDispatchPatch(std::uintptr_t a_func, std::uintptr_t b_func, std::uintptr_t a_rbxWorldSpaceOffset, std::uintptr_t a_targetReturn, std::uintptr_t a_targetSkip)
					{
						Label loadCustomCell;
						Label invalidMethod;
						Label retnLabel;
						Label completeTask;
						Label loadCoordinates;
						Label coordinateTarget;

						Label checkNormalLoad;
						Label resolveCell;

						mov(al, ptr[rbx + a_rbxWorldSpaceOffset + 0x1E]);  // ourMethod = task->ourMethod;
						cmp(al, 1);
						jne(checkNormalLoad);  // if (ourMethod != 1) goto checkNormalLoad;

						mov(rcx, ptr[rbx + a_rbxWorldSpaceOffset]);         // worldSpace = task->worldSpace;
						mov(edx, ptr[rbx + a_rbxWorldSpaceOffset + 0x10]);  // x = task->x;
						mov(r8d, ptr[rbx + a_rbxWorldSpaceOffset + 0x14]);  // y = task->y;

						sub(rsp, 0x20);
						call(ptr[rip + loadCustomCell]);  // LoadDistantCellNow(worldSpace, x, y);
						add(rsp, 0x20);

						jmp(ptr[rip + completeTask]);  // Custom tasks have no cell pointer for the native load.

						L(checkNormalLoad);
						cmp(al, 0);
						je(resolveCell);  // if (ourMethod == 0) goto resolveCell;

						mov(cl, al);

						sub(rsp, 0x20);
						call(ptr[rip + invalidMethod]);  // ThrowOurMethodException(ourMethod);
						add(rsp, 0x20);

						L(resolveCell);
						if (REL::Module::IsAE()) {
							cmp(byte[rbx + a_rbxWorldSpaceOffset + 0x1C], 0);  // The replaced gate: task->isCell.
							je(loadCoordinates);
						} else {
							mov(rcx, ptr[rbx + a_rbxWorldSpaceOffset - 0x8]);  // loadTask
							lea(rdx, ptr[rbx + a_rbxWorldSpaceOffset]);        // &task->worldSpace
						}

						jmp(ptr[rip + retnLabel]);
						if (REL::Module::IsAE()) {
							L(loadCoordinates);
							jmp(ptr[rip + coordinateTarget]);
							L(coordinateTarget);
							dq(a_targetReturn + 0xB);  // Native coordinate branch skips the direct-load instructions.
						}

						L(loadCustomCell);
						dq(a_func);

						L(invalidMethod);
						dq(b_func);

						L(retnLabel);
						dq(a_targetReturn);

						L(completeTask);
						dq(a_targetSkip);
					}
				};

				CellLoadDispatchPatch patch(reinterpret_cast<std::uintptr_t>(LoadDistantCellNow), reinterpret_cast<std::uintptr_t>(ThrowOurMethodException), REL::Relocate(0x20, 0x20, 0x28), worker + REL::Relocate(0x1F, 0x21), worker + REL::Relocate(0x39, 0x16B));
				patch.ready();
				if (REL::Module::IsAE()) {
					trampoline.write_branch<6>(addr, trampoline.allocate(patch));
				} else {
					REL::safe_write(addr + 5, REL::NOP3, 3);
					trampoline.write_branch<5>(addr, trampoline.allocate(patch));
				}
			}

			{
				auto addr = RELOCATION_ID(18150, 18541).address() + REL::Relocate(0xB094 - 0xAF20, 0x1CA, 0x177);
				if (UpdatedRuntime())
					addr = RELOCATION_ID(18150, 18541).address() + 0x1E4;

				struct CellLoadSelectionPatch : CodeGenerator
				{
					CellLoadSelectionPatch(std::uintptr_t a_func, std::uintptr_t a_target, std::uintptr_t rbx_offset)
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

				CellLoadSelectionPatch patch(reinterpret_cast<std::uintptr_t>(CellLoadHook), addr + REL::Relocate(0x7, 0xB), REL::Relocate(0x30, 0x30, 0x38));
				patch.ready();
				if (!REL::Module::IsAE())
					REL::safe_write(addr + 5, REL::NOP2, 2);
				else
					REL::safe_write(addr + 5, REL::NOP6, 6);
				trampoline.write_branch<5>(addr, trampoline.allocate(patch));
			}

			{
				const auto addr = RELOCATION_ID(18149, 18540).address() + REL::Relocate(0xE1B - 0xCC0, 0x167, 0x15E);

				struct CellLoadFlagsPatch : CodeGenerator
				{
					explicit CellLoadFlagsPatch(std::uintptr_t a_target, std::uintptr_t rbx_offset, Reg a_source)
					{
						Label retnLabel;

						mov(byte[rbx + rbx_offset + 0x2], 0);        // task->ourMethod = 0;
						mov(byte[rbx + rbx_offset], 1);              // loadFlags[0] = 1;
						mov(ptr[rbx + rbx_offset + 0x1], a_source);  // loadFlags[1] = uint8_t(a_source);

						jmp(ptr[rip + retnLabel]);

						L(retnLabel);
						dq(a_target + 0x8);
					}
				};

				CellLoadFlagsPatch patch(addr, REL::Relocate(0x3c, 0x3c, 0x44), Reg8(REL::Relocate(Reg::R15B, Reg::BPL)));
				patch.ready();
				REL::safe_write(addr + 5, REL::NOP3, 3);
				trampoline.write_branch<5>(addr, trampoline.allocate(patch));
			}
		}

		void Install()
		{
			if (Config::OnlyLoadFromCache) {
				gridState.cacheOnlyCells = std::make_unique<CacheOnlyCells>();
			} else {
				gridState.cellGrid = std::make_unique<CellInfoContainer>();
				InstallCellGridPatches();
				logger::info("Distant grass will stream after loading, with nearby cells taking priority.");
			}

			auto& trampoline = SKSE::GetTrampoline();

			{
				auto addr = RELOCATION_ID(13148, 13288).address() + REL::Relocate(0xA06 - 0x220, 0xA1D);
				auto retnOffset = REL::Relocate(0x159, 0x17D);
				if (UpdatedRuntime()) {
					addr = RELOCATION_ID(13148, 13288).address() + 0x8B3;
					retnOffset = 0x177;
				}

				struct UpdateGrassGridPatch : CodeGenerator
				{
					UpdateGrassGridPatch(std::uintptr_t a_func, std::uintptr_t a_target, Reg a_Y, Reg a_X)
					{
						Label funcLabel;
						Label retnLabel;

						push(rdx);  // Preserve the caller's live registers across the helper.
						push(rcx);

						mov(r9d, 0);    // addType = 0;
						mov(r8d, a_Y);  // movedY = int32_t(a_Y);

						if (REL::Module::IsAE() && !UpdatedRuntime())
							mov(edx, ptr[rsp + 0x10 + 0x44]);  // movedX = int32At(entrySP + 0x44);
						else
							mov(edx, a_X);  // movedX = int32_t(a_X);

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

				UpdateGrassGridPatch patch(reinterpret_cast<std::uintptr_t>(UpdateGrassGridNow), addr + retnOffset,
					Reg32(UpdatedRuntime() ? Reg::EBP : REL::Relocate(Reg::R14D, Reg::EDI)),
					Reg32(UpdatedRuntime() ? Reg::R15D : REL::Relocate(Reg::EBP, Reg::R14D)));
				patch.ready();
				trampoline.write_branch<6>(addr, trampoline.allocate(patch));
			}

			{
				const auto addr = RELOCATION_ID(15214, 15383).address() + REL::Relocate(0x78B7 - 0x7830, 0x7E);

				struct PackGrassCoordinatesPatch : CodeGenerator
				{
					explicit PackGrassCoordinatesPatch(std::uintptr_t a_target, Reg a_X, std::uintptr_t rsp_offset, std::uintptr_t rsp_stackOffset)
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

				PackGrassCoordinatesPatch patch(addr, Reg32(REL::Relocate(Reg::R15, Reg::R14)), REL::Relocate(0x48, 0x50), REL::Relocate(0x258, 0x288));
				patch.ready();
				REL::safe_fill(addr, REL::NOP, 0xB);
				trampoline.write_branch<5>(addr, trampoline.allocate(patch));
			}

			// Bypass the grass-distance clamp derived from uGridsToLoad.
			REL::safe_write(RELOCATION_ID(15202, 15370).address() + REL::Relocate(0xB06 - 0x890, 0x279) + 2, std::uint8_t{ 16 });
			REL::safe_write(RELOCATION_ID(528751, 15379).address() + REL::Relocate(0xFE - 0xE0, 0x1E) + 2, std::uint8_t{ 16 });

			stl::write_thunk_call<CellSelection>(RELOCATION_ID(15206, 15374).address() + (0x645C - 0x6200));
			stl::write_thunk_call<CellSelection>(RELOCATION_ID(15205, 15373).address() + (UpdatedRuntime() ? 0x590 : REL::Relocate(0x62B, 0x597)));
			stl::write_thunk_call<CellSelection>(RELOCATION_ID(15204, 15372).address() + (UpdatedRuntime() ? 0x305 : 0x2F5));

			stl::write_thunk_jump<UpdateGrassGridEnable>(RELOCATION_ID(13190, 13335).address());
			stl::write_thunk_jump<UpdateGrassGridDisable>(RELOCATION_ID(13191, 13336).address());

			GrassNodeShards::Install();
		}
	}

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
				for (auto& word : words)
					word = std::byteswap(word);

				header = std::bit_cast<decltype(header)>(words);
			}

			if (const auto written = RE::WriteStream(a_stream + 0x8, std::addressof(header), static_cast<std::uint32_t>(sizeof(header))); written != sizeof(header))
				return written;

			return RE::WriteStream(a_stream + 0x8, group->vertexBuffer->m_data, static_cast<std::uint32_t>(group->vertexBuffer->byteWidth));
		}

		inline static REL::Relocation<decltype(thunk)> func;
	};

	void GidFileCache::SetHelperInstalled(bool installed)
	{
		cacheHelperInstalled = installed;
	}

	void GidFileCache::FixFileFormat()
	{
		if (Config::GeneratingCache) {
			std::error_code error;
			std::filesystem::create_directories(Util::Files::GamePath(L"Data/Grass"), error);
			if (error)
				stl::report_and_fail("Error occured while trying to create or access data/grass folder");
		}

		stl::write_thunk_jump<SaveGrassGroup>(RELOCATION_ID(74601, 76329).address());

		OverrideSetting("bAllowLoadGrass:Grass", true);
		OverrideSetting("bAllowCreateGrass:Grass", !Config::OnlyLoadFromCache || Config::Updating);

		if (Config::Updating) {
			logger::info("Cache Updating Mode Enabled.");
			GidFileCache::EnableFileGeneration();
		}

		const auto addr = RELOCATION_ID(15204, 15372).address() + (UpdatedRuntime() ? 0x8A6 : 0x896);

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

		SkipConsoleGrassPatch patch(addr);
		patch.ready();

		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_branch<5>(addr, trampoline.allocate(patch));
		REL::safe_write(addr + 5, REL::NOP2, 2);
	}

	namespace Generation
	{
		struct GrassGridRadius
		{
			static int thunk() { return Generation::GetGrassGridRadius(); }

			inline static REL::Relocation<decltype(thunk)> func;
		};

		struct PathFileNameSave
		{
			static int thunk(char* buffer, std::size_t capacity, const char*, const char* world, int x, int y)
			{
				// Cells cached this session load from their file once they are distant.
				DistantGrass::gridState.cacheIndex.Insert(world, x, y);

				// Deletion and creation must use the same name to prevent appending to an old cache.
				return func(buffer, capacity, "Grass\\%sx%04dy%04d.cgid", world, x, y);
			}

			inline static REL::Relocation<decltype(thunk)> func;
		};

		struct PathFileNameLoad
		{
			static int thunk(char* buffer, std::size_t capacity, const char*, const char* world, int x, int y)
			{
				// Avoid loading vanilla .gid files from archives.
				return func(buffer, capacity, "Grass\\%sx%04dy%04d.cgid", world, x, y);
			}

			inline static REL::Relocation<decltype(thunk)> func;
		};

		struct GrassCountIncrement
		{
			static void thunk(RE::TESObjectCELL* cell)
			{
				RE::BSSpinLockGuard taskLock(cell->grassTaskLock);
				if (!RE::AddCellGrassTask::Get(cell))
					Generation::OnGrassQueued();
				func(cell);
			}

			inline static REL::Relocation<decltype(thunk)> func;
		};

		void InstallHooks()
		{
			if (!Config::UseGrassCache)
				return;

			if (!GidFileCache::cacheHelperInstalled)
				stl::write_thunk_call<PathFileNameLoad>(RELOCATION_ID(15206, 15374).address() + 0xD4);

			stl::write_thunk_call<PathFileNameSave>(RELOCATION_ID(15204, 15372).address() + (UpdatedRuntime() ? 0x666 : REL::Relocate(0x65A, 0x656)));

			if (Config::GeneratingCache)
				stl::write_thunk_call<GrassCountIncrement>(RELOCATION_ID(13190, 13335).address() + REL::Relocate(0xD40 - 0xC70, 0xD0));
		}

		void ApplyPatches()
		{
			const auto createOffset = UpdatedRuntime() ? 0x9D9 : REL::Relocate(0x2B25 - 0x2220, 0xB29);
			REL::safe_write(RELOCATION_ID(13148, 13288).address() + createOffset, REL::NOP5, 5);

			const auto addr = RELOCATION_ID(13190, 13335).address() + 0x106;

			struct GrassDispatchPatch : CodeGenerator
			{
				GrassDispatchPatch(std::uintptr_t a_onGrassDispatch, const std::uintptr_t a_target)
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

			GrassDispatchPatch patch(reinterpret_cast<std::uintptr_t>(&Generation::OnGrassDispatch), addr);
			patch.ready();

			auto& trampoline = SKSE::GetTrampoline();
			trampoline.write_branch<5>(addr, trampoline.allocate(patch));

			stl::write_thunk_call<Generation::GrassGridRadius, 6>(RELOCATION_ID(15202, 15370).address() + REL::Relocate(0x17E, 0x17D));

			GidFileCache::EnableFileGeneration();

			Generation::Enable();

			// Cache generation must continue while the game is unfocused.
			REL::safe_write(RELOCATION_ID(35565, 36564).address() + REL::Relocate(0x216 - 0x1E0, 0x51, 0x4b), REL::JMP8);
		}
	}
}
