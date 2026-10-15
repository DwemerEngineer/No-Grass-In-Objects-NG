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
				// The engine reads its zero-valued index field after the final state word rather than wrapping to state[0].
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

	GrassPatchCreation::CreatePatchCall::CreatePatchCall()
	{
		// The engine holds the current shape in rbx rather than an argument register.
		mov(r8, rbx);
		mov(rax, reinterpret_cast<std::uintptr_t>(GrassPatchCreation::CreatePatch));
		jmp(rax);
		ready();
	}

	bool GrassPatchCreation::Install()
	{
		const bool updated = REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99;
		const auto addGrass = RELOCATION_ID(15204, 15372).address();
		const auto createPatch = addGrass + (updated ? 0x519 : 0x4FF);
		const auto finishInstances = addGrass + (updated ? 0x5EC : REL::Relocate(0x5E0, 0x5DC, 0x5E0));

		// VR's instanced shapes have one more virtual before DoneAddingInstances.
		const bool finishMatches = REL::Module::IsVR() ? REL::make_pattern<"FF 90 D8 01 00 00">().match(finishInstances) : REL::make_pattern<"FF 90 D0 01 00 00">().match(finishInstances);
		if (!REL::make_pattern<"E8">().match(createPatch) || !finishMatches || createPatch + 5 + *reinterpret_cast<const std::int32_t*>(createPatch + 1) != RELOCATION_ID(15212, 15381).address()) {
			logger::warn("Grass placement call sites were modified by another plugin; ray casting, grass cliffs, extended grass count and grass scale are unavailable.");
			return false;
		}

		auto& trampoline = SKSE::GetTrampoline();
		CreatePatchCall patch;
		trampoline.write_call<5>(createPatch, trampoline.allocate(patch));
		trampoline.write_call<6>(finishInstances, GrassPatchCreation::FinishInstances);

		enabled = true;
		anniversaryRuntime = REL::Module::IsAE();
		extendedParameters = updated;
		logger::info("Shared grass placement enabled{}.", Config::ThreadedGrassPlacement ? ", with placement workers dispatched during cell preparation" : "");
		return true;
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

		// Grass read from a cache file is never placed here, so it does not wait for collision.
		const auto* world = cell->GetRuntimeData().worldSpace;
		const auto* coordinates = cell->GetCoordinates();
		const bool fromCache = globals::AllowLoadGrass() && world && coordinates && DistantGrass::gridState.cacheIndex.Contains(world->editorID.c_str(), coordinates->cellX, coordinates->cellY);

		GrassPatchCreation patchCreation(cell, abort);
		if (!patchCreation.PrepareCell(!manager->generateGrassDataFiles && !fromCache)) {
			PhysicsRaycast::GetSingleton().DeferCell(cell);
			DistantGrass::ClearCellAddGrassTask(cell);
			return;
		}

		auto& workers = DistantGrass::GrassWorkers::Get();
		workers.AcquireBlock(cell);
		const stl::scope_exit releaseBlock([&] { workers.ReleaseBlock(cell); });
		{
			RE::BSSpinLockGuard grassLock(cell->grassCreateLock);
			add(manager, cell, abort);
			ReleaseNativePlacement();
			if (!*abort)
				patchCreation.AddCliffGrass(manager);
		}
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
		if (work) {
			work->tasks.cancel();
			work->arena.execute([&] { work->tasks.wait(); });
		}

		current = previous;
	}

	std::uint32_t GrassPatchCreation::CreatePatch(RE::BGSGrassManager* manager, RE::CreateGrassParams* params, RE::BSMultiStreamInstanceTriShape* shape)
	{
		// The engine copied the previous patch out of the shared buffer before asking for this one.
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

	bool GrassPatchCreation::PrepareCell(bool requireCollision)
	{
		if (!enabled || !requireCollision || Config::OnlyLoadFromCache || !(Config::RayCast || Config::GrassCliffs) || cell->IsInteriorCell())
			return true;

		return settings && (!settings->Valid || PhysicsRaycast::GetSingleton().CanFilterCell(*cell));
	}

	bool GrassPatchCreation::Queue(RE::BGSGrassManager* manager, const RE::CreateGrassParams& params, RE::BSMultiStreamInstanceTriShape* shape)
	{
		if (!shape || !params.land || params.land->parentCell != cell)
			return false;

		const auto* minimumSize = globals::settings::minGrassSize;
		if (!minimumSize)
			return false;

		const auto minimumSpacing = minimumSize->GetInteger();
		if (shape->GetMultiStreamTrishapeRuntimeData().instanceSize != InstanceHalfwords || minimumSpacing <= 0 || !std::isfinite(params.grassParam->positionRange) || params.grassParam->positionRange <= 0.0f)
			return false;

		const float width = static_cast<float>(manager->grassPatchSize) * 2.0f;
		const float count = std::min(width / params.grassParam->positionRange, width / static_cast<float>(minimumSpacing));
		if (!std::isfinite(count) || count < 1.0f || count > 65535.0f)
			return false;

		const auto candidatesPerSide = static_cast<std::uint32_t>(count);
		const auto candidates = static_cast<std::size_t>(candidatesPerSide) * candidatesPerSide;
		if (patchCount && (patchCount == patches.size() || candidateCount + candidates > MaxBatchCandidates))
			Flush();

		auto& patch = patches[patchCount];
		patch.sideCount = candidatesPerSide;
		patch.instances.resize(static_cast<std::size_t>(patch.sideCount) * patch.sideCount * InstanceHalfwords);
		patch.params = params;
		patch.densityMultiplier = 1.0f;

		if (extendedParameters) {
			const auto extended = std::bit_cast<ExtendedGrassParams>(params);
			patch.densityMultiplier = extended.densityMultiplier;
			patch.params.distanceFromWaterLevel = extended.distanceFromWaterLevel;
			patch.params.underwaterState = extended.underwaterState;
			patch.params.waterHeight = extended.waterHeight;
			patch.params.minSlopeDegrees = extended.minSlopeDegrees;
			patch.params.maxSlopeDegrees = extended.maxSlopeDegrees;
		}

		if (!std::isfinite(patch.densityMultiplier) || patch.densityMultiplier < 0.0f)
			patch.densityMultiplier = 0.0f;

		if (settings)
			patch.candidates.resize(static_cast<std::size_t>(patch.sideCount) * patch.sideCount);

		patch.grass = *params.grassParam;
		patch.start = *params.grassPatchStart;
		std::transform(params.grassDensityPcts, params.grassDensityPcts + patch.density.size(), patch.density.begin(), [](float density) {
			return std::isfinite(density) && density > 0.0f ? density : 0.0f;
		});
		patch.shape = shape;
		patch.created = 0;
		patch.needsWorldLookup = false;
		patch.spacing = (1.0f / static_cast<float>(patch.sideCount)) * width * 0.5f;

		patch.minSlope = RE::GrassSlopeTable()[SlopeIndex(patch.params.maxSlopeDegrees)];
		patch.maxSlope = RE::GrassSlopeTable()[SlopeIndex(patch.params.minSlopeDegrees)];

		// A patch's random stream must not depend on which worker finishes first.
		patch.seed = static_cast<std::uint32_t>(params.CellX) * 0x9E3779B9u ^ static_cast<std::uint32_t>(params.CellY) * 0x85EBCA6Bu ^ patch.grass.grassFormID;
		patch.seed ^= std::rotl(std::bit_cast<std::uint32_t>(patch.start.x), 11) ^ std::rotl(std::bit_cast<std::uint32_t>(patch.start.y), 23);

		candidateCount += static_cast<std::size_t>(patch.sideCount) * patch.sideCount;
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

		// Wait before reusing patch storage or returning past the native cell lock.
		if (work) {
			if (*abort)
				work->tasks.cancel();

			work->arena.execute([&] { work->tasks.wait(); });
		}

		if (!*abort) {
			// World lookups use engine state that is only safe on this thread.
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

				// Finish full buffers into this cell's handle list so removal still owns every group.
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
		return static_cast<std::uint32_t>(anniversaryRuntime ? angle * 1.4222221374511719f : (angle * 0.017453292f) * 81.487328f) & 511u;
	}

	float GrassPatchCreation::Shade(const RE::NiColorA& color, Random& random, float colorRange) noexcept
	{
		const float luminance = (color.red * 0.31f + color.green * 0.37f) + color.blue * 0.31999999f;
		const float shade = luminance * (((random.SignedFloat() * 0.5f) * (colorRange * 0.5f)) + (1.0f - colorRange));
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

	void GrassPatchCreation::Generate(Patch& patch, bool allowWorldLookup) const noexcept
	{
		using namespace DirectX::PackedVector;

		if (!allowWorldLookup) {
			patch.created = 0;
			patch.resumeIndex = 0;
			patch.resumeRandomDraws = 0;
		}

		patch.needsWorldLookup = false;
		Random random(patch.seed);

		// Preserve the random stream without repeating terrain queries for completed candidates.
		for (std::uint64_t i = 0; i < patch.resumeRandomDraws; ++i)
			random.Next();

		const auto& params = patch.params;
		const auto& grass = patch.grass;
		const float reciprocal = 1.0f / static_cast<float>(patch.sideCount);

		const float originX = static_cast<float>(CellsPerOrigin * (params.CellX / CellsPerOrigin)) * CellSize;
		const float originY = static_cast<float>(CellsPerOrigin * (params.CellY / CellsPerOrigin)) * CellSize;
		alignas(16) std::array<std::byte, 248> coordinates;

		const auto startX = patch.resumeIndex / patch.sideCount;
		const auto startY = patch.resumeIndex % patch.sideCount;

		for (std::uint32_t x = startX; x < patch.sideCount; ++x) {
			const float column = static_cast<float>(x) + 0.5f;
			for (std::uint32_t y = x == startX ? startY : 0; y < patch.sideCount; ++y) {
				const auto randomDraws = random.DrawCount();
				const float row = static_cast<float>(y) + 0.5f;
				const float densityX = (column * 2.0f) * reciprocal;
				const float densityY = (row * 2.0f) * reciprocal;
				const auto index = static_cast<std::size_t>(densityX) + 3 * static_cast<std::size_t>(densityY);

				const float dx = densityX - static_cast<float>(static_cast<int>(densityX));
				const float dy = densityY - static_cast<float>(static_cast<int>(densityY));
				const auto& density = patch.density;
				const float lowerDensity = dx * density[index + 1] + (1.0f - dx) * density[index];
				const float upperDensity = (1.0f - dx) * density[index + 3] + dx * density[index + 4];
				const float densityAtPosition = (lowerDensity * (1.0f - dy) + upperDensity * dy) * 32768.0f;

				const float chanceValue = patch.densityMultiplier * densityAtPosition;
				const auto chance = chanceValue > 0.0f ? static_cast<std::uint32_t>(std::min(chanceValue, 32768.0f)) : 0u;
				if (!chance || (random.Next() & 0x7FFFu) >= chance)
					continue;

				const float px = (patch.start.x + ((column * patch.spacing) * 2.0f)) + random.SignedFloat() * patch.spacing;
				const float py = (patch.start.y + ((row * patch.spacing) * 2.0f)) + random.SignedFloat() * patch.spacing;
				const auto halfX = XMConvertFloatToHalf(px - originX);
				const auto halfY = XMConvertFloatToHalf(py - originY);
				const RE::NiPoint3 position{ originX + XMConvertHalfToFloat(halfX), originY + XMConvertHalfToFloat(halfY), 0.0f };

				RE::NiPoint3 interpolatedNormal;
				RE::NiPoint3 normal;
				RE::NiColorA color{};
				float height = 0.0f;

				// These loaded-land helpers only read terrain and perform math; they do not allocate or access engine TLS.
				if (params.land->loadedData && RE::GetLandCoordData(params.land, coordinates.data(), &position, 5)) {
					RE::GetLandData(params.land, coordinates.data(), &height, &interpolatedNormal, &normal, &color);
				} else {
					if (!allowWorldLookup) {
						patch.needsWorldLookup = true;
						patch.resumeIndex = x * patch.sideCount + y;
						patch.resumeRandomDraws = randomDraws;
						return;
					}

					auto* tes = RE::TES::GetSingleton();
					if (!tes || !tes->GetLandHeight(position, height))
						continue;

					RE::GetLandNormal(tes, &position, &interpolatedNormal, &normal);
					RE::GetLandColor(tes, &position, &color);
				}
				if (!AcceptWater(params, height) || normal.z < patch.minSlope || normal.z > patch.maxSlope)
					continue;

				const float shade = Shade(color, random, grass.colorRange);

				const float orientation = random.SignedFloat();
				const float variation = random.SignedFloat() * grass.heightRange;
				const Candidate candidate{ { position.x, position.y, height }, normal, shade, orientation, variation };

				if (settings)
					patch.candidates[patch.created] = candidate;
				else
					Encode(candidate, grass.fitsToSlope, patch.instances.data() + static_cast<std::size_t>(patch.created) * InstanceHalfwords, params.CellX, params.CellY);

				++patch.created;
			}
		}
	}

	// Land texture lookups go through the engine's loaded-cell grid, which is only safe off the placement workers.
	void GrassPatchCreation::MarkBlockedTextures(Patch& patch) const
	{
		for (std::uint32_t i = 0; i < patch.created; ++i) {
			auto& candidate = patch.candidates[i];
			const auto& position = candidate.position;
			candidate.onBlockedTexture = !settings->IsExempt(patch.params.land, position.x, position.y, position.z, &patch.grass) && settings->OnBlockedTexture(position.x, position.y, position.z);
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
			bool keep = !candidate.onBlockedTexture;
			if (keep && Config::RayCast) {
				try {
					keep = settings->CanPlaceGrass(patch.params.land, candidate.position.x, candidate.position.y, candidate.position.z, &patch.grass, false);
				} catch (const std::bad_alloc&) {
					PhysicsRaycast::GetSingleton().ReportAllocationFailure();
				}
			}

			if (keep) {
				auto* output = patch.instances.data() + static_cast<std::size_t>(accepted++) * InstanceHalfwords;
				Encode(candidate, patch.grass.fitsToSlope, output, patch.params.CellX, patch.params.CellY);
			}
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

		return Shade(color, random, colorRange);
	}

	void GrassPatchCreation::AddCliffGrass(RE::BGSGrassManager* manager)
	{
		if (!enabled || !settings || !settings->Valid || !Config::GrassCliffs || *abort || !cell->IsAttached() || cell->IsInteriorCell())
			return;

		const auto* coordinates = cell->GetCoordinates();
		std::unordered_map<RE::TESLandTexture*, std::vector<CliffSample>> tops;
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
		for (const auto& entry : tops)
			sampleCount += entry.second.size();
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

	void GrassPatchCreation::AddCliffGrassType(RE::BGSGrassManager* manager, RE::TESGrass& grass, std::uint32_t seed, const std::vector<CliffSample>& samples)
	{
		auto param = MakeGrassParam(grass);
		const auto* minimumSize = globals::settings::minGrassSize;
		const float spacing = std::max(param.positionRange, minimumSize ? static_cast<float>(minimumSize->GetInteger()) : 0.0f);
		const float chance = param.denstityPctPerQuadVertex[0];
		if (!std::isfinite(spacing) || spacing < 1.0f || !std::isfinite(chance) || chance <= 0.0f)
			return;

		// LoadGrassType keys shapes by cell once the extended grid repacks its coordinates, else by 12x12 block.
		const auto* coordinates = cell->GetCoordinates();
		const int keyDivisor = Config::ExtendGrassDistance ? 1 : CellsPerOrigin;
		std::uint64_t typeKey = 0;
		RE::BSFixedString modelKey;
		auto* shape = RE::LoadGrassType(manager, &param, static_cast<std::uint32_t>(coordinates->cellX / keyDivisor), static_cast<std::uint32_t>(coordinates->cellY / keyDivisor), &typeKey, &modelKey);
		if (!shape)
			return;

		if (!shape->GetIsAddingInstances())
			shape->BeginAddingInstances(InstanceHalfwords);
		auto* handle = GrassHandle(cell, shape, typeKey, modelKey);

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

					// Start just above the cliff surface so only objects standing on it block the grass.
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
