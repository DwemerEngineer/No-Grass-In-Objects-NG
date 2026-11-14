#include "GrassControl/CollisionGrid.h"

#include "GrassControl/Config.h"
#include "GrassControl/Globals.h"
#include "GrassControl/PhysicsRaycast.h"

namespace GrassControl::CollisionGrid
{
	static RE::NiPoint3 ToPoint3(const RE::hkVector4& vector)
	{
		return { vector.quad.m128_f32[0], vector.quad.m128_f32[1], vector.quad.m128_f32[2] };
	}

	static bool CastShape(const RE::hkpShape* shape, const RE::hkpShapeRayCastInput& input, RE::hkpShapeRayCastOutput& output)
	{
		using CastRay = bool* (*)(const RE::hkpShape*, bool*, const RE::hkpShapeRayCastInput&, RE::hkpShapeRayCastOutput&);
		const auto* vtable = *reinterpret_cast<void* const* const*>(shape);
		const auto castRay = reinterpret_cast<CastRay>(vtable[8]);
		bool hit = false;
		castRay(shape, &hit, input, output);

		return hit;
	}

	static bool Overlaps(const Bounds& bounds, const RE::NiPoint3& low, const RE::NiPoint3& high)
	{
		return bounds.min.x <= high.x && bounds.max.x >= low.x &&
		       bounds.min.y <= high.y && bounds.max.y >= low.y &&
		       bounds.min.z <= high.z && bounds.max.z >= low.z;
	}

	CellGeometry::CellGeometry(int x, int y, std::vector<Instance> a_instances, std::vector<Object> a_objects) :
		origin(x * CellSize - Margin, y * CellSize - Margin, 0.0f), instances(std::move(a_instances)), objects(std::move(a_objects)), bins(BinCount * BinCount)
	{
		for (std::uint32_t index = 0; index < instances.size(); ++index) {
			const auto& bounds = instances[index].bounds;
			const int minBinX = std::clamp(static_cast<int>((bounds.min.x - origin.x) / BinSize), 0, BinCount - 1);
			const int maxBinX = std::clamp(static_cast<int>((bounds.max.x - origin.x) / BinSize), 0, BinCount - 1);
			const int minBinY = std::clamp(static_cast<int>((bounds.min.y - origin.y) / BinSize), 0, BinCount - 1);
			const int maxBinY = std::clamp(static_cast<int>((bounds.max.y - origin.y) / BinSize), 0, BinCount - 1);

			for (int binX = minBinX; binX <= maxBinX; ++binX)
				for (int binY = minBinY; binY <= maxBinY; ++binY)
					bins[binY * BinCount + binX].push_back(index);
		}
	}

	template <class F>
	void CellGeometry::ForCandidates(const RE::NiPoint3& start, const RE::NiPoint3& end, F&& visit) const
	{
		const RE::NiPoint3 low{ std::min(start.x, end.x), std::min(start.y, end.y), std::min(start.z, end.z) };
		const RE::NiPoint3 high{ std::max(start.x, end.x), std::max(start.y, end.y), std::max(start.z, end.z) };

		const float binX = (low.x - origin.x) / BinSize;
		const float binY = (low.y - origin.y) / BinSize;
		const bool vertical = low.x == high.x && low.y == high.y;

		if (vertical && binX >= 0 && binY >= 0 && binX < BinCount && binY < BinCount) {
			for (const auto index : bins[static_cast<int>(binY) * BinCount + static_cast<int>(binX)]) {
				if (Overlaps(instances[index].bounds, low, high))
					visit(index);
			}

			return;
		}

		for (std::uint32_t index = 0; index < instances.size(); ++index) {
			if (Overlaps(instances[index].bounds, low, high))
				visit(index);
		}
	}

	std::optional<Hit> CellGeometry::Cast(const Instance& instance, const RE::NiPoint3& start, const RE::NiPoint3& end) const
	{
		constexpr float HavokScale = 0.0142875f;
		const auto* body = instance.body;
		auto localStart = instance.toLocal * start * HavokScale;
		auto localEnd = instance.toLocal * end * HavokScale;

		if (body->hasOffset) {
			const auto inverseRotation = body->offsetRotation.Transpose();
			localStart = inverseRotation * (localStart - body->offsetTranslation);
			localEnd = inverseRotation * (localEnd - body->offsetTranslation);
		}

		RE::hkpShapeRayCastInput input{};
		input.from = RE::hkVector4(localStart.x, localStart.y, localStart.z, 0.0f);
		input.to = RE::hkVector4(localEnd.x, localEnd.y, localEnd.z, 0.0f);

		RE::hkpShapeRayCastOutput output;
		output.Reset();

		if (!CastShape(body->shape, input, output) || !(output.hitFraction >= 0.0f && output.hitFraction <= 1.0f))
			return std::nullopt;

		auto normal = ToPoint3(output.normal);
		if (body->hasOffset)
			normal = body->offsetRotation * normal;

		normal = instance.toWorld.rotate * normal;
		const float length = normal.Length();
		normal = length > 0.0f ? normal / length : RE::NiPoint3{ 0.0f, 0.0f, 1.0f };

		return Hit{ &instance, output.hitFraction, normal };
	}

	bool CellGeometry::CastRay(const RE::NiPoint3& start, const RE::NiPoint3& end, std::uint64_t layerMask, std::vector<Hit>& hits) const
	{
		if (!PhysicsRaycast::EnsureThreadMemory())
			return false;

		ForCandidates(start, end, [&](std::uint32_t index) {
			const auto& instance = instances[index];
			if (instance.body->layer >= 64 || !(layerMask & (1ULL << instance.body->layer)))
				return;

			if (const auto hit = Cast(instance, start, end))
				hits.push_back(*hit);
		});

		return true;
	}

	bool CellGeometry::CastVolume(const RE::NiPoint3& start, const RE::NiPoint3& end, float halfWidthX, float halfWidthY, std::uint64_t layerMask, std::vector<Hit>& hits) const
	{
		if (!PhysicsRaycast::EnsureThreadMemory())
			return false;

		std::array<RE::NiPoint3, 5> offsets{};
		if (halfWidthY > 0.0f) {
			offsets[1] = { -halfWidthX, -halfWidthY, 0.0f };
			offsets[2] = { -halfWidthX, halfWidthY, 0.0f };
			offsets[3] = { halfWidthX, -halfWidthY, 0.0f };
			offsets[4] = { halfWidthX, halfWidthY, 0.0f };
		} else if (halfWidthX > 0.0f) {
			offsets[1] = { halfWidthX, 0.0f, 0.0f };
			offsets[2] = { -halfWidthX, 0.0f, 0.0f };
			offsets[3] = { 0.0f, halfWidthX, 0.0f };
			offsets[4] = { 0.0f, -halfWidthX, 0.0f };
		}

		const std::size_t sampleCount = halfWidthX > 0.0f || halfWidthY > 0.0f ? offsets.size() : 1;
		std::unordered_map<std::uint32_t, Hit> nearestHits;

		for (const auto& offset : std::span{ offsets }.first(sampleCount)) {
			const auto sampleStart = start + offset;
			const auto sampleEnd = end + offset;

			ForCandidates(sampleStart, sampleEnd, [&](std::uint32_t index) {
				const auto& instance = instances[index];
				if (instance.body->layer >= 64 || !(layerMask & (1ULL << instance.body->layer)))
					return;

				if (const auto hit = Cast(instance, sampleStart, sampleEnd)) {
					auto [entry, inserted] = nearestHits.try_emplace(index, *hit);
					if (!inserted && hit->fraction < entry->second.fraction)
						entry->second = *hit;
				}
			});
		}

		for (const auto& [index, hit] : nearestHits)
			hits.push_back(hit);

		return true;
	}

	void Grid::Start(int size)
	{
		radius = size / 2;
		cellCapacity = std::max<std::size_t>(512, static_cast<std::size_t>(2 * size * size));

		for (unsigned i = 0; i < LoaderThreads; ++i)
			workers.emplace_back([this](std::stop_token stop) { LoadCells(stop); });
	}

	void Grid::Follow(RE::FormID world, int x, int y)
	{
		{
			std::scoped_lock lock(mutex);
			if (world == centerWorld && x == centerX && y == centerY)
				return;

			centerWorld = world;
			centerX = x;
			centerY = y;

			std::erase_if(loadQueue, [&](const Queued& entry) {
				if (InGrid(entry.key))
					return false;

				pendingCells.erase(entry.key);

				return true;
			});

			const int loadedRadius = globals::GridsToLoad() / 2;
			for (int dx = -radius; dx <= radius; ++dx) {
				for (int dy = -radius; dy <= radius; ++dy) {
					if (std::max(std::abs(dx), std::abs(dy)) <= loadedRadius)
						continue;

					const Key key{ world, x + dx, y + dy };
					if (!loadedCells.contains(key) && pendingCells.insert(key).second)
						loadQueue.push_back({ key, {} });
				}
			}
		}

		wake.notify_all();
	}

	std::shared_ptr<const CellGeometry> Grid::Find(RE::FormID world, int x, int y) const
	{
		struct Cached
		{
			Key key;
			std::uint64_t generation;
			std::shared_ptr<const CellGeometry> geometry;
		};
		static thread_local Cached cached{};

		const Key key{ world, x, y };
		const auto currentGeneration = generation.load();
		if (cached.geometry && cached.key == key && cached.generation == currentGeneration)
			return cached.geometry;

		std::shared_ptr<const CellGeometry> geometry;
		{
			std::scoped_lock lock(mutex);
			if (const auto it = loadedCells.find(key); it != loadedCells.end())
				geometry = it->second.geometry;
		}

		if (geometry)
			cached = { key, currentGeneration, geometry };

		return geometry;
	}

	std::vector<std::pair<int, int>> Grid::TakeReady(RE::FormID world)
	{
		std::vector<Key> completed;
		{
			std::scoped_lock lock(mutex);
			completed.swap(completedCells);
		}

		std::vector<std::pair<int, int>> cells;
		for (const auto& key : completed) {
			if (key.world == world)
				cells.emplace_back(key.x, key.y);
		}

		return cells;
	}

	void Grid::Clear()
	{
		++generation;
		{
			std::scoped_lock lock(mutex);
			loadedCells.clear();
			loadedOrder.clear();
			loadQueue.clear();
			pendingCells.clear();
			completedCells.clear();
			centerWorld = 0;
		}

		{
			std::scoped_lock lock(placementMutex);
			placements.clear();
			placementOrder.clear();
		}

		PlacementReader::Clear();
	}

	bool Grid::InGrid(const Key& key) const
	{
		return key.world == centerWorld && std::max(std::abs(key.x - centerX), std::abs(key.y - centerY)) <= radius;
	}

	std::optional<Grid::Key> Grid::NextCell(std::stop_token stop)
	{
		std::unique_lock lock(mutex);
		while (!stop.stop_requested()) {
			if (!wake.wait(lock, stop, [&] { return !loadQueue.empty(); }))
				return std::nullopt;

			const auto now = std::chrono::steady_clock::now();
			auto nearest = loadQueue.end();
			int nearestDistance = INT_MAX;
			auto nextRetry = std::chrono::steady_clock::time_point::max();

			for (auto it = loadQueue.begin(); it != loadQueue.end(); ++it) {
				if (it->notBefore > now) {
					nextRetry = std::min(nextRetry, it->notBefore);
					continue;
				}

				const int distance = std::max(std::abs(it->key.x - centerX), std::abs(it->key.y - centerY));
				if (distance < nearestDistance) {
					nearestDistance = distance;
					nearest = it;
				}
			}

			if (nearest != loadQueue.end()) {
				const Key key = nearest->key;
				loadQueue.erase(nearest);
				return key;
			}

			wake.wait_until(lock, stop, nextRetry, [] { return false; });
		}

		return std::nullopt;
	}

	void Grid::LoadCells(std::stop_token stop)
	{
		while (const auto key = NextCell(stop)) {
			const auto loadGeneration = generation.load();
			std::shared_ptr<const CellGeometry> geometry;
			try {
				geometry = BuildCell(*key);
			} catch (const std::exception& e) {
				logger::warn("Could not preload collision for cell ({}, {}): {}", key->x, key->y, e.what());
			}

			if (loadGeneration != generation)
				continue;

			std::unique_lock lock(mutex);
			if (!InGrid(*key)) {
				pendingCells.erase(*key);
				continue;
			}

			if (!geometry) {
				loadQueue.push_back({ *key, std::chrono::steady_clock::now() + RetryDelay });
				lock.unlock();
				wake.notify_one();
				continue;
			}

			pendingCells.erase(*key);
			if (const auto it = loadedCells.find(*key); it != loadedCells.end())
				loadedOrder.erase(it->second.order);

			loadedOrder.push_back(*key);
			loadedCells.insert_or_assign(*key, LoadedCell{ std::move(geometry), std::prev(loadedOrder.end()) });

			while (loadedCells.size() > cellCapacity && !loadedOrder.empty()) {
				loadedCells.erase(loadedOrder.front());
				loadedOrder.pop_front();
			}

			completedCells.push_back(*key);
		}
	}

	std::shared_ptr<const Placements> Grid::GetPlacements(RE::TESWorldSpace* world, const Key& key)
	{
		{
			std::scoped_lock lock(placementMutex);
			if (const auto it = placements.find(key); it != placements.end())
				return it->second;
		}

		auto cellPlacements = PlacementReader::Read(world, key.x, key.y);
		if (!cellPlacements)
			return nullptr;

		auto result = std::make_shared<const Placements>(std::move(*cellPlacements));
		std::scoped_lock lock(placementMutex);

		if (placements.insert_or_assign(key, result).second)
			placementOrder.push_back(key);

		while (placements.size() > PlacementCapacity && !placementOrder.empty()) {
			placements.erase(placementOrder.front());
			placementOrder.pop_front();
		}

		return result;
	}

	std::shared_ptr<const CellGeometry> Grid::BuildCell(const Key& key)
	{
		auto* world = RE::TESForm::LookupByID<RE::TESWorldSpace>(key.world);
		if (!world)
			return nullptr;

		// Include objects that extend across cell borders
		std::array<std::shared_ptr<const Placements>, 9> neighborPlacements;
		for (std::size_t index = 0; index < neighborPlacements.size(); ++index) {
			const int cellX = key.x + static_cast<int>(index % 3) - 1;
			const int cellY = key.y + static_cast<int>(index / 3) - 1;
			neighborPlacements[index] = GetPlacements(world, { key.world, cellX, cellY });
			if (!neighborPlacements[index])
				return nullptr;
		}

		const RE::NiPoint3 cellMin{ key.x * CellGeometry::CellSize - CellGeometry::Margin, key.y * CellGeometry::CellSize - CellGeometry::Margin, 0.0f };
		const RE::NiPoint3 cellMax{ (key.x + 1) * CellGeometry::CellSize + CellGeometry::Margin, (key.y + 1) * CellGeometry::CellSize + CellGeometry::Margin, 0.0f };

		std::vector<Instance> instances;
		std::vector<Object> objects;
		auto& models = ModelCache::Get();

		for (std::size_t index = 0; index < neighborPlacements.size(); ++index) {
			for (const auto& placement : *neighborPlacements[index]) {
				const auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(placement.base);
				const auto* modelForm = base ? base->As<RE::TESModel>() : nullptr;
				const char* path = modelForm ? modelForm->GetModel() : nullptr;
				auto model = path && *path ? models.Load(path) : nullptr;
				if (!model)
					continue;

				RE::NiTransform modelToWorld;
				modelToWorld.rotate.SetEulerAnglesXYZ(placement.angle);
				modelToWorld.translate = placement.position;
				modelToWorld.scale = placement.scale;

				for (const auto& body : model->bodies) {
					auto bounds = body.bounds.Transformed(modelToWorld);
					if (bounds.max.x < cellMin.x || bounds.min.x > cellMax.x || bounds.max.y < cellMin.y || bounds.min.y > cellMax.y)
						continue;

					const auto bodyToWorld = modelToWorld * body.toModel;
					instances.push_back({ model, &body, placement.base, bodyToWorld, bodyToWorld.Invert(), modelToWorld.Invert(), bounds });
				}

				if (index == neighborPlacements.size() / 2)
					objects.push_back({ model, placement.base, modelToWorld });
			}
		}

		return std::make_shared<const CellGeometry>(key.x, key.y, std::move(instances), std::move(objects));
	}

	void Start()
	{
		const int configuredSize = std::clamp(Config::CollisionPreloadGrid, 0, 31);
		if (!configuredSize || !Config::ExtendGrassDistance || !(Config::RayCast || Config::GrassCliffs) || Config::OnlyLoadFromCache || Config::GeneratingCache)
			return;

		const int gridSize = configuredSize | 1;
		Grid::Get().Start(gridSize);
		logger::info("Preloading collision for a {0}x{0} grid", gridSize);
	}

	std::vector<std::pair<int, int>> Update()
	{
		auto& grid = Grid::Get();
		if (!grid.Running())
			return {};

		const auto* tes = RE::TES::GetSingleton();
		const auto* world = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		if (!world || tes->interiorCell)
			return {};

		grid.Follow(world->GetFormID(), tes->currentGridX, tes->currentGridY);

		return grid.TakeReady(world->GetFormID());
	}

	void Clear()
	{
		Grid::Get().Clear();
	}

	std::shared_ptr<const CellGeometry> Find(RE::TESObjectCELL& cell)
	{
		const auto* world = cell.GetRuntimeData().worldSpace;
		const auto* coordinates = cell.IsInteriorCell() ? nullptr : cell.GetCoordinates();
		auto& grid = Grid::Get();
		if (!grid.Running() || !world || !coordinates)
			return nullptr;

		return grid.Find(world->GetFormID(), coordinates->cellX, coordinates->cellY);
	}
}
