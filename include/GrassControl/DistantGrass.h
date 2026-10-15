#pragma once

namespace GrassControl::DistantGrass
{
	// One directory scan replaces a filesystem probe for every distant cell.
	class GrassCacheIndex final
	{
	public:
		bool Contains(std::string_view world, int x, int y);

		bool Empty();

		// Includes newly generated files without rescanning the directory.
		void Insert(std::string_view world, int x, int y);

	private:
		static std::string Lower(std::string_view text);
		static std::uint32_t Key(int x, int y);

		// Names are "<world>x<x>y<y>.cgid", and world editor IDs can themselves contain 'x' or 'y'.
		void AddFile(std::string name);
		void Scan();

		std::once_flag scanned;
		std::shared_mutex mutex;
		std::unordered_map<std::string, std::unordered_set<std::uint32_t>> worlds;
	};

	enum class GrassState : std::uint8_t
	{
		None,
		Lod,
		Active
	};

	enum class TerrainLoad : std::uint8_t
	{
		None,
		Queued,   // Queued on the loader; the next load of these coordinates is NGIO's.
		Claimed,  // The loader picked NGIO's load path for the task.
		Loading
	};

	class CellInfo final
	{
	public:
		CellInfo(int cellX, int cellY);

		const int x;
		const int y;

		// Guarded by gridState.cellMapMutex.
		RE::TESObjectCELL* cell = nullptr;
		GrassState have = GrassState::None;
		GrassState wanted = GrassState::None;
		bool queued = false;

		std::atomic<TerrainLoad> terrainLoad{ TerrainLoad::None };
		// Cancellation clears terrainLoad before the terrain worker has returned.
		std::atomic<bool> loadingTerrain{ false };

		bool AdvanceTerrain(TerrainLoad from, TerrainLoad to);

		void ClearTask()
		{
			queued = false;
			wanted = GrassState::None;
		}

		void Reset()
		{
			ClearTask();
			have = GrassState::None;
			terrainLoad = TerrainLoad::None;
		}

		[[nodiscard]] bool HasCacheFile(std::string_view world) const;
	};

	class CellInfoContainer final
	{
	public:
		CellInfoContainer();

		[[nodiscard]] std::shared_ptr<CellInfo> GetFromGrid(int x, int y);

		std::shared_ptr<CellInfo> FindByCell(RE::TESObjectCELL* cell);

		bool QueueDistantCells(RE::TESWorldSpace* world, int x, int y, int activeRadius, int grassRadius);

		// Caller holds gridState.cellMapMutex.
		void RetryDistantCell(int x, int y);

		void UpdateDistantCells(const RE::TES* tes);

		std::unordered_map<RE::TESObjectCELL*, std::shared_ptr<CellInfo>> map{ 1024 };

	private:
		inline static constexpr int GridHalf = 64;
		inline static constexpr int GridSide = GridHalf * 2 + 1;
		inline static constexpr std::size_t MaxDistantCellsInFlight = 32;
		inline static constexpr auto DistantStreamBudget = std::chrono::microseconds(1000);
		// Engine tasks can be dropped without our callback, which would otherwise hold a slot forever.
		inline static constexpr auto DistantCellTimeout = std::chrono::seconds(10);

		std::vector<std::shared_ptr<CellInfo>> grid;
		std::vector<std::pair<int, int>> pendingCells;
		std::size_t nextPendingCell = 0;
		std::vector<std::pair<std::shared_ptr<CellInfo>, std::chrono::steady_clock::time_point>> inFlight;
		RE::TESWorldSpace* pendingWorld = nullptr;
		int pendingX = 0;
		int pendingY = 0;
	};

	// Stand-in cells let cache-only mode load grass without loading real cells or generating placements.
	class CacheOnlyCells final
	{
	public:
		// addType -1 unloads every cell regardless of radius.
		void Retain(const RE::TESWorldSpace* world, int addType, int centerX, int centerY, int radius);

		// Returns true only when a load was queued.
		bool QueueLoad(RE::TESWorldSpace* world, int x, int y);

		// The engine task supplies the stand-in cell.
		void Load(RE::TESObjectCELL* cell);

		// Main thread only; grass must already be removed.
		void FreeDiscardedCells();

	private:
		enum class State : std::uint8_t
		{
			None,
			Loading,
			Loaded,
			Abort  // Unloaded while its load was queued; the load discards it.
		};

		struct Entry
		{
			RE::TESObjectCELL* standIn = nullptr;
			State state = State::None;
		};

		static std::uint64_t Key(RE::FormID world, int x, int y);
		static RE::TESObjectCELL* CreateStandIn(RE::TESWorldSpace* world, int x, int y);

		// Caller holds gridState.cellMapMutex. True when the entry can be dropped.
		bool Unload(Entry& entry);
		void Discard(Entry& entry);

		std::unordered_map<std::uint64_t, Entry> cells{ 256 };
		std::vector<RE::TESObjectCELL*> discarded;
	};

	class GrassGridState final
	{
	public:
		std::recursive_mutex cellMapMutex;
		std::atomic<bool> gridUpdatePending{ false };
		std::atomic<bool> streamingAllowed{ false };
		std::atomic<bool> gridUpdatesEnabled{ false };
		std::unique_ptr<CellInfoContainer> cellGrid;
		std::unique_ptr<CacheOnlyCells> cacheOnlyCells;
		int grassGridRadius = -1;
		GrassCacheIndex cacheIndex;
	};

	inline GrassGridState gridState;

	bool IsValidLoadedCell(RE::TESObjectCELL* cell, bool quickLoad);

	void Install();

	// Returns false when the caller must retry after collision or an existing task finishes.
	bool QueueGrassPlacement(RE::TESObjectCELL* cell);

	void ResetGridUpdates();

	int GetGrassGridRadius();

	bool ClearCellAddGrassTask(const RE::TESObjectCELL* cell);

	void RemoveCellGrass(RE::TESObjectCELL* cell);

	// A nonzero result selects NGIO's terrain-only load path.
	unsigned char CellLoadHook(int x, int y);

	void LoadDistantCellNow(std::uintptr_t world, int x, int y);

	void Update();

	// addType 1 enables grid updates, -1 disables them, and 0 moves the grid.
	void UpdateGrassGridNow(const RE::TES* tes, int movedX, int movedY, int addType);

	bool IsDistantCell(int centerX, int centerY, int x, int y, int activeRadius, int grassRadius);

	RE::TESObjectCELL* GetCurrentWorldspaceCell(const RE::TES* tes, RE::TESWorldSpace* ws, int x, int y, bool quickLoad, bool allowLoadNow);

	GrassState GetWantState(const CellInfo& cell, int curX, int curY, int uGrid, int grassRadius, bool canLoadFromFile, std::string_view world);

	// Caller holds gridState.cellMapMutex; returns false if a task is already queued.
	bool PrepareGrassTask(const std::shared_ptr<CellInfo>& info, RE::TESObjectCELL* cell, GrassState wanted);

	// Queue the visible center before spending time preparing the outer rings.
	template <class F>
	void ForEachGrassCell(int centerX, int centerY, int radius, F&& visit)
	{
		visit(centerX, centerY);

		for (int ring = 1; ring <= radius; ++ring) {
			for (int x = centerX - ring; x <= centerX + ring; ++x) {
				visit(x, centerY - ring);
				visit(x, centerY + ring);
			}

			for (int y = centerY - ring + 1; y < centerY + ring; ++y) {
				visit(centerX - ring, y);
				visit(centerX + ring, y);
			}
		}
	}

	template <class F>
	void ForEachDistantCell(int centerX, int centerY, int activeRadius, int grassRadius, F&& visit)
	{
		if (grassRadius <= activeRadius)
			return;

		ForEachGrassCell(centerX, centerY, grassRadius, [&](int x, int y) {
			if (IsDistantCell(centerX, centerY, x, y, activeRadius, grassRadius))
				visit(x, y);
		});
	}
}
