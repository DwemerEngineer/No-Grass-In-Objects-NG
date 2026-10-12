#include "GrassControl/DistantGrass.h"
#include "CasualLibrary.hpp"
#include "GrassControl/Config.h"
#include "GrassControl/Util.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace GrassControl;
using namespace GrassControl::DistantGrass;

namespace
{
	enum class GrassStates : unsigned char
	{
		None,

		Lod,

		Active
	};

	std::recursive_mutex cellMapMutex;
	std::atomic<bool> _canUpdateGridNormal{ false };
	bool load_only = false;
	int _chosenGrassGridRadius = -1;

	std::unique_ptr<CellInfoContainer> Map;
	std::unique_ptr<LoadOnlyCellContainer> LO2Map;
}

static float getChosenGrassFadeRange();

static bool IsValidLoadedCell(RE::TESObjectCELL* cell, bool quickLoad);

static RE::TESObjectCELL* GetCurrentWorldspaceCell(const RE::TES* tes, RE::TESWorldSpace* world, int x, int y, bool quickLoad, bool allowLoadNow);

static void Call_AddGrassNow(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, std::uintptr_t argument);

static GrassStates GetWantState(const std::shared_ptr<CellInfo>& cell, int curX, int curY, int uGrid, int grassRadius, bool canLoadFromFile, const std::string& worldName);

static unsigned char CalculateLoadState(int nowX, int nowY, int x, int y, int ugrid, int ggrid);

static void UpdateGrassGridNow_LoadOnly(const RE::TES* tes, int movedX, int movedY, int addType);

GrassControl::DistantGrass::CellInfo::CellInfo(const int _x, const int _y) :
	x(_x), y(_y)
{
}

bool GrassControl::DistantGrass::CellInfo::checkHasFile(const std::string& wsName) const
{
	if (wsName.empty()) {
		return false;
	}

	const auto fpath = Util::Files::GamePath(fmt::format("Data/Grass/{}x{:04}y{:04}.cgid", wsName, x, y));

	bool has = false;
	try {
		if (std::filesystem::exists(fpath)) {
			has = true;
		}
	} catch (...) {
		logger::error("Error occured while checking for cache files for cell");
	}

	return has;
}

GrassControl::DistantGrass::CellInfoContainer::CellInfoContainer() :
	grid(129 * 129)
{
	for (int x = 0; x <= 128; x++) {
		for (int y = 0; y <= 128; y++) {
			grid[x * 129 + y] = std::make_shared<CellInfo>(x - 64, y - 64);
		}
	}
}

std::shared_ptr<CellInfo> GrassControl::DistantGrass::CellInfoContainer::GetFromGrid(int x, int y) const
{
	if (x < -64 || y < -64 || x > 64 || y > 64)
		return {};

	x += 64;
	y += 64;
	return grid[x * 129 + y];
}

void GrassControl::DistantGrass::CellInfoContainer::unsafe_ForeachWithState(const std::function<bool(std::shared_ptr<CellInfo>)>& action)
{
	try {
		std::erase_if(this->map, [&](const auto& pair) {
			auto& [fst, snd] = pair;
			if (fst != nullptr) {
				if (!action(snd)) {
					return true;
				}
			}
			return false;
		});
	} catch (...) {
		logger::error("Exception occurred while trying to erase cell from loaded reference map. Attempting to continue.");
	}
}

std::shared_ptr<CellInfo> GrassControl::DistantGrass::CellInfoContainer::FindByCell(RE::TESObjectCELL* cell)
{
	{
		std::scoped_lock lock(cellMapMutex);

		auto it = this->map.find(cell);
		if (it != this->map.end()) {
			return it->second;
		}
		return {};
	}
}

int GrassControl::DistantGrass::LoadOnlyCellContainer::GetCount() const
{
	int count = 0;
	{
		std::scoped_lock lock(cellMapMutex);

		for (const auto& val : map | std::views::values) {
			if (val->State != CellData::CellState::None) {
				count++;
			}
		}
	}
	return count;
}

std::string GrassControl::DistantGrass::LoadOnlyCellContainer::MakeKey(const std::string& ws, int x, int y)
{
	std::string rtn;
	try {
		rtn = fmt::format("{}_{}_{}", x, y, ws);
	} catch (fmt::format_error& e) {
		logger::error("Error occured while extending Grass Distance:");
		logger::error("{}", e.what());
	}

	return rtn;
}

void GrassControl::DistantGrass::LoadOnlyCellContainer::UpdatePositionWithRemove(RE::TESWorldSpace* ws, int addType, int nowX, int nowY, int grassRadius)
{
	unsigned int wsId = ws != nullptr ? ws->formID : 0;

	std::vector<std::pair<std::string, std::shared_ptr<CellData>>> queueDelete;

	{
		std::scoped_lock lock(cellMapMutex);

		for (const auto& [key, val] : this->map) {
			if (!val) {
				queueDelete.push_back({ key, val });
				continue;
			}

			bool shouldKeep = addType >= 0 && val->WsId == wsId && std::abs(val->X - nowX) <= grassRadius && std::abs(val->Y - nowY) <= grassRadius;

			if (!shouldKeep) {
				queueDelete.push_back({ key, val });
			}
		}

		for (auto& pair : queueDelete) {
			bool erase = !pair.second || pair.second->State == CellData::CellState::None || _DoUnload(pair.second);
			if (erase) {
				this->map.erase(pair.first);
			}
		}
	}
}

void GrassControl::DistantGrass::LoadOnlyCellContainer::QueueLoad(RE::TESWorldSpace* ws, int x, int y)
{
	if (ws == nullptr)
		return;

	std::string key = MakeKey(ws->editorID.c_str(), x, y);
	if (key.empty())
		return;

	{
		std::scoped_lock lock(cellMapMutex);

		auto it = this->map.find(key);
		std::shared_ptr<CellData> d = it != this->map.end() ? it->second : nullptr;

		if (d == nullptr) {
			d = std::make_shared<CellData>();
			this->map.insert_or_assign(key, d);

			d->X = x;
			d->Y = y;
			d->WsId = ws->formID;
		}

		{
			// An aborted load can still satisfy a renewed request.
			if (d->State == CellData::CellState::Abort && d->DummyCell_Ptr != nullptr) {
				d->State = CellData::CellState::Loading;
				return;
			}

			if (d->State != CellData::CellState::None) {
				return;
			}

			d->State = CellData::CellState::Loading;

			if (d->DummyCell_Ptr == nullptr) {
				char* tempPtr = nullptr;
				if (REL::Relocate(false, REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_6_629)) {
					tempPtr = new char[0x148];
					memset(tempPtr, 0, 0x148);
				} else {
					tempPtr = new char[0x140];
					memset(tempPtr, 0, 0x140);
				}

				d->DummyCell_Ptr = reinterpret_cast<RE::TESObjectCELL*>(tempPtr);

				auto exteriorData = new RE::EXTERIOR_DATA();
				exteriorData->cellX = x;
				exteriorData->cellY = y;

				d->DummyCell_Ptr->GetRuntimeData().cellData.exterior = exteriorData;
				d->DummyCell_Ptr->GetRuntimeData().worldSpace = ws;
			}

			using func_t = void (*)(RE::TESObjectCELL*);
			REL::Relocation<func_t> func{ RELOCATION_ID(13137, 13277) };

			func(d->DummyCell_Ptr);
		}
	}
}

bool GrassControl::DistantGrass::LoadOnlyCellContainer::_DoUnload(std::shared_ptr<CellData>& cellData)
{
	if (cellData == nullptr)
		return false;

	{
		std::scoped_lock lock(cellMapMutex);

		if (!cellData || cellData->State == CellData::CellState::None || cellData->State == CellData::CellState::Abort) {
			return false;
		}

		if (cellData->State == CellData::CellState::Loading) {
			cellData->State = CellData::CellState::Abort;
			return false;
		}

		if (cellData->DummyCell_Ptr) {
			REL::Relocation<void (*)(RE::BGSGrassManager*, RE::TESObjectCELL*)> func{ RELOCATION_ID(15207, 15375) };
			func(RE::BGSGrassManager::GetSingleton(), cellData->DummyCell_Ptr);
		}

		_DiscardDummyCell(cellData);
		cellData->State = CellData::CellState::None;

		return true;
	}
}

void GrassControl::DistantGrass::LoadOnlyCellContainer::_DiscardDummyCell(const std::shared_ptr<CellData>& cellData)
{
	auto cell = cellData->DummyCell_Ptr;
	cellData->DummyCell_Ptr = nullptr;

	if (cell == nullptr)
		return;

	REL::Relocation<void (*)(RE::ExtraDataList*)> ClearGrassHandles{ RELOCATION_ID(11931, 12070) };
	ClearGrassHandles(&cell->extraList);

	REL::Relocation<void (*)(const RE::ExtraDataList&, void*)> SetAddGrassTask{ RELOCATION_ID(11932, 12071) };
	SetAddGrassTask(cell->extraList, nullptr);

	{
		std::scoped_lock lock(cellMapMutex);

		this->discardedCells.push_back(cell);
	}
}

void GrassControl::DistantGrass::LoadOnlyCellContainer::FreeDiscardedCells()
{
	std::scoped_lock lock(cellMapMutex);

	for (auto cell : this->discardedCells) {
		delete cell->GetRuntimeData().cellData.exterior;
		delete[] reinterpret_cast<char*>(cell);
	}

	this->discardedCells.clear();
}

void GrassControl::DistantGrass::LoadOnlyCellContainer::Unload(const RE::TESWorldSpace* ws, const int x, const int y)
{
	if (ws == nullptr)
		return;

	std::string key = MakeKey(ws->editorID.c_str(), x, y);
	std::shared_ptr<CellData> data;

	{
		std::scoped_lock lock(cellMapMutex);

		if (!this->map.empty()) {
			auto it = this->map.find(key);
			data = it == this->map.end() ? nullptr : it->second;
		}

		if (data == nullptr)
			return;

		if (_DoUnload(data)) {
			this->map.erase(key);
		}
	}
}

void GrassControl::DistantGrass::LoadOnlyCellContainer::_DoLoad(const RE::TESWorldSpace* ws, const int x, const int y)
{
	if (ws == nullptr)
		return;

	std::string key = MakeKey(ws->editorID.c_str(), x, y);
	std::shared_ptr<CellData> d;
	{
		std::scoped_lock lock(cellMapMutex);

		auto it = this->map.find(key);
		d = it == this->map.end() ? nullptr : it->second;
	}

	if (d == nullptr)
		return;

	{
		std::scoped_lock lock(cellMapMutex);

		if (d->State == CellData::CellState::Loaded) {
		} else if (d->State == CellData::CellState::Loading) {
			REL::Relocation<void (*)(RE::BGSGrassManager*, RE::TESObjectCELL*)> func{ RELOCATION_ID(15206, 15374) };

			if (d->DummyCell_Ptr == nullptr)
				return;

			func(RE::BGSGrassManager::GetSingleton(), d->DummyCell_Ptr);
			d->State = CellData::CellState::Loaded;
		} else if (d->DummyCell_Ptr != nullptr) {
			_DiscardDummyCell(d);
			d->State = CellData::CellState::None;
		} else {
			d->State = CellData::CellState::None;
		}
	}
}

void GrassControl::DistantGrass::LoadOnlyCellContainer::UnloadAll()
{
	std::scoped_lock lock(cellMapMutex);

	// In-flight loads still own their dummy cells.
	std::erase_if(this->map, [&](auto& pair) {
		return !pair.second || pair.second->State == CellData::CellState::None || _DoUnload(pair.second);
	});
}

void GrassControl::DistantGrass::RemoveGrassHook(RE::TESObjectCELL* cell, uintptr_t arg_1)
{
	if (cell != nullptr && Map) {
		std::scoped_lock lock(cellMapMutex);

		auto c = Map->FindByCell(cell);
		if (c && (c->self_data & 0xFF) != 0) {
			logger::debug("RemoveGrassDueToAdd({}, {})", c->x, c->y);

			REL::Relocation<void (*)(uintptr_t, RE::TESObjectCELL*)> func{ RELOCATION_ID(15207, 15375) };
			func(arg_1, cell);
		}

		if (!c) {
			logger::debug("RemoveGrassDueToAdd(null): warning: c == null");
		}
	} else if (Config::DebugLogEnable) {
		logger::debug("RemoveGrassDueToAdd(null)");
	}
}

void GrassControl::DistantGrass::CellUnloadHook(bool did, RE::TESObjectCELL* cellObj)
{
	if (cellObj) {
		if (const auto* coords = cellObj->GetCoordinates()) {
			logger::debug("DataHandler::UnloadCell({}, {}) did: {}", coords->cellX, coords->cellY, did);
		}
	}
}

unsigned char GrassControl::DistantGrass::CellLoadHook(const int x, const int y)
{
	unsigned char isOurMethod = 0;
	auto c = Map->GetFromGrid(x, y);
	if (c && InterlockedCompareExchange64(&c->furtherLoad, 2, 1) == 1) {
		isOurMethod = 1;
	}
	return isOurMethod;
}

float getChosenGrassFadeRange()
{
	float r = static_cast<float>(Config::OverwriteGrassFadeRange);
	if (r < 0.0f) {
		auto setting = RE::INISettingCollection::GetSingleton()->GetSetting("fGrassFadeRange:Grass");
		r = setting->data.f;
	}

	return r;
}

int GrassControl::DistantGrass::getChosenGrassGridRadius()
{
	int r = _chosenGrassGridRadius;
	if (r < 0) {
		float dist = static_cast<float>(Config::OverwriteGrassDistance);
		if (dist < 0.0f) {
			auto addr = RELOCATION_ID(501108, 359413).address();
			dist = Memory::Internal::read<float>(addr + 8);
		}

		float range = getChosenGrassFadeRange();

		float total = std::max(0.0f, dist) + std::max(0.0f, range);
		float cells = total / 4096.0f;

		int icells = !std::isfinite(cells) || cells > 32.0f ? 33 : static_cast<int>(std::ceil(cells));
		RE::Setting* largeGrid = nullptr;

		switch (Config::DynDOLODGrassMode) {
		case 1:
			icells = GetUGrids() / 2;
			break;
		case 2:
			largeGrid = RE::GetINISetting("uLargeRefLODGridSize:General");
			if (largeGrid)
				icells = largeGrid->GetInteger() / 2;
			else
				icells = 9 / 2;

			break;
		default:
			break;
		}

		if (icells < 2) {
			r = 2;
		} else if (icells > 32) {
			logger::info("GrassControl: calculated iGrassCellRadius is {}! Using 32 instead. This probably means fGrassFadeRange or fGrassFadeStartDistance is very strange value.", icells);
			r = 32;
		} else {
			r = icells;
		}

		int imin = 0;
		if (Config::OverwriteMinGrassSize >= 0) {
			imin = Config::OverwriteMinGrassSize;
		} else {
			try {
				imin = Memory::Internal::read<int>(RELOCATION_ID(501113, 359421).address() + 8);
			} catch (...) {
			}
		}

		logger::debug("ChosenGrassGridRadius(null) chose: {}, dist: {}, range: {}, iMinGrassSize: {}", r, dist, range, imin);

		logger::info("ChosenGrassSettings: grid: {}, dist: {}, range: {}, iMinGrassSize: {}", r, dist, range, imin);

		_chosenGrassGridRadius = r;
	}

	return r;
}

void GrassControl::DistantGrass::CellLoadNow_Our(uintptr_t ws, int x, int y)
{
	auto c = Map->GetFromGrid(x, y);
	if (!c || InterlockedCompareExchange64(&c->furtherLoad, 0, 2) != 2) {
		return;
	}

	if (ws == 0)
		return;

	REL::Relocation<RE::TESObjectCELL* (*)(uintptr_t, int, int)> func{ RELOCATION_ID(20026, 20460) };

	auto cell = func(ws, x, y);
	if (IsValidLoadedCell(cell, false)) {
		if (*cell->GetName()) {
			logger::debug("FurtherLoadSuccessFirst({}, {}) Name: {}, FormId: {:x}", x, y, cell->GetName(), cell->formID);
		} else {
			logger::debug("FurtherLoadSuccessFirst({}, {}) Name: Wilderness, FormId: {:x}", x, y, cell->formID);
		}
	} else if (cell != nullptr) {
		auto landPtr = cell->GetRuntimeData().cellLand;
		if (landPtr != nullptr && (Memory::Internal::read<uint8_t>(&(landPtr->data.flags)) & 8) == 0) {
			REL::Relocation<void (*)(RE::TESObjectLAND*, int, int)> fnc{ RELOCATION_ID(18331, 18747) };

			fnc(landPtr, 0, 1);

			if (IsValidLoadedCell(cell, false)) {
				logger::debug("FurtherLoadSuccessSecond({}, {}) {}", c->x, c->y, cell->formID);
			}
		}
	}
}

bool GrassControl::DistantGrass::ClearCellAddGrassTask(const RE::TESObjectCELL* cell)
{
	if (cell == nullptr)
		return false;

	void* task = nullptr;

	REL::Relocation<void (*)(const RE::ExtraDataList&, void**)> GetAddGrassTask{ RELOCATION_ID(11933, 12072) };
	GetAddGrassTask(cell->extraList, &task);

	if (task == nullptr)
		return false;

	Memory::Internal::write<uint8_t>(reinterpret_cast<uintptr_t>(task) + 0x48, 1);
	auto refPtr = reinterpret_cast<volatile long*>(reinterpret_cast<uintptr_t>(task) + 8);
	if (InterlockedDecrement(refPtr) == 0) {
		auto vtable = *reinterpret_cast<void***>(task);
		using Dtor = void(__fastcall*)(void*, bool);
		auto dtor = reinterpret_cast<Dtor>(vtable[0]);
		dtor(task, 1);
	}

	REL::Relocation<void (*)(const RE::ExtraDataList&, void*)> SetAddGrassTask{ RELOCATION_ID(11932, 12071) };

	SetAddGrassTask(cell->extraList, nullptr);

	return true;
}

bool IsValidLoadedCell(RE::TESObjectCELL* cell, bool quickLoad)
{
	if (cell == nullptr)
		return false;

	if (quickLoad)
		return true;

	auto land = cell->GetRuntimeData().cellLand;
	if (land == nullptr)
		return false;

	auto data = land->loadedData;
	if (data == nullptr)
		return false;

	return true;
}

RE::TESObjectCELL* GetCurrentWorldspaceCell(const RE::TES* tes, RE::TESWorldSpace* ws, int x, int y, bool quickLoad, bool allowLoadNow)
{
	auto data = Memory::Internal::read<uintptr_t>(RELOCATION_ID(514141, 400269).address());
	if (data == 0 || ws == nullptr)
		return nullptr;

	using func_t = RE::TESObjectCELL* (*)(const RE::TES*, int, int);
	REL::Relocation<func_t> func{ RELOCATION_ID(13174, 13319) };

	RE::TESObjectCELL* cell = func(tes, x, y);
	if (IsValidLoadedCell(cell, quickLoad)) {
		if (*cell->GetName()) {
			logger::debug("add_GetCell({}, {}) (0): Name: {}, FormId: {:X}", x, y, cell->GetName(), cell->formID);
		} else {
			logger::debug("add_GetCell({}, {}) (0): Name: Wilderness, FormId: {:X}", x, y, cell->formID);
		}

		return cell;
	}

	if (allowLoadNow) {
		auto c = Map->GetFromGrid(x, y);
		if (c) {
			InterlockedCompareExchange64(&c->furtherLoad, 0, 2);
		}
		{
			RE::BSSpinLockGuard lock(tes->lock);

			try {
				REL::Relocation<RE::TESObjectCELL* (*)(RE::TESWorldSpace*, int16_t, int16_t)> Func2{ RELOCATION_ID(20026, 20460) };
				cell = Func2(ws, x, y);
				if (IsValidLoadedCell(cell, quickLoad)) {
					if (*cell->GetName()) {
						logger::debug("add_GetCell({}, {}) (0): Name: {}, FormId: {:X}", x, y, cell->GetName(), cell->formID);
					} else {
						logger::debug("add_GetCell({}, {}) (0): Name: Wilderness, FormId: {:X}", x, y, cell->formID);
					}
					return cell;
				}
			} catch (...) {
			}
		}
		if (cell != nullptr) {
			REL::Relocation<RE::TESObjectLAND* (*)(RE::TESObjectCELL*)> func2{ RELOCATION_ID(18513, 18970) };

			auto landPtr = func2(cell);
			if (landPtr != nullptr && (Memory::Internal::read<uint8_t>(&(landPtr->data.flags)) & 8) == 0) {
				REL::Relocation<void (*)(RE::TESObjectLAND*, int, int)> Func{ RELOCATION_ID(18331, 18747) };

				Func(landPtr, 0, 1);

				if (IsValidLoadedCell(cell, false)) {
					if (*cell->GetName()) {
						logger::debug("add_GetCell({}, {}) (0): Name: {}, FormId: {:X}", x, y, cell->GetName(), cell->formID);
					} else {
						logger::debug("add_GetCell({}, {}) (0): Name: Wilderness, FormId: {:X}", x, y, cell->formID);
					}

					return cell;
				}
			}
		}
	}

	std::string failData = "null";
	if (cell != nullptr) {
		auto cellStates = cell->cellState.get();
		failData = "loadState: " + std::to_string(static_cast<int>(cellStates)) + " hasTemp: " + std::to_string((Memory::Internal::read<uint8_t>(reinterpret_cast<uintptr_t>(cell) + 0x40) & 0x10) != 0 ? 1 : 0);
	}

	logger::debug("add_GetCell({}, {}) FAIL {}", x, y, failData);

	return nullptr;
}

void Call_AddGrassNow(RE::BGSGrassManager* GrassMgr, RE::TESObjectCELL* cell, uintptr_t customArg)
{
	if (Memory::Internal::read<uint8_t>(customArg) != 0)
		return;

	{
		std::scoped_lock lock(cellMapMutex);

		auto c = Map->FindByCell(cell);
		if (!c) {
			logger::debug("AddedGrass(null): warning: c == null");
			return;
		}

		int d = c->self_data;
		int tg = d >> 8 & 0xFF;

		bool added = false;
		bool quickLoad = (d >> 16 & 0xFF) != 0;
		if (IsValidLoadedCell(cell, quickLoad)) {
			if (*cell->GetName()) {
				logger::debug("AddingGrass({}, {}) ca: {}, cell: Name: {}, FormId: {:X}", c->x, c->y, Memory::Internal::read<uint8_t>(customArg), cell->GetName(), cell->formID);
			} else {
				logger::debug("AddingGrass({}, {}) ca: {}, cell: Name: Wilderness, FormId: {:X}", c->x, c->y, Memory::Internal::read<uint8_t>(customArg), cell->formID);
			}

			using func_t = void (*)(RE::BGSGrassManager*, RE::TESObjectCELL*, uintptr_t);
			REL::Relocation<func_t> func{ RELOCATION_ID(15204, 15372) };

			func(GrassMgr, cell, customArg);

			added = true;
			logger::debug("AddedGrass({}, {})", c->x, c->y);
		} else {
			logger::debug("AddedGrassFAIL({}, {}) <NotValidLoadedCell>", c->x, c->y);
		}

		c->self_data = added ? tg : (d & 0xFF);
	}
}

GrassStates GetWantState(const std::shared_ptr<CellInfo>& c, const int curX, const int curY, const int uGrid, const int grassRadius, const bool canLoadFromFile, const std::string& wsName)
{
	int diffX = std::abs(curX - c->x);
	int diffY = std::abs(curY - c->y);

	if (diffX > grassRadius || diffY > grassRadius) {
		return GrassStates::None;
	}

	if (load_only) {
		return GrassStates::Active;
	}

	int uHalf = uGrid / 2;
	if (diffX > uHalf || diffY > uHalf) {
		if (canLoadFromFile && c->checkHasFile(wsName)) {
			return GrassStates::Active;
		}

		return GrassStates::Lod;
	}

	return GrassStates::Active;
}

void GrassControl::DistantGrass::Handle_RemoveGrassFromCell_Call(RE::TESObjectCELL* cell)
{
	auto grassMgr = RE::BGSGrassManager::GetSingleton();
	std::string why = "celldtor";
	if (cell == nullptr) {
		logger::debug(fmt::runtime("RemoveGrassOther(null): <- " + why));
		return;
	}
	{
		std::scoped_lock lock(cellMapMutex);

		auto c = Map->FindByCell(cell);
		if (c) {
			if (c->self_data >> 24 != 0) {
				logger::debug(fmt::runtime("RemoveGrassOther({}, {}) <- " + why + " warning: busy"), c->x, c->y);
			} else {
				logger::debug(fmt::runtime("RemoveGrassOther({}, {}) <- " + why), c->x, c->y);
			}

			c->self_data = 0;
			c->cell = nullptr;
			InterlockedExchange64(&c->furtherLoad, 0);
			Map->map.erase(cell);
			ClearCellAddGrassTask(cell);
		} else {
			if (!cell->IsInteriorCell()) {
				logger::debug("cell_dtor({}, {})", cell->GetCoordinates()->cellX, cell->GetCoordinates()->cellY);
			}
		}
	}
	REL::Relocation<void (*)(RE::BGSGrassManager*, RE::TESObjectCELL*)> func{ RELOCATION_ID(15207, 15375) };

	func(grassMgr, cell);
}

unsigned char CalculateLoadState(const int nowX, const int nowY, const int x, const int y, const int ugrid, const int ggrid)
{
	int diffX = std::abs(x - nowX);
	int diffY = std::abs(y - nowY);

	if (diffX <= ugrid && diffY <= ugrid)
		return 2;

	if (diffX <= ggrid && diffY <= ggrid)
		return 1;

	return 0;
}

void GrassControl::DistantGrass::UpdateGrassGridEnsureLoad(RE::TESWorldSpace* ws, const int nowX, const int nowY)
{
	if (!Map)
		return;

	if (ws == nullptr)
		return;

	int grassRadius = getChosenGrassGridRadius();

	int uHalf = GetUGrids() / 2;
	int bigSide = std::max(grassRadius, uHalf);
	bool canLoadGrass = RE::INISettingCollection::GetSingleton()->GetSetting("bAllowLoadGrass:Grass")->GetBool();

	std::string wsName = ws->editorID.c_str();

	int wanted = 0;
	int alreadyLoaded = 0;
	int loaded = 0;

	for (int x = nowX - bigSide; x <= nowX + bigSide; x++) {
		for (int y = nowY - bigSide; y <= nowY + bigSide; y++) {
			unsigned char wantState = CalculateLoadState(nowX, nowY, x, y, uHalf, grassRadius);
			if (wantState != 1)
				continue;

			wanted++;

			auto c = Map->GetFromGrid(x, y);
			if (!c)
				continue;

			auto xs = static_cast<short>(x);
			auto ys = static_cast<short>(y);

			auto xsu = static_cast<unsigned short>(xs);

			auto ysu = static_cast<unsigned short>(ys);

			unsigned int mask = static_cast<unsigned int>(xsu) << 16;
			mask |= ysu;

			REL::Relocation<RE::TESObjectCELL* (*)(RE::TESWorldSpace*, uintptr_t)> func{ RELOCATION_ID(13549, 13641) };

			auto cell = func(ws, mask);
			if (IsValidLoadedCell(cell, false)) {
				alreadyLoaded++;
				continue;
			}

			bool quickLoad = false;
			if (canLoadGrass && c->checkHasFile(wsName)) {
				quickLoad = true;
			}

			if (quickLoad && IsValidLoadedCell(cell, true)) {
				alreadyLoaded++;
				continue;
			}

			InterlockedCompareExchange64(&c->furtherLoad, 0, 2);

			REL::Relocation<RE::TESObjectCELL* (*)(RE::TESWorldSpace*, int, int)> func2{ RELOCATION_ID(20026, 20460) };

			cell = func2(ws, x, y);
			if (IsValidLoadedCell(cell, false)) {
				loaded++;
				logger::debug("InstantLoadSuccessFirst({}, {}) {}", x, y, cell->GetFormEditorID());
			} else if (cell != nullptr) {
				REL::Relocation<RE::TESObjectLAND* (*)(RE::TESObjectCELL*)> Func{ RELOCATION_ID(18513, 18970) };

				auto landPtr = Func(cell);
				if (landPtr != nullptr && (Memory::Internal::read<uint8_t>(&(landPtr->data.flags)) & 8) == 0) {
					REL::Relocation<void (*)(RE::TESObjectLAND*, int, int)> fnc{ RELOCATION_ID(18331, 18747) };

					fnc(landPtr, 0, 1);

					if (IsValidLoadedCell(cell, false)) {
						loaded++;
						logger::debug("InstantLoadSuccessSecond({}, {}) {}", c->x, c->y, cell->GetFormEditorID());
					}
				}
			}
		}
	}

	logger::debug("UpdateGrassGridEnsureLoad({}, {}) wanted: {}, alreadyLoaded: {}, loaded: {}", nowX, nowY, wanted, alreadyLoaded, loaded);
}

void GrassControl::DistantGrass::UpdateGrassGridQueue(const int prevX, const int prevY, const int movedX, const int movedY)
{
	auto tes = RE::TES::GetSingleton();
	auto wsObj = tes->GetRuntimeData2().worldSpace;

	if (wsObj == nullptr)
		return;

	int grassRadius = getChosenGrassGridRadius();
	int uHalf = GetUGrids() / 2;
	int bigSide = std::max(grassRadius, uHalf);
	bool canLoadGrass = RE::INISettingCollection::GetSingleton()->GetSetting("bAllowLoadGrass:Grass")->GetBool();

	std::string wsName = wsObj->editorID.c_str();

	// This hook can run before TES commits its new grid coordinates.
	int nowX = tes->currentGridX;
	int nowY = tes->currentGridY;
	int destX = nowX + movedX;
	int destY = nowY + movedY;

	int queued = 0;
	int dropped = 0;
	int blocked = 0;

	for (int x = std::min(nowX, destX) - bigSide; x <= std::max(nowX, destX) + bigSide; x++) {
		for (int y = std::min(nowY, destY) - bigSide; y <= std::max(nowY, destY) + bigSide; y++) {
			unsigned char wantState = CalculateLoadState(nowX, nowY, x, y, uHalf, grassRadius);
			if (wantState != 1)
				wantState = CalculateLoadState(destX, destY, x, y, uHalf, grassRadius);

			if (wantState != 1)
				continue;

			auto c = Map->GetFromGrid(x, y);
			if (!c)
				continue;

			auto xs = static_cast<short>(x);
			auto ys = static_cast<short>(y);

			auto xsu = static_cast<unsigned short>(xs);

			auto ysu = static_cast<unsigned short>(ys);

			unsigned int mask = static_cast<unsigned int>(xsu) << 16;
			mask |= ysu;

			REL::Relocation<RE::TESObjectCELL* (*)(RE::TESWorldSpace*, intptr_t)> func{ RELOCATION_ID(13549, 13641) };

			auto cell = func(wsObj, mask);
			if (IsValidLoadedCell(cell, false))
				continue;

			bool quickLoad = false;
			if (canLoadGrass && c->checkHasFile(wsName))
				quickLoad = true;

			if (quickLoad && IsValidLoadedCell(cell, true))
				continue;

			if (InterlockedCompareExchange64(&c->furtherLoad, 1, 0) != 0) {
				blocked++;
				continue;
			}

			REL::Relocation<void (*)(uintptr_t, RE::TESWorldSpace*, int, int, int)> QueueCellLoad{ RELOCATION_ID(18150, 18541) };
			QueueCellLoad(Memory::Internal::read<uintptr_t>(RELOCATION_ID(514741, 400899).address()), wsObj, x, y, 0);

			if (InterlockedCompareExchange64(&c->furtherLoad, 0, 1) == 1) {
				dropped++;
			} else {
				queued++;
			}
		}
	}

	logger::debug("UpdateGrassGridQueue({}, {} -> {}, {}) queued: {}, dropped: {}, blocked: {}; raw prev: ({}, {}) moved: ({}, {})", nowX, nowY, destX, destY, queued, dropped, blocked, prevX, prevY, movedX, movedY);
}

void GrassControl::DistantGrass::UpdateGrassGridNow(const RE::TES* tes, int movedX, int movedY, int addType)
{
	if (addType == 1) {
		_canUpdateGridNormal = true;
	} else if (addType == -1) {
		_canUpdateGridNormal = false;
	} else if (addType == 0) {
		if (!_canUpdateGridNormal) {
			return;
		}
	}

	if (load_only) {
		UpdateGrassGridNow_LoadOnly(tes, movedX, movedY, addType);
		return;
	}

	logger::debug("UpdateGrassGridNowBegin({}, {}) type: {}", movedX, movedY, addType);

	int grassRadius = getChosenGrassGridRadius();
	int uHalf = GetUGrids() / 2;
	int bigSide = std::max(grassRadius, uHalf);
	auto ws = tes->GetRuntimeData2().worldSpace;
	auto grassMgr = RE::BGSGrassManager::GetSingleton();
	bool canLoadGrass = RE::INISettingCollection::GetSingleton()->GetSetting("bAllowLoadGrass:Grass")->GetBool();
	std::string wsName;
	if (ws != nullptr) {
		wsName = ws->editorID.c_str();
	}

	int nowX = tes->currentGridX;
	int nowY = tes->currentGridY;

	auto invokeList = std::vector<RE::TESObjectCELL*>();
	if (addType <= 0) {
		{
			std::scoped_lock lock(cellMapMutex);

			Map->unsafe_ForeachWithState([&](std::shared_ptr<CellInfo> c) {
				const int retentionRadius = grassRadius + 1;
				auto want = addType < 0 ? GrassStates::None : GetWantState(c, nowX, nowY, GetUGrids(), retentionRadius, false, "");
				if (want == GrassStates::None) {
					auto cell = c->cell;
					c->cell = nullptr;
					c->self_data = 0;
					InterlockedExchange64(&c->furtherLoad, 0);
					ClearCellAddGrassTask(cell);

					logger::debug("RemoveGrassGrid({}, {})", c->x, c->y);

					invokeList.push_back(cell);
					return false;
				}

				return true;
			});
		}

		if (!invokeList.empty()) {
			for (auto cell : invokeList) {
				REL::Relocation<void (*)(RE::BGSGrassManager*, RE::TESObjectCELL*)> func{ RELOCATION_ID(15207, 15375) };

				func(grassMgr, cell);
			}

			invokeList.clear();
		}
	}

	if (addType >= 0) {
		{
			std::scoped_lock lock(cellMapMutex);

			int minX = nowX - bigSide;
			int maxX = nowX + bigSide;
			int minY = nowY - bigSide;
			int maxY = nowY + bigSide;

			logger::debug("X: ({}, {})", minX, maxX);
			logger::debug("NowX: ({}, {})", nowX, 0);
			logger::debug("Bigside: ({}, {})", bigSide, 0);
			logger::debug("X: ({}, {})", minX, maxX);
			logger::debug("NowX: ({}, {})", nowX, 0);
			logger::debug("Y: ({}, {})", minY, maxY);
			logger::debug("NowY: ({}, {})", nowY, 0);
			logger::debug("GrassRadius: ({}, {})", grassRadius, 0);
			logger::debug("uHalf: ({}, {})", uHalf, 0);

			for (int x = minX; x <= maxX; x++) {
				for (int y = minY; y <= maxY; y++) {
					auto c = Map->GetFromGrid(x, y);
					if (!c) {
						continue;
					}

					int d = c->self_data;
					bool busy = d >> 24 != 0;

					auto have = static_cast<GrassStates>(d & 0xFF);
					auto want = GetWantState(c, nowX, nowY, GetUGrids(), grassRadius, canLoadGrass, wsName);

					if (want != GrassStates::None && have == GrassStates::None) {
						bool canQuickLoad = want == GrassStates::Active;
						auto cellPtr = GetCurrentWorldspaceCell(tes, ws, x, y, canQuickLoad, addType > 0);

						if (cellPtr != nullptr) {
							if (c->cell != cellPtr) {
								if (c->cell != nullptr) {
									logger::debug("c.cell({}, {}) warning: already had cell!", c->x, c->y);
								}
								c->cell = cellPtr;
								Map->map.try_emplace(cellPtr, c);
							}

							c->self_data = 0x01000000 | (canQuickLoad ? 0x00010000 : 0) | (static_cast<int>(want) << 8) | (d & 0xFF);

							logger::debug("QueueGrass({}, {})", x, y);

							if (!busy)
								invokeList.push_back(cellPtr);
						}
					}
				}
			}
		}

		if (!invokeList.empty()) {
			for (auto cell : invokeList) {
				REL::Relocation<void (*)(RE::TESObjectCELL*)> queueGrassCreation{ RELOCATION_ID(13137, 13277) };

				queueGrassCreation(cell);
			}
		}
	}

	logger::debug("UpdateGrassGridNowEnd({}, {}) type: {}", movedX, movedY, addType);
}

void UpdateGrassGridNow_LoadOnly(const RE::TES* tes, int movedX, int movedY, int addType)
{
	logger::debug("UpdateGrassGridNowBegin_LoadOnly({}, {}) type: {}", movedX, movedY, addType);

	LO2Map->FreeDiscardedCells();

	int grassRadius = getChosenGrassGridRadius();
	auto ws = tes->GetRuntimeData2().worldSpace;
	auto grassMgr = RE::BGSGrassManager::GetSingleton();
	int nowX = tes->currentGridX;
	int nowY = tes->currentGridY;

	if (addType <= 0) {
		LO2Map->UpdatePositionWithRemove(ws, addType, nowX, nowY, grassRadius);
	}

	if (addType >= 0) {
		auto minX = static_cast<short>(nowX - grassRadius);
		auto maxX = static_cast<short>(nowX + grassRadius);
		auto minY = static_cast<short>(nowY - grassRadius);
		auto maxY = static_cast<short>(nowY + grassRadius);

		for (short x = minX; x <= maxX; x++) {
			for (short y = minY; y <= maxY; y++) {
				if (LO2Map)
					LO2Map->QueueLoad(ws, x, y);
			}
		}
	}

	logger::debug("UpdateGrassGridNowEnd_LoadOnly({}, {}) type: {}; c_count: {}; shapeCount: {}", movedX, movedY, addType, LO2Map->GetCount(), Memory::Internal::read<int>((uintptr_t)grassMgr + 0x58));
}

int GrassControl::DistantGrass::GetUGrids()
{
	auto uGridsSetting = RE::GetINISetting("uGridsToLoad:General");
	if (uGridsSetting)
		return uGridsSetting->GetInteger();
	return 5;
}

void GrassControl::DistantGrass::Initialize(bool loadOnly)
{
	load_only = loadOnly;
	if (loadOnly) {
		LO2Map = std::make_unique<LoadOnlyCellContainer>();
		logger::debug("LO2Map created at {:x}", reinterpret_cast<std::uintptr_t>(LO2Map.get()));
	} else {
		Map = std::make_unique<CellInfoContainer>();
		logger::debug("Map created at {:x}", reinterpret_cast<std::uintptr_t>(Map.get()));
	}
}

void GrassControl::DistantGrass::OnGrassCreated(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, std::uintptr_t argument)
{
	if (exists(std::filesystem::path(Util::GetProgressFilePath())) || !cell) {
		return;
	}

	if (Config::OnlyLoadFromCache) {
		const auto* coordinates = cell->GetCoordinates();
		if (LO2Map && coordinates) {
			LO2Map->_DoLoad(cell->GetRuntimeData().worldSpace, coordinates->cellX, coordinates->cellY);
		}
	} else {
		Call_AddGrassNow(manager, cell, argument);
	}
}

namespace GrassControl::DistantGrass
{
	void UnloadAll()
	{
		if (LO2Map) {
			LO2Map->UnloadAll();
		}
	}

	void ResetGridUpdates()
	{
		_canUpdateGridNormal = false;
	}
}
