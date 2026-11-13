#include "GrassControl/GrassPatchCreation.h"

#include "GrassControl/CliffSurfaces.h"
#include "GrassControl/Config.h"
#include "GrassControl/DistantGrass.h"
#include "GrassControl/Engine.h"
#include "GrassControl/Globals.h"
#include "GrassControl/GrassCellCreation.h"
#include "GrassControl/Main.h"
#include "GrassControl/PhysicsRaycast.h"
#include "GrassControl/Profiler.h"
#include "GrassControl/RaycastHelper.h"
#include "GrassControl/Util.h"

namespace GrassControl
{
	GrassDensityField GrassDensityField::Blend(const std::array<const GrassDensityField*, 9>& fields, const std::array<bool, 9>& present)
	{
		GrassDensityField result;

		for (int y = 0; y < Side; ++y) {
			for (int x = 0; x < Side; ++x) {
				float sum = 0.0f;
				int count = 0;

				for (int dy = -1; dy <= 1; ++dy) {
					const int sy = y - dy * (Side - 1);
					if (sy < 0 || sy >= Side)
						continue;

					for (int dx = -1; dx <= 1; ++dx) {
						const int sx = x - dx * (Side - 1);
						const int tile = (dy + 1) * 3 + dx + 1;

						if (sx < 0 || sx >= Side || !present[tile])
							continue;

						if (fields[tile])
							sum += fields[tile]->values[sy * Side + sx];

						++count;
					}
				}

				result.values[y * Side + x] = count ? sum / float(count) : 0.0f;
			}
		}

		return result;
	}

	float GrassDensityField::Sample(float x, float y) const noexcept
	{
		if (!std::isfinite(x) || !std::isfinite(y))
			return 0.0f;

		const float gx = std::clamp(x / Step, 0.0f, float(Side - 1));
		const float gy = std::clamp(y / Step, 0.0f, float(Side - 1));

		const int ix = std::min(int(gx), Side - 2);
		const int iy = std::min(int(gy), Side - 2);

		const float tx = gx - float(ix);
		const float ty = gy - float(iy);
		const int index = iy * Side + ix;

		return std::lerp(std::lerp(values[index], values[index + 1], tx),
			std::lerp(values[index + Side], values[index + Side + 1], tx), ty);
	}

	void GrassTerrainCache::Publish(std::uint32_t world, int x, int y, Snapshot fields)
	{
		std::scoped_lock lock(mutex);
		const Key key{ world, x, y };

		auto entry = tiles.find(key);
		if (entry != tiles.end()) {
			entry->second = std::move(fields);
			return;
		}

		// Queue first so insertion failure can roll back
		order.push_back(key);
		try {
			tiles.emplace(key, std::move(fields));
		} catch (...) {
			order.pop_back();
			throw;
		}

		if (tiles.size() > Capacity) {
			tiles.erase(order.front());
			order.pop_front();
		}
	}

	std::array<GrassTerrainCache::Snapshot, 9> GrassTerrainCache::Neighborhood(std::uint32_t world, int x, int y)
	{
		std::array<Snapshot, 9> result{};
		std::scoped_lock lock(mutex);

		for (int dy = -1; dy <= 1; ++dy) {
			for (int dx = -1; dx <= 1; ++dx) {
				const auto entry = tiles.find(Key{ world, x + dx, y + dy });
				if (entry != tiles.end())
					result[(dy + 1) * 3 + dx + 1] = entry->second;
			}
		}

		return result;
	}

	void GrassTerrainCache::Clear()
	{
		std::scoped_lock lock(mutex);
		tiles.clear();
		order.clear();
	}

	GrassPatchCreation::Random::Random(std::uint32_t seed)
	{
		state[0] = seed;

		for (std::uint32_t i = 1; i < state.size(); ++i)
			state[i] = 1812433253u * (state[i - 1] ^ (i + (state[i - 1] >> 30)));
	}

	std::uint32_t GrassPatchCreation::Random::Next()
	{
		if (index == 0) {
			for (std::size_t i = 0; i < state.size(); ++i) {
				// The engine uses zero after the last word, not state[0]
				const auto next = i + 1 < state.size() ? state[i + 1] : 0u;
				const auto value = (state[i] & 1u) + ((next % 624u) & 0x7FFFFFFFu);
				state[i] = state[(i + 397) % state.size()] ^ (value * 2u) ^ ((value & 1u) ? 0x9908B0DFu : 0u);
			}
		}

		++draws;
		const auto value = state[index];
		index = (index + 1) % state.size();
		const auto first = ((((value >> 11) ^ value) & 0xFF3A58ADu) << 7) ^ (value >> 11) ^ value;
		const auto second = ((first & 0xFFFFDF8Cu) << 15) ^ first;

		return (second >> 18) ^ second;
	}

	float GrassPatchCreation::Random::SignedFloat()
	{
		return static_cast<float>(Next()) * 4.6566129e-10f - 1.0f;
	}

	GrassPatchCreation::StableRandom::StableRandom(std::uint64_t seed)
	{
		// SplitMix64 seed mixing
		seed += 0x9E3779B97F4A7C15ull;
		seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ull;
		seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBull;
		state = seed ^ (seed >> 31);
	}

	std::uint32_t GrassPatchCreation::StableRandom::Next()
	{
		const auto oldState = state;
		state = oldState * 6364136223846793005ull + 1442695040888963407ull;
		const auto shifted = static_cast<std::uint32_t>(((oldState >> 18) ^ oldState) >> 27);

		return std::rotr(shifted, static_cast<int>(oldState >> 59));
	}

	float GrassPatchCreation::StableRandom::SignedFloat()
	{
		return static_cast<float>(Next()) * (2.0f / 4294967296.0f) - 1.0f;
	}

	void GrassPatchCreation::Enable()
	{
		enabled = true;
	}

	int GrassPatchCreation::ShapeCoordinate(int cell)
	{
		// Match engine truncation toward zero
		return Config::ExtendGrassDistance ? cell / CellsPerShape * CellsPerShape : cell / CellsPerOrigin;
	}

	RE::TESObjectCELL* GrassPatchCreation::GetCell()
	{
		return current ? current->cell : nullptr;
	}

	void GrassPatchCreation::ReleaseNativePlacement()
	{
		if (holdsNative) {
			holdsNative = false;
			nativeMutex.unlock();
		}
	}

	void GrassPatchCreation::AddCell(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort, AddCellGrassFunc add)
	{
		const Profiler::Scope profile;
		const stl::scope_exit releaseNative([] { ReleaseNativePlacement(); });
		if (!enabled || !cell || *abort) {
			add(manager, cell, abort);
			return;
		}

		const bool fromCache = globals::AllowLoadGrass() && DistantGrass::gridState.cacheIndex.Contains(*cell);

		auto& workers = DistantGrass::GetCreationState().workers;
		const auto block = workers.AcquireBlock(cell);
		const stl::scope_exit releaseBlock([&] {
			ReleaseNativePlacement();
			workers.ReleaseBlock(block);
		});

		RE::BSSpinLockGuard grassLock(cell->grassCreateLock);
		GrassPatchCreation patchCreation(cell, abort);

		if (!patchCreation.PrepareCell(NeedsCollision(manager, fromCache))) {
			PhysicsRaycast::GetSingleton().DeferCell(cell);
			DistantGrass::ClearCellAddGrassTask(cell);
			return;
		}

		patchCreation.PrepareSmooth(manager, fromCache);
		add(manager, cell, abort);
		ReleaseNativePlacement();

		// Cache serialization has already finished
		if (!*abort && !Config::GeneratingCache)
			patchCreation.AddCliffGrass(manager);
	}

	GrassPatchCreation::GrassPatchCreation(RE::TESObjectCELL* a_cell, bool* a_abort) :
		previous(current), cell(a_cell), abort(a_abort)
	{
		current = nullptr;
		if (!cell || !enabled)
			return;

		if (cell->IsAttached() && (Config::RayCast || Config::GrassCliffs))
			settings = Plugin::GetRaycastSettings();

		workerCount = Config::ThreadedGrassPlacement ? static_cast<unsigned int>(std::max(1, oneapi::tbb::info::default_concurrency())) : 1;
		scale = Config::GlobalGrassScale > 0.0001 ? Config::GlobalGrassScale : 1.0;
		current = this;
	}

	GrassPatchCreation::~GrassPatchCreation()
	{
		CancelWork();
		current = previous;
	}

	std::uint32_t GrassPatchCreation::CreatePatch(RE::BGSGrassManager* manager, RE::CreateGrassParams* params, RE::BSMultiStreamInstanceTriShape* shape)
	{
		// The engine has consumed the previous patch
		ReleaseNativePlacement();

		if (!current && enabled && params->land && params->land->parentCell) {
			bool abort = false;
			GrassPatchCreation patchCreation(params->land->parentCell, &abort);
			const auto count = CreatePatch(manager, params, shape);
			patchCreation.Flush();
			return count;
		}

		if (current) {
			try {
				if (current->Queue(manager, *params, shape))
					return 0;
			} catch (const std::bad_alloc&) {
				logger::warn("Unable to allocate private grass placement buffers; using engine placement.");
			}

			current->Flush();
		}

		nativeMutex.lock();
		holdsNative = true;

		return RE::CreateGrassPatch(manager, params);
	}

	void GrassPatchCreation::FinishInstances(RE::BSMultiStreamInstanceTriShape* shape, RE::BSTArray<std::uint32_t>& instances)
	{
		if (current)
			current->Flush();

		ReleaseNativePlacement();
		shape->DoneAddingInstances(instances);
	}

	bool GrassPatchCreation::NeedsCollision(const RE::BGSGrassManager* manager, bool fromCache)
	{
		return !manager->generateGrassDataFiles && (fromCache ? Config::GrassCliffs : !Config::OnlyLoadFromCache);
	}

	bool GrassPatchCreation::PrepareCell(bool requireCollision)
	{
		if (!enabled || !requireCollision || !(Config::RayCast || Config::GrassCliffs) || cell->IsInteriorCell())
			return true;

		return settings && (!settings->Valid || PhysicsRaycast::GetSingleton().CanFilterCell(*cell));
	}

	std::vector<RE::TESGrass*> GrassPatchCreation::TextureGrasses(RE::TESLandTexture* texture)
	{
		std::vector<RE::TESGrass*> result;
		if (!texture)
			return result;

		const auto* setting = globals::settings::maxGrassTypesPerTexture;
		const int maximum = std::max({ 0, setting ? setting->GetInteger() : 2, Config::EnsureMaxGrassTypesPerTextureSetting });
		int used = 0;
		for (auto* grass : texture->textureGrassList) {
			if (!grass)
				continue;

			// The engine accepts used == maximum
			if (used++ > maximum)
				break;

			const auto* model = grass->GetModel();
			if (model && *model)
				result.push_back(grass);
		}

		return result;
	}

	GrassPatchCreation::DensityFields GrassPatchCreation::ReadDensityFields(RE::TESObjectLAND* land)
	{
		constexpr int PaintedLayers = 6;
		constexpr int QuadrantSide = 17;
		constexpr int FullWeight = 255;

		const auto* data = land ? land->loadedData : nullptr;
		if (!data)
			return {};

		struct Coverage
		{
			std::shared_ptr<GrassDensityField> field;
			float density;
			std::uint8_t layerMask = 0;
		};
		std::unordered_map<RE::FormID, Coverage> coverage;
		std::array<unsigned char, GrassDensityField::Side * GrassDensityField::Side> contributors{};

		// Terrain must remain loaded while copying
		for (int quadrant = 0; quadrant < 4; ++quadrant) {
			for (auto& [id, entry] : coverage)
				entry.layerMask = 0;

			for (int layer = 0; layer <= PaintedLayers; ++layer) {
				auto* texture = layer < PaintedLayers ? data->quadTextures[quadrant][layer] : data->defQuadTextures[quadrant];
				if (layer == PaintedLayers && !texture)
					texture = RE::TESLandTexture::GetDefaultLandTexture();

				for (const auto* grass : TextureGrasses(texture)) {
					auto [it, inserted] = coverage.try_emplace(grass->GetFormID());
					auto& entry = it->second;
					if (inserted) {
						entry.field = std::make_shared<GrassDensityField>();
						entry.density = static_cast<float>(grass->GetDensity()) * 0.01f;
					}
					entry.layerMask |= static_cast<std::uint8_t>(1 << layer);
				}
			}

			const int quadrantX = (quadrant % 2) * (QuadrantSide - 1);
			const int quadrantY = (quadrant / 2) * (QuadrantSide - 1);

			for (int y = 0; y < QuadrantSide; ++y) {
				for (int x = 0; x < QuadrantSide; ++x) {
					const auto& weights = data->percents[quadrant][y * QuadrantSide + x];
					int paintedWeight = 0;
					for (const auto weight : weights)
						paintedWeight += static_cast<std::uint8_t>(weight);

					const int index = (y + quadrantY) * GrassDensityField::Side + x + quadrantX;
					++contributors[index];

					for (auto& [id, entry] : coverage) {
						if (!entry.layerMask)
							continue;

						int grassWeight = entry.layerMask & (1 << PaintedLayers) ? std::max(FullWeight - paintedWeight, 0) : 0;
						for (int layer = 0; layer < PaintedLayers; ++layer) {
							if (entry.layerMask & (1 << layer))
								grassWeight += static_cast<std::uint8_t>(weights[layer]);
						}
						entry.field->values[index] += entry.density * static_cast<float>(std::min(grassWeight, FullWeight)) / FullWeight;
					}
				}
			}
		}

		// Average shared edges to prevent quadrant seams
		DensityFields fields;
		for (auto& [id, entry] : coverage) {
			for (std::size_t i = 0; i < entry.field->values.size(); ++i)
				entry.field->values[i] /= static_cast<float>(contributors[i]);
			fields.emplace(id, std::move(entry.field));
		}

		return fields;
	}

	std::shared_ptr<const GrassDensityField> GrassPatchCreation::DensityField(RE::TESObjectLAND* land, RE::FormID grassID)
	{
		if (const auto entry = densityFields.find(grassID); entry != densityFields.end())
			return entry->second;

		auto fields = ReadDensityFields(land);
		densityFields.merge(fields);
		auto& field = densityFields[grassID];
		if (!field && land && land->loadedData && RE::TESForm::LookupByID<RE::TESGrass>(grassID))
			field = std::make_shared<GrassDensityField>();

		return field;
	}

	GrassTerrainCache::Snapshot GrassPatchCreation::CaptureTerrain(RE::TESObjectCELL* cell)
	{
		if (!enabled || !Config::SmoothGrassPlacement || !cell)
			return {};

		auto* land = cell->GetRuntimeData().cellLand;
		const auto* coordinates = cell->GetCoordinates();
		auto* world = cell->GetRuntimeData().worldSpace;

		if (!land || !land->loadedData || !coordinates || !world)
			return {};

		try {
			auto fields = std::make_shared<const DensityFields>(ReadDensityFields(land));
			terrainCache.Publish(world->GetFormID(), coordinates->cellX, coordinates->cellY, fields);
			return fields;
		} catch (const std::bad_alloc&) {
			logger::warn("Unable to copy grass terrain; neighboring cells will use their available coverage.");
			return {};
		}
	}

	void GrassPatchCreation::ClearTerrainCache()
	{
		terrainCache.Clear();
	}

	void GrassPatchCreation::PrepareSmooth(RE::BGSGrassManager* manager, bool fromCache)
	{
		placementManager = manager;
		if (!manager || !manager->enableGrass || *abort)
			return;

		auto cellFields = CaptureTerrain(cell);
		if (!cellFields || ((fromCache || Config::OnlyLoadFromCache) && !manager->generateGrassDataFiles))
			return;

		const auto* coordinates = cell->GetCoordinates();
		const auto* world = cell->GetRuntimeData().worldSpace;
		try {
			auto neighbors = terrainCache.Neighborhood(world->GetFormID(), coordinates->cellX, coordinates->cellY);
			neighbors[4] = std::move(cellFields);

			std::array<bool, 9> present{};
			std::unordered_set<RE::FormID> grassTypes;
			for (std::size_t i = 0; i < neighbors.size(); ++i) {
				present[i] = neighbors[i] != nullptr;
				if (!present[i])
					continue;

				for (const auto& [id, field] : *neighbors[i])
					grassTypes.insert(id);
			}

			DensityFields blended;
			for (const auto id : grassTypes) {
				std::array<const GrassDensityField*, 9> fields{};
				for (std::size_t i = 0; i < neighbors.size(); ++i) {
					if (!neighbors[i])
						continue;

					if (const auto entry = neighbors[i]->find(id); entry != neighbors[i]->end())
						fields[i] = entry->second.get();
				}

				auto field = std::make_shared<GrassDensityField>(GrassDensityField::Blend(fields, present));
				if (std::ranges::any_of(field->values, [](float value) { return value > 0.0f; }))
					blended.emplace(id, std::move(field));
			}

			densityFields = std::move(blended);
			smoothReady = true;
		} catch (const std::bad_alloc&) {
			logger::warn("Unable to capture smooth grass terrain; keeping patch placement.");
		}
	}

	RE::BSTArray<RE::CellGrassData*>* GrassPatchCreation::BeginCell(RE::ExtraDataList* extraList, bool create, BeginCellFunc begin)
	{
		auto* handles = begin(extraList, create);
		if (current && current->smoothReady && handles && handles->empty()) {
			try {
				current->GenerateSmoothCell();
				current->smoothCompleted = true;
			} catch (const std::bad_alloc&) {
				// Stop workers before removing partial groups
				current->CancelWork();
				current->placementManager->RemoveCellGrass(current->cell);
				handles = begin(extraList, create);
				logger::warn("Unable to build smooth grass; removed partial groups and restored patch placement.");
			}
		}
		return handles;
	}

	std::uint32_t GrassPatchCreation::SelectGrass(RE::TESObjectLAND* land, std::uint32_t quadrant, std::uint32_t vertex, RE::GrassParam* params, std::uint32_t capacity, SelectGrassFunc select)
	{
		if (current && current->smoothCompleted)
			return 0;
		return select(land, quadrant, vertex, params, capacity);
	}

	bool GrassPatchCreation::Queue(RE::BGSGrassManager* manager, const RE::CreateGrassParams& params, RE::BSMultiStreamInstanceTriShape* shape, std::uint32_t cellSide, std::uint32_t first, std::uint32_t end)
	{
		if (!shape || !params.land || params.land->parentCell != cell)
			return false;

		const auto* minimumSize = globals::settings::minGrassSize;
		if (!minimumSize)
			return false;

		const auto minimumSpacing = minimumSize->GetInteger();
		if (shape->GetMultiStreamTrishapeRuntimeData().instanceSize != InstanceHalfwords || minimumSpacing <= 0)
			return false;

		const auto& grass = *params.grassParam;
		const bool validRange = std::isfinite(grass.positionRange) && (cellSide ? grass.positionRange >= 0.0f : grass.positionRange > 0.0f);
		if (!validRange)
			return false;

		const float width = static_cast<float>(manager->grassPatchSize) * 2.0f;
		const float sideCount = cellSide ? static_cast<float>(cellSide) : std::min(width / grass.positionRange, width / static_cast<float>(minimumSpacing));
		if (!std::isfinite(sideCount) || sideCount < 1.0f || sideCount > 65535.0f)
			return false;

		const auto candidatesPerSide = cellSide ? cellSide : static_cast<std::uint32_t>(sideCount);
		const auto candidates = cellSide ? std::size_t(end - first) : static_cast<std::size_t>(candidatesPerSide) * candidatesPerSide;
		if (patchCount && (patchCount == patches.size() || candidateCount + candidates > MaxBatchCandidates))
			Flush();

		auto& patch = patches[patchCount];
		patch.sideCount = candidatesPerSide;
		patch.firstCandidate = cellSide ? first : 0;
		patch.endCandidate = cellSide ? end : candidatesPerSide * candidatesPerSide;
		patch.instances.resize(candidates * InstanceHalfwords);
		patch.params = params;
		patch.densityMultiplier = 1.0f;

		auto runtimeParams = params;
		if (const auto* multiplier = runtimeParams.GetDensityMultiplier()) {
			patch.densityMultiplier = *multiplier;
			patch.params.distanceFromWaterLevel = runtimeParams.GetDistanceFromWaterLevel();
			patch.params.underwaterState = runtimeParams.GetUnderwaterState();
			patch.params.waterHeight = runtimeParams.GetWaterHeight();
			patch.params.minSlopeDegrees = runtimeParams.GetMinSlopeDegrees();
			patch.params.maxSlopeDegrees = runtimeParams.GetMaxSlopeDegrees();
		}

		if (!std::isfinite(patch.densityMultiplier) || patch.densityMultiplier < 0.0f)
			patch.densityMultiplier = 0.0f;

		if (settings)
			patch.candidates.resize(candidates);

		patch.grass = grass;
		patch.densityField = Config::SmoothGrassPlacement ? DensityField(params.land, patch.grass.grassFormID) : nullptr;
		patch.start = *params.grassPatchStart;

		std::transform(params.grassDensityPcts, params.grassDensityPcts + patch.density.size(), patch.density.begin(), [](float density) {
			return std::isfinite(density) && density > 0.0f ? density : 0.0f;
		});

		patch.shape = shape;
		patch.created = 0;
		patch.needsWorldLookup = false;
		patch.spacing = (1.0f / static_cast<float>(patch.sideCount)) * (cellSide ? CellSize : width) * 0.5f;

		patch.minSlope = RE::GrassSlopeTable()[SlopeIndex(patch.params.maxSlopeDegrees)];
		patch.maxSlope = RE::GrassSlopeTable()[SlopeIndex(patch.params.minSlopeDegrees)];

		// Seeds must not depend on scheduling
		patch.seed = static_cast<std::uint32_t>(params.CellX) * 0x9E3779B9u ^ static_cast<std::uint32_t>(params.CellY) * 0x85EBCA6Bu ^ patch.grass.grassFormID;
		patch.seed ^= std::rotl(std::bit_cast<std::uint32_t>(patch.start.x), 11) ^ std::rotl(std::bit_cast<std::uint32_t>(patch.start.y), 23);

		candidateCount += candidates;
		++patchCount;
		publishedPatches.store(patchCount, std::memory_order_release);

		if (!*abort && candidateCount >= DispatchCandidates && workerCount > 1)
			Dispatch();

		return true;
	}

	void GrassPatchCreation::Dispatch()
	{
		if (activeCallbacks.load(std::memory_order_relaxed) >= workerCount - 1)
			return;

		try {
			if (!work)
				work = std::make_unique<Work>(workerCount);

			activeCallbacks.fetch_add(1, std::memory_order_relaxed);
			try {
				work->arena.enqueue(work->tasks.defer([this] {
					Run();
					activeCallbacks.fetch_sub(1, std::memory_order_release);
				}));
			} catch (const std::bad_alloc&) {
				activeCallbacks.fetch_sub(1, std::memory_order_relaxed);
				throw;
			}

		} catch (const std::bad_alloc&) {
			workerCount = 1;
			logger::warn("Unable to schedule grass placement work; continuing on the engine worker.");
		}
	}

	void GrassPatchCreation::LimitParallelism(bool limit)
	{
		if (limit == parallelismControl.has_value())
			return;
		if (limit)
			parallelismControl.emplace(oneapi::tbb::global_control::max_allowed_parallelism, GameplayParallelism);
		else
			parallelismControl.reset();
	}

	GrassPatchCreation::Work::Work(unsigned int concurrency) :
		arena(static_cast<int>(concurrency))
	{
		arena.initialize();
	}

	void GrassPatchCreation::Run() noexcept
	{
		auto index = nextPatch.load(std::memory_order_relaxed);
		while (index < publishedPatches.load(std::memory_order_acquire)) {
			if (nextPatch.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) {
				Generate(patches[index]);
				index = nextPatch.load(std::memory_order_relaxed);
			}
		}
	}

	void GrassPatchCreation::Flush()
	{
		if (patchCount == 0)
			return;

		if (!*abort)
			Run();

		// Finish workers before releasing patch storage and locks
		if (work) {
			if (*abort)
				work->tasks.cancel();

			work->arena.execute([&] { work->tasks.wait(); });
		}

		if (!*abort) {
			// World lookups stay on the calling thread
			for (std::size_t i = 0; i < patchCount; ++i) {
				if (patches[i].needsWorldLookup)
					Generate(patches[i], true);
			}

			if (settings)
				FilterPatches();

			for (std::size_t i = 0; i < patchCount && !*abort; ++i) {
				if (patches[i].created)
					Commit(patches[i]);
			}
		}

		ResetBatch();
	}

	void GrassPatchCreation::CancelWork()
	{
		if (work) {
			work->tasks.cancel();
			work->arena.execute([&] { work->tasks.wait(); });
			work.reset();
		}
		ResetBatch();
	}

	void GrassPatchCreation::ResetBatch()
	{
		patchCount = 0;
		candidateCount = 0;
		publishedPatches.store(0, std::memory_order_relaxed);
		nextPatch.store(0, std::memory_order_relaxed);
		activeCallbacks.store(0, std::memory_order_relaxed);
	}

	void GrassPatchCreation::Commit(Patch& patch)
	{
		auto& runtime = patch.shape->GetMultiStreamTrishapeRuntimeData();
		const auto capacity = (InstanceBufferBytes - 1) / (HalfwordBytes * runtime.instanceSize);
		std::uint32_t written = 0;

		while (written < patch.created) {
			if (runtime.instanceCount >= capacity) {
				auto* data = cell->extraList.GetByType<RE::ExtraCellGrassData>();
				if (!data)
					return;

				const auto handle = std::ranges::find_if(data->grassHandles, [&](const auto* entry) { return entry && entry->triShape.get() == patch.shape; });
				if (handle == data->grassHandles.end())
					return;

				// Retain every instance group for cell removal
				patch.shape->DoneAddingInstances((*handle)->instanceGroupIndicies);
				patch.shape->BeginAddingInstances(runtime.instanceSize);
			}

			const auto count = std::min(patch.created - written, capacity - runtime.instanceCount);
			patch.shape->AddInstances(count, patch.instances[static_cast<std::size_t>(written) * InstanceHalfwords]);
			written += count;
		}
	}

	std::uint32_t GrassPatchCreation::SlopeIndex(std::uint32_t degrees)
	{
		const float angle = static_cast<float>(std::min(degrees, 90u));
		return static_cast<std::uint32_t>(REL::Module::IsAE() ? angle * 1.4222221374511719f : (angle * 0.017453292f) * 81.487328f) & 511u;
	}

	float GrassPatchCreation::Shade(const RE::NiColorA& color, float roll, float colorRange) noexcept
	{
		const float luminance = (color.red * 0.31f + color.green * 0.37f) + color.blue * 0.31999999f;
		const float shade = luminance * (((roll * 0.5f) * (colorRange * 0.5f)) + (1.0f - colorRange));
		return shade >= 1.0f ? 0.99000001f : std::max(0.0f, shade);
	}

	bool GrassPatchCreation::AcceptWater(const RE::CreateGrassParams& params, float height) const
	{
		const float lower = params.waterHeight - params.distanceFromWaterLevel;
		const float upper = params.waterHeight + params.distanceFromWaterLevel;
		switch (params.underwaterState) {
		case 0:
			return height >= lower;
		case 1:
			return height >= params.waterHeight && height <= upper;
		case 2:
			return height <= lower;
		case 3:
			return height <= params.waterHeight && height >= lower;
		case 4:
			return height >= upper || height <= lower;
		case 5:
			return height >= lower && height <= upper;
		default:
			return true;
		}
	}

	float GrassPatchCreation::SampleDensity(const Patch& patch, const RE::NiPoint3& position) noexcept
	{
		if (patch.densityField) {
			const float cellX = static_cast<float>(patch.params.CellX) * CellSize;
			const float cellY = static_cast<float>(patch.params.CellY) * CellSize;
			return patch.densityField->Sample(position.x - cellX, position.y - cellY);
		}

		const float halfPatchWidth = patch.spacing * static_cast<float>(patch.sideCount);
		const float densityX = std::clamp((position.x - patch.start.x) / halfPatchWidth, 0.0f, 2.0f);
		const float densityY = std::clamp((position.y - patch.start.y) / halfPatchWidth, 0.0f, 2.0f);
		const int column = std::min(static_cast<int>(densityX), 1);
		const int row = std::min(static_cast<int>(densityY), 1);
		const int index = row * 3 + column;
		const float blendX = densityX - static_cast<float>(column);
		const float blendY = densityY - static_cast<float>(row);
		const auto& density = patch.density;

		return std::lerp(std::lerp(density[index], density[index + 1], blendX),
			std::lerp(density[index + 3], density[index + 4], blendX), blendY);
	}

	void GrassPatchCreation::Generate(Patch& patch, bool allowWorldLookup) const noexcept
	{
		using namespace DirectX::PackedVector;

		if (!allowWorldLookup) {
			patch.created = 0;
			patch.resumeIndex = patch.firstCandidate;
			patch.resumeRandomDraws = 0;
		}

		patch.needsWorldLookup = false;

		// Engine placement shares one random stream
		const bool stablePlacement = Config::SmoothGrassPlacement;
		Random engineRandom(patch.seed);

		// Resume without repeating terrain queries
		if (!stablePlacement) {
			for (std::uint64_t i = 0; i < patch.resumeRandomDraws; ++i)
				engineRandom.Next();
		}

		const auto& params = patch.params;
		const auto& grass = patch.grass;
		const float reciprocal = 1.0f / static_cast<float>(patch.sideCount);

		const float originX = static_cast<float>(CellsPerOrigin * (params.CellX / CellsPerOrigin)) * CellSize;
		const float originY = static_cast<float>(CellsPerOrigin * (params.CellY / CellsPerOrigin)) * CellSize;
		alignas(16) std::array<std::byte, 248> coordinates;

		for (auto candidateIndex = patch.resumeIndex; candidateIndex < patch.endCandidate; ++candidateIndex) {
			const auto gridX = candidateIndex / patch.sideCount;
			const auto gridY = candidateIndex % patch.sideCount;
			const float column = static_cast<float>(gridX) + 0.5f;
			const float row = static_cast<float>(gridY) + 0.5f;
			const float densityX = (column * 2.0f) * reciprocal;
			const float densityY = (row * 2.0f) * reciprocal;
			const auto densityIndex = static_cast<std::size_t>(densityX) + 3 * static_cast<std::size_t>(densityY);

			const float blendX = densityX - static_cast<float>(static_cast<int>(densityX));
			const float blendY = densityY - static_cast<float>(static_cast<int>(densityY));
			const auto& density = patch.density;
			const float lowerDensity = blendX * density[densityIndex + 1] + (1.0f - blendX) * density[densityIndex];
			const float upperDensity = (1.0f - blendX) * density[densityIndex + 3] + blendX * density[densityIndex + 4];
			const float densityAtPosition = (lowerDensity * (1.0f - blendY) + upperDensity * blendY) * 32768.0f;
			const float chanceValue = patch.densityMultiplier * densityAtPosition;
			const auto chance = chanceValue > 0.0f ? static_cast<std::uint32_t>(std::min(chanceValue, 32768.0f)) : 0u;

			// False defers terrain lookup to the caller
			const auto processCandidate = [&](auto& random) {
				if (!stablePlacement && !chance)
					return true;

				const auto roll = random.Next() & 0x7FFFu;
				if (!stablePlacement && roll >= chance)
					return true;

				const float candidateX = (patch.start.x + ((column * patch.spacing) * 2.0f)) + random.SignedFloat() * patch.spacing;
				const float candidateY = (patch.start.y + ((row * patch.spacing) * 2.0f)) + random.SignedFloat() * patch.spacing;
				const auto halfX = XMConvertFloatToHalf(candidateX - originX);
				const auto halfY = XMConvertFloatToHalf(candidateY - originY);
				const RE::NiPoint3 position{ originX + XMConvertHalfToFloat(halfX), originY + XMConvertHalfToFloat(halfY), 0.0f };

				if (stablePlacement) {
					const float coverage = SampleDensity(patch, position);
					const float probability = std::clamp(coverage * patch.densityMultiplier, 0.0f, 1.0f);
					if (static_cast<float>(roll) >= probability * 32768.0f)
						return true;
				}

				RE::NiPoint3 interpolatedNormal;
				RE::NiPoint3 normal;
				RE::NiColorA color{};
				float height = 0.0f;

				// No allocations or engine TLS
				if (params.land->loadedData && RE::GetLandCoordData(params.land, coordinates.data(), &position, 5)) {
					RE::GetLandData(params.land, coordinates.data(), &height, &interpolatedNormal, &normal, &color);
				} else {
					if (!allowWorldLookup)
						return false;

					auto* tes = RE::TES::GetSingleton();
					if (!tes || !tes->GetLandHeight(position, height))
						return true;

					RE::GetLandNormal(tes, &position, &interpolatedNormal, &normal);
					RE::GetLandColor(tes, &position, &color);
				}

				if (!AcceptWater(params, height) || normal.z < patch.minSlope || normal.z > patch.maxSlope)
					return true;

				const float shade = Shade(color, random.SignedFloat(), grass.colorRange);
				const float orientation = random.SignedFloat();
				const float variation = random.SignedFloat() * grass.heightRange;
				const Candidate candidate{ { position.x, position.y, height }, normal, shade, orientation, variation };

				if (settings)
					patch.candidates[patch.created] = candidate;
				else
					Encode(candidate, grass.fitsToSlope, patch.instances.data() + static_cast<std::size_t>(patch.created) * InstanceHalfwords, params.CellX, params.CellY);

				++patch.created;
				return true;
			};

			const auto drawsBeforeCandidate = engineRandom.DrawCount();
			bool completed;
			if (stablePlacement) {
				StableRandom random((static_cast<std::uint64_t>(patch.seed) << 32) | candidateIndex);
				completed = processCandidate(random);
			} else {
				completed = processCandidate(engineRandom);
			}

			if (!completed) {
				patch.needsWorldLookup = true;
				patch.resumeIndex = candidateIndex;
				patch.resumeRandomDraws = drawsBeforeCandidate;
				return;
			}
		}
	}

	// Loaded-cell grid access must stay on the caller
	void GrassPatchCreation::MarkBlockedTextures(Patch& patch) const
	{
		for (std::uint32_t i = 0; i < patch.created; ++i) {
			auto& candidate = patch.candidates[i];
			const auto& position = candidate.position;
			candidate.onBlockedTexture = !settings->IsExempt(patch.params.land, position.x, position.y, position.z, &patch.grass) && settings->OnBlockedTexture(patch.params.land, position.x, position.y, position.z);
		}
	}

	void GrassPatchCreation::FilterPatches()
	{
		if (Config::RayCast && settings->Textures) {
			for (std::size_t i = 0; i < patchCount; ++i)
				MarkBlockedTextures(patches[i]);
		}

		std::array<bool, std::tuple_size_v<decltype(patches)>> filtered{};

		if (work && patchCount > 1) {
			const auto caller = std::this_thread::get_id();
			std::atomic<std::uint32_t> unavailable{ 0 };

			oneapi::tbb::task_group_context context{ oneapi::tbb::task_group_context::isolated };
			work->arena.execute([&] {
				oneapi::tbb::parallel_for(
					std::size_t{ 0 }, patchCount, [&](std::size_t i) {
						if (*abort || !PhysicsRaycast::EnsureThreadMemory())
							return;

						const auto before = PhysicsRaycast::UnavailableQueries();
						Filter(patches[i]);
						if (std::this_thread::get_id() != caller)
							unavailable.fetch_add(PhysicsRaycast::UnavailableQueries() - before, std::memory_order_relaxed);
						filtered[i] = true;
					},
					context);
			});

			PhysicsRaycast::AddUnavailableQueries(unavailable.load(std::memory_order_relaxed));
		}

		for (std::size_t i = 0; i < patchCount && !*abort; ++i) {
			if (!filtered[i])
				Filter(patches[i]);
		}
	}

	void GrassPatchCreation::Filter(Patch& patch)
	{
		std::uint32_t accepted = 0;
		for (std::uint32_t i = 0; i < patch.created && !*abort; ++i) {
			const auto& candidate = patch.candidates[i];
			if (candidate.onBlockedTexture)
				continue;

			bool canPlace = true;
			if (Config::RayCast) {
				try {
					canPlace = settings->CanPlaceGrass(patch.params.land, candidate.position.x, candidate.position.y, candidate.position.z, &patch.grass, false);
				} catch (const std::bad_alloc&) {
					PhysicsRaycast::GetSingleton().ReportAllocationFailure();
				}
			}

			if (!canPlace)
				continue;

			auto* output = patch.instances.data() + static_cast<std::size_t>(accepted++) * InstanceHalfwords;
			Encode(candidate, patch.grass.fitsToSlope, output, patch.params.CellX, patch.params.CellY);
		}

		patch.created = accepted;
	}

	static RE::GrassParam MakeGrassParam(RE::TESGrass& grass)
	{
		const auto* waveOffsetRange = globals::WaveOffsetRange();

		RE::GrassParam param{};
		param.modelName = grass.GetModel();
		param.grassFormID = grass.GetFormID();
		param.waveOffsetRange = waveOffsetRange ? std::bit_cast<std::uint32_t>(waveOffsetRange->GetFloat()) : 0;
		param.wavePeriod = grass.GetWavePeriod();
		param.colorRange = grass.GetColorRange();
		param.positionRange = grass.GetPositionRange();
		param.heightRange = grass.GetHeightRange();
		param.hasVertexLighting = grass.GetVertexLighting();
		param.fitsToSlope = grass.GetFitToSlope();
		param.hasUniformScaling = grass.GetUniformScaling();
		std::ranges::fill(param.denstityPctPerQuadVertex, static_cast<float>(grass.GetDensity()) * 0.01f);

		return param;
	}

	static RE::CellGrassData* GrassHandle(RE::TESObjectCELL* cell, RE::BSMultiStreamInstanceTriShape* shape, std::uint64_t typeKey, const RE::BSFixedString& modelKey)
	{
		auto* data = cell->extraList.GetByType<RE::ExtraCellGrassData>();
		if (!data) {
			data = RE::BSExtraData::Create<RE::ExtraCellGrassData>();
			cell->extraList.Add(data);
		}

		for (auto* handle : data->grassHandles) {
			if (handle && handle->triShape.get() == shape)
				return handle;
		}

		auto* storage = RE::malloc(sizeof(RE::CellGrassData));
		if (!storage)
			throw std::bad_alloc();

		auto* handle = new (storage) RE::CellGrassData{};
		handle->grassTypeKey = typeKey;
		handle->grassModelKey = modelKey;
		handle->triShape.reset(shape);
		data->grassHandles.push_back(handle);

		return handle;
	}

	std::pair<RE::BSMultiStreamInstanceTriShape*, RE::CellGrassData*> GrassPatchCreation::PrepareShape(RE::BGSGrassManager* manager, RE::GrassParam& param)
	{
		const auto* coordinates = cell->GetCoordinates();
		std::uint64_t typeKey = 0;
		RE::BSFixedString modelKey;

		auto* shape = RE::LoadGrassType(manager, &param, static_cast<std::uint32_t>(ShapeCoordinate(coordinates->cellX)), static_cast<std::uint32_t>(ShapeCoordinate(coordinates->cellY)), &typeKey, &modelKey);
		if (!shape)
			return {};

		auto* handle = GrassHandle(cell, shape, typeKey, modelKey);

		if (!shape->GetIsAddingInstances())
			shape->BeginAddingInstances(InstanceHalfwords);

		return { shape, handle };
	}

	void GrassPatchCreation::GenerateSmoothCell()
	{
		auto* land = cell->GetLand();
		const auto* coordinates = cell->GetCoordinates();
		const auto* minimumSize = globals::settings::minGrassSize;

		if (!land || !land->loadedData || !coordinates || !minimumSize || minimumSize->GetInteger() <= 0)
			throw std::bad_alloc();

		std::vector<RE::FormID> grassTypes;
		grassTypes.reserve(densityFields.size());

		for (const auto& [id, field] : densityFields)
			grassTypes.push_back(id);
		std::ranges::sort(grassTypes);

		for (const auto id : grassTypes) {
			if (*abort)
				break;

			auto* grass = RE::TESForm::LookupByID<RE::TESGrass>(id);
			if (!grass)
				continue;

			auto param = MakeGrassParam(*grass);
			const float spacing = std::max(static_cast<float>(minimumSize->GetInteger()), param.positionRange);
			const float patchWidth = static_cast<float>(placementManager->grassPatchSize) * 2.0f;
			if (!std::isfinite(spacing) || spacing <= 0.0f || patchWidth <= 0.0f)
				continue;

			// Preserve the native candidate budget across patch boundaries
			const float patchCandidatesPerSide = std::floor(patchWidth / spacing);
			if (patchCandidatesPerSide < 1.0f)
				continue;

			const auto candidatesPerSide = static_cast<std::uint32_t>(std::clamp(std::lround((CellSize / patchWidth) * patchCandidatesPerSide), 1L, 4096L));
			const auto [shape, handle] = PrepareShape(placementManager, param);
			if (!shape)
				continue;

			const stl::scope_exit finish([&] { shape->DoneAddingInstances(handle->instanceGroupIndicies); });
			RE::NiPoint3 start{ static_cast<float>(coordinates->cellX) * CellSize, static_cast<float>(coordinates->cellY) * CellSize, 0.0f };

			RE::CreateGrassParams params{};
			params.CellX = coordinates->cellX;
			params.CellY = coordinates->cellY;
			params.land = land;
			params.grassPatchStart = &start;
			params.grassParam = &param;
			params.grassDensityPcts = param.denstityPctPerQuadVertex;
			if (auto* multiplier = params.GetDensityMultiplier())
				*multiplier = 1.0f;
			params.GetDistanceFromWaterLevel() = static_cast<float>(grass->GetDistanceFromWaterLevel());
			params.GetUnderwaterState() = static_cast<std::uint32_t>(grass->GetUnderwaterState());
			params.GetWaterHeight() = cell->GetExteriorWaterHeight();
			params.GetMinSlopeDegrees() = grass->GetMinSlopeDegrees();
			params.GetMaxSlopeDegrees() = grass->GetMaxSlopeDegrees();

			const auto totalCandidates = candidatesPerSide * candidatesPerSide;
			for (std::uint32_t firstCandidate = 0; firstCandidate < totalCandidates && !*abort; firstCandidate += SmoothPatchCandidates) {
				const auto endCandidate = std::min(firstCandidate + SmoothPatchCandidates, totalCandidates);
				if (!Queue(placementManager, params, shape, candidatesPerSide, firstCandidate, endCandidate))
					throw std::bad_alloc();
			}

			Flush();
		}
	}

	std::optional<GrassPatchCreation::CliffTop> GrassPatchCreation::FindCliffTop(float x, float y, float top, float bottom) const
	{
		const glm::vec4 begin(x, y, top, 0), end(x, y, bottom, 0);
		const auto result = PhysicsRaycast::GetSingleton().CastRay(begin, end, cell, *settings);
		if (result.status != PhysicsRaycast::QueryStatus::Success || result.hits.empty())
			return std::nullopt;

		const auto& highest = *std::ranges::min_element(result.hits, {}, &PhysicsRaycast::HitResult::hitFraction);
		const auto point = begin + (end - begin) * highest.hitFraction;
		if (!highest.hitCliff || !settings->IsCliffAt(highest.formID, highest.reference, { point.x, point.y, point.z }))
			return std::nullopt;

		const auto normal = highest.normal.z < 0.0f ? -highest.normal : highest.normal;
		return CliffTop{ { point.x, point.y, point.z }, { normal.x, normal.y, normal.z }, highest.formID };
	}

	bool GrassPatchCreation::SupportedAround(float x, float y, float z, float reach) const
	{
		const std::array<glm::vec2, 4> offsets{ glm::vec2(reach, 0.0f), glm::vec2(-reach, 0.0f), glm::vec2(0.0f, reach), glm::vec2(0.0f, -reach) };
		for (const auto& offset : offsets) {
			const glm::vec4 begin(x + offset.x, y + offset.y, z + CliffEdgeTolerance, 0), end(x + offset.x, y + offset.y, z - CliffEdgeTolerance, 0);
			const auto result = PhysicsRaycast::GetSingleton().CastRay(begin, end, cell, *settings);

			if (result.status != PhysicsRaycast::QueryStatus::Success || result.hits.empty())
				return false;
		}

		return true;
	}

	float GrassPatchCreation::LandShade(float x, float y, Random& random, float colorRange) const
	{
		RE::NiColorA color{ 0.5f, 0.5f, 0.5f, 1.0f };
		auto* land = cell->GetRuntimeData().cellLand;
		alignas(16) std::array<std::byte, 248> coordinates;
		const RE::NiPoint3 position{ x, y, 0.0f };

		if (land && land->loadedData && RE::GetLandCoordData(land, coordinates.data(), &position, 5)) {
			float height = 0.0f;
			RE::NiPoint3 interpolatedNormal;
			RE::NiPoint3 normal;
			RE::GetLandData(land, coordinates.data(), &height, &interpolatedNormal, &normal, &color);
		}

		return Shade(color, random.SignedFloat(), colorRange);
	}

	void GrassPatchCreation::AddCliffGrass(RE::BGSGrassManager* manager)
	{
		if (!enabled || !settings || !settings->Valid || !Config::GrassCliffs || *abort || !cell->IsAttached() || cell->IsInteriorCell())
			return;

		const auto* coordinates = cell->GetCoordinates();
		std::unordered_map<RE::TESLandTexture*, std::vector<RE::NiPoint3>> tops;

		for (const auto& surface : CliffSurfaces::Get(cell)) {
			if (*abort)
				return;

			const auto& center = surface.center;
			const float radius = std::min(surface.radius, MaxCliffRadius);

			for (float x = center.x - radius + CliffSampleStep * 0.5f; x < center.x + radius; x += CliffSampleStep) {
				for (float y = center.y - radius + CliffSampleStep * 0.5f; y < center.y + radius; y += CliffSampleStep) {
					const auto top = FindCliffTop(x, y, center.z + radius + 1.0f, center.z - radius - 1.0f);

					if (top && top->formID == surface.object && top->position.GetSquaredDistance(center) <= radius * radius)
						tops[surface.landTexture].push_back({ x, y, top->position.z });
				}
			}
		}

		std::size_t sampleCount = 0;
		for (auto& [texture, samples] : tops) {
			const auto position = [](const RE::NiPoint3& sample) { return std::tie(sample.x, sample.y, sample.z); };
			std::ranges::sort(samples, {}, position);

			const auto duplicates = std::ranges::unique(samples, {}, position);
			samples.erase(duplicates.begin(), duplicates.end());
			sampleCount += samples.size();
		}

		if (sampleCount)
			logger::debug("Cliff tops in cell ({}, {}): {} samples over {} landscape textures", coordinates->cellX, coordinates->cellY, sampleCount, tops.size());

		const auto* maxTypesSetting = globals::settings::maxGrassTypesPerTexture;
		const int maxTypes = maxTypesSetting ? std::max(1, maxTypesSetting->GetInteger()) : 7;

		for (const auto& [landTexture, samples] : tops) {
			int types = 0;
			for (auto* grass : landTexture->textureGrassList) {
				if (*abort || types >= maxTypes)
					break;
				if (!grass)
					continue;

				++types;
				const auto seed = static_cast<std::uint32_t>(coordinates->cellX) * 0x9E3779B9u ^ static_cast<std::uint32_t>(coordinates->cellY) * 0x85EBCA6Bu ^ grass->GetFormID() ^ std::rotl(landTexture->GetFormID(), 13);
				AddCliffGrassType(manager, *grass, seed, samples);
			}
		}
	}

	void GrassPatchCreation::AddCliffGrassType(RE::BGSGrassManager* manager, RE::TESGrass& grass, std::uint32_t seed, const std::vector<RE::NiPoint3>& samples)
	{
		auto param = MakeGrassParam(grass);
		const auto* minimumSize = globals::settings::minGrassSize;
		const float spacing = std::max(param.positionRange, minimumSize ? static_cast<float>(minimumSize->GetInteger()) : 0.0f);
		const float chance = param.denstityPctPerQuadVertex[0];

		if (!std::isfinite(spacing) || spacing < 1.0f || !std::isfinite(chance) || chance <= 0.0f)
			return;

		const auto* coordinates = cell->GetCoordinates();
		const auto [shape, handle] = PrepareShape(manager, param);
		if (!shape)
			return;

		const float minSlope = RE::GrassSlopeTable()[SlopeIndex(grass.GetMaxSlopeDegrees())];
		const float maxSlope = RE::GrassSlopeTable()[SlopeIndex(grass.GetMinSlopeDegrees())];

		Patch patch{};
		patch.shape = shape;
		patch.grass = param;
		patch.params.CellX = coordinates->cellX;
		patch.params.CellY = coordinates->cellY;

		const auto& bounds = grass.boundData;
		const float reach = std::max({ MinBladeReach, std::abs(static_cast<float>(bounds.boundMax.x - bounds.boundMin.x)) * 0.5f, std::abs(static_cast<float>(bounds.boundMax.y - bounds.boundMin.y)) * 0.5f });

		Random random(seed);
		const int perSide = std::max(1, static_cast<int>(CliffSampleStep / spacing));
		const float step = CliffSampleStep / static_cast<float>(perSide);

		for (const auto& sample : samples) {
			for (int i = 0; i < perSide && !*abort; ++i) {
				for (int j = 0; j < perSide; ++j) {
					const float x = sample.x - CliffSampleStep * 0.5f + (static_cast<float>(i) + 0.5f) * step + random.SignedFloat() * step * 0.5f;
					const float y = sample.y - CliffSampleStep * 0.5f + (static_cast<float>(j) + 0.5f) * step + random.SignedFloat() * step * 0.5f;

					if (static_cast<float>(random.Next() & 0x7FFFu) >= chance * 32768.0f)
						continue;

					const auto top = FindCliffTop(x, y, sample.z + CliffSampleStep, sample.z - CliffSampleStep);
					if (!top || top->normal.z < minSlope || top->normal.z > maxSlope || !SupportedAround(x, y, top->position.z, reach))
						continue;

					if (Config::RayCast && !settings->IsExempt(cell->GetRuntimeData().cellLand, x, y, top->position.z, &param) && settings->OnBlockedTexture(cell->GetRuntimeData().cellLand, x, y, top->position.z))
						continue;

					// Start above the cliff to avoid self-hits
					if (Config::RayCast && !settings->CanPlaceGrass(cell->GetRuntimeData().cellLand, x, y, top->position.z + settings->RayDepth + 1.0f, &param, true))
						continue;

					const Candidate candidate{ top->position, top->normal, LandShade(x, y, random, param.colorRange), random.SignedFloat(), random.SignedFloat() * param.heightRange };
					patch.instances.resize(static_cast<std::size_t>(patch.created + 1) * InstanceHalfwords);
					Encode(candidate, param.fitsToSlope, patch.instances.data() + static_cast<std::size_t>(patch.created) * InstanceHalfwords, coordinates->cellX, coordinates->cellY);
					++patch.created;
				}
			}
		}

		if (patch.created)
			Commit(patch);

		shape->DoneAddingInstances(handle->instanceGroupIndicies);
	}

	RE::NiPoint3 GrassPatchCreation::SlopeTangent(const RE::NiPoint3& normal) const
	{
		RE::NiPoint3 tangent;
		if (std::abs(normal.x) <= std::abs(normal.y) && std::abs(normal.x) <= std::abs(normal.z))
			tangent = { 0.0f, -normal.z, normal.y };
		else if (std::abs(normal.y) <= std::abs(normal.x) && std::abs(normal.y) <= std::abs(normal.z))
			tangent = { -normal.z, 0.0f, normal.x };
		else
			tangent = { -normal.y, normal.x, 0.0f };

		const float length = std::sqrt((tangent.y * tangent.y + tangent.x * tangent.x) + tangent.z * tangent.z);
		if (length > 0.000001f)
			tangent *= 1.0f / length;
		else
			tangent = { 0.0f, 0.0f, 0.0f };

		return tangent;
	}

	void GrassPatchCreation::Encode(const Candidate& candidate, bool fitsToSlope, std::uint16_t* instance, int cellX, int cellY) const noexcept
	{
		using namespace DirectX::PackedVector;

		const float cosine = candidate.orientation;
		const auto& normal = candidate.normal;
		const float sine = std::sqrt(1.0f - cosine * cosine);
		std::array<float, 9> rotation{ cosine, -sine, 0.0f, sine, cosine, 0.0f, 0.0f, 0.0f, 1.0f };

		if (fitsToSlope) {
			const auto tangent = SlopeTangent(normal);

			const std::array<float, 9> basis{
				tangent.y * normal.z - normal.y * tangent.z, tangent.x, normal.x,
				normal.x * tangent.z - tangent.x * normal.z, tangent.y, normal.y,
				normal.y * tangent.x - normal.x * tangent.y, tangent.z, normal.z
			};
			const auto upright = rotation;
			for (std::size_t i = 0; i < 3; ++i) {
				for (std::size_t j = 0; j < 3; ++j)
					rotation[i * 3 + j] = (basis[i * 3] * upright[j] + basis[i * 3 + 1] * upright[j + 3]) + basis[i * 3 + 2] * upright[j + 6];
			}
		}

		instance[0] = XMConvertFloatToHalf(candidate.position.x - static_cast<float>(CellsPerOrigin * (cellX / CellsPerOrigin)) * CellSize);
		instance[1] = XMConvertFloatToHalf(candidate.position.y - static_cast<float>(CellsPerOrigin * (cellY / CellsPerOrigin)) * CellSize);
		instance[2] = XMConvertFloatToHalf(candidate.position.z);
		instance[3] = XMConvertFloatToHalf(candidate.shade);

		constexpr std::array<std::size_t, 9> order{ 0, 1, 2, 7, 3, 4, 5, 8, 6 };
		for (std::size_t i = 0; i < order.size(); ++i)
			instance[4 + i] = XMConvertFloatToHalf(rotation[order[i]]);

		const float variation = candidate.variation;
		const float scaled = scale == 1.0 ? variation : static_cast<float>(static_cast<double>(variation + 1.0f) * scale) - 1.0f;
		instance[13] = XMConvertFloatToHalf(scaled);
		instance[14] = 0;
		instance[15] = 0;
	}
}
