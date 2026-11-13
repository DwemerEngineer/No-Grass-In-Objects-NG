#pragma once

namespace GrassControl
{
	struct GrassDensityField
	{
		inline static constexpr int Side = 33;
		inline static constexpr float Step = 128.0f;
		std::array<float, Side * Side> values{};

		static GrassDensityField Blend(const std::array<const GrassDensityField*, 9>& fields, const std::array<bool, 9>& present);

		float Sample(float x, float y) const noexcept;  // Cell-relative coordinates
	};

	class GrassTerrainCache
	{
	public:
		using Fields = std::unordered_map<std::uint32_t, std::shared_ptr<const GrassDensityField>>;
		using Snapshot = std::shared_ptr<const Fields>;
		using Key = std::tuple<std::uint32_t, int, int>;
		inline static constexpr std::size_t Capacity = 256;

		void Publish(std::uint32_t world, int x, int y, Snapshot fields);

		std::array<Snapshot, 9> Neighborhood(std::uint32_t world, int x, int y);

		void Clear();

	private:
		std::mutex mutex;
		std::map<Key, Snapshot> tiles;
		std::deque<Key> order;
	};

	class RaycastHelper;

	class GrassPatchCreation final
	{
	public:
		GrassPatchCreation(RE::TESObjectCELL* cell, bool* abort);
		~GrassPatchCreation();

		GrassPatchCreation(const GrassPatchCreation&) = delete;
		GrassPatchCreation& operator=(const GrassPatchCreation&) = delete;

		static void Enable();
		static RE::TESObjectCELL* GetCell();

		// Block corners for extended grass, block indices otherwise
		static int ShapeCoordinate(int cell);

		static void LimitParallelism(bool limit);

		static void ReleaseNativePlacement();
		using AddCellGrassFunc = void (*)(RE::BGSGrassManager*, RE::TESObjectCELL*, bool*);

		// Wait for collision outside the extended grid
		static void AddCell(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort, AddCellGrassFunc add);
		static std::uint32_t CreatePatch(RE::BGSGrassManager* manager, RE::CreateGrassParams* params, RE::BSMultiStreamInstanceTriShape* shape);
		static void FinishInstances(RE::BSMultiStreamInstanceTriShape* shape, RE::BSTArray<std::uint32_t>& instances);

		using BeginCellFunc = RE::BSTArray<RE::CellGrassData*>* (*)(RE::ExtraDataList*, bool);
		using SelectGrassFunc = std::uint32_t (*)(RE::TESObjectLAND*, std::uint32_t, std::uint32_t, RE::GrassParam*, std::uint32_t);

		static RE::BSTArray<RE::CellGrassData*>* BeginCell(RE::ExtraDataList* extraList, bool create, BeginCellFunc begin);
		static std::uint32_t SelectGrass(RE::TESObjectLAND* land, std::uint32_t quadrant, std::uint32_t vertex, RE::GrassParam* params, std::uint32_t capacity, SelectGrassFunc select);

		static bool NeedsCollision(const RE::BGSGrassManager* manager, bool fromCache);
		bool PrepareCell(bool requireCollision = true);
		void PrepareSmooth(RE::BGSGrassManager* manager, bool fromCache);

		static GrassTerrainCache::Snapshot CaptureTerrain(RE::TESObjectCELL* cell);
		static void ClearTerrainCache();

		void AddCliffGrass(RE::BGSGrassManager* manager);

	private:
		inline static constexpr std::size_t InstanceHalfwords = 16;
		inline static constexpr std::uint32_t HalfwordBytes = sizeof(std::uint16_t);
		inline static constexpr std::size_t MaxBatchCandidates = 16384;
		inline static constexpr std::size_t DispatchCandidates = 1024;
		inline static constexpr std::uint32_t SmoothPatchCandidates = 256;
		inline static constexpr std::uint32_t InstanceBufferBytes = 0x40000;
		inline static constexpr int CellsPerOrigin = 12;
		inline static constexpr int CellsPerShape = CellsPerOrigin;
		inline static constexpr float CellSize = 4096.0f;
		inline static constexpr std::size_t GameplayParallelism = 6;

		// Engine-compatible random sequence
		class Random final
		{
		public:
			explicit Random(std::uint32_t seed);
			std::uint32_t Next();
			float SignedFloat();
			std::uint64_t DrawCount() const { return draws; }

		private:
			std::array<std::uint32_t, 624> state;
			std::size_t index = 623;
			std::uint64_t draws = 0;
		};

		// Per-candidate PCG32 (XSH-RR)
		class StableRandom final
		{
		public:
			explicit StableRandom(std::uint64_t seed);
			std::uint32_t Next();
			float SignedFloat();

		private:
			std::uint64_t state;
		};

		struct Candidate
		{
			RE::NiPoint3 position;
			RE::NiPoint3 normal;
			float shade;
			float orientation;
			float variation;
			bool onBlockedTexture = false;
		};

		struct Patch
		{
			RE::CreateGrassParams params;
			RE::GrassParam grass;
			RE::NiPoint3 start;
			std::array<float, 9> density;
			std::shared_ptr<const GrassDensityField> densityField;
			RE::BSMultiStreamInstanceTriShape* shape;
			std::vector<std::uint16_t> instances;
			std::vector<Candidate> candidates;
			std::uint32_t sideCount;
			std::uint32_t firstCandidate = 0;
			std::uint32_t endCandidate = 0;
			std::uint32_t seed;
			std::uint32_t created = 0;
			bool needsWorldLookup = false;
			std::uint32_t resumeIndex = 0;
			std::uint64_t resumeRandomDraws = 0;
			float minSlope;
			float maxSlope;
			float spacing;
			float densityMultiplier = 1.0f;
		};

		class BackgroundPriority : public oneapi::tbb::task_scheduler_observer
		{
		public:
			explicit BackgroundPriority(oneapi::tbb::task_arena& arena) :
				task_scheduler_observer(arena) { observe(true); }
			~BackgroundPriority() override { observe(false); }

			void on_scheduler_entry(bool worker) override
			{
				if (worker && depth++ == 0) {
					previousPriority = GetThreadPriority(GetCurrentThread());
					if (previousPriority != THREAD_PRIORITY_ERROR_RETURN)
						SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
				}
			}
			void on_scheduler_exit(bool worker) override
			{
				if (worker && --depth == 0 && previousPriority != THREAD_PRIORITY_ERROR_RETURN)
					SetThreadPriority(GetCurrentThread(), previousPriority);
			}

		private:
			inline static thread_local unsigned depth = 0;
			inline static thread_local int previousPriority = THREAD_PRIORITY_ERROR_RETURN;
		};

		struct Work
		{
			explicit Work(unsigned int concurrency);

			oneapi::tbb::task_arena arena;
			BackgroundPriority priority{ arena };
			oneapi::tbb::task_group_context context{ oneapi::tbb::task_group_context::isolated };
			oneapi::tbb::task_group tasks{ context };
		};

		inline static constexpr float CliffSampleStep = 128.0f;
		inline static constexpr float MaxCliffRadius = 8192.0f;
		// Allowed ground drop at cliff edges
		inline static constexpr float CliffEdgeTolerance = 30.0f;
		inline static constexpr float MinBladeReach = 16.0f;

		struct CliffTop
		{
			RE::NiPoint3 position;
			RE::NiPoint3 normal;
			RE::FormID formID;
		};

		// Only the highest hit counts
		std::optional<CliffTop> FindCliffTop(float x, float y, float top, float bottom) const;
		// Check support on all four sides
		bool SupportedAround(float x, float y, float z, float reach) const;
		void AddCliffGrassType(RE::BGSGrassManager* manager, RE::TESGrass& grass, std::uint32_t seed, const std::vector<RE::NiPoint3>& samples);
		float LandShade(float x, float y, Random& random, float colorRange) const;

		using DensityFields = GrassTerrainCache::Fields;
		inline static GrassTerrainCache terrainCache;

		static std::vector<RE::TESGrass*> TextureGrasses(RE::TESLandTexture* texture);
		static DensityFields ReadDensityFields(RE::TESObjectLAND* land);
		std::pair<RE::BSMultiStreamInstanceTriShape*, RE::CellGrassData*> PrepareShape(RE::BGSGrassManager* manager, RE::GrassParam& param);
		void GenerateSmoothCell();

		std::shared_ptr<const GrassDensityField> DensityField(RE::TESObjectLAND* land, RE::FormID grassID);

		bool Queue(RE::BGSGrassManager* manager, const RE::CreateGrassParams& params, RE::BSMultiStreamInstanceTriShape* shape, std::uint32_t cellSide = 0, std::uint32_t first = 0, std::uint32_t end = 0);
		void Flush();
		void CancelWork();
		void ResetBatch();
		void Dispatch();
		void Run() noexcept;
		void Generate(Patch& patch, bool allowWorldLookup = false) const noexcept;
		static float SampleDensity(const Patch& patch, const RE::NiPoint3& position) noexcept;

		void MarkBlockedTextures(Patch& patch) const;
		void Filter(Patch& patch);
		void FilterPatches();
		RE::NiPoint3 SlopeTangent(const RE::NiPoint3& normal) const;
		void Commit(Patch& patch);
		void Encode(const Candidate& candidate, bool fitsToSlope, std::uint16_t* instance, int cellX, int cellY) const noexcept;
		static std::uint32_t SlopeIndex(std::uint32_t degrees);
		// roll is in [-1, 1]
		static float Shade(const RE::NiColorA& color, float roll, float colorRange) noexcept;
		bool AcceptWater(const RE::CreateGrassParams& params, float height) const;

		inline static std::optional<oneapi::tbb::global_control> parallelismControl;
		inline static bool enabled = false;
		inline static thread_local GrassPatchCreation* current = nullptr;
		// Keep instanceData locked until AddCellGrass consumes it
		inline static std::mutex nativeMutex;
		inline static thread_local bool holdsNative = false;

		GrassPatchCreation* previous;
		RE::TESObjectCELL* cell;
		bool* abort;
		std::shared_ptr<const RaycastHelper> settings;
		// Worker buffer addresses must stay fixed
		std::array<Patch, 64> patches;
		DensityFields densityFields;
		RE::BGSGrassManager* placementManager = nullptr;
		bool smoothReady = false;
		bool smoothCompleted = false;
		std::size_t patchCount = 0;
		std::size_t candidateCount = 0;
		std::atomic<std::size_t> publishedPatches{ 0 };
		std::atomic<std::size_t> nextPatch{ 0 };
		std::atomic<unsigned int> activeCallbacks{ 0 };
		std::unique_ptr<Work> work;
		unsigned int workerCount = 1;
		double scale = 1.0;
	};
}
