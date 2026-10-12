#pragma once

#include "GrassControl/Util.h"

#include <climits>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace RE
{
	class TESObjectCELL;

	class TESWorldSpace;
}

namespace GrassControl::GidFileCache
{
	void SetHelperInstalled(bool installed);

	void FixFileFormat(bool onlyLoad);

	void EnableFileGeneration();
}

namespace GrassControl::Generation
{
	class GidFileWorldGenerateTask;

	class GidFileCellGenerateTask;

	using ProgressSet = std::unordered_set<std::string, Util::CaseInsensitiveString::Hash, Util::CaseInsensitiveString::Equal>;

	class GidFileGenerationTask final
	{
	public:
		GidFileGenerationTask();

		bool RunOne();

		bool HasDone(const std::string& key, const std::string& wsName, int x = INT_MIN, int y = INT_MIN) const;

		void WriteProgressFile(const std::string& key, const std::string& wsName, int x = INT_MIN, int y = INT_MIN);

		int GetCompletedWorldspaces() const { return DoneWS; }

		int GetTotalWorldspaces() const { return TotalWS; }

	private:
		void Init();

		void Begin();

		void End();

		mutable std::mutex ProgressLocker;
		std::ofstream FileStream;
		bool IsResuming = false;
		ProgressSet ProgressDone;
		std::vector<std::unique_ptr<GidFileWorldGenerateTask>> WorldTodo;
		int DoneWS = 0;
		int TotalWS = 0;
		int _istate = 0;
	};

	class GidFileWorldGenerateTask final
	{
	public:
		GidFileWorldGenerateTask(GidFileGenerationTask& parent, RE::TESWorldSpace* ws, const std::string& wsName);

		GidFileGenerationTask& Parent;

		RE::TESWorldSpace* WorldSpace;

		std::string Name;

		int TotalCellDo = 0;

		int DidCellDo = 0;

		bool RunOne();

		void Remove(GidFileCellGenerateTask* t);

	private:
		std::list<GidFileCellGenerateTask*> CellTodo{};

		std::vector<std::unique_ptr<GidFileCellGenerateTask>> _grid{};

		void Free();

		void Begin();

		void End() const;

		int _istate = 0;

		[[nodiscard]] GidFileCellGenerateTask* GetGrid(int x, int y) const;

		int uhalf = 0;
		int ugrid = 0;

		[[nodiscard]] GidFileCellGenerateTask* FindBestTodo() const;
	};

	class GidFileCellGenerateTask final
	{
	public:
		GidFileCellGenerateTask(GidFileWorldGenerateTask& parent, int x, int y);

		GidFileWorldGenerateTask& Parent;

		const int X;

		const int Y;

		bool RunOne();

	private:
		RE::TESObjectCELL* Cell{};

		bool Begin();

		void End();

		bool Process();
	};

	void InstallHooks();

	void ApplyPatches();

	void Start();

	void Update();

	void Enable();

	void OnGrassQueued();

	void OnGrassDispatch();

	void OnGrassCreated(RE::TESObjectCELL* cell);

	int GetGrassGridRadius();
}
