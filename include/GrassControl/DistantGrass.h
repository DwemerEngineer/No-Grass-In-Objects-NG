#pragma once

#include "GrassControl/Util.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RE
{
	class BGSGrassManager;

	class TES;

	class TESObjectCELL;

	class TESWorldSpace;
}

namespace GrassControl::DistantGrass
{
	class CellInfo final
	{
	public:
		CellInfo(int _x, int _y);

		int x;
		int y;

		RE::TESObjectCELL* cell = nullptr;
		int self_data = 0;
		volatile long long furtherLoad = 0;

		[[nodiscard]] bool checkHasFile(const std::string& wsName) const;
	};

	class CellInfoContainer final
	{
	public:
		CellInfoContainer();

		[[nodiscard]] std::shared_ptr<CellInfo> GetFromGrid(int x, int y) const;

		void unsafe_ForeachWithState(const std::function<bool(std::shared_ptr<CellInfo>)>& action);

		std::shared_ptr<CellInfo> FindByCell(RE::TESObjectCELL* cell);

		std::unordered_map<RE::TESObjectCELL*, std::shared_ptr<CellInfo>> map{ 1024 };

	private:
		std::vector<std::shared_ptr<CellInfo>> grid;
	};

	class LoadOnlyCellContainer final
	{
	public:
		[[nodiscard]] int GetCount() const;

		void UpdatePositionWithRemove(RE::TESWorldSpace* ws, int addType, int nowX, int nowY, int grassRadius);

		void QueueLoad(RE::TESWorldSpace* ws, int x, int y);

		void FreeDiscardedCells();

		void Unload(const RE::TESWorldSpace* ws, int x, int y);

		void _DoLoad(const RE::TESWorldSpace* ws, int x, int y);

		void UnloadAll();

	private:
		class CellData final
		{
		public:
			enum class CellState : int
			{
				None = 0,
				Queued = 1,
				Loading = 2,
				Loaded = 3,
				Abort = 4
			};

			RE::TESObjectCELL* DummyCell_Ptr = nullptr;
			RE::FormID WsId = 0;
			int X = 0;
			int Y = 0;
			CellState State = CellState::None;
		};

		static std::string MakeKey(const std::string& ws, int x, int y);

		bool _DoUnload(std::shared_ptr<CellData>& cellData);

		void _DiscardDummyCell(const std::shared_ptr<CellData>& cellData);

		std::unordered_map<std::string, std::shared_ptr<CellData>, Util::CaseInsensitiveString::Hash, Util::CaseInsensitiveString::Equal> map{ 256 };
		std::vector<RE::TESObjectCELL*> discardedCells;
	};

	void ReplaceGrassGrid(bool loadOnly);

	void InstallHooks(bool loadOnly);

	void UnloadAll();

	void ResetGridUpdates();

	void Initialize(bool loadOnly);

	int getChosenGrassGridRadius();

	int GetUGrids();

	void RemoveGrassHook(RE::TESObjectCELL* cell, std::uintptr_t manager);

	void CellUnloadHook(bool did, RE::TESObjectCELL* cell);

	bool ClearCellAddGrassTask(const RE::TESObjectCELL* cell);

	void Handle_RemoveGrassFromCell_Call(RE::TESObjectCELL* cell);

	void OnGrassCreated(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, std::uintptr_t argument);

	unsigned char CellLoadHook(int x, int y);

	void CellLoadNow_Our(std::uintptr_t world, int x, int y);

	void UpdateGrassGridEnsureLoad(RE::TESWorldSpace* world, int x, int y);

	void UpdateGrassGridQueue(int previousX, int previousY, int movedX, int movedY);

	void UpdateGrassGridNow(const RE::TES* tes, int movedX, int movedY, int addType);
}
