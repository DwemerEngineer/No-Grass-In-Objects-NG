#include "GrassControl/GrassPatchCreation.h"

#include "GrassControl/CliffSurfaces.h"
#include "GrassControl/CollisionGrid.h"
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
	namespace
	{
		constexpr std::uint64_t SplitMix64(std::uint64_t value) noexcept
		{
			value += 0x9E3779B97F4A7C15ull;
			value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
			value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
			return value ^ (value >> 31);
		}

		// Corners a (0, 0), b (1, 0), c (0, 1), d (1, 1)
		float Bilerp(float a, float b, float c, float d, float tx, float ty) noexcept
		{
			return std::lerp(std::lerp(a, b, tx), std::lerp(c, d, tx), ty);
		}

		// 3-by-3 neighborhood index centered on (0, 0)
		constexpr std::size_t NeighborIndex(int dx, int dy) noexcept
		{
			return static_cast<std::size_t>((dy + 1) * 3 + dx + 1);
		}
		constexpr std::size_t CenterNeighbor = NeighborIndex(0, 0);

		std::uint32_t CellSeed(int cellX, int cellY) noexcept
		{
			return static_cast<std::uint32_t>(cellX) * 0x9E3779B9u ^ static_cast<std::uint32_t>(cellY) * 0x85EBCA6Bu;
		}

		// Smooth value noise in [-1, 1] on an integer lattice
		float DensityNoise(float x, float y, std::uint64_t salt) noexcept
		{
			const auto corner = [salt](int cornerX, int cornerY) {
				const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cornerX)) << 32) | static_cast<std::uint32_t>(cornerY);
				return static_cast<float>(SplitMix64(key ^ salt) >> 40) * (2.0f / 16777216.0f) - 1.0f;
			};

			const int lowerX = static_cast<int>(std::floor(x));
			const int lowerY = static_cast<int>(std::floor(y));
			const float fractionX = x - static_cast<float>(lowerX);
			const float fractionY = y - static_cast<float>(lowerY);
			const float smoothX = fractionX * fractionX * (3.0f - 2.0f * fractionX);
			const float smoothY = fractionY * fractionY * (3.0f - 2.0f * fractionY);

			return Bilerp(corner(lowerX, lowerY), corner(lowerX + 1, lowerY), corner(lowerX, lowerY + 1), corner(lowerX + 1, lowerY + 1), smoothX, smoothY);
		}

		float StoredCoordinate(float value) noexcept
		{
			using namespace DirectX::PackedVector;
			return XMConvertHalfToFloat(XMConvertFloatToHalf(value));
		}

		// The neighboring half float above or below a stored value
		float AdjacentStoredCoordinate(float stored, bool up) noexcept
		{
			using namespace DirectX::PackedVector;
			auto bits = XMConvertFloatToHalf(stored);
			const bool negative = (bits & 0x8000u) != 0;
			if ((bits & 0x7FFFu) == 0)
				bits = up ? 0x0001u : 0x8001u;
			else if (negative == up)
				--bits;
			else
				++bits;
			return XMConvertHalfToFloat(bits);
		}

		// Stays in [low, high) so candidates in different squares never share a stored position
		std::optional<float> StoredCoordinateWithin(float value, float low, float high) noexcept
		{
			auto stored = StoredCoordinate(value);
			if (stored < low)
				stored = AdjacentStoredCoordinate(stored, true);
			else if (stored >= high)
				stored = AdjacentStoredCoordinate(stored, false);

			return stored >= low && stored < high ? std::optional(stored) : std::nullopt;
		}

		struct TerrainSample
		{
			float height = 0.0f;
			RE::NiPoint3 normal;
			RE::NiColorA color{};
		};

		// No allocations or engine TLS
		bool SampleLoadedLand(RE::TESObjectLAND* land, const RE::NiPoint3& position, TerrainSample& sample) noexcept
		{
			if (!land || !land->loadedData)
				return false;

			alignas(16) std::array<std::byte, 248> coordinates;
			if (!RE::GetLandCoordData(land, coordinates.data(), &position, 5))
				return false;

			RE::NiPoint3 interpolatedNormal;
			RE::GetLandData(land, coordinates.data(), &sample.height, &interpolatedNormal, &sample.normal, &sample.color);
			return true;
		}

		bool SampleWorldLand(const RE::NiPoint3& position, TerrainSample& sample)
		{
			auto* tes = RE::TES::GetSingleton();
			if (!tes || !tes->GetLandHeight(position, sample.height))
				return false;

			RE::NiPoint3 interpolatedNormal;
			RE::GetLandNormal(tes, &position, &interpolatedNormal, &sample.normal);
			RE::GetLandColor(tes, &position, &sample.color);
			return true;
		}
	}

	GrassDensityField::Warp GrassDensityField::MakeWarp(int cellX, int cellY) noexcept
	{
		constexpr float MaxOffset = 64.0f;
		constexpr float LatticePerCell = 8.0f;
		constexpr float LatticePerStep = LatticePerCell / static_cast<float>(CellSide - 1);

		// Two octaves of noise: a broad shape plus finer detail
		const auto offset = [](float x, float y, std::uint64_t salt) {
			return MaxOffset * (0.75f * DensityNoise(x, y, salt) + 0.25f * DensityNoise(x * 2.0f + 19.25f, y * 2.0f - 7.5f, salt ^ 0xD1B54A32D192ED03ull));
		};

		Warp result;
		for (int y = 0; y < CellSide; ++y) {
			for (int x = 0; x < CellSide; ++x) {
				// World-continuous lattice coordinates keep neighboring cells seamless
				const float latticeX = static_cast<float>(cellX) * LatticePerCell + static_cast<float>(x) * LatticePerStep;
				const float latticeY = static_cast<float>(cellY) * LatticePerCell + static_cast<float>(y) * LatticePerStep;
				result[y * CellSide + x] = { offset(latticeX, latticeY, 0xA24BAED4963EE407ull), offset(latticeX, latticeY, 0x9FB21C651E98DF25ull) };
			}
		}

		return result;
	}

	GrassDensityField GrassDensityField::Blend(const std::array<const GrassDensityField*, 9>& fields, const std::array<bool, 9>& present, std::shared_ptr<const Warp> warp)
	{
		// Neighboring cells share their edge row and column
		constexpr int Stride = CellSide - 1;

		// Average the present cells that cover this point
		const auto blendPoint = [&](int x, int y, float& value) {
			float sum = 0.0f;
			int count = 0;

			for (int dy = -1; dy <= 1; ++dy) {
				const int sourceY = y - Border - dy * Stride;
				if (sourceY < 0 || sourceY >= CellSide)
					continue;

				for (int dx = -1; dx <= 1; ++dx) {
					const int sourceX = x - Border - dx * Stride;
					const auto neighbor = NeighborIndex(dx, dy);
					if (sourceX < 0 || sourceX >= CellSide || !present[neighbor])
						continue;

					// A present cell without this grass contributes zero
					if (fields[neighbor])
						sum += fields[neighbor]->values[(sourceY + Border) * Side + sourceX + Border];

					++count;
				}
			}

			if (count)
				value = sum / float(count);
			return count != 0;
		};

		GrassDensityField result;
		result.warp = std::move(warp);
		result.hasBorder = true;

		constexpr std::array<std::pair<int, int>, 8> fallbackOffsets{ { { 0, -1 }, { -1, 0 }, { 1, 0 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } } };
		for (int y = 0; y < Side; ++y) {
			for (int x = 0; x < Side; ++x) {
				auto& value = result.values[y * Side + x];
				if (blendPoint(x, y, value))
					continue;

				// Use a fixed world-axis order so adjacent cells choose the same covered point
				for (const auto& [dx, dy] : fallbackOffsets) {
					if (blendPoint(x + dx, y + dy, value))
						break;
				}
			}
		}

		return result;
	}

	RE::NiPoint2 GrassDensityField::WarpOffset(float x, float y) const noexcept
	{
		const float gridX = std::clamp(x / Step, 0.0f, float(CellSide - 1));
		const float gridY = std::clamp(y / Step, 0.0f, float(CellSide - 1));
		const int lowerX = std::min(int(gridX), CellSide - 2);
		const int lowerY = std::min(int(gridY), CellSide - 2);
		const float tx = gridX - float(lowerX);
		const float ty = gridY - float(lowerY);

		const int index = lowerY * CellSide + lowerX;
		const auto& a = (*warp)[index];
		const auto& b = (*warp)[index + 1];
		const auto& c = (*warp)[index + CellSide];
		const auto& d = (*warp)[index + CellSide + 1];

		return { Bilerp(a.x, b.x, c.x, d.x, tx, ty), Bilerp(a.y, b.y, c.y, d.y, tx, ty) };
	}

	float GrassDensityField::Sample(float x, float y) const noexcept
	{
		if (!std::isfinite(x) || !std::isfinite(y))
			return 0.0f;

		if (warp) {
			const auto offset = WarpOffset(x, y);
			x += offset.x;
			y += offset.y;
		}

		// Unblended fields have an unfilled border, so stay within the cell
		const float minimum = hasBorder ? 0.0f : float(Border);
		const float maximum = hasBorder ? float(Side - 1) : float(Side - Border - 1);
		const float gridX = std::clamp(x / Step + float(Border), minimum, maximum);
		const float gridY = std::clamp(y / Step + float(Border), minimum, maximum);

		const int lowerX = std::min(int(gridX), Side - 2);
		const int lowerY = std::min(int(gridY), Side - 2);
		const float tx = gridX - float(lowerX);
		const float ty = gridY - float(lowerY);

		const int index = lowerY * Side + lowerX;
		return Bilerp(values[index], values[index + 1], values[index + Side], values[index + Side + 1], tx, ty);
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
					result[NeighborIndex(dx, dy)] = entry->second;
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

	GrassPatchCreation::StableRandom::StableRandom(std::uint64_t seed) :
		state(SplitMix64(seed))
	{
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
		const int block = cell / CellsPerOrigin;
		return Config::ExtendGrassDistance ? block * CellsPerOrigin : block;
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

		GrassPatchCreation patchCreation(cell, abort);
		const auto cliffSurfaces = Config::GeneratingCache ? std::vector<CliffSurfaces::Surface>{} : patchCreation.CollectCliffSurfaces();
		RE::BSSpinLockGuard grassLock(cell->grassCreateLock);

		if (!patchCreation.PrepareCell(manager, fromCache)) {
			PhysicsRaycast::GetSingleton().DeferCell(cell);
			DistantGrass::ClearCellAddGrassTask(cell);
			return;
		}

		patchCreation.PrepareSmooth(manager, fromCache);
		add(manager, cell, abort);
		ReleaseNativePlacement();

		// Cache serialization has already finished
		if (!*abort && !Config::GeneratingCache)
			patchCreation.AddCliffGrass(manager, cliffSurfaces);
	}

	GrassPatchCreation::GrassPatchCreation(RE::TESObjectCELL* a_cell, bool* a_abort) :
		previous(current), cell(a_cell), abort(a_abort)
	{
		current = nullptr;
		if (!cell || !enabled)
			return;

		if ((Config::RayCast || Config::GrassCliffs) && (cell->IsAttached() || CollisionGrid::Find(*cell)))
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
		return enabled && (Config::RayCast || Config::GrassCliffs) && !manager->generateGrassDataFiles && (fromCache ? Config::GrassCliffs : !Config::OnlyLoadFromCache);
	}

	bool GrassPatchCreation::CanPrepareCell(const RE::BGSGrassManager* manager, RE::TESObjectCELL& cell, bool fromCache)
	{
		if (cell.IsInteriorCell() || !NeedsCollision(manager, fromCache))
			return true;

		const auto settings = Plugin::GetRaycastSettings();
		return settings && (!settings->Valid || PhysicsRaycast::GetSingleton().CanFilterCell(cell));
	}

	bool GrassPatchCreation::PrepareCell(const RE::BGSGrassManager* manager, bool fromCache)
	{
		if (!CanPrepareCell(manager, *cell, fromCache))
			return false;

		// Collision can become available after construction
		if (!settings && NeedsCollision(manager, fromCache))
			settings = Plugin::GetRaycastSettings();
		return true;
	}

	std::vector<RE::TESGrass*> GrassPatchCreation::TextureGrasses(RE::TESLandTexture* texture)
	{
		std::vector<RE::TESGrass*> result;
		if (!texture)
			return result;

		const auto* setting = globals::settings::maxGrassTypesPerTexture;
		const int maximum = std::max({ 0, setting ? setting->GetInteger() : 2, Config::EnsureMaxGrassTypesPerTextureSetting });
		int used = 0;
		for (auto* grass : RE::GetGrassList(texture)) {
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
		constexpr int BaseLayer = PaintedLayers;  // The quadrant's default texture fills unpainted weight
		constexpr int QuadrantSide = 17;
		constexpr int FullWeight = 255;
		using Field = GrassDensityField;

		const auto* data = land ? land->loadedData : nullptr;
		if (!data)
			return {};

		struct Coverage
		{
			std::shared_ptr<GrassDensityField> field;
			float density;
			std::uint8_t layerMask = 0;  // Layers in the current quadrant that grow this grass
		};
		std::unordered_map<RE::FormID, Coverage> coverage;
		std::array<unsigned char, Field::Side * Field::Side> contributors{};

		const auto layerTexture = [&](int quadrant, int layer) {
			if (layer < PaintedLayers)
				return data->quadTextures[quadrant][layer];

			auto* texture = data->defQuadTextures[quadrant];
			return texture ? texture : RE::TESLandTexture::GetDefaultLandTexture();
		};

		const auto grassWeight = [&](std::uint8_t layerMask, const auto& weights, int paintedWeight) {
			int weight = layerMask & (1 << BaseLayer) ? std::max(FullWeight - paintedWeight, 0) : 0;
			for (int layer = 0; layer < PaintedLayers; ++layer) {
				if (layerMask & (1 << layer))
					weight += static_cast<std::uint8_t>(weights[layer]);
			}
			return std::min(weight, FullWeight);
		};

		// Terrain must remain loaded while copying
		for (int quadrant = 0; quadrant < 4; ++quadrant) {
			for (auto& [id, entry] : coverage)
				entry.layerMask = 0;

			for (int layer = 0; layer <= BaseLayer; ++layer) {
				for (const auto* grass : TextureGrasses(layerTexture(quadrant, layer))) {
					auto [it, inserted] = coverage.try_emplace(grass->GetFormID());
					auto& entry = it->second;
					if (inserted) {
						entry.field = std::make_shared<GrassDensityField>();
						entry.density = static_cast<float>(grass->GetDensity()) * 0.01f;
					}
					entry.layerMask |= static_cast<std::uint8_t>(1 << layer);
				}
			}

			// Quadrants overlap by one vertex along shared edges
			const int quadrantX = (quadrant % 2) * (QuadrantSide - 1);
			const int quadrantY = (quadrant / 2) * (QuadrantSide - 1);

			for (int y = 0; y < QuadrantSide; ++y) {
				for (int x = 0; x < QuadrantSide; ++x) {
					const auto& weights = data->percents[quadrant][y * QuadrantSide + x];
					int paintedWeight = 0;
					for (const auto weight : weights)
						paintedWeight += static_cast<std::uint8_t>(weight);

					const int index = (y + quadrantY + Field::Border) * Field::Side + x + quadrantX + Field::Border;
					++contributors[index];

					for (auto& [id, entry] : coverage) {
						if (entry.layerMask)
							entry.field->values[index] += entry.density * static_cast<float>(grassWeight(entry.layerMask, weights, paintedWeight)) / FullWeight;
					}
				}
			}
		}

		// Average shared edges to prevent quadrant seams
		DensityFields fields;
		for (auto& [id, entry] : coverage) {
			for (std::size_t i = 0; i < entry.field->values.size(); ++i) {
				if (contributors[i])
					entry.field->values[i] /= static_cast<float>(contributors[i]);
			}
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
			neighbors[CenterNeighbor] = std::move(cellFields);

			auto warp = std::make_shared<const GrassDensityField::Warp>(GrassDensityField::MakeWarp(coordinates->cellX, coordinates->cellY));
			densityFields = BlendNeighborhood(neighbors, std::move(warp));
			smoothReady = true;
		} catch (const std::bad_alloc&) {
			logger::warn("Unable to capture smooth grass terrain; keeping patch placement.");
		}
	}

	GrassPatchCreation::DensityFields GrassPatchCreation::BlendNeighborhood(const std::array<GrassTerrainCache::Snapshot, 9>& neighbors, std::shared_ptr<const GrassDensityField::Warp> warp)
	{
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

			auto field = std::make_shared<GrassDensityField>(GrassDensityField::Blend(fields, present, warp));
			if (std::ranges::any_of(field->values, [](float value) { return value > 0.0f; }))
				blended.emplace(id, std::move(field));
		}

		return blended;
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

	// AE 1.7.99+ inserts a density multiplier that shifts the water and slope members
	void GrassPatchCreation::ApplyRuntimeParams(Patch& patch, const RE::CreateGrassParams& params)
	{
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
	}

	bool GrassPatchCreation::Queue(RE::BGSGrassManager* manager, const RE::CreateGrassParams& params, RE::BSMultiStreamInstanceTriShape* shape, std::uint32_t cellSide, std::uint32_t first, std::uint32_t end, const RE::NiPoint2* origin)
	{
		if (!shape || !params.land || params.land->parentCell != cell)
			return false;

		const auto* minimumSize = globals::settings::minGrassSize;
		if (!minimumSize)
			return false;

		const auto minimumSpacing = minimumSize->GetInteger();
		if (shape->GetMultiStreamTrishapeRuntimeData().instanceSize != InstanceHalfwords || minimumSpacing <= 0)
			return false;

		// Smooth cells pass their grid side and a [first, end) slice; engine patches size the grid from spacing
		const bool cellSlice = cellSide != 0;
		const auto& grass = *params.grassParam;
		const bool validRange = std::isfinite(grass.positionRange) && (cellSlice ? grass.positionRange >= 0.0f : grass.positionRange > 0.0f);
		if (!validRange)
			return false;

		const float patchWidth = static_cast<float>(manager->grassPatchSize) * 2.0f;
		const float sideCount = cellSlice ? static_cast<float>(cellSide) : std::min(patchWidth / grass.positionRange, patchWidth / static_cast<float>(minimumSpacing));
		if (!std::isfinite(sideCount) || sideCount < 1.0f || sideCount > 65535.0f)
			return false;

		const auto candidatesPerSide = cellSlice ? cellSide : static_cast<std::uint32_t>(sideCount);
		const auto candidates = cellSlice ? std::size_t(end - first) : static_cast<std::size_t>(candidatesPerSide) * candidatesPerSide;
		if (patchCount && (patchCount == patches.size() || candidateCount + candidates > MaxBatchCandidates))
			Flush();

		auto& patch = patches[patchCount];
		patch.shape = shape;
		patch.params = params;
		patch.grass = grass;
		patch.start = *params.grassPatchStart;
		patch.origin = origin ? *origin : RE::NiPoint2{ OriginCoordinate(params.CellX), OriginCoordinate(params.CellY) };
		ApplyRuntimeParams(patch, params);

		patch.sideCount = candidatesPerSide;
		patch.firstCandidate = cellSlice ? first : 0;
		patch.endCandidate = cellSlice ? end : candidatesPerSide * candidatesPerSide;
		patch.width = cellSlice ? CellSize : patchWidth;
		patch.halfSpacing = (1.0f / static_cast<float>(patch.sideCount)) * patch.width * 0.5f;

		std::transform(params.grassDensityPcts, params.grassDensityPcts + patch.density.size(), patch.density.begin(), [](float density) {
			return std::isfinite(density) && density > 0.0f ? density : 0.0f;
		});
		patch.densityField = Config::SmoothGrassPlacement ? DensityField(params.land, patch.grass.grassFormID) : nullptr;
		std::tie(patch.minSlope, patch.maxSlope) = SlopeLimits(patch.params.minSlopeDegrees, patch.params.maxSlopeDegrees);

		// Seeds must not depend on scheduling
		patch.seed = CellSeed(params.CellX, params.CellY) ^ patch.grass.grassFormID;
		patch.seed ^= std::rotl(std::bit_cast<std::uint32_t>(patch.start.x), 11) ^ std::rotl(std::bit_cast<std::uint32_t>(patch.start.y), 23);

		patch.instances.resize(candidates * InstanceHalfwords);
		if (settings)
			patch.candidates.resize(candidates);
		patch.created = 0;
		patch.needsWorldLookup = false;

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

	std::pair<float, float> GrassPatchCreation::SlopeLimits(std::uint32_t minDegrees, std::uint32_t maxDegrees)
	{
		const auto* table = RE::GrassSlopeTable();
		return { table[SlopeIndex(maxDegrees)], table[SlopeIndex(minDegrees)] };
	}

	float GrassPatchCreation::OriginCoordinate(int cell) noexcept
	{
		// Match engine truncation toward zero
		return static_cast<float>(CellsPerOrigin * (cell / CellsPerOrigin)) * CellSize;
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

		const float halfPatchWidth = patch.halfSpacing * static_cast<float>(patch.sideCount);
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

	std::uint32_t GrassPatchCreation::NativeDensityThreshold(const Patch& patch, float column, float row, float reciprocalSide) noexcept
	{
		// The engine samples density on a 3-by-3 grid spanning the patch
		const float densityX = (column * 2.0f) * reciprocalSide;
		const float densityY = (row * 2.0f) * reciprocalSide;
		const auto index = static_cast<std::size_t>(densityX) + 3 * static_cast<std::size_t>(densityY);
		const float blendX = densityX - static_cast<float>(static_cast<int>(densityX));
		const float blendY = densityY - static_cast<float>(static_cast<int>(densityY));
		const auto& density = patch.density;

		// Keep the engine's arithmetic order for its integer density roll
		const float lower = blendX * density[index + 1] + (1.0f - blendX) * density[index];
		const float upper = (1.0f - blendX) * density[index + 3] + blendX * density[index + 4];
		const float densityAtCenter = (lower * (1.0f - blendY) + upper * blendY) * 32768.0f;
		const float threshold = patch.densityMultiplier * densityAtCenter;
		return threshold > 0.0f ? static_cast<std::uint32_t>(std::min(threshold, 32768.0f)) : 0u;
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
		const float originX = patch.origin.x;
		const float originY = patch.origin.y;

		// Grid line k relative to the origin, shared exactly by neighboring squares
		const auto gridLine = [&](float start, float origin, std::uint32_t k) {
			return (start - origin) + patch.width * static_cast<float>(k) / static_cast<float>(patch.sideCount);
		};

		// Positions are stored as half floats; smooth placement keeps them in the candidate's square so blades never stack
		const auto storedPosition = [&](float x, float y, std::uint32_t gridX, std::uint32_t gridY) -> std::optional<RE::NiPoint2> {
			if (!stablePlacement)
				return RE::NiPoint2{ StoredCoordinate(x - originX), StoredCoordinate(y - originY) };

			const auto storedX = StoredCoordinateWithin(x - originX, gridLine(patch.start.x, originX, gridX), gridLine(patch.start.x, originX, gridX + 1));
			const auto storedY = StoredCoordinateWithin(y - originY, gridLine(patch.start.y, originY, gridY), gridLine(patch.start.y, originY, gridY + 1));
			if (!storedX || !storedY)
				return std::nullopt;

			return RE::NiPoint2{ *storedX, *storedY };
		};

		const auto store = [&](const Candidate& candidate) {
			if (settings)
				patch.candidates[patch.created] = candidate;
			else
				Encode(candidate, grass.fitsToSlope, patch.instances.data() + static_cast<std::size_t>(patch.created) * InstanceHalfwords);

			++patch.created;
		};

		for (auto candidateIndex = patch.resumeIndex; candidateIndex < patch.endCandidate; ++candidateIndex) {
			const auto gridX = candidateIndex / patch.sideCount;
			const auto gridY = candidateIndex % patch.sideCount;
			const float column = static_cast<float>(gridX) + 0.5f;
			const float row = static_cast<float>(gridY) + 0.5f;
			const auto chance = NativeDensityThreshold(patch, column, row, reciprocal);

			// Keep the engine's draw order: roll, jitter x, jitter y, shade, orientation, variation
			// False defers terrain lookup to the caller
			const auto processCandidate = [&](auto& random) {
				if (!stablePlacement && !chance)
					return true;

				const auto roll = random.Next() & 0x7FFFu;
				if (!stablePlacement && roll >= chance)
					return true;

				// Each candidate starts at its grid square's center and can move half a square along either axis
				const float candidateX = patch.start.x + ((column * patch.halfSpacing) * 2.0f) + random.SignedFloat() * patch.halfSpacing;
				const float candidateY = patch.start.y + ((row * patch.halfSpacing) * 2.0f) + random.SignedFloat() * patch.halfSpacing;

				// Smooth density follows the jittered position before it is rounded for storage
				if (stablePlacement) {
					const float coverage = SampleDensity(patch, { candidateX, candidateY, 0.0f });
					const float probability = std::clamp(coverage * patch.densityMultiplier, 0.0f, 1.0f);
					if (static_cast<float>(roll) >= probability * 32768.0f)
						return true;
				}

				// Query terrain where the stored instance will actually be
				const auto stored = storedPosition(candidateX, candidateY, gridX, gridY);
				if (!stored)
					return true;

				const RE::NiPoint3 position{ originX + stored->x, originY + stored->y, 0.0f };
				TerrainSample terrain;
				if (!SampleLoadedLand(params.land, position, terrain)) {
					if (!allowWorldLookup)
						return false;
					if (!SampleWorldLand(position, terrain))
						return true;
				}

				if (!AcceptWater(params, terrain.height) || terrain.normal.z < patch.minSlope || terrain.normal.z > patch.maxSlope)
					return true;

				const float shade = Shade(terrain.color, random.SignedFloat(), grass.colorRange);
				const float orientation = random.SignedFloat();
				const float variation = random.SignedFloat() * grass.heightRange;
				store({ { position.x, position.y, terrain.height }, *stored, terrain.normal, shade, orientation, variation });
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
			Encode(candidate, patch.grass.fitsToSlope, output);
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

	bool GrassPatchCreation::UseCentredShapes(const RE::BGSGrassManager* manager)
	{
		// Extended grass keys shapes by corner cell, leaving room for distinct keys
		// Cache files are corner-relative, so generation keeps the engine's shapes
		return Config::ExtendGrassDistance && !Config::GeneratingCache && manager && !manager->generateGrassDataFiles;
	}

	float GrassPatchCreation::CentredOrigin(int cell, int& key, float spacing) noexcept
	{
		// Wind phase uses local x + y and repeats every 640π units, so shift by whole cycles to stay in step
		constexpr double WindCycle = 640.0 * std::numbers::pi;
		constexpr double BlockSize = static_cast<double>(CellsPerOrigin) * CellSize;

		// Block 0 spans both signs, so each sign gets its own shape
		const int block = cell / CellsPerOrigin;
		const double corner = static_cast<double>(OriginCoordinate(cell));
		double low;
		double high;
		if (cell >= 0) {
			low = corner;
			high = corner + BlockSize;
			key = block * CellsPerOrigin + CellsPerOrigin / 2;
		} else {
			// Negative blocks end one cell above their corner; block 0's negative half has 11 cells
			high = block == 0 ? 0.0 : corner + CellSize;
			low = high - (block == 0 ? BlockSize - CellSize : BlockSize);
			key = block * CellsPerOrigin - CellsPerOrigin / 2;
		}

		const double centre = (low + high) * 0.5;
		const float origin = static_cast<float>(corner + std::round((centre - corner) / WindCycle) * WindCycle);
		const float start = static_cast<float>(cell) * CellSize;
		const float first = StoredCoordinate(start - origin);
		const float last = StoredCoordinate(start + CellSize - origin);
		const float precision = std::max(AdjacentStoredCoordinate(first, true) - first, last - AdjacentStoredCoordinate(last, false));
		if (precision <= spacing)
			return origin;

		// Per-cell keys use a different residue from corner and block-centred shapes
		key = cell * CellsPerOrigin + 1;
		return start + CellSize * 0.5f;
	}

	GrassPatchCreation::PreparedShape GrassPatchCreation::PrepareShape(RE::BGSGrassManager* manager, RE::GrassParam& param, float spacing, bool centred)
	{
		const auto* coordinates = cell->GetCoordinates();
		std::uint64_t typeKey = 0;
		RE::BSFixedString modelKey;

		int keyX = ShapeCoordinate(coordinates->cellX);
		int keyY = ShapeCoordinate(coordinates->cellY);
		RE::NiPoint2 origin{ OriginCoordinate(coordinates->cellX), OriginCoordinate(coordinates->cellY) };
		if (centred) {
			origin.x = CentredOrigin(coordinates->cellX, keyX, spacing);
			origin.y = CentredOrigin(coordinates->cellY, keyY, spacing);
		}

		auto* shape = RE::LoadGrassType(manager, &param, static_cast<std::uint32_t>(keyX), static_cast<std::uint32_t>(keyY), &typeKey, &modelKey);
		if (!shape)
			return {};

		// Move new shapes before instances exist, since Community Shaders captures the origin then
		auto& translate = shape->local.translate;
		if (centred && (translate.x != origin.x || translate.y != origin.y)) {
			translate.x = origin.x;
			translate.y = origin.y;

			RE::NiUpdateData update{};
			shape->UpdateWorldData(&update);
			shape->previousWorld = shape->world;
		}

		auto* handle = GrassHandle(cell, shape, typeKey, modelKey);

		if (!shape->GetIsAddingInstances())
			shape->BeginAddingInstances(InstanceHalfwords);

		return { shape, handle, origin };
	}

	// Keeps the native candidate budget across patch boundaries; zero skips the grass
	std::uint32_t GrassPatchCreation::SmoothCandidatesPerSide(float spacing, float patchWidth)
	{
		if (!std::isfinite(spacing) || spacing <= 0.0f || patchWidth <= 0.0f)
			return 0;

		const float patchCandidatesPerSide = std::floor(patchWidth / spacing);
		if (patchCandidatesPerSide < 1.0f)
			return 0;

		return static_cast<std::uint32_t>(std::clamp(std::lround((CellSize / patchWidth) * patchCandidatesPerSide), 1L, 4096L));
	}

	// The result points at param and start, which must outlive it
	RE::CreateGrassParams GrassPatchCreation::MakeSmoothParams(const RE::TESGrass& grass, RE::GrassParam& param, RE::NiPoint3& start) const
	{
		const auto* coordinates = cell->GetCoordinates();

		RE::CreateGrassParams params{};
		params.CellX = coordinates->cellX;
		params.CellY = coordinates->cellY;
		params.land = cell->GetLand();
		params.grassPatchStart = &start;
		params.grassParam = &param;
		params.grassDensityPcts = param.denstityPctPerQuadVertex;
		if (auto* multiplier = params.GetDensityMultiplier())
			*multiplier = 1.0f;
		params.GetDistanceFromWaterLevel() = static_cast<float>(grass.GetDistanceFromWaterLevel());
		params.GetUnderwaterState() = static_cast<std::uint32_t>(grass.GetUnderwaterState());
		params.GetWaterHeight() = cell->GetExteriorWaterHeight();
		params.GetMinSlopeDegrees() = grass.GetMinSlopeDegrees();
		params.GetMaxSlopeDegrees() = grass.GetMaxSlopeDegrees();

		return params;
	}

	void GrassPatchCreation::GenerateSmoothCell()
	{
		auto* land = cell->GetLand();
		const auto* coordinates = cell->GetCoordinates();
		const auto* minimumSize = globals::settings::minGrassSize;

		if (!land || !land->loadedData || !coordinates || !minimumSize || minimumSize->GetInteger() <= 0)
			throw std::bad_alloc();

		const bool centred = UseCentredShapes(placementManager);
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
			const auto candidatesPerSide = SmoothCandidatesPerSide(spacing, static_cast<float>(placementManager->grassPatchSize) * 2.0f);
			if (!candidatesPerSide)
				continue;

			const auto prepared = PrepareShape(placementManager, param, CellSize / static_cast<float>(candidatesPerSide), centred);
			if (!prepared.shape)
				continue;

			const stl::scope_exit finish([&] { prepared.shape->DoneAddingInstances(prepared.handle->instanceGroupIndicies); });
			RE::NiPoint3 start{ static_cast<float>(coordinates->cellX) * CellSize, static_cast<float>(coordinates->cellY) * CellSize, 0.0f };
			const auto params = MakeSmoothParams(*grass, param, start);

			// Queue the cell in slices so workers can share it
			const auto totalCandidates = candidatesPerSide * candidatesPerSide;
			for (std::uint32_t firstCandidate = 0; firstCandidate < totalCandidates && !*abort; firstCandidate += SmoothPatchCandidates) {
				const auto endCandidate = std::min(firstCandidate + SmoothPatchCandidates, totalCandidates);
				if (!Queue(placementManager, params, prepared.shape, candidatesPerSide, firstCandidate, endCandidate, &prepared.origin))
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
		if (!settings->IsCliffAt(highest, { point.x, point.y, point.z }))
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
		// Neutral grey when the land is not loaded
		TerrainSample terrain{ .color = { 0.5f, 0.5f, 0.5f, 1.0f } };
		SampleLoadedLand(cell->GetRuntimeData().cellLand, { x, y, 0.0f }, terrain);

		return Shade(terrain.color, random.SignedFloat(), colorRange);
	}

	std::vector<CliffSurfaces::Surface> GrassPatchCreation::CollectCliffSurfaces() const
	{
		if (!enabled || !cell || !settings || !settings->Valid || !Config::GrassCliffs || *abort || cell->IsInteriorCell())
			return {};

		return CliffSurfaces::Get(cell);
	}

	void GrassPatchCreation::AddCliffGrass(RE::BGSGrassManager* manager, const std::vector<CliffSurfaces::Surface>& surfaces)
	{
		if (surfaces.empty() || *abort)
			return;

		const auto* coordinates = cell->GetCoordinates();
		std::unordered_map<RE::TESLandTexture*, std::vector<RE::NiPoint3>> tops;

		for (const auto& surface : surfaces) {
			if (*abort)
				return;

			auto* landTexture = CliffSurfaces::GetTextureIndex().Find(surface.texture);
			if (!landTexture || RE::GetGrassList(landTexture).empty())
				continue;

			const auto& center = surface.center;
			const float radius = std::min(surface.radius, MaxCliffRadius);

			for (float x = center.x - radius + CliffSampleStep * 0.5f; x < center.x + radius; x += CliffSampleStep) {
				for (float y = center.y - radius + CliffSampleStep * 0.5f; y < center.y + radius; y += CliffSampleStep) {
					const auto top = FindCliffTop(x, y, center.z + radius + 1.0f, center.z - radius - 1.0f);

					if (top && top->formID == surface.object && top->position.GetSquaredDistance(center) <= radius * radius)
						tops[landTexture].push_back({ x, y, top->position.z });
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

		const auto* maxTypesSetting = globals::settings::maxGrassTypesPerTexture;
		const int maxTypes = maxTypesSetting ? std::max(1, maxTypesSetting->GetInteger()) : 7;
		std::uint32_t blades = 0;
		for (const auto& [landTexture, samples] : tops) {
			int types = 0;
			for (auto* grass : RE::GetGrassList(landTexture)) {
				if (*abort || types >= maxTypes)
					break;
				if (!grass)
					continue;

				++types;
				const auto seed = CellSeed(coordinates->cellX, coordinates->cellY) ^ grass->GetFormID() ^ std::rotl(landTexture->GetFormID(), 13);
				blades += AddCliffGrassType(manager, *grass, seed, samples);
			}
		}

		if (sampleCount)
			logger::debug("Cliff tops in cell ({}, {}): {} samples over {} landscape textures, {} blades", coordinates->cellX, coordinates->cellY, sampleCount, tops.size(), blades);
	}

	std::uint32_t GrassPatchCreation::AddCliffGrassType(RE::BGSGrassManager* manager, RE::TESGrass& grass, std::uint32_t seed, const std::vector<RE::NiPoint3>& samples)
	{
		auto param = MakeGrassParam(grass);
		const auto* minimumSize = globals::settings::minGrassSize;
		const float spacing = std::max(param.positionRange, minimumSize ? static_cast<float>(minimumSize->GetInteger()) : 0.0f);
		const float chance = param.denstityPctPerQuadVertex[0];

		if (!std::isfinite(spacing) || spacing < 1.0f || !std::isfinite(chance) || chance <= 0.0f)
			return 0;

		const auto* coordinates = cell->GetCoordinates();
		const int perSide = std::max(1, static_cast<int>(CliffSampleStep / spacing));
		const float step = CliffSampleStep / static_cast<float>(perSide);
		const auto prepared = PrepareShape(manager, param, step, UseCentredShapes(manager));
		if (!prepared.shape)
			return 0;

		const stl::scope_exit finish([&] { prepared.shape->DoneAddingInstances(prepared.handle->instanceGroupIndicies); });

		float minSlope, maxSlope;
		std::tie(minSlope, maxSlope) = SlopeLimits(grass.GetMinSlopeDegrees(), grass.GetMaxSlopeDegrees());

		Patch patch{};
		patch.shape = prepared.shape;
		patch.origin = prepared.origin;
		patch.grass = param;
		patch.params.CellX = coordinates->cellX;
		patch.params.CellY = coordinates->cellY;

		// Blades need support this far out so they do not hang over the edge
		const auto& bounds = grass.boundData;
		const float reach = std::max({ MinBladeReach, std::abs(static_cast<float>(bounds.boundMax.x - bounds.boundMin.x)) * 0.5f, std::abs(static_cast<float>(bounds.boundMax.y - bounds.boundMin.y)) * 0.5f });

		auto* land = cell->GetRuntimeData().cellLand;
		const auto canPlace = [&](float x, float y, const CliffTop& top) {
			if (top.normal.z < minSlope || top.normal.z > maxSlope || !SupportedAround(x, y, top.position.z, reach))
				return false;
			if (!Config::RayCast)
				return true;
			if (!settings->IsExempt(land, x, y, top.position.z, &param) && settings->OnBlockedTexture(land, x, y, top.position.z))
				return false;

			// Start above the cliff to avoid self-hits
			return settings->CanPlaceGrass(land, x, y, top.position.z + settings->RayDepth + 1.0f, &param, true);
		};

		// Subdivide each cliff sample into a jittered grid at the grass spacing
		Random random(seed);

		for (const auto& sample : samples) {
			for (int i = 0; i < perSide && !*abort; ++i) {
				for (int j = 0; j < perSide; ++j) {
					const float x = sample.x - CliffSampleStep * 0.5f + (static_cast<float>(i) + 0.5f) * step + random.SignedFloat() * step * 0.5f;
					const float y = sample.y - CliffSampleStep * 0.5f + (static_cast<float>(j) + 0.5f) * step + random.SignedFloat() * step * 0.5f;

					if (static_cast<float>(random.Next() & 0x7FFFu) >= chance * 32768.0f)
						continue;

					const auto top = FindCliffTop(x, y, sample.z + CliffSampleStep, sample.z - CliffSampleStep);
					if (!top || !canPlace(x, y, *top))
						continue;

					const Candidate candidate{ top->position, { StoredCoordinate(top->position.x - prepared.origin.x), StoredCoordinate(top->position.y - prepared.origin.y) }, top->normal, LandShade(x, y, random, param.colorRange), random.SignedFloat(), random.SignedFloat() * param.heightRange };
					patch.instances.resize(static_cast<std::size_t>(patch.created + 1) * InstanceHalfwords);
					Encode(candidate, param.fitsToSlope, patch.instances.data() + static_cast<std::size_t>(patch.created) * InstanceHalfwords);
					++patch.created;
				}
			}
		}

		const auto created = patch.created;
		if (created)
			Commit(patch);

		return created;
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

	void GrassPatchCreation::Encode(const Candidate& candidate, bool fitsToSlope, std::uint16_t* instance) const noexcept
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

		instance[0] = XMConvertFloatToHalf(candidate.localPosition.x);
		instance[1] = XMConvertFloatToHalf(candidate.localPosition.y);
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
