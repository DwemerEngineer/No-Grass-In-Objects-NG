#include "GrassControl/GidFileCache.h"

#include "GrassControl/AddCellGrassTask.h"
#include "GrassControl/Config.h"
#include "GrassControl/Engine.h"
#include "GrassControl/Globals.h"
#include "GrassControl/Util.h"

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

namespace GrassControl::Generation
{
	// Progress keys are persisted, so their format must remain compatible with existing progress files.
	static std::string WorldKey(std::string_view world)
	{
		return world.empty() ? std::string("ws") : fmt::format("ws_{}", world);
	}

	static std::string CellKey(std::string_view world, int x, int y)
	{
		return world.empty() ? fmt::format("cell_{}_{}", x, y) : fmt::format("cell_{}_{}_{}", world, x, y);
	}

	CacheGenerator::CacheGenerator()
	{
		progressFile.open(Util::GetProgressFilePath(), std::ios::app);
		if (!progressFile)
			stl::report_and_fail("Unable to open the grass generation progress file for writing.");
	}

	bool CacheGenerator::Step()
	{
		if (!prepared) {
			prepared = true;
			Prepare();
		}

		LogStepTime();

		if (worlds.empty()) {
			Finish();
			return false;
		}

		if (!worldStarted) {
			worldStarted = true;
			BeginWorld();
		}

		for (;;) {
			std::optional<std::pair<int, int>> retry;
			{
				std::scoped_lock lock(mutex);
				for (auto it = retryCells.begin(); it != retryCells.end();) {
					const auto [x, y] = *it;
					it = retryCells.erase(it);
					if (x < -GridHalf || x > GridHalf || y < -GridHalf || y > GridHalf || done.contains(CellKey(worlds.back().name, x, y)))
						continue;

					retry = { x, y };
					break;
				}
			}
			if (retry && !IsPending(retry->first, retry->second)) {
				pending[Index(retry->first, retry->second)] = true;
				++pendingCount;
				--doneCells;
			}
			if (!pendingCount)
				break;

			const auto [x, y] = retry ? *retry : NextCell();
			if (!VisitCell(x, y)) {
				Clear(x, y, true);
				continue;
			}

			for (int cellX = x - uHalf; cellX <= x + uHalf; ++cellX) {
				for (int cellY = y - uHalf; cellY <= y + uHalf; ++cellY)
					Clear(cellX, cellY);
			}

			stepCell = { x, y };
			stepStarted = std::chrono::steady_clock::now();
			stepBuildMicroseconds = 0;
			stepBuiltCells = 0;
			return true;
		}

		MarkDone(WorldKey(worlds.back().name));
		worlds.pop_back();
		worldStarted = false;
		++doneWorlds;
		OnGrassDispatch();

		return true;
	}

	void CacheGenerator::LogStepTime()
	{
		if (!stepCell)
			return;

		const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stepStarted).count();
		logger::info("Grid at ({}, {}) took {} ms; {} cells spent {} ms building grass", stepCell->first, stepCell->second, total, stepBuiltCells.load(), stepBuildMicroseconds.load() / 1000);
		stepCell.reset();
	}

	void CacheGenerator::MarkCell(std::string_view world, int x, int y)
	{
		MarkDone(CellKey(world, x, y));
	}

	void CacheGenerator::RetryCell(int x, int y)
	{
		std::scoped_lock lock(mutex);
		retryCells.emplace(x, y);
	}

	void CacheGenerator::Prepare()
	{
		const auto progressPath = Util::GetProgressFilePath();
		if (std::ifstream progress{ progressPath }) {
			std::scoped_lock lock(mutex);
			for (std::string line; std::getline(progress, line);) {
				line = Util::StringHelpers::Trim(line);
				if (!line.empty())
					done.insert(line);
			}
			if (progress.bad())
				stl::report_and_fail("Unable to read the complete grass generation progress file.");
		}

		const bool resuming = !done.empty();
		if (!resuming) {
			const auto directory = Util::Files::GamePath(L"Data/Grass");
			std::filesystem::create_directories(directory);
			if (!Config::Updating) {
				for (const auto& entry : std::filesystem::directory_iterator(directory)) {
					const auto extension = entry.path().extension();
					if (extension == ".dgid" || extension == ".cgid" || extension == ".fail")
						std::filesystem::remove(entry.path());
				}
			}
		}

		const auto message = resuming ?
		                         fmt::format("Resuming grass cache generation now with {} entries completed.\n\nThis will take a while !\n\nIf the game crashes you can run it again to resume.\n\nWhen all is finished the game will say.\n\nOpen console to see progress.", done.size()) :
		                         std::string("Generating new grass cache now.\n\nThis will take a while!\n\nIf the game crashes you can run it again to resume.\n\nWhen all is finished the game will say.\n\nOpen console to see progress.");
		logger::info("{}", message);
		RE::DebugMessageBox(message.c_str());

		const auto parseList = [](const std::string& text, const char* label) {
			ProgressSet names;
			for (auto& name : Util::StringHelpers::Split(text, ';', true)) {
				name = Util::StringHelpers::Trim(name);
				logger::info("{}: {}", label, name);
				names.insert(name);
			}
			return names;
		};

		const auto skip = parseList(Config::SkipPregenerateWorldSpaces, "Skipping");
		const auto only = parseList(Config::OnlyPregenerateWorldSpaces, "Only Generating");

		for (auto* world : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESWorldSpace>()) {
			std::string name = world->editorID.c_str();
			if (!only.empty() ? name.empty() || !only.contains(name) : !name.empty() && skip.contains(name))
				continue;

			++totalWorlds;
			if (HasDone(WorldKey(name)))
				++doneWorlds;
			else
				worlds.push_back({ world, std::move(name) });
		}
	}

	void CacheGenerator::BeginWorld()
	{
		uGrids = globals::GridsToLoad();
		uHalf = uGrids / 2;
		pending.assign(GridSide * GridSide, false);
		pendingCount = 0;
		doneCells = 0;

		const auto& name = worlds.back().name;
		for (int x = -GridHalf; x <= GridHalf; ++x) {
			for (int y = -GridHalf; y <= GridHalf; ++y) {
				if (HasDone(CellKey(name, x, y))) {
					++doneCells;
				} else {
					pending[Index(x, y)] = true;
					++pendingCount;
				}
			}
		}
	}

	std::size_t CacheGenerator::Index(int x, int y)
	{
		return static_cast<std::size_t>((x + GridHalf) * GridSide + y + GridHalf);
	}

	bool CacheGenerator::IsPending(int x, int y) const
	{
		return x >= -GridHalf && x <= GridHalf && y >= -GridHalf && y <= GridHalf && pending[Index(x, y)];
	}

	void CacheGenerator::Clear(int x, int y, bool completed)
	{
		if (!IsPending(x, y))
			return;

		pending[Index(x, y)] = false;
		--pendingCount;
		++doneCells;
		if (completed)
			MarkCell(worlds.back().name, x, y);
	}

	// The pending cell whose loaded grid covers the most other pending cells.
	std::pair<int, int> CacheGenerator::NextCell() const
	{
		std::pair<int, int> best{ 0, 0 };
		int bestCount = -1;
		for (int x = -GridHalf; x <= GridHalf; ++x) {
			for (int y = -GridHalf; y <= GridHalf; ++y) {
				if (!pending[Index(x, y)])
					continue;

				int count = 0;
				for (int cellX = x - uHalf; cellX <= x + uHalf; ++cellX) {
					for (int cellY = y - uHalf; cellY <= y + uHalf; ++cellY)
						count += IsPending(cellX, cellY);
				}

				if (count > bestCount) {
					best = { x, y };
					bestCount = count;
					if (bestCount >= uGrids * uGrids)
						return best;
				}
			}
		}

		return best;
	}

	bool CacheGenerator::VisitCell(int x, int y)
	{
		auto& [world, name] = worlds.back();
		RE::TESObjectCELL* cell = nullptr;
		{
			RE::BSSpinLockGuard lock(RE::TES::GetSingleton()->lock);
			cell = RE::GetWorldCell(world, x, y);
		}
		if (!cell)
			return false;

		const auto failFile = Util::Files::GamePath(fmt::format("Data/Grass/{}x{:04}y{:04}.fail", name, x, y));
		int fails = 1;
		bool skip = false;

		const auto countFailure = [&](std::string& contents) {
			const auto text = Util::TrimView(contents);
			const auto [end, result] = std::from_chars(text.data(), text.data() + text.size(), fails);
			if (text.empty() || result != std::errc{} || end != text.data() + text.size() || fails <= 0)
				return false;

			if (fails >= Config::MaxFailures) {
				skip = true;
				return true;
			}

			logger::info("{}({},{}) failed {} times; trying again", name, x, y, fails);
			contents = std::to_string(++fails) + "\n";
			return true;
		};

		// The fail count is written before moving, so a cell that crashes the game is skipped after enough attempts.
		std::error_code error;
		bool saved = Util::Files::UpdateText(failFile, countFailure, error);

		if (!saved && Util::Files::IsMissing(error)) {
			saved = Util::Files::CreateText(failFile, "1\n", error);
			if (!saved && error == std::errc::file_exists)
				saved = Util::Files::UpdateText(failFile, countFailure, error);
		}

		if (!saved)
			stl::report_and_fail(fmt::format("Cannot record grass generation failures for {}: {}", Util::Files::PathForLog(failFile), error.message()));

		if (skip) {
			logger::info("{}({},{}) failed {} times; skipping", name, x, y, fails);
			return false;
		}

		const double percent = std::clamp(static_cast<double>(doneCells) / (GridSide * GridSide), 0.0, 1.0) * 100.0;
		const auto message = fmt::format("Generating grass for {}({}, {}) {:f} pct, world {} out of {}", name, x, y, percent, doneWorlds + 1, totalWorlds);
		RE::ConsoleLog::GetSingleton()->Print("%s", message.c_str());
		logger::info("{}", message);

		const auto* coordinates = cell->GetCoordinates();
		std::array<float, 8> positionAndRotation{};
		positionAndRotation[0] = static_cast<float>(coordinates->cellX) * 4096.0f + 2048.0f;
		positionAndRotation[1] = static_cast<float>(coordinates->cellY) * 4096.0f + 2048.0f;
		RE::PositionPlayer(RE::PlayerCharacter::GetSingleton(), positionAndRotation.data(), positionAndRotation.data() + 4, cell);

		return true;
	}

	void CacheGenerator::Finish()
	{
		progressFile.close();
		std::error_code error;
		std::filesystem::remove(Util::GetProgressFilePath(), error);
	}

	bool CacheGenerator::HasDone(const std::string& key) const
	{
		std::scoped_lock lock(mutex);
		return done.contains(key);
	}

	void CacheGenerator::MarkDone(const std::string& key)
	{
		std::scoped_lock lock(mutex);
		if (!done.insert(key).second)
			return;

		progressFile << key << std::endl;
		if (!progressFile)
			stl::report_and_fail("Unable to save grass generation progress.");

		lastActivity = GetTickCount64();
	}

	static void RunFreezeCheck()
	{
		while (runState == RunState::Running) {
			const auto last = lastActivity.load();
			const auto now = GetTickCount64();
			if (now - last < static_cast<std::uint64_t>(Config::FreezeTime)) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				continue;
			}

			RE::DebugMessageBox("Grass generation appears to have frozen! Restart the game.");
			Util::ReportAndFailTimed("Grass generation appears to have frozen! Restart the game.");
		}
	}

	void Enable()
	{
		patchesApplied = true;
	}

	void Start()
	{
		if (!patchesApplied || generator)
			return;

		GidFileCache::EnableFileGeneration();
		generator = std::make_unique<CacheGenerator>();
		lastActivity = GetTickCount64();
		runState = RunState::Running;

		if (Config::FreezeCheck)
			std::thread(RunFreezeCheck).detach();
	}

	void Update()
	{
		if (!patchesApplied || runState != RunState::Running || queuedGrass.load() != 0 || awaitingDispatch.exchange(true))
			return;

		lastActivity = GetTickCount64();
		if (!generator->Step()) {
			runState = RunState::Complete;
			RE::ConsoleLog::GetSingleton()->Print("Grass generation finished successfully!");
			stl::report_and_fail("Grass generation finished successfully!");
		}
	}

	void OnGrassQueued()
	{
		++queuedGrass;
	}

	void OnGrassDispatch()
	{
		awaitingDispatch = false;
	}

	void OnGrassCreated(RE::TESObjectCELL* cell, bool* abort, std::chrono::steady_clock::duration buildTime)
	{
		if (!patchesApplied)
			return;

		stepBuildMicroseconds += std::chrono::duration_cast<std::chrono::microseconds>(buildTime).count();
		++stepBuiltCells;

		// Start publishes the generator before the running state.
		if (runState == RunState::Running) {
			if (*abort) {
				const auto& task = RE::AddCellGrassTask::FromAbort(abort)->GetRuntimeData();
				generator->RetryCell(task.cellX, task.cellY);
			} else if (cell) {
				const auto* world = cell->GetRuntimeData().worldSpace;
				const auto* coordinates = cell->GetCoordinates();
				if (world && coordinates) {
					generator->MarkCell(world->editorID.c_str(), coordinates->cellX, coordinates->cellY);
					std::error_code error;
					std::filesystem::remove(Util::Files::GamePath(fmt::format("Data/Grass/{}x{:04}y{:04}.fail", world->editorID.c_str(), coordinates->cellX, coordinates->cellY)), error);
				}
			}
		}

		auto queued = queuedGrass.load();
		while (queued > 0 && !queuedGrass.compare_exchange_weak(queued, queued - 1)) {}
	}

	int GetGrassGridRadius()
	{
		return (globals::GridsToLoad() - 1) / 2;
	}
}
