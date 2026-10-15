#pragma once

namespace GrassControl
{
	class RaycastHelper;

	class GrassPatchCreation final
	{
	public:
		GrassPatchCreation(RE::TESObjectCELL* cell, bool* abort);
		~GrassPatchCreation();

		GrassPatchCreation(const GrassPatchCreation&) = delete;
		GrassPatchCreation& operator=(const GrassPatchCreation&) = delete;

		// Failure leaves native placement active without NGIO filtering.
		static bool Install();
		static RE::TESObjectCELL* GetCell();
		static bool IsEnabled() { return enabled; }
		// Reserve cores for frame processing during gameplay.
		static void LimitParallelism(bool limit);

		static void ReleaseNativePlacement();
		using AddCellGrassFunc = void (*)(RE::BGSGrassManager*, RE::TESObjectCELL*, bool*);

		// Outside the extended grid, defer native placement until collision is ready.
		static void AddCell(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort, AddCellGrassFunc add);
		static std::uint32_t CreatePatch(RE::BGSGrassManager* manager, RE::CreateGrassParams* params, RE::BSMultiStreamInstanceTriShape* shape);
		static void FinishInstances(RE::BSMultiStreamInstanceTriShape* shape, RE::BSTArray<std::uint32_t>& instances);

		bool PrepareCell(bool requireCollision = true);

		// Caller holds grassCreateLock; grass types come from the matching landscape texture.
		void AddCliffGrass(RE::BGSGrassManager* manager);

	private:
		inline static constexpr std::size_t InstanceHalfwords = 16;
		inline static constexpr std::uint32_t HalfwordBytes = sizeof(std::uint16_t);
		inline static constexpr std::size_t MaxBatchCandidates = 16384;
		inline static constexpr std::size_t DispatchCandidates = 1024;
		inline static constexpr std::uint32_t InstanceBufferBytes = 0x40000;
		inline static constexpr int CellsPerOrigin = 12;
		inline static constexpr float CellSize = 4096.0f;
		inline static constexpr std::size_t GameplayParallelism = 6;

		class CreatePatchCall final : public Xbyak::CodeGenerator
		{
		public:
			CreatePatchCall();
		};

		// Skyrim 1.7.99 inserts the density multiplier before the water and slope parameters.
		struct ExtendedGrassParams
		{
			int cellX;
			int cellY;
			int quad;
			std::uint32_t padding;
			RE::TESObjectLAND* land;
			RE::NiPoint3* grassPatchStart;
			RE::GrassParam* grassParam;
			float* grassDensityPcts;
			float densityMultiplier;
			float distanceFromWaterLevel;
			std::uint32_t underwaterState;
			float waterHeight;
			std::uint32_t minSlopeDegrees;
			std::uint32_t maxSlopeDegrees;
			std::array<std::byte, 8> trailingPadding;
		};
		static_assert(sizeof(ExtendedGrassParams) == sizeof(RE::CreateGrassParams));
		static_assert(offsetof(ExtendedGrassParams, densityMultiplier) == 0x30);
		static_assert(offsetof(ExtendedGrassParams, maxSlopeDegrees) == 0x44);

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
			RE::BSMultiStreamInstanceTriShape* shape;
			std::vector<std::uint16_t> instances;
			std::vector<Candidate> candidates;
			std::uint32_t sideCount;
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
			// Cancellation must not propagate between placement and another plugin's tasks.
			oneapi::tbb::task_group_context context{ oneapi::tbb::task_group_context::isolated };
			oneapi::tbb::task_group tasks{ context };
		};

		inline static constexpr float CliffSampleStep = 128.0f;
		inline static constexpr float MaxCliffRadius = 8192.0f;
		// A blade on a cliff needs ground within this height around its footprint, so it never hangs over an edge.
		inline static constexpr float CliffEdgeTolerance = 30.0f;
		inline static constexpr float MinBladeReach = 16.0f;

		struct CliffTop
		{
			RE::NiPoint3 position;
			RE::NiPoint3 normal;
			RE::FormID formID;
		};

		struct CliffSample
		{
			float x;
			float y;
			float z;
		};

		// Returns only the highest hit, and only if it belongs to a cliff.
		std::optional<CliffTop> FindCliffTop(float x, float y, float top, float bottom) const;
		// Requires level support at the blade's reach on all four sides.
		bool SupportedAround(float x, float y, float z, float reach) const;
		void AddCliffGrassType(RE::BGSGrassManager* manager, RE::TESGrass& grass, std::uint32_t seed, const std::vector<CliffSample>& samples);
		float LandShade(float x, float y, Random& random, float colorRange) const;

		bool Queue(RE::BGSGrassManager* manager, const RE::CreateGrassParams& params, RE::BSMultiStreamInstanceTriShape* shape);
		void Flush();
		void Dispatch();
		void Run() noexcept;
		void Generate(Patch& patch, bool allowWorldLookup = false) const noexcept;
		void MarkBlockedTextures(Patch& patch) const;
		void Filter(Patch& patch);
		void FilterPatches();
		RE::NiPoint3 SlopeTangent(const RE::NiPoint3& normal) const;
		void Commit(Patch& patch);
		void Encode(const Candidate& candidate, bool fitsToSlope, std::uint16_t* instance, int cellX, int cellY) const noexcept;
		static std::uint32_t SlopeIndex(std::uint32_t degrees);
		static float Shade(const RE::NiColorA& color, Random& random, float colorRange) noexcept;
		bool AcceptWater(const RE::CreateGrassParams& params, float height) const;

		inline static std::optional<oneapi::tbb::global_control> parallelismControl;
		inline static bool enabled = false;
		inline static bool anniversaryRuntime = false;
		inline static bool extendedParameters = false;
		inline static thread_local GrassPatchCreation* current = nullptr;
		// Engine placement writes grassManager->instanceData, which AddCellGrass reads back after the call returns.
		inline static std::mutex nativeMutex;
		inline static thread_local bool holdsNative = false;

		GrassPatchCreation* previous;
		RE::TESObjectCELL* cell;
		bool* abort;
		std::shared_ptr<const RaycastHelper> settings;
		// Published patches cannot move while workers are reading their buffers.
		std::array<Patch, 64> patches;
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
