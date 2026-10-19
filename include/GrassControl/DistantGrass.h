#pragma once

namespace GrassControl::DistantGrass
{
	// Cell destruction must also wait for callers that have not acquired grassCreateLock yet.
	class CellHoldCount final
	{
	public:
		bool TryHold()
		{
			auto value = state.load(std::memory_order_acquire);
			do {
				if ((value & Destroying) || (value & Holders) == Holders)
					return false;
			} while (!state.compare_exchange_weak(value, value + 1, std::memory_order_acq_rel, std::memory_order_acquire));
			return true;
		}

		void Release()
		{
			if ((state.fetch_sub(1, std::memory_order_release) & Holders) == 1)
				state.notify_all();
		}

		void BeginDestruction() { state.fetch_or(Destroying, std::memory_order_acq_rel); }

		bool IsBeingDestroyed() const { return (state.load(std::memory_order_acquire) & Destroying) != 0; }

		// Call after BeginDestruction, without holding the map, task or placement lock.
		void WaitUntilReleased() const
		{
			auto value = state.load(std::memory_order_acquire);
			while (value & Holders) {
				state.wait(value, std::memory_order_acquire);
				value = state.load(std::memory_order_acquire);
			}
		}

	private:
		inline static constexpr std::uint32_t Destroying = std::uint32_t{ 1 } << 31;
		inline static constexpr std::uint32_t Holders = Destroying - 1;
		std::atomic<std::uint32_t> state{ 0 };
	};

	class CellHolder final
	{
	public:
		CellHolder() = default;
		explicit CellHolder(std::shared_ptr<CellHoldCount> count)
		{
			if (count && count->TryHold())
				holds = std::move(count);
		}
		CellHolder(const CellHolder&) = delete;
		CellHolder& operator=(const CellHolder&) = delete;
		CellHolder(CellHolder&& other) noexcept : holds(std::move(other.holds)) {}
		CellHolder& operator=(CellHolder&& other) noexcept
		{
			if (this != &other) {
				Release();
				holds = std::move(other.holds);
			}
			return *this;
		}
		~CellHolder() { Release(); }
		explicit operator bool() const { return holds != nullptr; }

	private:
		void Release()
		{
			if (auto count = std::move(holds))
				count->Release();
		}

		std::shared_ptr<CellHoldCount> holds;
	};

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
		std::uint64_t taskGeneration = 0;

		std::atomic<TerrainLoad> terrainLoad{ TerrainLoad::None };
		// Stays set until a running terrain worker has returned.
		std::atomic<bool> loadingTerrain{ false };

		bool AdvanceTerrain(TerrainLoad from, TerrainLoad to);

		void ClearTask()
		{
			++taskGeneration;
			queued = false;
			wanted = GrassState::None;
		}

		void Reset()
		{
			ClearTask();
			have = GrassState::None;
			if (AdvanceTerrain(TerrainLoad::Queued, TerrainLoad::None) || AdvanceTerrain(TerrainLoad::Claimed, TerrainLoad::None))
				loadingTerrain = false;
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
		inline static constexpr auto DistantRetryDelay = std::chrono::seconds(1);

		std::vector<std::shared_ptr<CellInfo>> grid;
		std::vector<std::pair<int, int>> pendingCells;
		std::size_t nextPendingCell = 0;
		std::vector<std::pair<std::shared_ptr<CellInfo>, std::chrono::steady_clock::time_point>> inFlight;
		RE::TESWorldSpace* pendingWorld = nullptr;
		int pendingX = 0;
		int pendingY = 0;
		std::chrono::steady_clock::time_point nextRetry{};
	};

	// Stand-in cells let cache-only mode load grass without loading real cells or generating placements.
	class CacheOnlyCells final
	{
	public:
		// addType -1 unloads every cell regardless of radius.
		void Retain(const RE::TESWorldSpace* world, int addType, int centerX, int centerY, int radius);

		bool QueueLoad(RE::TESWorldSpace* world, int x, int y);

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
		// Initial exterior attachment need not call TES's separate grass-enable function.
		std::atomic<bool> gridUpdatePending{ true };
		std::atomic<bool> streamingAllowed{ false };
		std::atomic<bool> gridUpdatesEnabled{ true };

		void SuspendForLoad()
		{
			streamingAllowed = false;
			gridUpdatePending = true;
			// Loading pauses streaming; only the explicit disable hook changes the user's toggle.
		}
		std::unique_ptr<CellInfoContainer> cellGrid;
		std::unique_ptr<CacheOnlyCells> cacheOnlyCells;
		// Entries survive grid eviction until the cell destructor has waited for their holders.
		std::unordered_map<RE::TESObjectCELL*, std::shared_ptr<CellHoldCount>> cellHolds{ 1024 };
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

	bool ClearCellAddGrassTask(RE::TESObjectCELL* cell, const bool* taskAbort = nullptr);

	// Look up tracking by address before dereferencing a cell that may be undergoing destruction.
	CellHolder TryHoldTrackedCell(RE::TESObjectCELL* cell);

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

	// Visit the center first so nearby grass takes priority over the outer rings.
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
