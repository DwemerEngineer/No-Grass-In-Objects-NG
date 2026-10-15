#include "GrassControl/PhysicsRaycast.h"

#include "GrassControl/AddCellGrassTask.h"
#include "GrassControl/Config.h"
#include "GrassControl/DistantGrass.h"
#include "GrassControl/Engine.h"
#include "GrassControl/RaycastHelper.h"
#include "GrassControl/Util.h"

namespace GrassControl
{
	bool PhysicsRaycast::ValidDimension(double value) { return std::isfinite(value) && value > 0.001 && value <= MaxDimension; }

	bool PhysicsRaycast::ValidSettingsValues(int mode, double height, double depth, double width, double multiplier, double textureWidth)
	{
		return mode >= 0 && mode <= 2 && std::isfinite(height) && height >= 0 && height <= MaxDimension &&
		       std::isfinite(depth) && depth >= 0 && depth <= MaxDimension && ValidDimension(height + depth) &&
		       std::isfinite(width) && width >= 0 && width <= MaxDimension &&
		       std::isfinite(multiplier) && multiplier > 0 && multiplier <= 100 &&
		       std::isfinite(textureWidth) && textureWidth >= 0 && textureWidth <= MaxDimension;
	}

	bool PhysicsRaycast::ValidPoint(float x, float y, float z)
	{
		return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
		       std::abs(x) <= 100000000.0f && std::abs(y) <= 100000000.0f && std::abs(z) <= 100000000.0f;
	}

	bool PhysicsRaycast::ValidHit(float fraction, float x, float y, float z)
	{
		const double lengthSquared = double(x) * x + double(y) * y + double(z) * z;

		return std::isfinite(fraction) && fraction >= 0 && fraction <= 1 &&
		       std::isfinite(lengthSquared) && lengthSquared > 0.000001 && lengthSquared < 4.0;
	}

	// Count pending writers before polling so new grass readers yield to them.
	void PhysicsRaycast::WorldReadGate::EnterWriteLock(RE::BSReadWriteLock* lock)
	{
		auto* writerThread = reinterpret_cast<volatile long*>(lock);
		auto* state = writerThread + 1;
		const auto thread = static_cast<long>(GetCurrentThreadId());
		if (*writerThread == thread) {
			_InterlockedIncrement(state);
			return;
		}

		const bool watched = lock == watchedLock.load(std::memory_order_relaxed);
		if (watched)
			watchedWriters.fetch_add(1);
		for (std::uint32_t tries = 0; _InterlockedCompareExchange(state, static_cast<long>(0x80000001), 0);) {
			if (tries < 10000) {
				++tries;
				Sleep(0);
			} else {
				Sleep(1);
			}
		}
		if (watched)
			watchedWriters.fetch_sub(1);
		*writerThread = thread;
		_mm_mfence();
	}

	void PhysicsRaycast::WorldReadGate::Enter(RE::bhkWorld* world)
	{
		// A thread already inside the window must not wait for the window to drain.
		if (depth++)
			return;

		watchedLock.store(&world->worldLock, std::memory_order_relaxed);
		std::unique_lock lock(mutex);
		for (;;) {
			const auto now = std::chrono::steady_clock::now();
			if (!readers && now - opened >= WindowLength)
				reopenAt = std::max(reopenAt, opened + WindowLength + WriterGap);
			const bool writerWaiting = watchedWriters.load() != 0;
			if (readers && owner == world && now - opened < WindowLength && !writerWaiting) {
				++readers;
				return;
			}
			if (!readers && now >= reopenAt && !writerWaiting)
				break;
			if (readers) {
				drained.wait(lock);
			} else {
				const auto nextRead = reopenAt;
				lock.unlock();
				while (std::chrono::steady_clock::now() < nextRead || watchedWriters.load())
					std::this_thread::yield();
				lock.lock();
			}
		}
		owner = world;
		const auto now = std::chrono::steady_clock::now();
		if (now - opened >= WindowLength)
			opened = now;
		readers = 1;
		world->worldLock.LockForRead();
	}

	void PhysicsRaycast::WorldReadGate::Exit()
	{
		if (--depth)
			return;

		std::scoped_lock lock(mutex);
		if (--readers)
			return;
		owner->worldLock.UnlockForRead();
		owner = nullptr;
		const auto now = std::chrono::steady_clock::now();
		reopenAt = now;
		if (now - opened >= WindowLength || watchedWriters.load())
			reopenAt += WriterGap;
		drained.notify_all();
	}

	PhysicsRaycast::WorldReadGuard::WorldReadGuard(WorldReadGate& gate, RE::bhkWorld* world) :
		gate(gate)
	{
		gate.Enter(world);
	}

	PhysicsRaycast::WorldReadGuard::~WorldReadGuard()
	{
		gate.Exit();
	}

	void PhysicsRaycast::QueryResources::SetShape(int mode, float halfWidthX, float halfWidthY, float radius, float height)
	{
		auto* wrapper = shape.get();
		if (mode == 1) {
			auto* capsule = static_cast<RE::hkpCapsuleShape*>(wrapper->referencedObject.get());
			const float scaledRadius = radius * HavokScale;
			capsule->radius = scaledRadius;

			// Havok caches the radius in both endpoint W components.
			capsule->vertexA = RE::hkVector4(0.0f, 0.0f, 0.0f, scaledRadius);
			capsule->vertexB = RE::hkVector4(0.0f, 0.0f, height * HavokScale, scaledRadius);
		} else {
			auto* box = static_cast<RE::hkpBoxShape*>(wrapper->referencedObject.get());
			const float halfExtentX = halfWidthX * HavokScale;
			const float halfExtentY = halfWidthY * HavokScale;
			const float halfExtentZ = std::abs(height) * HavokScale / 2.0f;

			// Havok caches the smallest half extent in W.
			box->halfExtents = RE::hkVector4(halfExtentX, halfExtentY, halfExtentZ, std::min({ halfExtentX, halfExtentY, halfExtentZ }));
		}
	}

	bool PhysicsRaycast::ValidRay(const glm::vec4& start, const glm::vec4& end)
	{
		const auto difference = end - start;
		return ValidPoint(start.x, start.y, start.z) && ValidPoint(end.x, end.y, end.z) &&
		       ValidDimension(glm::length(glm::vec3(difference)));
	}

	void PhysicsRaycast::WorldReadGate::InstallWriterSignal()
	{
		if (installed)
			return;
		const auto address = RELOCATION_ID(66977, 68234).address();
		if (REL::make_pattern<"E9">().match(address) || REL::make_pattern<"FF 25">().match(address)) {
			logger::warn("World write lock is already hooked; retaining its handler and using timed query windows.");
			return;
		}
		SKSE::GetTrampoline().write_branch<5>(address, PhysicsRaycast::EnterWriteLock);
		installed = true;
	}

	void PhysicsRaycast::InstallWriterSignal()
	{
		GetSingleton().worldReadGate.InstallWriterSignal();
	}

	void PhysicsRaycast::EnterWriteLock(RE::BSReadWriteLock* lock)
	{
		GetSingleton().worldReadGate.EnterWriteLock(lock);
	}

	PhysicsRaycast& PhysicsRaycast::GetSingleton()
	{
		static PhysicsRaycast instance;
		return instance;
	}

	// The deleting destructor returns this storage to the current Havok heap.
	RE::hkpSimpleShapePhantom* PhysicsRaycast::AllocatePhantom()
	{
		auto* router = RE::hkMemoryRouter::GetInstancePtr();
		return router && router->Heap ? static_cast<RE::hkpSimpleShapePhantom*>(router->Heap->BlockAlloc(sizeof(RE::hkpSimpleShapePhantom))) : nullptr;
	}

	const RE::hkpCdBody* PhysicsRaycast::RootBody(const RE::hkpCdBody& body)
	{
		const auto* root = &body;
		std::size_t depth = 0;

		while (root->parent) {
			if (++depth > 64)
				return nullptr;

			root = root->parent;
		}

		return root;
	}

	bool PhysicsRaycast::CopyHit(const RE::hkpCdBody& body, const RaycastHelper& settings, bool ignoreCliff, HitResult& hit, bool& invalid)
	{
		const auto* root = RootBody(body);
		if (!root) {
			invalid = true;
			return false;
		}

		const auto* collidable = static_cast<const RE::hkpCollidable*>(root);

		// The remaining bits encode part/flags/system group, not a layer.
		const auto layer = collidable->broadPhaseHandle.collisionFilterInfo.filter & 0x7F;
		if (layer >= 64 || !(settings.RaycastMask & (1ULL << layer)))
			return false;

		RE::NiPointer<RE::TESObjectREFR> reference(RE::TESHavokUtilities::FindCollidableRef(*collidable));
		if (reference) {
			if (const auto* form = reference->GetBaseObject())
				hit.formID = form->formID;
		}

		if (settings.Ignore && settings.Ignore->Contains(hit.formID))
			return false;

		hit.hitCliff = settings.IsCliff(hit.formID);
		// Shape-filtered cliffs are only partly cliff, so the caller decides once it knows which shape was hit.
		const bool shapeRule = hit.hitCliff && settings.HasCliffShapeRule(hit.formID);
		if (ignoreCliff && hit.hitCliff && !shapeRule)
			return false;

		if (reference && (shapeRule || settings.partIgnoredObjects.contains(hit.formID)))
			hit.reference = reference->GetHandle();

		return true;
	}

	bool PhysicsRaycast::CellHasCollision(const RE::TESObjectCELL& cell) const
	{
		const auto* loaded = cell.GetRuntimeData().loadedData;
		return cell.IsAttached() && loaded && loaded->refsFullyLoaded && !loaded->criticalQueuedRefCount && !loaded->queuedRefCount;
	}

	PhysicsRaycast::RayResult PhysicsRaycast::CastInWorld(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL& cell, const RaycastHelper& settings, bool ignoreCliff, RE::GrassParam* param, bool volumeQuery)
	{
		if (queriesDisabled)
			return { QueryStatus::Unavailable, {}, "loading disabled queries" };

		// Attachment can precede the reference tasks that add collision to Havok.
		if (!CellHasCollision(cell) || cell.IsInteriorCell())
			return { QueryStatus::Unavailable, {}, "cell references are not loaded" };

		// queryMutex keeps loading from replacing the borrowed world wrapper.
		RE::NiPointer<RE::bhkWorld> owner(cell.GetbhkWorld());
		if (!owner)
			return { QueryStatus::Unavailable, {}, "no Havok world wrapper" };

		const auto* router = RE::hkMemoryRouter::GetInstancePtr();
		if (!router || !router->Heap)
			return { QueryStatus::Unavailable, {}, "no Havok memory router or heap on worker" };

		WorldReadGuard worldLock(worldReadGate, owner.get());

		RE::hkRefPtr<RE::hkpWorld> world(owner->GetWorld1());
		if (!world || !CellHasCollision(cell) || cell.GetbhkWorld() != owner.get())
			return { QueryStatus::Unavailable, {}, "Havok world changed or cell is unloading" };

		const auto havokStart = start * HavokScale;
		const auto havokEnd = end * HavokScale;

		if (volumeQuery) {
			if (!world->collisionInput)
				return { QueryStatus::Unavailable, {}, "no Havok collision input" };

			float halfWidthX = 20, halfWidthY = 20, radius = 20;
			if (settings.Width > 0) {
				halfWidthX = halfWidthY = radius = static_cast<float>(settings.Width / 2);
			} else if (param) {
				if (auto* grass = RE::TESForm::LookupByID<RE::TESGrass>(param->grassFormID)) {
					const auto& bounds = grass->boundData;
					if (bounds.boundMax.x != 0 && bounds.boundMin.x != 0) {
						halfWidthX = std::abs(float(bounds.boundMax.x - bounds.boundMin.x)) / 2;
						halfWidthY = std::abs(float(bounds.boundMax.y - bounds.boundMin.y)) / 2;
						radius = std::max(halfWidthX, halfWidthY) / 2;
					}

					radius *= static_cast<float>(settings.WidthMultiplier);
				}
			}

			const float height = end.z - start.z;
			if (!ValidDimension(height) ||
				(settings.Mode == 1 ? !ValidDimension(radius) : (!ValidDimension(halfWidthX) || !ValidDimension(halfWidthY))))
				return { QueryStatus::InvalidInput, {} };

			if (!resources.PrepareShape(settings, halfWidthX, halfWidthY, radius, height))
				return { QueryStatus::Failed, {} };

			const RE::hkVector4 position(havokStart.x, havokStart.y, settings.Mode == 1 ? havokStart.z : (havokStart.z + havokEnd.z) / 2, 1.0f);
			resources.shapePhantom->SetPosition(position);

			BodyCollector collector(settings, ignoreCliff, resources.shapePhantom->GetCollidable());

			RE::GetPenetrations(world.get(), resources.shapePhantom->GetCollidable(), *world->collisionInput, &collector);
			if (collector.invalid)
				return { QueryStatus::Failed, {} };

			return { QueryStatus::Success, std::move(collector.hits) };
		}

		RayCollector collector(settings, ignoreCliff);

		RE::hkpWorldRayCastInput input{};
		input.from = RE::hkVector4(havokStart.x, havokStart.y, havokStart.z, 1);
		input.to = RE::hkVector4(havokEnd.x, havokEnd.y, havokEnd.z, 1);

		// Use the collector overload so ignored hits cannot hide other blockers.
		RE::CastRay(world.get(), input, collector);
		if (collector.invalid)
			return { QueryStatus::Failed, {} };

		return { QueryStatus::Success, std::move(collector.hits) };
	}

	PhysicsRaycast::RayResult PhysicsRaycast::Query(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, const RaycastHelper& settings, bool ignoreCliff, RE::GrassParam* param, bool volumeQuery)
	{
		if (!settings.Valid || !ValidRay(start, end) || (volumeQuery && settings.Mode == 0))
			return { QueryStatus::InvalidInput, {} };

		if (!cell)
			return {};

		std::shared_lock lock(queryMutex);

		if (resources.errorCell != cell->formID) {
			resources.errorCell = cell->formID;
			resources.errors = 0;
		}

		RayResult result;
		try {
			result = CastInWorld(start, end, *cell, settings, ignoreCliff, param, volumeQuery);
		} catch (const std::bad_alloc&) {
			result = { QueryStatus::Failed, {} };
			resources.Reset();
		}

		queryCounts[static_cast<std::size_t>(result.status)].fetch_add(1, std::memory_order_relaxed);
		if (result.status == QueryStatus::Success && !result.hits.empty())
			queriesWithHits.fetch_add(1, std::memory_order_relaxed);
		if (result.status == QueryStatus::Unavailable) {
			lastUnavailableReason = result.unavailableReason;
			++unavailableQueries;
		}

		if (result.status == QueryStatus::Failed || result.status == QueryStatus::InvalidInput) {
			if (resources.errors < 20) {
				++resources.errors;
				logger::warn("Raycast query failed in cell 0x{:x}, status {}", cell->formID, static_cast<int>(result.status));
				if (resources.errors == 20)
					warningPending = true;
			}
		}

		return result;
	}

	void PhysicsRaycast::HavokDelete::operator()(RE::hkReferencedObject* object) const
	{
		if (object)
			object->RemoveReference();
	}

	PhysicsRaycast::RayCollector::RayCollector(const RaycastHelper& settings, bool ignoreCliff) : settings(settings), ignoreCliff(ignoreCliff) { earlyOutHitFraction = 1.0f; }

	void PhysicsRaycast::RayCollector::AddRayHit(const RE::hkpCdBody& body, const RE::hkpShapeRayCastCollectorOutput& output)
	{
		if (invalid)
			return;

		HitResult hit{};
		hit.hitFraction = output.hitFraction;
		hit.normal = { output.normal.quad.m128_f32[0], output.normal.quad.m128_f32[1], output.normal.quad.m128_f32[2] };

		if (!ValidHit(hit.hitFraction, hit.normal.x, hit.normal.y, hit.normal.z)) {
			invalid = true;
			return;
		}

		hit.normal = glm::normalize(hit.normal);

		if (CopyHit(body, settings, ignoreCliff, hit, invalid)) {
			if (hits.size() >= MaxHits) {
				invalid = true;
				return;
			}

			try {
				hits.push_back(std::move(hit));
			} catch (const std::bad_alloc&) {
				// Never unwind a plugin allocation failure through Havok callbacks.
				invalid = true;
				earlyOutHitFraction = 0.0f;
			}
		}
	}

	PhysicsRaycast::BodyCollector::BodyCollector(const RaycastHelper& settings, bool ignoreCliff, const RE::hkpCdBody* queryBody) :
		settings(settings), ignoreCliff(ignoreCliff), queryBody(queryBody)
	{}

	PhysicsRaycast::BodyCollector::~BodyCollector() = default;

	void PhysicsRaycast::BodyCollector::addCdBodyPair(const RE::hkpCdBody& a, const RE::hkpCdBody& b)
	{
		if (invalid)
			return;

		HitResult hit{};
		const auto* rootA = RootBody(a);
		const auto* rootB = RootBody(b);
		if (!rootA || !rootB) {
			invalid = true;
			return;
		}

		// Compound agents can pass child bodies instead of the original collidables.
		if (rootA != queryBody && rootB != queryBody) {
			invalid = true;
			return;
		}

		const auto* other = rootA == queryBody ? rootB : rootA;
		if (CopyHit(*other, settings, ignoreCliff, hit, invalid)) {
			if (hits.size() >= MaxHits) {
				invalid = true;
				return;
			}

			try {
				hits.push_back(std::move(hit));
			} catch (const std::bad_alloc&) {
				invalid = true;
				earlyOut = true;
			}
		}
	}

	void PhysicsRaycast::BodyCollector::Reset()
	{
		earlyOut = false;
		hits.clear();
		invalid = false;
	}

	void PhysicsRaycast::QueryResources::Reset()
	{
		shapePhantom.reset();
		shape.reset();
		shapeMode = -1;
	}

	bool PhysicsRaycast::QueryResources::PrepareShape(const RaycastHelper& settings, float halfWidthX, float halfWidthY, float radius, float height)
	{
		if (shapeMode != settings.Mode)
			Reset();

		if (!shape) {
			auto* storage = RE::malloc<RE::bhkShape>(0x28);
			if (!storage)
				return false;

			RE::bhkShape* constructed = nullptr;
			if (settings.Mode == 1) {
				RE::NiPoint3 bottom(0, 0, 0);
				RE::NiPoint3 top(0, 0, height);

				constructed = RE::ConstructCapsuleShape(storage, bottom, top, radius);
			} else {
				RE::NiPoint3 extents(halfWidthX, halfWidthY, height / 2);

				constructed = RE::ConstructBoxShape(storage, &extents);

				if (constructed && REL::Module::IsAE()) {
					BoxInfo info{};
					info.halfExtents = RE::hkVector4(halfWidthX * HavokScale, halfWidthY * HavokScale, height * HavokScale / 2, 0);
					RE::InitializeBoxShape(constructed, &info);
				}
			}

			if (!constructed) {
				RE::free(storage);
				return false;
			}

			shape.reset(constructed);
			shapeMode = settings.Mode;
		}

		if (!shape->referencedObject)
			return false;

		SetShape(settings.Mode, halfWidthX, halfWidthY, radius, height);

		if (!shapePhantom) {
			auto* storage = AllocatePhantom();
			if (!storage)
				return false;

			RE::hkTransform identityTransform{};
			identityTransform.rotation.col0 = RE::hkVector4(1, 0, 0, 0);
			identityTransform.rotation.col1 = RE::hkVector4(0, 1, 0, 0);
			identityTransform.rotation.col2 = RE::hkVector4(0, 0, 1, 0);
			identityTransform.translation = RE::hkVector4(0, 0, 0, 1);

			auto* constructed = RE::ConstructSimpleShapePhantom(storage, static_cast<RE::hkpShape*>(shape->referencedObject.get()), identityTransform, 0);
			if (!constructed) {
				RE::hkMemoryRouter::GetInstance().Heap->BlockFree(storage, sizeof(RE::hkpSimpleShapePhantom));
				return false;
			}

			shapePhantom.reset(constructed);
		}

		return true;
	}

	PhysicsRaycast::RayResult PhysicsRaycast::CastRay(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, const RaycastHelper& settings, bool ignoreCliff)
	{
		return Query(start, end, cell, settings, ignoreCliff, nullptr, false);
	}

	PhysicsRaycast::RayResult PhysicsRaycast::CastVolume(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, RE::GrassParam* param, const RaycastHelper& settings, bool ignoreCliff)
	{
		return Query(start, end, cell, settings, ignoreCliff, param, true);
	}

	bool PhysicsRaycast::ValidateSettings(const RaycastHelper& settings)
	{
		return ValidSettingsValues(settings.Mode, settings.RayHeight, settings.RayDepth, settings.Width, settings.WidthMultiplier, settings.TextureWidth);
	}

	void PhysicsRaycast::SetQueriesDisabled(bool disabled)
	{
		std::scoped_lock lock(queryMutex, pendingMutex);

		if (disabled && !queriesDisabled)
			pendingCells.clear();

		queriesDisabled = disabled;
		if (disabled) {
			std::scoped_lock cacheLock(shapeCache.mutex);
			shapeCache.snapshots.clear();
			shapeCache.snapshotsTaken = std::chrono::steady_clock::now();
		}

		resources.Reset();
		resources.errors = 0;
		warningPending = false;
	}

	bool PhysicsRaycast::CanFilterCell(const RE::TESObjectCELL& cell)
	{
		std::shared_lock lock(queryMutex);

		if (queriesDisabled || !CellHasCollision(cell))
			return false;

		// Wait for neighboring cells whose collision can extend into this cell, without waiting for the entire loaded grid.
		const auto* tes = RE::TES::GetSingleton();
		const auto* grid = tes ? tes->gridCells : nullptr;
		const auto* coordinates = const_cast<RE::TESObjectCELL&>(cell).GetCoordinates();
		if (!grid || !coordinates)
			return false;

		for (std::uint32_t x = 0; x < grid->length; ++x) {
			for (std::uint32_t y = 0; y < grid->length; ++y) {
				auto* neighbor = grid->GetCell(x, y);
				// Slots past a worldspace's edge hold no cell and have no collision to wait for.
				const auto* at = neighbor ? neighbor->GetCoordinates() : nullptr;
				if (!at)
					continue;
				if (std::abs(at->cellX - coordinates->cellX) <= 1 && std::abs(at->cellY - coordinates->cellY) <= 1 && !CellHasCollision(*neighbor))
					return false;
			}
		}

		// Loaded references can still have bodies queued for addition to the Havok world.
		RE::NiPointer<RE::bhkWorld> owner(cell.GetbhkWorld());
		if (!owner)
			return false;

		RE::BSReadLockGuard worldLock(owner->worldLock);
		const auto* world = owner->GetWorld1();
		return world && !world->pendingOperationsCount && !world->pendingBodyOperationsCount && !world->criticalOperationsLockCount;
	}

	void PhysicsRaycast::DeferCell(RE::TESObjectCELL* cell)
	{
		if (!cell || cell->IsInteriorCell())
			return;

		std::scoped_lock lock(pendingMutex);

		try {
			pendingCells.insert(cell->formID);
		} catch (const std::bad_alloc&) {
			warningPending = true;
		}
	}

	void PhysicsRaycast::RetryPendingCells()
	{
		auto* tes = RE::TES::GetSingleton();
		auto* manager = RE::BGSGrassManager::GetSingleton();
		if (!tes || !manager || manager->generateGrassDataFiles)
			return;

		// Collision arrives over several frames, so polling faster only repeats the same failed checks.
		const auto now = std::chrono::steady_clock::now();
		if (now - lastRetry < std::chrono::milliseconds(100))
			return;

		lastRetry = now;

		if (queriesDisabled)
			return;

		std::unordered_set<RE::FormID> waiting;
		{
			std::scoped_lock lock(pendingMutex);
			waiting.swap(pendingCells);
		}

		for (const auto id : waiting) {
			auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
			if (!cell || cell->IsInteriorCell() || cell->GetRuntimeData().worldSpace != tes->GetRuntimeData2().worldSpace)
				continue;

			if (!Config::ExtendGrassDistance && !CanFilterCell(*cell)) {
				DeferCell(cell);
				continue;
			}

			bool queued = false;
			{
				RE::BSSpinLockGuard taskLock(cell->grassTaskLock);
				if (auto* task = RE::AddCellGrassTask::Get(cell->extraList)) {
					task->Release();
					queued = true;
				}
			}

			if (queued || !DistantGrass::QueueGrassPlacement(cell))
				DeferCell(cell);
		}
	}

	void PhysicsRaycast::ThreadMemory::Send(std::uint32_t type) const
	{
		ThreadEvent event{ this, type };
		RE::SendHavokThreadEvent(&event);
	}

	PhysicsRaycast::ThreadMemory::~ThreadMemory()
	{
		if (!started)
			return;

		// This thread's query phantom is freed through its router, so it must go before the router does.
		resources.Reset();
		Send(1);
	}

	bool PhysicsRaycast::EnsureThreadMemory()
	{
		if (const auto* router = RE::hkMemoryRouter::GetInstancePtr())
			return router->Heap != nullptr;

		// The Havok handler accepts paired BSThread start and exit events for non-engine threads.
		// The opaque thread key must match across both events; the receiver is unused.
		// Construct query storage first so ThreadMemory can reset it before either thread-local object is destroyed.
		resources.Reset();
		static thread_local ThreadMemory memory;
		if (!memory.started) {
			memory.Send(0);
			memory.started = true;
		}

		const auto* router = RE::hkMemoryRouter::GetInstancePtr();
		return router && router->Heap;
	}

	void PhysicsRaycast::ReportAllocationFailure()
	{
		warningPending = true;
	}

	void PhysicsRaycast::ReportStatistics()
	{
		std::scoped_lock lock(pendingMutex);

		logger::info("Physics queries: completed {}, with blockers {}, unavailable {}, invalid {}, failed {}; pending first placements {}, queries disabled {}; last unavailable: {}", queryCounts[0].load(), queriesWithHits.load(), queryCounts[1].load(), queryCounts[2].load(), queryCounts[3].load(), pendingCells.size(), queriesDisabled.load(), lastUnavailableReason.load());
	}

	void PhysicsRaycast::Update()
	{
		RetryPendingCells();

		if (warningPending.exchange(false) && Config::RayCastError)
			RE::DebugMessageBox("NGIO could not complete repeated physics queries. Grass placement used its fallback behavior. See the NGIO log for the affected cell and query status.");
	}

	std::optional<PhysicsRaycast::ShapeNames> PhysicsRaycast::GetShapeNames(RE::ObjectRefHandle handle, RE::NiPoint3 position)
	{
		std::shared_lock lock(queryMutex);

		if (queriesDisabled)
			return std::nullopt;

		const auto reference = handle.get();
		if (!reference)
			return std::nullopt;

		const auto* cell = reference->GetParentCell();
		if (!cell || !cell->IsAttached())
			return std::nullopt;

		std::shared_ptr<const Util::ShapeSnapshot> snapshot;
		{
			std::scoped_lock cacheLock(shapeCache.mutex);
			const auto now = std::chrono::steady_clock::now();
			if (now - shapeCache.snapshotsTaken > std::chrono::seconds(2)) {
				shapeCache.snapshots.clear();
				shapeCache.snapshotsTaken = now;
			}

			if (const auto it = shapeCache.snapshots.find(handle.native_handle()); it != shapeCache.snapshots.end())
				snapshot = it->second;
		}

		if (!snapshot) {
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer)
				return std::nullopt;

			std::optional<Util::ShapeSnapshot> captured;
			{
				renderer->Lock();
				const SKSE::stl::scope_exit rendererLock([renderer] { renderer->Unlock(); });

				RE::NiPointer<RE::NiAVObject> root(reference->Get3D());
				if (root)
					captured = Util::CaptureShapes(root.get());
			}

			// A failed capture is cached too, so a missing or oversized model is not walked again for every candidate.
			snapshot = captured ? std::make_shared<const Util::ShapeSnapshot>(std::move(*captured)) : std::make_shared<const Util::ShapeSnapshot>();
			std::scoped_lock cacheLock(shapeCache.mutex);
			shapeCache.snapshots.try_emplace(handle.native_handle(), snapshot);
		}

		if (const auto* shape = Util::ClosestShape(*snapshot, position))
			return ShapeNames{ shape->name, snapshot->root };

		return std::nullopt;
	}
}
