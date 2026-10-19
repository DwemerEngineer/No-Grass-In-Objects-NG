#include "GrassControl/GrassCellCreation.h"

#include "GrassControl/AddCellGrassTask.h"
#include "GrassControl/Config.h"
#include "GrassControl/DistantGrass.h"
#include "GrassControl/Engine.h"
#include "GrassControl/Globals.h"
#include "GrassControl/GrassPatchCreation.h"
#include "GrassControl/PhysicsRaycast.h"
#include "GrassControl/Profiler.h"

namespace GrassControl::DistantGrass
{
	CreationState::CreationState()
	{
		PhysicsRaycast::GetSingleton();
	}

	static bool UseGrassWorkers(const RE::BGSGrassManager* manager, RE::TESObjectCELL* cell)
	{
		// Cell workers require extended-grid tracking; ordinary retries use the native task queue.
		return gridState.cellGrid && Config::ThreadedGrassPlacement && !Config::OnlyLoadFromCache && manager && !manager->generateGrassDataFiles && !Config::GeneratingCache &&
		       cell && cell->GetCoordinates();
	}

	// Callers hold grassCreateLock.
	static std::vector<RE::CellGrassData*> TakeGrassHandles(RE::TESObjectCELL* cell)
	{
		std::vector<RE::CellGrassData*> handles;
		if (auto* data = cell->extraList.GetByType<RE::ExtraCellGrassData>()) {
			handles.assign(data->grassHandles.begin(), data->grassHandles.end());
			data->grassHandles.clear();
		}

		return handles;
	}

	// The native removal frees every entry in the cell's list, so only the dropped entries are left in it while it runs.
	static void ReplaceGrassHandles(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, const std::vector<RE::CellGrassData*>& drop, const std::vector<RE::CellGrassData*>& keep)
	{
		const auto attach = [&](const std::vector<RE::CellGrassData*>& handles) {
			auto* data = cell->extraList.GetByType<RE::ExtraCellGrassData>();
			if (!data) {
				data = RE::BSExtraData::Create<RE::ExtraCellGrassData>();
				cell->extraList.Add(data);
			}

			for (auto* handle : handles)
				data->grassHandles.push_back(handle);
		};

		if (!drop.empty()) {
			attach(drop);
			RE::RemoveCellGrass(manager, cell);
		}

		if (!keep.empty())
			attach(keep);
	}

	static void UpdateCellGrass(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort)
	{
		const auto holder = TryHoldTrackedCell(cell);
		if (!holder)
			return;

		std::unique_lock lock(gridState.cellMapMutex);
		if (*abort)
			return;

		const auto info = gridState.cellGrid ? gridState.cellGrid->FindByCell(cell) : nullptr;
		if (!info) {
			lock.unlock();
			ClearCellAddGrassTask(cell, abort);
			return;
		}

		const auto have = info->have;
		const auto wanted = info->wanted;
		const auto generation = info->taskGeneration;
		if (wanted == GrassState::None || have == wanted || have == GrassState::Active || *abort || !IsValidLoadedCell(cell, wanted == GrassState::Active)) {
			info->ClearTask();
			lock.unlock();
			ClearCellAddGrassTask(cell, abort);
			return;
		}

		const auto* world = cell->GetRuntimeData().worldSpace;
		const bool fromCache = globals::AllowLoadGrass() && world && info->HasCacheFile(world->editorID.c_str());
		const bool requireCollision = wanted == GrassState::Active && GrassPatchCreation::NeedsCollision(manager, fromCache);
		// Native placement can enter the loader while its callbacks need the map lock.
		lock.unlock();
		RE::BSSpinLockGuard grassLock(cell->grassCreateLock);
		lock.lock();
		if (!gridState.cellGrid || gridState.cellGrid->FindByCell(cell) != info || info->cell != cell || info->taskGeneration != generation || *abort) {
			if (info->cell == cell && info->taskGeneration == generation)
				info->ClearTask();

			lock.unlock();
			ClearCellAddGrassTask(cell, abort);
			return;
		}
		GrassPatchCreation patchCreation(cell, abort);
		if (!patchCreation.PrepareCell(requireCollision)) {
			info->ClearTask();
			lock.unlock();
			PhysicsRaycast::GetSingleton().DeferCell(cell);
			ClearCellAddGrassTask(cell, abort);
			return;
		}
		lock.unlock();

		const auto unavailableBefore = PhysicsRaycast::UnavailableQueries();
		if (have == GrassState::None) {
			RE::AddCellGrass(manager, cell, abort);
		} else {
			auto& state = State();
			ReattachRetiredGrass(cell);
			{
				std::scoped_lock retiredLock(state.retiredMutex);
				auto& retired = state.retiredGrass[cell];
				retired.entries = TakeGrassHandles(cell);
				retired.deadline = std::chrono::steady_clock::now() + RetiredGrassTimeout;
			}
			RE::AddCellGrass(manager, cell, abort);
			if (*abort)
				ReattachRetiredGrass(cell);
			state.swaps.fetch_add(1, std::memory_order_relaxed);
		}

		GrassPatchCreation::ReleaseNativePlacement();
		if (!*abort)
			patchCreation.AddCliffGrass(manager);

		// Collision that vanished mid-placement left some grass unfiltered; keep the cell replaceable and retry it.
		const bool unfiltered = requireCollision && PhysicsRaycast::UnavailableQueries() != unavailableBefore;
		lock.lock();
		if (info->cell == cell && info->taskGeneration == generation) {
			info->ClearTask();
			info->have = *abort ? have : unfiltered ? GrassState::Lod :
			                                          wanted;
		}
		lock.unlock();

		if (unfiltered && !*abort)
			PhysicsRaycast::GetSingleton().DeferCell(cell);
	}

	static void BuildCellGrass(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort)
	{
		if (*abort)
			return;

		{
			const Profiler::Scope profile;
			const stl::scope_exit releaseNative([] { GrassPatchCreation::ReleaseNativePlacement(); });
			UpdateCellGrass(manager, cell, abort);
		}

		if (gridState.cellGrid && gridState.streamingAllowed && !gridState.gridUpdatePending)
			gridState.cellGrid->UpdateDistantCells(RE::TES::GetSingleton());
	}

	static void ReleaseTask(bool* abort)
	{
		RE::AddCellGrassTask::FromAbort(abort)->DecRefCount();
	}

	// A shape's staging buffer belongs to the thread that starts it, so builds sharing a shape must not overlap.
	std::int64_t GrassWorkers::GrassBlockOf(const RE::TESObjectCELL* cell) const
	{
		const auto* coordinates = cell ? const_cast<RE::TESObjectCELL*>(cell)->GetCoordinates() : nullptr;
		if (!coordinates)
			return INT64_MIN;

		// Truncating division matches the engine's own block keys.
		const int divisor = Config::ExtendGrassDistance && GrassPatchCreation::IsEnabled() ? 1 : 12;
		return (static_cast<std::int64_t>(coordinates->cellX / divisor) << 32) | static_cast<std::uint32_t>(coordinates->cellY / divisor);
	}

	GrassWorkers& GrassWorkers::Get()
	{
		return State().workers;
	}

	GrassWorkers::~GrassWorkers()
	{
		{
			std::scoped_lock lock(mutex);
			stopping = true;
		}

		arena.execute([&] { tasks.wait(); });
		for (const auto& job : jobs)
			ReleaseTask(job.abort);
	}

	bool GrassWorkers::Dispatch()
	{
		while (!stopping && callbacks < ThreadCount && callbacks < jobs.size() + static_cast<std::size_t>(running.load(std::memory_order_relaxed))) {
			++callbacks;
			try {
				arena.enqueue(tasks.defer([this] { Work(); }));
			} catch (const std::bad_alloc&) {
				--callbacks;
				break;
			}
		}

		if (!stopping && !callbacks && !jobs.empty()) {
			++callbacks;
			return true;
		}

		return false;
	}

	void GrassWorkers::ReleaseBlock(std::int64_t block)
	{
		bool runInline;
		{
			std::scoped_lock lock(mutex, blockMutex);
			busyBlocks.erase(block);
			runInline = Dispatch();
		}

		blockReleased.notify_all();
		if (runInline)
			Work();
	}

	bool GrassWorkers::Submit(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort, CellHolder& holder)
	{
		const auto* coordinates = cell->GetCoordinates();
		if (!coordinates)
			return false;

		bool runInline;
		bool submitted = false;
		RE::AddCellGrassTask::FromAbort(abort)->IncRefCount();

		const stl::scope_exit releaseUnsubmitted([&] {
			if (!submitted)
				ReleaseTask(abort);
		});

		try {
			std::scoped_lock lock(mutex);
			if (stopping)
				return false;

			jobs.push_back({ manager, cell, abort, GrassBlockOf(cell), coordinates->cellX, coordinates->cellY });
			submitted = true;
			runInline = Dispatch();

		} catch (const std::bad_alloc&) {
			return false;
		}

		// Inline work can enter the loader; it must not keep the submitting cell held while streaming other cells.
		holder = {};
		if (runInline)
			Work();

		return true;
	}

	bool GrassWorkers::Busy()
	{
		std::scoped_lock lock(mutex);
		return running.load(std::memory_order_relaxed) > 0 || !jobs.empty();
	}

	std::optional<GrassWorkers::Job> GrassWorkers::Take()
	{
		const auto* tes = RE::TES::GetSingleton();
		const int centerX = tes ? tes->currentGridX : 0;
		const int centerY = tes ? tes->currentGridY : 0;

		std::scoped_lock blocks(blockMutex);
		auto best = jobs.end();
		int bestDistance = INT_MAX;

		for (auto it = jobs.begin(); it != jobs.end(); ++it) {
			const int distance = std::max(std::abs(it->x - centerX), std::abs(it->y - centerY));
			if (distance < bestDistance && !busyBlocks.contains(it->block)) {
				best = it;
				bestDistance = distance;
			}
		}

		if (best == jobs.end())
			return std::nullopt;

		Job job = *best;
		busyBlocks.insert(job.block);
		jobs.erase(best);
		running.fetch_add(1, std::memory_order_relaxed);

		return job;
	}

	std::optional<GrassWorkers::Job> GrassWorkers::Next()
	{
		std::scoped_lock lock(mutex);
		std::optional<Job> job;
		if (!stopping && !jobs.empty()) {
			try {
				job = Take();
			} catch (const std::bad_alloc&) {
				// Without memory to reserve its block the job cannot build, but is still taken so it can be deferred.
				job = jobs.front();
				job->reserved = false;
				jobs.pop_front();
			}
		}

		// Ending under the queue lock lets a job submitted afterwards dispatch a new callback.
		if (!job)
			--callbacks;

		return job;
	}

	void GrassWorkers::Run(const Job& job)
	{
		const stl::scope_exit finish([&] {
			ReleaseTask(job.abort);
			if (job.reserved) {
				running.fetch_sub(1, std::memory_order_relaxed);
				ReleaseBlock(job.block);
			}
		});

		try {
			if (job.reserved) {
				BuildCellGrass(job.manager, job.cell, job.abort);
				return;
			}
		} catch (const std::bad_alloc&) {
		}

		if (!*job.abort)
			DeferUnbuilt(job);
	}

	// Keep the last completed state; a failed first build has no LOD grass to retain.
	void GrassWorkers::DeferUnbuilt(const Job& job)
	{
		const auto holder = TryHoldTrackedCell(job.cell);
		if (!holder)
			return;

		std::unique_lock lock(gridState.cellMapMutex);
		if (*job.abort)
			return;

		const auto info = gridState.cellGrid ? gridState.cellGrid->FindByCell(job.cell) : nullptr;
		if (!info || info->cell != job.cell)
			return;

		info->ClearTask();
		gridState.cellGrid->RetryDistantCell(info->x, info->y);
		lock.unlock();
		ClearCellAddGrassTask(job.cell, job.abort);
		auto& physics = PhysicsRaycast::GetSingleton();
		physics.DeferCell(job.cell);
		physics.ReportAllocationFailure();
	}

	void GrassWorkers::Work()
	{
		try {
			PhysicsRaycast::EnsureThreadMemory();
			while (const auto job = Next())
				Run(*job);
		} catch (...) {
			std::scoped_lock lock(mutex);
			--callbacks;
			throw;
		}
	}

	std::int64_t GrassWorkers::AcquireBlock(const RE::TESObjectCELL* cell)
	{
		const auto block = GrassBlockOf(cell);
		std::unique_lock lock(blockMutex);
		blockReleased.wait(lock, [&] { return !busyBlocks.contains(block); });
		busyBlocks.insert(block);
		return block;
	}

	GrassModelPrefetch& GrassModelPrefetch::Get()
	{
		return State().modelPrefetch;
	}

	void GrassModelPrefetch::Request(RE::TESObjectCELL* cell)
	{
		auto* land = cell ? cell->GetRuntimeData().cellLand : nullptr;
		const auto* data = land ? land->loadedData : nullptr;
		if (!data)
			return;

		std::vector<const char*> models;
		const auto add = [&](RE::TESLandTexture* texture) {
			if (!texture)
				return;

			for (auto* grass : texture->textureGrassList) {
				const char* model = grass ? grass->GetModel() : nullptr;
				if (model && *model)
					models.push_back(model);
			}
		};

		for (auto* texture : data->defQuadTextures)
			add(texture);

		for (const auto& quadrant : data->quadTextures) {
			for (auto* texture : quadrant)
				add(texture);
		}

		bool queued = false;
		{
			std::scoped_lock lock(mutex);
			for (const auto* model : models) {
				if (requested.emplace(model).second) {
					jobs.emplace_back(model);
					queued = true;
				}
			}

			if (queued && !thread.joinable())
				thread = std::jthread([this](std::stop_token stop) { Work(stop); });
		}

		if (queued)
			ready.notify_all();
	}

	void GrassModelPrefetch::Reset()
	{
		std::vector<RE::NiPointer<RE::NiNode>> release;
		std::scoped_lock lock(mutex);
		++generation;
		jobs.clear();
		requested.clear();
		release.swap(loaded);
	}

	void GrassModelPrefetch::Work(std::stop_token stop)
	{
		for (;;) {
			std::string model;
			std::uint64_t started;
			{
				std::unique_lock lock(mutex);
				if (!ready.wait(lock, stop, [&] { return !jobs.empty(); }))
					return;

				model = std::move(jobs.front());
				jobs.pop_front();
				started = generation;
			}

			while (GrassWorkers::Get().Busy() && !stop.stop_requested())
				std::this_thread::sleep_for(std::chrono::milliseconds(20));

			RE::NiPointer<RE::NiNode> node;
			if (auto* manager = RE::BGSGrassManager::GetSingleton()) {
				RE::BSWriteLockGuard grassLock(manager->grassTypeLock);
				RE::BSModelDB::DBTraits::ArgsType args;

				if (RE::BSModelDB::Demand(model.c_str(), node, args) != RE::BSResource::ErrorCode::kNone)
					node.reset();
			}

			// Let waiting grass workers take the lock between models.
			std::this_thread::yield();
			if (!node)
				continue;

			std::scoped_lock lock(mutex);
			if (started == generation)
				loaded.push_back(std::move(node));
		}
	}

	void GrassNodeShards::Install()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_NiNode[0] };
		attachChild = vtable.write_vfunc(REL::Relocate(0x35, 0x35, 0x36), AttachChild);
		detachChildOut = vtable.write_vfunc(REL::Relocate(0x37, 0x37, 0x38), DetachChildOut);
		detachChild = vtable.write_vfunc(REL::Relocate(0x38, 0x38, 0x39), DetachChild);
	}

	RE::NiNode* GrassNodeShards::GrassNode()
	{
		auto* manager = RE::BGSGrassManager::GetSingleton();
		return manager ? manager->grassNode.get() : nullptr;
	}

	RE::NiNode* GrassNodeShards::FreeShard(RE::NiNode* root)
	{
		std::erase_if(shards, [root](const RE::NiPointer<RE::NiNode>& shard) { return shard->parent != root; });
		for (auto& shard : shards) {
			if (shard->GetChildren().size() < ShardCapacity)
				return shard.get();
		}

		RE::NiPointer<RE::NiNode> shard(RE::NiNode::Create(ShardCapacity));
		if (!shard)
			return nullptr;

		// The root's culling only visits children with bounds or kAlwaysDraw, and a shard's bound is not maintained.
		shard->GetFlags().set(RE::NiAVObject::Flag::kAlwaysDraw);
		shard->world = root->world;
		attachChild(root, shard.get(), true);

		return shards.emplace_back(std::move(shard)).get();
	}

	void GrassNodeShards::AttachChild(RE::NiNode* node, RE::NiAVObject* child, bool firstAvail)
	{
		auto* root = GrassNode();
		if (!child || node != root || child->AsNode())
			return attachChild(node, child, firstAvail);

		std::scoped_lock lock(mutex);
		if (auto* shard = FreeShard(root))
			attachChild(shard, child, true);  // Reuse detached slots so a shard's array never grows past its capacity.
		else
			attachChild(node, child, firstAvail);
	}

	RE::NiNode* GrassNodeShards::Owner(RE::NiNode* node, RE::NiAVObject* child)
	{
		auto* parent = child ? child->parent : nullptr;
		return parent && parent != node && node == GrassNode() && parent->parent == node ? parent : node;
	}

	void GrassNodeShards::DetachChildOut(RE::NiNode* node, RE::NiAVObject* child, RE::NiPointer<RE::NiAVObject>& out)
	{
		detachChildOut(Owner(node, child), child, out);
	}

	void GrassNodeShards::DetachChild(RE::NiNode* node, RE::NiAVObject* child)
	{
		detachChild(Owner(node, child), child);
	}

	void ReattachRetiredGrass(RE::TESObjectCELL* cell)
	{
		auto& state = State();
		RetiredGrass retired;
		{
			std::scoped_lock retiredLock(state.retiredMutex);
			const auto it = state.retiredGrass.find(cell);
			if (it == state.retiredGrass.end())
				return;

			retired = std::move(it->second);
			state.retiredGrass.erase(it);
		}

		ReplaceGrassHandles(RE::BGSGrassManager::GetSingleton(), cell, {}, retired.entries);
	}

	void RetireReplacedGrass()
	{
		auto& state = State();
		std::vector<RE::TESObjectCELL*> cells;
		{
			std::scoped_lock retiredLock(state.retiredMutex);
			for (const auto& entry : state.retiredGrass)
				cells.push_back(entry.first);
		}

		const auto now = std::chrono::steady_clock::now();
		for (auto* cell : cells) {
			CellHolder holder;
			{
				// A cell with a queued or running grass task still owns its lists.
				std::unique_lock lock(gridState.cellMapMutex, std::try_to_lock);
				if (!lock)
					return;
				const auto info = gridState.cellGrid ? gridState.cellGrid->FindByCell(cell) : nullptr;
				if (!info || info->cell != cell || info->queued)
					continue;
				holder = TryHoldTrackedCell(cell);
				if (!holder)
					continue;
			}

			RE::BSSpinLockGuard grassLock(cell->grassCreateLock);
			RetiredGrass retired;
			{
				std::scoped_lock retiredLock(state.retiredMutex);
				const auto it = state.retiredGrass.find(cell);
				if (it == state.retiredGrass.end())
					continue;

				bool faded = true;
				if (const auto* data = cell->extraList.GetByType<RE::ExtraCellGrassData>()) {
					for (const auto* handle : data->grassHandles) {
						const auto groups = handle ? handle->instanceGroupIndicies.size() : 0;
						faded &= !handle || (handle->instanceGroupFadeAlphas.size() >= groups &&
												std::ranges::all_of(handle->instanceGroupFadeAlphas, [](float alpha) { return alpha >= 1.0f; }));
					}
				}
				if (!faded && now < it->second.deadline)
					continue;

				retired = std::move(it->second);
				state.retiredGrass.erase(it);
			}

			ReplaceGrassHandles(RE::BGSGrassManager::GetSingleton(), cell, retired.entries, TakeGrassHandles(cell));
		}
	}

	void ReportStatistics()
	{
		logger::info("Grass swaps: {}", State().swaps.load());
	}

	void ResetGrassModelPrefetch()
	{
		GrassModelPrefetch::Get().Reset();
	}

	void QueueGrassCreation(RE::TESObjectCELL* cell)
	{
		CellHolder holder;
		if (gridState.cellGrid) {
			holder = TryHoldTrackedCell(cell);
			if (!holder)
				return;
		}

		auto* manager = RE::BGSGrassManager::GetSingleton();
		if (!UseGrassWorkers(manager, cell)) {
			RE::BSSpinLockGuard taskLock(cell->grassTaskLock);
			RE::AddCellGrassTask::Queue(cell);
			return;
		}

		GrassModelPrefetch::Get().Request(cell);

		// Retain a native task for cancellation while bypassing the IOManager queue.
		RE::NiPointer<RE::AddCellGrassTask> task;
		{
			RE::BSSpinLockGuard taskLock(cell->grassTaskLock);
			if (RE::AddCellGrassTask::Get(cell))
				return;

			task = RE::AddCellGrassTask::Create(cell);
			if (!task)
				return;

			RE::AddCellGrassTask::Set(cell, task.get());
		}
		if (!GrassWorkers::Get().Submit(manager, cell, task->Abort(), holder)) {
			RE::BSSpinLockGuard taskLock(cell->grassTaskLock);
			if (!*task->Abort() && RE::AddCellGrassTask::Get(cell) == task) {
				RE::AddCellGrassTask::Set(cell, nullptr);
				RE::AddCellGrassTask::Queue(cell);
			}
		}
	}

	void OnGrassCreated(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort)
	{
		if (!cell)
			return;

		// Stand-in cells are NGIO's own, and an aborted load still has to discard them.
		if (gridState.cacheOnlyCells) {
			gridState.cacheOnlyCells->Load(cell);
			return;
		}

		auto holder = TryHoldTrackedCell(cell);
		if (!holder)
			return;

		if (abort && *abort)
			return;

		if (abort && UseGrassWorkers(manager, cell)) {
			if (GrassWorkers::Get().Submit(manager, cell, abort, holder))
				return;

			if (*abort)
				return;
		}

		auto& workers = GrassWorkers::Get();
		const auto block = workers.AcquireBlock(cell);
		const stl::scope_exit releaseBlock([&] { workers.ReleaseBlock(block); });
		holder = {};

		bool none = false;
		BuildCellGrass(manager, cell, abort ? abort : &none);
	}
}
