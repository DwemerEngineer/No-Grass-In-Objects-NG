#pragma once

namespace GrassControl::Util
{
	struct ShapeSnapshot;
}

namespace GrassControl
{
	class RaycastHelper;

	class PhysicsRaycast final
	{
	public:
		enum class QueryStatus
		{
			Success,
			Unavailable,
			InvalidInput,
			Failed
		};

		struct HitResult
		{
			glm::vec3 normal{};
			float hitFraction = 0.0f;
			RE::FormID formID = 0;
			RE::ObjectRefHandle reference;
			bool hitCliff = false;
		};

		struct RayResult
		{
			QueryStatus status = QueryStatus::Unavailable;
			std::vector<HitResult> hits;
			const char* unavailableReason = "no cell";
		};

		struct ShapeNames
		{
			std::string shape;
			std::string root;
		};

		static PhysicsRaycast& GetSingleton()
		{
			static PhysicsRaycast instance;
			return instance;
		}

		// Exposes waiting writers so grass readers can yield before taking the world lock.
		static void InstallWriterSignal();

		RayResult CastRay(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, const RaycastHelper& settings, bool ignoreCliff = false);

		RayResult CastVolume(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, RE::GrassParam* param, const RaycastHelper& settings, bool ignoreCliff = false);

		std::optional<ShapeNames> GetShapeNames(RE::ObjectRefHandle reference, RE::NiPoint3 position);

		static bool ValidateSettings(const RaycastHelper& settings);

		static bool ValidPoint(float x, float y, float z);

		void SetQueriesDisabled(bool disabled);

		bool CanFilterCell(const RE::TESObjectCELL& cell);

		void DeferCell(RE::TESObjectCELL* cell);

		void ReportAllocationFailure();

		void ReportStatistics();

		// Grass dispatch and DebugMessageBox require the game thread.
		void Update();

		// Counts unavailable queries on this thread; affected grass needs another filtering pass.
		static std::uint32_t UnavailableQueries() { return unavailableQueries; }

		static void AddUnavailableQueries(std::uint32_t count) { unavailableQueries += count; }

		// Returns false if the thread has no usable Havok heap after initialization.
		static bool EnsureThreadMemory();

	private:
		inline static constexpr float HavokScale = 0.0142875f;
		inline static constexpr double MaxDimension = 65536.0;
		inline static constexpr std::size_t MaxHits = 4096;

		static bool ValidDimension(double value);
		static bool ValidSettingsValues(int mode, double height, double depth, double width, double multiplier, double textureWidth);
		static bool ValidHit(float fraction, float x, float y, float z);
		static bool ValidRay(const glm::vec4& start, const glm::vec4& end);

		// Pending engine writers take priority over grass queries.
		class WorldReadGate final
		{
		public:
			void InstallWriterSignal();
			void EnterWriteLock(RE::BSReadWriteLock* lock);
			void Enter(RE::bhkWorld* world);
			void Exit();

		private:
			std::atomic<RE::BSReadWriteLock*> watchedLock{ nullptr };
			std::atomic<std::uint32_t> watchedWriters{ 0 };
			bool installed = false;
			inline static thread_local RE::BSReadWriteLock* heldLock = nullptr;
			inline static thread_local std::uint32_t depth = 0;
		};

		struct WorldReadGuard
		{
			WorldReadGuard(WorldReadGate& gate, RE::bhkWorld* world);
			~WorldReadGuard();
			WorldReadGuard(const WorldReadGuard&) = delete;
			WorldReadGuard& operator=(const WorldReadGuard&) = delete;

		private:
			WorldReadGate& gate;
		};

		struct ThreadEvent
		{
			const void* thread;
			std::uint32_t type;
		};

		struct ThreadMemory
		{
			void Send(std::uint32_t type) const;
			~ThreadMemory();

			bool started = false;
		};

		static ThreadMemory& GetThreadMemory()
		{
			static thread_local ThreadMemory memory;
			return memory;
		}

		// Share snapshots across placement threads to avoid repeated model walks under the renderer lock.
		struct ShapeCache
		{
			std::mutex mutex;
			std::unordered_map<std::uint32_t, std::shared_ptr<const Util::ShapeSnapshot>> snapshots;
			std::chrono::steady_clock::time_point snapshotsTaken = std::chrono::steady_clock::now();
		};

		static void EnterWriteLock(RE::BSReadWriteLock* lock);

		struct alignas(16) BoxInfo
		{
			std::uint32_t material = 0;
			float radius = 0.0f;
			std::uint64_t padding = 0;
			RE::hkVector4 halfExtents;
		};
		static_assert(offsetof(BoxInfo, halfExtents) == 0x10);
		static_assert(sizeof(BoxInfo) == 0x20);

		struct HavokDelete
		{
			void operator()(RE::hkReferencedObject* object) const;
		};

		class RayCollector final : public RE::hkpRayHitCollector
		{
		public:
			RayCollector(const RaycastHelper& settings, bool ignoreCliff);

			void AddRayHit(const RE::hkpCdBody& body, const RE::hkpShapeRayCastCollectorOutput& output) override;

			std::vector<HitResult> hits;
			bool invalid = false;

		private:
			const RaycastHelper& settings;
			bool ignoreCliff;
		};

		// Implements hkpCdBodyPairCollector ABI: virtual slots 0-2 and earlyOut at +8.
		class BodyCollector final
		{
		public:
			BodyCollector(const RaycastHelper& settings, bool ignoreCliff, const RE::hkpCdBody* queryBody);

			virtual ~BodyCollector();

			virtual void addCdBodyPair(const RE::hkpCdBody& a, const RE::hkpCdBody& b);

			virtual void Reset();

		private:
			bool earlyOut = false;
			char padding[7]{};

		public:
			std::vector<HitResult> hits;
			bool invalid = false;

		private:
			const RaycastHelper& settings;
			bool ignoreCliff;
			const RE::hkpCdBody* queryBody;
		};

		struct QueryResources
		{
			void Reset();

			void SetShape(int mode, float halfWidthX, float halfWidthY, float radius, float height);

			bool PrepareShape(const RaycastHelper& settings, float halfWidthX, float halfWidthY, float radius, float height);

			// Must remain unattached so dimensions can change without updating broadphase pairs.
			std::unique_ptr<RE::hkpSimpleShapePhantom, HavokDelete> shapePhantom;
			RE::NiPointer<RE::bhkShape> shape;
			int shapeMode = -1;
			RE::FormID errorCell = 0;
			unsigned errors = 0;
		};

		PhysicsRaycast() = default;

		void RetryPendingCells();

		bool CellHasCollision(const RE::TESObjectCELL& cell) const;

		static const RE::hkpCdBody* RootBody(const RE::hkpCdBody& body);

		RayResult Query(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, const RaycastHelper& settings, bool ignoreCliff, RE::GrassParam* param, bool volumeQuery);

		RayResult CastInWorld(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL& cell, const RaycastHelper& settings, bool ignoreCliff, RE::GrassParam* param, bool volumeQuery);

		static bool CopyHit(const RE::hkpCdBody& body, const RaycastHelper& settings, bool ignoreCliff, HitResult& hit, bool& invalid);

		static RE::hkpSimpleShapePhantom* AllocatePhantom();

		WorldReadGate worldReadGate;

		// Exclusive access drains active queries before loading can replace their world.
		std::shared_mutex queryMutex;
		inline static thread_local QueryResources resources;
		inline static thread_local std::uint32_t unavailableQueries = 0;
		std::mutex pendingMutex;
		std::unordered_set<RE::FormID> pendingCells;
		std::chrono::steady_clock::time_point lastRetry{};
		std::atomic<bool> queriesDisabled{ true };
		std::atomic<bool> warningPending{ false };
		std::array<std::atomic<std::uint64_t>, 4> queryCounts{};
		std::atomic<std::uint64_t> queriesWithHits{ 0 };
		std::atomic<const char*> lastUnavailableReason{ "none" };
		ShapeCache shapeCache;
	};
}
