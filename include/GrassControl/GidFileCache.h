#pragma once

#include "GrassControl/Util.h"

namespace GrassControl::GidFileCache
{
	void SetHelperInstalled(bool installed);

	// Use NGIO's .cgid extension and PC instance layout.
	void FixFileFormat();

	void EnableFileGeneration();
}

// Generation moves the player to load each grid through the engine.
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
		int doneCells = 0;
		int uGrids = 5;
		int uHalf = 2;
	};

	void InstallHooks();

	void ApplyPatches();

	void Enable();

	void Start();

	void Update();

	void OnGrassQueued();

	void OnGrassDispatch();

	void OnGrassCreated(RE::TESObjectCELL* cell);

	int GetGrassGridRadius();
}
