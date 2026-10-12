#include "GrassControl/GidFileCache.h"
#include "CasualLibrary.hpp"
#include "GrassControl/Config.h"
#include "GrassControl/Util.h"

#include <atomic>
#include <climits>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace GrassControl::GidFileCache
{
	void EnableFileGeneration()
	{
		auto* setting = RE::INISettingCollection::GetSingleton()->GetSetting("bGenerateGrassDataFiles:Grass");
		if (!setting) {
			stl::report_and_fail("Unable to find bGenerateGrassDataFiles:Grass.");
		}

		setting->data.b = true;

		// The manager caches this setting at construction; later INI writes do not update it.
		if (auto* manager = RE::BGSGrassManager::GetSingleton()) {
			manager->generateGrassDataFiles = true;
		}
	}
}

namespace
{
	using namespace GrassControl;
	using namespace GrassControl::Generation;

	enum class RunState
	{
		Idle,
		Running,
		Complete
	};

	std::unique_ptr<GidFileGenerationTask> currentTask;
	std::atomic<RunState> runState{ RunState::Idle };
	std::atomic<std::int64_t> queuedGrassCounter{ 0 };
	std::atomic<std::int64_t> queuedGrassMode{ 0 };
	std::atomic<std::uint64_t> lastActivity{ 0 };
	std::atomic<bool> patchesApplied{ false };
	const std::string KeyWS = "ws";
	const std::string KeyCell = "cell";

	std::string GenerateProgressKey(const std::string& key, const std::string& wsName, const int x, const int y)
	{
		std::string bld;
		if (!key.empty()) {
			bld.append(key);
		}

		if (!wsName.empty()) {
			if (!bld.empty()) {
				bld += '_';
			}

			bld.append(wsName);
		}

		if (x != INT_MIN) {
			if (!bld.empty()) {
				bld += '_';
			}

			bld.append(std::to_string(x));
		}

		if (y != INT_MIN) {
			if (!bld.empty()) {
				bld += '_';
			}

			bld.append(std::to_string(y));
		}

		return bld;
	}

	void RunFreezeCheck()
	{
		while (true) {
			if (runState != RunState::Running)
				break;

			uint64_t last = lastActivity.load();
			uint64_t now = GetTickCount64();

			if (now - last < Config::FreezeTime) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				continue;
			}

			RE::DebugMessageBox("Grass generation appears to have frozen! Restart the game.");
			Util::ReportAndFailTimed("Grass generation appears to have frozen! Restart the game.");
		}
	}
}

namespace GrassControl::Generation
{
	GidFileGenerationTask::GidFileGenerationTask()
	{
		FileStream.open(Util::GetProgressFilePath(), std::ios::app);
		if (!FileStream) {
			stl::report_and_fail("Unable to open the grass generation progress file for writing.");
		}
	}

	void GidFileGenerationTask::Init()
	{
		auto fi = std::filesystem::path(Util::GetProgressFilePath());
		if (exists(fi)) {
			{
				std::scoped_lock lock(ProgressLocker);

				std::ifstream fs;
				try {
					fs = std::ifstream(Util::GetProgressFilePath());
					if (!fs) {
						throw std::system_error(errno, std::system_category(), "failed to open " + Util::Files::PathForLog(Util::GetProgressFilePath()));
					}
				} catch (const std::system_error& e) {
					logger::error("Error reading PrecacheGrass.txt: {}", e.what());
					stl::report_and_fail("Unable to read the grass generation progress file.");
				}

				std::string l;
				while (std::getline(fs, l)) {
					l = Util::StringHelpers::trim(l);

					if (l.empty())
						continue;

					ProgressDone.insert(l);
				}

				if (fs.bad()) {
					stl::report_and_fail("Unable to read the complete grass generation progress file.");
				}
			}

			IsResuming = !ProgressDone.empty();
		}

		if (ProgressDone.empty()) {
			auto dir = Util::Files::GamePath(L"Data/Grass");
			if (!exists(dir)) {
				create_directories(dir);
			}

			if (!Config::Updating) {
				for (const auto& entry : std::filesystem::directory_iterator(dir)) {
					if (entry.path().extension() == ".dgid" || entry.path().extension() == ".cgid" || entry.path().extension() == ".fail") {
						remove(entry.path());
					}
				}
			}
		}

		std::string tx;
		if (IsResuming) {
			auto entries = ProgressDone.size();
			tx = "Resuming grass cache generation now with " + std::to_string(entries) + " entries completed.\n\nThis will take a while !\n\nIf the game crashes you can run it again to resume.\n\nWhen all is finished the game will say.\n\nOpen console to see progress.";
		} else {
			tx = "Generating new grass cache now.\n\nThis will take a while!\n\nIf the game crashes you can run it again to resume.\n\nWhen all is finished the game will say.\n\nOpen console to see progress.";
		}

		logger::info("{}", tx);
		RE::DebugMessageBox(tx.c_str());
	}

	bool GidFileGenerationTask::HasDone(const std::string& key, const std::string& wsName, const int x, const int y) const
	{
		std::string text = GenerateProgressKey(key, wsName, x, y);
		{
			std::scoped_lock lock(ProgressLocker);

			return ProgressDone.contains(text);
		}
	}

	void GidFileGenerationTask::WriteProgressFile(const std::string& key, const std::string& wsName, const int x, const int y)
	{
		std::string text = GenerateProgressKey(key, wsName, x, y);
		if (text.empty())
			return;

		{
			std::scoped_lock lock(ProgressLocker);

			auto [fst, snd] = ProgressDone.insert(text);
			if (!snd)
				return;

			FileStream << text << std::endl;
			if (!FileStream) {
				stl::report_and_fail("Unable to save grass generation progress.");
			}
		}
	}

	void GidFileGenerationTask::Begin()
	{
		char Char = ';';
		auto skip = Util::StringHelpers::split(Config::SkipPregenerateWorldSpaces, Char, true);
		ProgressSet skipSet;
		for (auto& x : skip) {
			x = Util::StringHelpers::trim(x);
			logger::info("Skipping: {}", x);
			skipSet.insert(x);
		}

		auto only = Util::StringHelpers::split(Config::OnlyPregenerateWorldSpaces, Char, true);
		ProgressSet onlySet;
		for (auto& x : only) {
			x = Util::StringHelpers::trim(x);
			logger::info("Only Generating: {}", x);
			onlySet.insert(x);
		}

		auto& all = RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESWorldSpace>();
		for (auto f : all) {
			auto ws = f;

			std::string name = ws->editorID.c_str();

			if (!onlySet.empty()) {
				if (name.empty() || !onlySet.contains(name))
					continue;
			} else if (!name.empty() && skipSet.contains(name))
				continue;

			TotalWS++;

			if (HasDone(KeyWS, name)) {
				DoneWS++;
				continue;
			}

			WorldTodo.push_back(std::make_unique<GidFileWorldGenerateTask>(*this, ws, name));
		}
	}

	void GidFileGenerationTask::End()
	{
		if (FileStream) {
			FileStream.close();
		}

		auto fi = std::filesystem::path(Util::GetProgressFilePath());
		if (exists(fi)) {
			remove(fi);
		}
	}

	bool GidFileGenerationTask::RunOne()
	{
		if (_istate == 0) {
			_istate = 1;

			Init();

			Begin();
		}

		if (!WorldTodo.empty()) {
			auto& t = WorldTodo.back();

			if (!t->RunOne()) {
				WorldTodo.pop_back();
				DoneWS++;

				OnGrassDispatch();
			}

			return true;
		}

		if (_istate == 1) {
			_istate = 2;

			End();
		}

		return false;
	}

	GidFileWorldGenerateTask::GidFileWorldGenerateTask(GidFileGenerationTask& parent, RE::TESWorldSpace* ws, const std::string& wsName) :
		Parent(parent), WorldSpace(ws), Name(wsName)
	{
		_grid.resize(129 * 129);

		ugrid = Memory::Internal::read<int>(RELOCATION_ID(501244, 359675).address() + 8);
		uhalf = ugrid / 2;
	}

	void GidFileWorldGenerateTask::Free()
	{
		_grid.clear();
	}

	void GidFileWorldGenerateTask::Begin()
	{
		int min = -64;
		int max = 64;

		TotalCellDo = 129 * 129;

		for (int x = min; x <= max; x++) {
			for (int y = min; y <= max; y++) {
				if (Parent.HasDone(KeyCell, Name, x, y)) {
					DidCellDo++;
					continue;
				}

				auto cg = std::make_unique<GidFileCellGenerateTask>(*this, x, y);
				CellTodo.push_back(cg.get());
				_grid[(x + 64) * 129 + (y + 64)] = std::move(cg);
			}
		}
	}

	void GidFileWorldGenerateTask::End() const
	{
		Parent.WriteProgressFile(KeyWS, Name);
	}

	GidFileCellGenerateTask* GidFileWorldGenerateTask::GetGrid(int x, int y) const
	{
		x += 64;
		y += 64;
		if (x < 0 || x > 128 || y < 0 || y > 128) {
			return nullptr;
		}

		return _grid[x * 129 + y].get();
	}

	GidFileCellGenerateTask* GidFileWorldGenerateTask::FindBestTodo() const
	{
		GidFileCellGenerateTask* best = nullptr;
		int bestCount = 0;

		int ufull = ugrid * ugrid;

		for (auto& n : CellTodo) {
			int minx = n->X - uhalf;
			int maxx = n->X + uhalf;
			int miny = n->Y - uhalf;
			int maxy = n->Y + uhalf;
			int cur = 0;

			for (int x = minx; x <= maxx; x++) {
				for (int y = miny; y <= maxy; y++) {
					auto t = GetGrid(x, y);
					if (t != nullptr)
						cur++;
				}
			}

			if (cur == 0) {
				stl::report_and_fail("Grass generation has crashed!");
			}

			if (best == nullptr || cur > bestCount) {
				best = n;
				bestCount = cur;

				if (bestCount >= ufull)
					break;
			}
		}

		return best;
	}

	bool GidFileWorldGenerateTask::RunOne()
	{
		if (_istate == 0) {
			_istate = 1;

			Begin();
		}

		while (!CellTodo.empty()) {
			auto t = FindBestTodo();
			if (t == nullptr)
				stl::report_and_fail("Grass generation has crashed!");

			if (!t->RunOne()) {
				Parent.WriteProgressFile(KeyCell, Name, t->X, t->Y);
				Remove(t);
				DidCellDo++;
				continue;
			}

			int minx = t->X - uhalf;
			int maxx = t->X + uhalf;
			int miny = t->Y - uhalf;
			int maxy = t->Y + uhalf;
			for (int x = minx; x <= maxx; x++) {
				for (int y = miny; y <= maxy; y++) {
					auto tx = GetGrid(x, y);
					if (tx != nullptr) {
						Remove(tx);
						Parent.WriteProgressFile(KeyCell, Name, x, y);
						DidCellDo++;
					}
				}
			}

			return true;
		}

		if (_istate == 1) {
			_istate = 2;

			End();

			Free();
		}

		return false;
	}

	void GidFileWorldGenerateTask::Remove(GidFileCellGenerateTask* t)
	{
		CellTodo.remove(t);

		int x = t->X + 64;
		int y = t->Y + 64;
		int ix = x * 129 + y;
		if (_grid[ix].get() == t) {
			_grid[ix].reset();
		} else {
			Util::ReportAndFailTimed("Grass generation has crashed!");
		}
	}

	GidFileCellGenerateTask::GidFileCellGenerateTask(GidFileWorldGenerateTask& parent, const int x, const int y) :
		Parent(parent), X(x), Y(y)
	{}

	bool GidFileCellGenerateTask::Begin()
	{
		RE::BSSpinLockGuard lock(RE::TES::GetSingleton()->lock);

		RE::TESObjectCELL* cellPtr = nullptr;

		REL::Relocation<RE::TESObjectCELL* (*)(RE::TESWorldSpace*, int32_t, int32_t)> Func2{ RELOCATION_ID(20026, 20460) };
		cellPtr = Func2(this->Parent.WorldSpace, this->X, this->Y);

		if (cellPtr == nullptr)
			return false;

		Cell = cellPtr;

		return true;
	}

	void GidFileCellGenerateTask::End()
	{
		Cell = nullptr;
	}

	bool GidFileCellGenerateTask::Process()
	{
		if (Cell == nullptr)
			return false;

		const auto fileKey = Util::Files::GamePath(fmt::format("Data/Grass/{}x{:04}y{:04}.fail", this->Parent.Name, this->X, this->Y));
		int fails = 1;
		bool skip = false;
		const auto updateCount = [&](std::string& contents) {
			const auto text = Util::TrimView(contents);
			if (text.empty()) {
				return false;
			}

			const auto [end, result] = std::from_chars(text.data(), text.data() + text.size(), fails);
			if (result != std::errc{} || end != text.data() + text.size() || fails <= 0) {
				return false;
			}

			if (fails >= Config::MaxFailures) {
				skip = true;
				return true;
			}

			logger::info("{}({},{}) failed {} times; trying again", this->Parent.Name, this->X, this->Y, fails);
			contents = std::to_string(++fails) + "\n";
			return true;
		};

		std::error_code error;
		bool saved = Util::Files::UpdateText(fileKey, updateCount, error);
		if (!saved && Util::Files::IsMissing(error)) {
			saved = Util::Files::CreateText(fileKey, "1\n", error);
			if (!saved && error == std::errc::file_exists) {
				saved = Util::Files::UpdateText(fileKey, updateCount, error);
			}
		}

		if (!saved) {
			stl::report_and_fail(fmt::format("Cannot record grass generation failures for {}: {}", Util::Files::PathForLog(fileKey), error.message()));
		}

		if (skip) {
			logger::info("{}({},{}) failed {} times; skipping", this->Parent.Name, this->X, this->Y, fails);
			return false;
		}

		double pct = 0.0;
		if (Parent.TotalCellDo > 0) {
			pct = std::max(0.0, std::min(static_cast<double>(Parent.DidCellDo) / static_cast<double>(Parent.TotalCellDo), 1.0)) * 100.0;
		}

		const std::string msg = "Generating grass for " + this->Parent.Name + "(" + std::to_string(this->X) + ", " + std::to_string(this->Y) + ") " + std::to_string(pct) + " pct, world " + std::to_string(Parent.Parent.GetCompletedWorldspaces() + 1) + " out of " + std::to_string(Parent.Parent.GetTotalWorldspaces());
		RE::ConsoleLog::GetSingleton()->Print("%s", msg.c_str());
		logger::info("{}", msg);
		{
			std::array<float, 8> positionAndRotation{};
			auto* alloc = positionAndRotation.data();

			Memory::Internal::write<float>(reinterpret_cast<uintptr_t>(alloc), static_cast<float>(Cell->GetCoordinates()->cellX) * 4096.0f + 2048.0f);
			Memory::Internal::write<float>(reinterpret_cast<uintptr_t>(alloc) + 4, static_cast<float>(Cell->GetCoordinates()->cellY) * 4096.0f + 2048.0f);
			Memory::Internal::write<float>(reinterpret_cast<uintptr_t>(alloc) + 8, 0.0f);

			REL::Relocation<void (*)(RE::PlayerCharacter*, uintptr_t, uintptr_t, RE::TESObjectCELL*, int)> func{ RELOCATION_ID(39657, 40744) };
			func(RE::PlayerCharacter::GetSingleton(), reinterpret_cast<uintptr_t>(alloc), reinterpret_cast<uintptr_t>(alloc) + 0x10, Cell, 0);
		}

		if (std::filesystem::exists(fileKey)) {
			std::filesystem::remove(fileKey);
		}

		return true;
	}

	bool GidFileCellGenerateTask::RunOne()
	{
		bool did = Begin();

		if (did && !Process()) {
			did = false;
		}

		End();

		return did;
	}

	void Start()
	{
		if (!patchesApplied || currentTask) {
			return;
		}

		GidFileCache::EnableFileGeneration();
		currentTask = std::make_unique<GidFileGenerationTask>();
		lastActivity = GetTickCount64();
		runState = RunState::Running;
		if (Config::FreezeCheck) {
			std::thread(RunFreezeCheck).detach();
		}
	}

	void Update()
	{
		if (!patchesApplied)
			return;

		if (runState == RunState::Running) {
			if (queuedGrassCounter.load() != 0)
				return;

			if (queuedGrassMode.exchange(1) != 0)
				return;

			lastActivity = GetTickCount64();

			if (!currentTask->RunOne()) {
				runState = RunState::Complete;

				RE::ConsoleLog::GetSingleton()->Print("Grass generation finished successfully!");
				stl::report_and_fail("Grass generation finished successfully!");
			}
		}
	}

	void Enable()
	{
		patchesApplied = true;
	}

	void OnGrassQueued()
	{
		++queuedGrassCounter;
	}

	void OnGrassDispatch()
	{
		queuedGrassMode = 0;
	}

	void OnGrassCreated(RE::TESObjectCELL* cell)
	{
		if (!patchesApplied) {
			return;
		}

		// Start publishes the task before the running state. Worker callbacks
		// must acquire that publication before accessing its progress stream.
		if (runState == RunState::Running && cell) {
			auto* world = cell->GetRuntimeData().worldSpace;
			auto* coordinates = cell->GetCoordinates();
			if (world && coordinates) {
				currentTask->WriteProgressFile(KeyCell, world->editorID.c_str(), coordinates->cellX, coordinates->cellY);
			}
		}

		--queuedGrassCounter;
	}

	int GetGrassGridRadius()
	{
		static int chosenGrassGridRadius = -1;
		int r = chosenGrassGridRadius;
		if (r < 0) {
			int ugrid = Memory::Internal::read<int>(RELOCATION_ID(501244, 359675).address() + 8);
			r = (ugrid - 1) / 2;

			chosenGrassGridRadius = r;
		}

		return r;
	}
}
