#pragma once

#include "CasualLibrary.hpp"
#include "GrassControl/GidFileCache.h"
#include "GrassControl/Main.h"

namespace GrassControl
{
	enum class GrassStates : unsigned char
	{
		None,

		Lod,

		Active
	};

	static std::recursive_mutex cellMapMutex;

	class DistantGrass final
	{
		class LoadOnlyCellInfoContainer2;

	public:
		static void RemoveGrassHook(RE::TESObjectCELL* cell, uintptr_t arg_1);

		static void CellUnloadHook(bool did, RE::TESObjectCELL* cellObj);

		static void ReplaceGrassGrid(bool _loadOnly);

		static inline std::atomic<bool> _canUpdateGridNormal = false;

	private:
		static inline bool load_only = false;

		static float getChosenGrassFadeRange();

		static int getChosenGrassGridRadius();
		static int _chosenGrassGridRadius;

		static bool DidApply;
		static void CellLoadNow_Our(uintptr_t ws, int x, int y);

		static inline uintptr_t addr_DataHandler = RELOCATION_ID(514141, 400269).address();
		static inline uintptr_t addr_QueueLoadCellUnkGlobal = RELOCATION_ID(514741, 400899).address();

	public:
		static void InstallHooks()
		{
			Hooks::Install();
		}

	private:
		static bool ClearCellAddGrassTask(const RE::TESObjectCELL* cell);

		class cell_info final
		{
		public:
			cell_info(int _x, int _y);
			cell_info();

			int x;
			int y;

			RE::TESObjectCELL* cell = nullptr;
			int self_data = 0;
			volatile long long furtherLoad = 0;

			[[nodiscard]] bool checkHasFile(const std::string& wsName) const;
		};

		class CellInfoContainer
		{
		public:
			CellInfoContainer();

			[[nodiscard]] std::shared_ptr<cell_info> GetFromGrid(int x, int y) const;

		private:
			std::vector<std::shared_ptr<cell_info>> grid{};

		public:
			std::unordered_map<RE::TESObjectCELL*, std::shared_ptr<cell_info>> map = std::unordered_map<RE::TESObjectCELL*, std::shared_ptr<cell_info>>(1024);

			void unsafe_ForeachWithState(const std::function<bool(std::shared_ptr<cell_info>)>& action);

			std::shared_ptr<cell_info> FindByCell(RE::TESObjectCELL* cell);
		};

		static unsigned char CellLoadHook(int x, int y);

	public:
		inline static std::unique_ptr<CellInfoContainer> Map;
		//private static LoadOnlyCellInfoContainer LOMap;
		inline static std::unique_ptr<LoadOnlyCellInfoContainer2> LO2Map;

	private:
		static bool IsValidLoadedCell(RE::TESObjectCELL* cell, bool quickLoad);

		static RE::TESObjectCELL* GetCurrentWorldspaceCell(const RE::TES* tes, RE::TESWorldSpace* ws, int x, int y, bool quickLoad, bool allowLoadNow);

		static void Call_AddGrassNow(RE::BGSGrassManager* GrassMgr, RE::TESObjectCELL* cell, uintptr_t customArg);

		static GrassStates GetWantState(const std::shared_ptr<cell_info>& c, int curX, int curY, int uGrid, int grassRadius, bool canLoadFromFile, const std::string& wsName);

		static void Handle_RemoveGrassFromCell_Call(RE::TESObjectCELL* cell);

		static unsigned char CalculateLoadState(int nowX, int nowY, int x, int y, int ugrid, int ggrid);

		static void UpdateGrassGridEnsureLoad(RE::TESWorldSpace* ws, int nowX, int nowY);

		static void UpdateGrassGridQueue(int prevX, int prevY, int movedX, int movedY);

		static void UpdateGrassGridNow(const RE::TES* tes, int movedX, int movedY, int addType);

		static void UpdateGrassGridNow_LoadOnly(const RE::TES* tes, int movedX, int movedY, int addType);

		static int GetUGrids();

		class LoadOnlyCellInfoContainer2 final
		{
			class _cell_data final
			{
			public:
				RE::TESObjectCELL* DummyCell_Ptr = nullptr;
				unsigned int WsId = 0;
				int X = 0;
				int Y = 0;
				enum class _cell_states : int
				{
					None = 0,
					Queued = 1,
					Loading = 2,
					Loaded = 3,
					Abort = 4
				};
				_cell_states State = _cell_states::None;
			};

			struct case_insensitive_unordered_map
			{
				struct comp
				{
					bool operator()(const std::string& lhs, const std::string& rhs) const
					{
						// On non Windows OS, use the function "strcasecmp" in #include <strings.h>
						return _stricmp(lhs.c_str(), rhs.c_str()) == 0;
					}
				};
				struct hash
				{
					std::size_t operator()(std::string str) const
					{
						for (char& index : str) {
							auto ch = static_cast<unsigned char>(index);
							index = static_cast<unsigned char>(std::tolower(ch));
						}
						return std::hash<std::string>{}(str);
					}
				};
			};

			std::unordered_map<std::string, std::shared_ptr<_cell_data>, case_insensitive_unordered_map::hash, case_insensitive_unordered_map::comp> map = std::unordered_map<std::string, std::shared_ptr<_cell_data>, case_insensitive_unordered_map::hash, case_insensitive_unordered_map::comp>(256);

			// Only used for debug message so it's fine to be slow.
		public:
			[[nodiscard]] int GetCount() const;

		private:
			static std::string MakeKey(const std::string& ws, int x, int y);

		public:
			void UpdatePositionWithRemove(RE::TESWorldSpace* ws, int addType, int nowX, int nowY, int grassRadius);

			void QueueLoad(RE::TESWorldSpace* ws, int x, int y);

		private:
			bool _DoUnload(std::shared_ptr<_cell_data>& cellData);

			std::vector<RE::TESObjectCELL*> discardedCells{};

			void _DiscardDummyCell(const std::shared_ptr<_cell_data>& cellData);

		public:
			void FreeDiscardedCells();

			void Unload(const RE::TESWorldSpace* ws, int x, int y);

			void _DoLoad(const RE::TESWorldSpace* ws, int x, int y);

			void UnloadAll();
		};

	protected:
		struct Hooks
		{
			struct WriteProgress
			{
				static void thunk(RE::BGSGrassManager* GrassMgr, RE::TESObjectCELL* cell, uintptr_t unk)
				{
					if (exists(std::filesystem::path(Util::GetProgressFilePath())) || cell == nullptr)
						return;

					if (Config::OnlyLoadFromCache) {
						auto ext = cell->GetCoordinates();
						auto x = ext->cellX;
						auto y = ext->cellY;
						LO2Map->_DoLoad(cell->GetRuntimeData().worldSpace, x, y);
					} else {
						Call_AddGrassNow(GrassMgr, cell, unk);
					}
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct CellSelection
			{
				static void thunk(uintptr_t arg_1, uintptr_t arg_2, int arg_3, int arg_4, uintptr_t arg_5, uintptr_t arg_6)
				{
					func(arg_1, arg_2, arg_3 * 12, arg_4 * 12, arg_5, arg_6);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct LoadGrassType
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
					UpdateGrassGridNow(a_TES, 0, 0, 1);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct UpdateGrassGridAddRemove2
			{
				static void thunk(RE::TES* a_TES)
				{
					UpdateGrassGridNow(a_TES, 0, 0, -1);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct CallUpdateGrassGridEnsureLoad
			{
				static void thunk(RE::TES* a_TES, std::int32_t arg_2)
				{
					func(a_TES, arg_2);
					UpdateGrassGridEnsureLoad(a_TES->GetRuntimeData2().worldSpace, a_TES->currentGridX, a_TES->currentGridY);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			static void Install()
			{
				if (Config::ExtendGrassDistance) {
					stl::write_thunk_jump<WriteProgress>(RELOCATION_ID(13138, 13278).address() + REL::Relocate(0xF, 0xF));

					// Fix weird shape selection.
					// Vanilla game groups shape selection by 12 x 12 cells, we want a shape per cell.
					stl::write_thunk_call<CellSelection>(RELOCATION_ID(15206, 15374).address() + REL::Relocate(0x645C - 0x6200, 0x645C - 0x6200));
					stl::write_thunk_call<LoadGrassType>(RELOCATION_ID(15205, 15373).address() + REL::Relocate(0x617, 0x583));
					stl::write_thunk_call<CellSelection>(RELOCATION_ID(15204, 15372).address() + REL::Relocate(0x2F5, 0x2F5));

					stl::write_thunk_jump<UpdateGrassGridAddRemove>(RELOCATION_ID(13190, 13335).address());
					Memory::Internal::write<uint8_t>(RELOCATION_ID(13190, 13335).address() + 5, 0xC3, true);
					stl::write_thunk_jump<UpdateGrassGridAddRemove2>(RELOCATION_ID(13191, 13336).address());
					Memory::Internal::write<uint8_t>(RELOCATION_ID(13191, 13336).address() + 5, 0xC3, true);

					if (!load_only) {
						stl::write_thunk_call<CallUpdateGrassGridEnsureLoad>(RELOCATION_ID(13148, 13288).address() + REL::Relocate(0x29AF - 0x2220, 0x9A9));
					}
				}
			}
		};
	};
}
