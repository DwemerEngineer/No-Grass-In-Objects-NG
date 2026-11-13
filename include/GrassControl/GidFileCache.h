#pragma once

#include "GrassControl/Util.h"

namespace GrassControl::GidFileCache
{
	inline bool cacheHelperInstalled = false;

	// Use .cgid files and the PC instance layout
	void FixFileFormat();

	void EnableFileGeneration();
}

namespace GrassControl::Generation
{
	class CacheGenerator final
	{
	public:
		CacheGenerator();

		bool Step();
		void MarkCell(std::string_view world, int x, int y);

	private:
		using ProgressSet = std::unordered_set<std::string, Util::CaseInsensitiveString::Hash, Util::CaseInsensitiveString::Equal>;

		inline static constexpr int GridHalf = 64;
		inline static constexpr int GridSide = GridHalf * 2 + 1;

		struct World
		{
			RE::TESWorldSpace* world;
			std::string name;
		};

		void Prepare();
		void BeginWorld();
		static std::size_t Index(int x, int y);
		bool IsPending(int x, int y) const;
		void Clear(int x, int y, bool completed = false);
		std::pair<int, int> NextCell() const;
		bool VisitCell(int x, int y);
		void Finish();
		bool HasDone(const std::string& key) const;
		void MarkDone(const std::string& key);
		void FinishStep();

		mutable std::mutex mutex;
		std::ofstream progressFile;
		ProgressSet done;
		std::vector<World> worlds;
		int totalWorlds = 0;
		int doneWorlds = 0;
		bool prepared = false;
		bool worldStarted = false;
		std::vector<bool> pending;
		int pendingCount = 0;
		int totalCells = 0;
		int doneCells = 0;
		int uGrids = 5;
		int uHalf = 2;
		std::optional<std::pair<int, int>> stepCell;
		std::chrono::steady_clock::time_point stepStarted;
	};

	enum class RunState
	{
		Idle,
		Running,
		Complete
	};

	inline std::unique_ptr<CacheGenerator> generator;
	inline std::atomic<RunState> runState{ RunState::Idle };
	inline std::atomic<std::int64_t> queuedGrass{ 0 };
	// Wait for grass dispatch after moving the player
	inline std::atomic<bool> awaitingDispatch{ false };
	inline std::atomic<std::uint64_t> lastActivity{ 0 };
	inline std::atomic<bool> patchesApplied{ false };
	inline std::atomic<std::int64_t> stepBuildMicroseconds{ 0 };
	inline std::atomic<int> stepBuiltCells{ 0 };

	void InstallHooks();

	void ApplyPatches();

	void Enable();

	void Start();

	void Update();

	void OnGrassQueued();

	void OnGrassDispatch();

	void OnGrassCreated(RE::TESObjectCELL* cell, bool* abort, std::chrono::steady_clock::duration buildTime);

	int GetGrassGridRadius();
}
