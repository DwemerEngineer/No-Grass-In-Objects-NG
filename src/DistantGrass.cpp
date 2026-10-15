#include "GrassControl/DistantGrass.h"

#include "GrassControl/AddCellGrassTask.h"
#include "GrassControl/CliffSurfaces.h"
#include "GrassControl/Config.h"
#include "GrassControl/Engine.h"
#include "GrassControl/Globals.h"
#include "GrassControl/GrassCellCreation.h"
#include "GrassControl/GrassPatchCreation.h"
#include "GrassControl/PhysicsRaycast.h"
#include "GrassControl/Util.h"

namespace GrassControl::DistantGrass
{
	bool GrassCacheIndex::Contains(std::string_view world, int x, int y)
	{
		std::call_once(scanned, [this] { Scan(); });

		std::shared_lock lock(mutex);
		const auto it = worlds.find(Lower(world));

		return it != worlds.end() && it->second.contains(Key(x, y));
	}

	bool GrassCacheIndex::Empty()
	{
		std::call_once(scanned, [this] { Scan(); });

		std::shared_lock lock(mutex);
		return worlds.empty();
	}

	void GrassCacheIndex::Insert(std::string_view world, int x, int y)
	{
		std::unique_lock lock(mutex);
		worlds[Lower(world)].insert(Key(x, y));
	}

	std::string GrassCacheIndex::Lower(std::string_view text)
	{
		std::string result(text);
		std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return result;
	}

	std::uint32_t GrassCacheIndex::Key(int x, int y)
	{
		return static_cast<std::uint32_t>(static_cast<std::uint16_t>(x)) << 16 | static_cast<std::uint16_t>(y);
	}

	void GrassCacheIndex::AddFile(std::string name)
	{
		constexpr std::string_view extension = ".cgid";
		if (name.size() <= extension.size() || !name.ends_with(extension))
			return;

		name.resize(name.size() - extension.size());
		const auto ySplit = name.rfind('y');
		if (ySplit == std::string::npos || ySplit == 0)
			return;

		const auto xSplit = name.rfind('x', ySplit - 1);
		if (xSplit == std::string::npos || xSplit == 0)
			return;

		int x = 0;
		int y = 0;

		const char* xEnd = name.data() + ySplit;
		const char* yEnd = name.data() + name.size();
		const auto parsedX = std::from_chars(name.data() + xSplit + 1, xEnd, x);
		const auto parsedY = std::from_chars(name.data() + ySplit + 1, yEnd, y);

		if (parsedX.ec != std::errc{} || parsedX.ptr != xEnd || parsedY.ec != std::errc{} || parsedY.ptr != yEnd ||
			x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX)
			return;

		worlds[name.substr(0, xSplit)].insert(Key(x, y));
	}

	void GrassCacheIndex::Scan()
	{
		const auto started = std::chrono::steady_clock::now();
		std::unique_lock lock(mutex);
		std::size_t files = 0;
		std::error_code error;
		for (std::filesystem::directory_iterator it(Util::Files::GamePath("Data/Grass"), error), end; !error && it != end; it.increment(error)) {
			if (it->is_regular_file(error)) {
				AddFile(Lower(it->path().filename().string()));
				++files;
			}
		}

		logger::info("Indexed {} grass cache files for {} worldspaces in {:.1f} ms", files, worlds.size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
	}

	bool IsDistantCell(int centerX, int centerY, int x, int y, int activeRadius, int grassRadius)
	{
		const int distance = std::max(std::abs(x - centerX), std::abs(y - centerY));
		return distance > activeRadius && distance <= grassRadius;
	}

	CellInfo::CellInfo(const int cellX, const int cellY) :
		x(cellX), y(cellY)
	{
	}

	bool CellInfo::AdvanceTerrain(TerrainLoad from, TerrainLoad to)
	{
		return terrainLoad.compare_exchange_strong(from, to);
	}

	bool CellInfo::HasCacheFile(std::string_view world) const
	{
		return !world.empty() && gridState.cacheIndex.Contains(world, x, y);
	}

	CellInfoContainer::CellInfoContainer() :
		grid(GridSide * GridSide)
	{}

	std::shared_ptr<CellInfo> CellInfoContainer::GetFromGrid(int x, int y)
	{
		if (x < -GridHalf || y < -GridHalf || x > GridHalf || y > GridHalf)
			return {};

		std::scoped_lock lock(gridState.cellMapMutex);

		auto& cell = grid[(x + GridHalf) * GridSide + y + GridHalf];
		if (!cell)
			cell = std::make_shared<CellInfo>(x, y);

		return cell;
	}

	std::shared_ptr<CellInfo> CellInfoContainer::FindByCell(RE::TESObjectCELL* cell)
	{
		std::scoped_lock lock(gridState.cellMapMutex);

		const auto it = map.find(cell);
		return it != map.end() ? it->second : nullptr;
	}

	unsigned char CellLoadHook(const int x, const int y)
	{
		const auto info = gridState.cellGrid->GetFromGrid(x, y);
		return info && info->AdvanceTerrain(TerrainLoad::Queued, TerrainLoad::Claimed);
	}

	static float GetGrassFadeRange()
	{
		if (Config::OverwriteGrassFadeRange >= 0.0)
			return static_cast<float>(Config::OverwriteGrassFadeRange);

		return globals::settings::grassFadeRange ? globals::settings::grassFadeRange->GetFloat() : 0.0f;
	}

	int GetGrassGridRadius()
	{
		if (gridState.grassGridRadius >= 0)
			return gridState.grassGridRadius;

		float distance = static_cast<float>(Config::OverwriteGrassDistance);
		if (distance < 0.0f)
			distance = globals::settings::grassStartFadeDistance ? globals::settings::grassStartFadeDistance->GetFloat() : 0.0f;

		const float range = GetGrassFadeRange();
		const float cells = (std::max(0.0f, distance) + std::max(0.0f, range)) / 4096.0f;

		int radius = !std::isfinite(cells) || cells > 32.0f ? 33 : static_cast<int>(std::ceil(cells));
		switch (Config::DynDOLODGrassMode) {
		case 1:
			radius = globals::GridsToLoad() / 2;
			break;
		case 2:
			radius = (globals::settings::largeRefLODGridSize ? globals::settings::largeRefLODGridSize->GetInteger() : 9) / 2;
			break;
		default:
			break;
		}

		if (radius > 32)
			logger::info("Calculated grass grid radius is {}; using 32. fGrassStartFadeDistance or fGrassFadeRange is probably very large.", radius);

		gridState.grassGridRadius = std::clamp(radius, 2, 32);
		logger::info("ChosenGrassSettings: grid: {}, dist: {}, range: {}", gridState.grassGridRadius, distance, range);
		return gridState.grassGridRadius;
	}

	// LoadLand sets this flag once it has started loading the land's data.
	static constexpr std::uint32_t LandDataLoaded = 1 << 3;

	// Coordinate-created cells have no LAND form, so their terrain is loaded through the worldspace.
	static void LoadTerrain(RE::TESObjectCELL* cell)
	{
		auto* land = RE::GetLand(cell);
		if (!land || (land->data.flags.underlying() & LandDataLoaded) != 0)
			return;

		if (!land->GetFile(-1)) {
			land->data.flags.set(RE::OBJ_LAND::Flag::kVertexNormals_HeightMap, RE::OBJ_LAND::Flag::kVertexColors,
				RE::OBJ_LAND::Flag::kLayers, RE::OBJ_LAND::Flag::kMPCD);
		}

		RE::LoadLand(land);
	}

	void LoadDistantCellNow(std::uintptr_t ws, int x, int y)
	{
		const auto info = gridState.cellGrid->GetFromGrid(x, y);
		if (!info || !info->AdvanceTerrain(TerrainLoad::Claimed, TerrainLoad::Loading))
			return;

		const stl::scope_exit finish([&] {
			info->AdvanceTerrain(TerrainLoad::Loading, TerrainLoad::None);
			info->loadingTerrain = false;
		});

		if (!ws || !gridState.streamingAllowed)
			return;

		// Leave reference loading to the normal cell loader to avoid resetting NPCs.
		auto* cell = RE::CreateCellAt(RE::TESDataHandler::GetSingleton(), x, y, reinterpret_cast<RE::TESWorldSpace*>(ws));
		if (cell && !IsValidLoadedCell(cell, false))
			LoadTerrain(cell);

		bool queue = false;
		{
			std::scoped_lock lock(gridState.cellMapMutex);

			auto* tes = RE::TES::GetSingleton();
			auto* world = tes->GetRuntimeData2().worldSpace;
			if (!gridState.streamingAllowed || !gridState.gridUpdatesEnabled || tes->interiorCell || reinterpret_cast<std::uintptr_t>(world) != ws)
				return;

			const auto wanted = GetWantState(*info, tes->currentGridX, tes->currentGridY, globals::GridsToLoad(), GetGrassGridRadius(), globals::AllowLoadGrass(), world->editorID.c_str());
			if (wanted != GrassState::None && info->have == GrassState::None && IsValidLoadedCell(cell, wanted == GrassState::Active))
				queue = PrepareGrassTask(info, cell, wanted);
		}

		if (queue)
			QueueGrassCreation(cell);
	}

	bool ClearCellAddGrassTask(RE::TESObjectCELL* cell)
	{
		if (!cell)
			return false;

		const auto task = RE::AddCellGrassTask::Get(cell);
		if (!task)
			return false;

		*task->Abort() = true;
		RE::AddCellGrassTask::Set(cell, nullptr);
		return true;
	}

	bool IsValidLoadedCell(RE::TESObjectCELL* cell, bool quickLoad)
	{
		if (!cell)
			return false;

		if (quickLoad)
			return true;

		const auto* land = cell->GetRuntimeData().cellLand;
		return land && land->loadedData;
	}

	RE::TESObjectCELL* GetCurrentWorldspaceCell(const RE::TES* tes, RE::TESWorldSpace* ws, int x, int y, bool quickLoad, bool allowLoadNow)
	{
		if (!RE::TESDataHandler::GetSingleton() || !ws)
			return nullptr;

		auto* cell = RE::GetLoadedCell(tes, x, y);
		if (IsValidLoadedCell(cell, quickLoad))
			return cell;
		if (!allowLoadNow)
			return nullptr;

		if (const auto info = gridState.cellGrid->GetFromGrid(x, y))
			info->AdvanceTerrain(TerrainLoad::Claimed, TerrainLoad::None);

		{
			RE::BSSpinLockGuard lock(const_cast<RE::TES*>(tes)->lock);
			cell = RE::GetWorldCell(ws, x, y);
		}

		if (IsValidLoadedCell(cell, quickLoad))
			return cell;

		if (!cell)
			return nullptr;

		LoadTerrain(cell);
		return IsValidLoadedCell(cell, false) ? cell : nullptr;
	}

	GrassState GetWantState(const CellInfo& c, const int curX, const int curY, const int uGrid, const int grassRadius, const bool canLoadFromFile, std::string_view world)
	{
		const int diffX = std::abs(curX - c.x);
		const int diffY = std::abs(curY - c.y);
		if (diffX > grassRadius || diffY > grassRadius)
			return GrassState::None;

		const int uHalf = uGrid / 2;
		if (diffX <= uHalf && diffY <= uHalf)
			return GrassState::Active;

		return canLoadFromFile && c.HasCacheFile(world) ? GrassState::Active : GrassState::Lod;
	}

	void RemoveCellGrass(RE::TESObjectCELL* cell)
	{
		if (!cell)
			return;

		{
			std::scoped_lock lock(gridState.cellMapMutex);

			if (const auto info = gridState.cellGrid->FindByCell(cell)) {
				info->Reset();
				info->cell = nullptr;
				gridState.cellGrid->map.erase(cell);
				gridState.cellGrid->RetryDistantCell(info->x, info->y);
			}

			// A queued task may belong to a cell NGIO never tracked, and must not run once the cell is gone.
			ClearCellAddGrassTask(cell);
		}

		RE::BSSpinLockGuard grassLock(cell->grassCreateLock);
		ReattachRetiredGrass(cell);
		RE::RemoveCellGrass(RE::BGSGrassManager::GetSingleton(), cell);
		CliffSurfaces::Forget(cell->GetFormID());
	}

	bool PrepareGrassTask(const std::shared_ptr<CellInfo>& info, RE::TESObjectCELL* cell, GrassState wanted)
	{
		if (info->cell != cell) {
			if (info->cell)
				gridState.cellGrid->map.erase(info->cell);

			info->cell = cell;
			gridState.cellGrid->map.try_emplace(cell, info);
		}

		const bool wasQueued = info->queued;
		info->queued = true;
		info->wanted = wanted;
		return !wasQueued;
	}

	bool CellInfoContainer::QueueDistantCells(RE::TESWorldSpace* world, int x, int y, int activeRadius, int grassRadius)
	{
		std::unique_lock lock(gridState.cellMapMutex, std::defer_lock);
		if (world) {
			if (!lock.try_lock())
				return false;
		} else {
			lock.lock();
		}

		if (pendingWorld == world && pendingX == x && pendingY == y)
			return true;

		pendingWorld = world;
		pendingX = x;
		pendingY = y;
		pendingCells.clear();
		nextPendingCell = 0;
		if (!world)
			inFlight.clear();

		if (world) {
			ForEachDistantCell(x, y, activeRadius, grassRadius, [&](int cellX, int cellY) {
				pendingCells.emplace_back(cellX, cellY);
			});
		}

		return true;
	}

	void CellInfoContainer::RetryDistantCell(int x, int y)
	{
		// The normal cell buffer can unload a cell while its terrain is still within grass distance.
		if (pendingWorld && IsDistantCell(pendingX, pendingY, x, y, globals::GridsToLoad() / 2, GetGrassGridRadius()))
			pendingCells.emplace_back(x, y);
	}

	void CellInfoContainer::UpdateDistantCells(const RE::TES* tes)
	{
		if (!tes)
			return;

		std::unique_lock lock(gridState.cellMapMutex, std::try_to_lock);
		if (!lock.owns_lock())
			return;

		const auto started = std::chrono::steady_clock::now();
		std::erase_if(inFlight, [&](const auto& entry) {
			const auto& info = entry.first;
			return started - entry.second > DistantCellTimeout || (!info->loadingTerrain && info->terrainLoad == TerrainLoad::None && !info->queued);
		});

		const bool canLoadGrass = globals::AllowLoadGrass();

		while (inFlight.size() < MaxDistantCellsInFlight && nextPendingCell < pendingCells.size()) {
			if (!gridState.gridUpdatesEnabled || !pendingWorld || tes->interiorCell ||
				tes->GetRuntimeData2().worldSpace != pendingWorld || tes->currentGridX != pendingX || tes->currentGridY != pendingY)
				return;

			const auto [x, y] = pendingCells[nextPendingCell++];
			auto info = GetFromGrid(x, y);
			if (!info || info->have != GrassState::None || info->queued)
				continue;

			auto* world = pendingWorld;
			const auto wanted = GetWantState(*info, pendingX, pendingY, globals::GridsToLoad(), GetGrassGridRadius(), canLoadGrass, world->editorID.c_str());
			if (auto* cell = GetCurrentWorldspaceCell(tes, world, x, y, wanted == GrassState::Active, false)) {
				PrepareGrassTask(info, cell, wanted);
				inFlight.emplace_back(info, started);
				lock.unlock();
				QueueGrassCreation(cell);
			} else {
				if (!info->AdvanceTerrain(TerrainLoad::None, TerrainLoad::Queued))
					continue;

				info->loadingTerrain = true;
				inFlight.emplace_back(info, started);

				// Cell-loading callbacks acquire gridState.cellMapMutex while the engine holds its loader lock.
				lock.unlock();
				RE::QueueCellLoad(world, x, y);
				if (info->AdvanceTerrain(TerrainLoad::Queued, TerrainLoad::None))
					info->loadingTerrain = false;
			}

			if (std::chrono::steady_clock::now() - started >= DistantStreamBudget || !lock.try_lock())
				return;
		}
	}

	void UpdateGrassGridNow(const RE::TES* tes, int movedX, int movedY, int addType)
	{
		if (!tes || (!gridState.cellGrid && !gridState.cacheOnlyCells))
			return;

		if (addType == 1) {
			gridState.gridUpdatesEnabled = true;
			gridState.streamingAllowed = false;
		} else if (addType == -1) {
			gridState.gridUpdatesEnabled = false;
			gridState.gridUpdatePending = false;
			gridState.streamingAllowed = false;

			if (gridState.cellGrid)
				gridState.cellGrid->QueueDistantCells(nullptr, 0, 0, 0, 0);

		} else if (!gridState.gridUpdatesEnabled) {
			return;
		}

		const auto started = std::chrono::steady_clock::now();
		const int grassRadius = GetGrassGridRadius();
		const int uGrids = globals::GridsToLoad();

		auto* world = tes->GetRuntimeData2().worldSpace;
		const int centerX = tes->currentGridX;
		const int centerY = tes->currentGridY;

		if (auto* cacheOnly = gridState.cacheOnlyCells.get()) {
			cacheOnly->FreeDiscardedCells();
			if (addType <= 0)
				cacheOnly->Retain(world, addType, centerX, centerY, grassRadius);
			if (addType >= 0 && world) {
				ForEachGrassCell(centerX, centerY, grassRadius, [&](int x, int y) {
					cacheOnly->QueueLoad(world, x, y);
				});
			}
			return;
		}

		const bool canLoadGrass = globals::AllowLoadGrass();
		const std::string worldName = world ? world->editorID.c_str() : "";
		int queued = 0;

		// Try-locks keep the main thread from waiting on grass workers; a busy map defers the move to the next frame.
		const auto lockMap = [&](std::unique_lock<std::recursive_mutex>& lock) {
			if (addType != 0) {
				lock.lock();
				return true;
			}

			if (lock.try_lock())
				return true;

			gridState.gridUpdatePending = true;

			return false;
		};

		if (addType <= 0) {
			std::vector<RE::TESObjectCELL*> remove;
			{
				std::unique_lock lock(gridState.cellMapMutex, std::defer_lock);
				if (!lockMap(lock))
					return;

				std::erase_if(gridState.cellGrid->map, [&](const auto& entry) {
					const auto& info = entry.second;
					if (addType >= 0 && GetWantState(*info, centerX, centerY, uGrids, grassRadius + 1, false, "") != GrassState::None)
						return false;

					auto* cell = info->cell;
					info->cell = nullptr;
					info->Reset();

					ClearCellAddGrassTask(cell);
					remove.push_back(cell);

					return true;
				});
			}

			for (auto* cell : remove) {
				RE::BSSpinLockGuard grassLock(cell->grassCreateLock);
				ReattachRetiredGrass(cell);
				RE::RemoveCellGrass(RE::BGSGrassManager::GetSingleton(), cell);
				CliffSurfaces::Forget(cell->GetFormID());
			}
		}

		if (addType >= 0 && world) {
			ForEachGrassCell(centerX, centerY, std::min(grassRadius, uGrids / 2), [&](int x, int y) {
				RE::TESObjectCELL* queue = nullptr;
				{
					std::unique_lock lock(gridState.cellMapMutex, std::defer_lock);
					if (!lockMap(lock))
						return;

					auto info = gridState.cellGrid->GetFromGrid(x, y);
					if (!info)
						return;

					const auto wanted = GetWantState(*info, centerX, centerY, uGrids, grassRadius, canLoadGrass, worldName);
					if (wanted == GrassState::None || info->have == wanted || info->have == GrassState::Active)
						return;

					auto* cell = GetCurrentWorldspaceCell(tes, world, x, y, wanted == GrassState::Active, addType > 0);
					if (cell && PrepareGrassTask(info, cell, wanted))
						queue = cell;
				}

				// Engine workers can start this cell before the rest of the grid is prepared.
				if (queue) {
					QueueGrassCreation(queue);
					++queued;
				}
			});
		}

		if (addType >= 0 && !gridState.cellGrid->QueueDistantCells(world, centerX, centerY, uGrids / 2, grassRadius))
			gridState.gridUpdatePending = true;

		const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		logger::debug("Grass grid ({}, {}) moved ({}, {}), type {}, radius {}, queued {}, preparation {:.3f} ms", centerX, centerY, movedX, movedY, addType, grassRadius, queued, milliseconds);
	}

	std::uint64_t CacheOnlyCells::Key(RE::FormID world, int x, int y)
	{
		return static_cast<std::uint64_t>(world) << 32 | static_cast<std::uint32_t>(static_cast<std::uint16_t>(x)) << 16 | static_cast<std::uint16_t>(y);
	}

	// Zeroed cell storage carrying only coordinates and a worldspace stands in for the real cell.
	RE::TESObjectCELL* CacheOnlyCells::CreateStandIn(RE::TESWorldSpace* world, int x, int y)
	{
		const std::size_t size = REL::Module::IsAE() && REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_6_629 ? 0x148 : 0x140;
		auto* cell = reinterpret_cast<RE::TESObjectCELL*>(new std::byte[size]{});

		auto* exterior = new RE::EXTERIOR_DATA();
		exterior->cellX = x;
		exterior->cellY = y;

		cell->GetRuntimeData().cellData.exterior = exterior;
		cell->GetRuntimeData().worldSpace = world;

		return cell;
	}

	void CacheOnlyCells::Retain(const RE::TESWorldSpace* world, int addType, int centerX, int centerY, int radius)
	{
		const RE::FormID worldId = world ? world->formID : 0;
		std::scoped_lock lock(gridState.cellMapMutex);
		std::erase_if(cells, [&](auto& item) {
			const auto& [key, entry] = item;
			const int x = static_cast<std::int16_t>(key >> 16);
			const int y = static_cast<std::int16_t>(key);
			const bool keep = addType >= 0 && static_cast<RE::FormID>(key >> 32) == worldId && std::abs(x - centerX) <= radius && std::abs(y - centerY) <= radius;
			return !keep && (entry.state == State::None || Unload(item.second));
		});
	}

	bool CacheOnlyCells::QueueLoad(RE::TESWorldSpace* world, int x, int y)
	{
		if (!world)
			return false;

		RE::TESObjectCELL* standIn = nullptr;
		{
			std::scoped_lock lock(gridState.cellMapMutex);
			auto& entry = cells[Key(world->formID, x, y)];

			// An aborted load can still satisfy a renewed request.
			if (entry.state == State::Abort && entry.standIn) {
				entry.state = State::Loading;
				return false;
			}

			if (entry.state != State::None)
				return false;

			entry.state = State::Loading;
			if (!entry.standIn)
				entry.standIn = CreateStandIn(world, x, y);
			standIn = entry.standIn;
		}

		RE::AddCellGrassTask::Queue(standIn);

		return true;
	}

	void CacheOnlyCells::Load(RE::TESObjectCELL* cell)
	{
		const auto* world = cell->GetRuntimeData().worldSpace;
		const auto* coordinates = cell->GetCoordinates();
		if (!world || !coordinates)
			return;

		std::scoped_lock lock(gridState.cellMapMutex);
		const auto it = cells.find(Key(world->formID, coordinates->cellX, coordinates->cellY));
		if (it == cells.end())
			return;

		auto& entry = it->second;
		if (entry.state == State::Loading && entry.standIn) {
			RE::LoadCellGrassFile(RE::BGSGrassManager::GetSingleton(), entry.standIn);
			entry.state = State::Loaded;
		} else if (entry.state != State::Loaded) {
			Discard(entry);
			entry.state = State::None;
		}
	}

	bool CacheOnlyCells::Unload(Entry& entry)
	{
		switch (entry.state) {
		case State::Loading:
			entry.state = State::Abort;
			return false;
		case State::Loaded:
			if (entry.standIn)
				RE::RemoveCellGrass(RE::BGSGrassManager::GetSingleton(), entry.standIn);
			Discard(entry);
			entry.state = State::None;
			return true;
		default:
			return false;
		}
	}

	void CacheOnlyCells::Discard(Entry& entry)
	{
		auto* cell = std::exchange(entry.standIn, nullptr);
		if (!cell)
			return;

		RE::ClearGrassHandles(&cell->extraList);
		RE::AddCellGrassTask::Set(cell, nullptr);
		discarded.push_back(cell);
	}

	void CacheOnlyCells::FreeDiscardedCells()
	{
		std::scoped_lock lock(gridState.cellMapMutex);
		for (auto* cell : discarded) {
			delete cell->GetRuntimeData().cellData.exterior;
			delete[] reinterpret_cast<std::byte*>(cell);
		}

		discarded.clear();
	}

	bool QueueGrassPlacement(RE::TESObjectCELL* cell)
	{
		if (Config::ExtendGrassDistance) {
			std::unique_lock lock(gridState.cellMapMutex, std::try_to_lock);
			if (!lock)
				return false;

			const auto info = gridState.cellGrid ? gridState.cellGrid->FindByCell(cell) : nullptr;
			if (!info)
				return true;

			if (info->queued)
				return false;

			auto* tes = RE::TES::GetSingleton();
			auto* world = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
			if (!world || world != cell->GetRuntimeData().worldSpace)
				return true;

			const bool canLoadGrass = globals::AllowLoadGrass();
			const auto wanted = GetWantState(*info, tes->currentGridX, tes->currentGridY, globals::GridsToLoad(), GetGrassGridRadius(), canLoadGrass, world->editorID.c_str());
			if (wanted == GrassState::None || info->have == wanted || info->have == GrassState::Active)
				return true;

			const bool fromCache = canLoadGrass && info->HasCacheFile(world->editorID.c_str());
			if (wanted == GrassState::Active && !fromCache && !PhysicsRaycast::GetSingleton().CanFilterCell(*cell))
				return false;

			PrepareGrassTask(info, cell, wanted);
		}

		QueueGrassCreation(cell);
		return true;
	}

	void ResetGridUpdates()
	{
		gridState.gridUpdatesEnabled = false;
		gridState.gridUpdatePending = false;
		gridState.streamingAllowed = false;

		if (gridState.cellGrid)
			gridState.cellGrid->QueueDistantCells(nullptr, 0, 0, 0, 0);

		ResetGrassModelPrefetch();
	}

	void Update()
	{
		const bool loading = globals::IsLoading();
		GrassPatchCreation::LimitParallelism(!loading);

		auto* tes = RE::TES::GetSingleton();
		if (!gridState.cellGrid || !gridState.gridUpdatesEnabled || !tes || loading)
			return;

		RetireReplacedGrass();

		if (gridState.gridUpdatePending.exchange(false))
			UpdateGrassGridNow(tes, 0, 0, 0);

		if (gridState.gridUpdatePending)
			return;

		gridState.streamingAllowed = true;
		gridState.cellGrid->UpdateDistantCells(tes);
	}
}
