#pragma once

namespace GrassControl::DistantGrass
{
	class CellHolder;

	class GrassWorkers
	{
	public:
		inline static constexpr std::size_t ThreadCount = 4;

		~GrassWorkers();

		// Serialize builds that share engine grass shapes
		std::int64_t AcquireBlock(const RE::TESObjectCELL* cell);

		// The block key remains valid even if cancellation unloads the cell
		void ReleaseBlock(std::int64_t block);

		// Releases the holder on success and leaves it held on failure
		bool Submit(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort, CellHolder& holder);

	private:
		struct Job
		{
			RE::BGSGrassManager* manager;
			RE::TESObjectCELL* cell;
			bool* abort;
			std::int64_t block;
			int x;
			int y;
			bool reserved = true;  // Unreserved jobs are deferred
		};

		std::int64_t GrassBlockOf(const RE::TESObjectCELL* cell) const;

		// Caller holds mutex
		std::optional<Job> Take();

		bool Dispatch();

		std::optional<Job> Next();
		void Run(const Job& job);
		void DeferUnbuilt(const Job& job);
		void Work();

		std::mutex mutex;
		std::mutex blockMutex;
		std::condition_variable blockReleased;
		std::unordered_set<std::int64_t> busyBlocks;
		std::deque<Job> jobs;
		std::atomic<int> running{ 0 };
		unsigned callbacks = 0;
		bool stopping = false;
		oneapi::tbb::task_arena arena{ static_cast<int>(ThreadCount), 0 };
		oneapi::tbb::task_group_context context{ oneapi::tbb::task_group_context::isolated };
		oneapi::tbb::task_group tasks{ context };
	};

	// Prefetch models to avoid disk access under the grass lock
	class GrassModelPrefetch
	{
	public:
		// Requires the cell's land to remain loaded
		void Request(RE::TESObjectCELL* cell);

		// Grass types retain models still in use after Reset
		void Reset();

	private:
		void Work(std::stop_token stop);

		std::mutex mutex;
		std::condition_variable_any ready;
		std::deque<std::string> jobs;
		std::unordered_set<std::string> requested;
		std::vector<RE::ModelDBHandle::U_Entry*> retainedModels;
		std::uint64_t generation = 0;
		std::jthread thread;  // Destroy the worker before its queue
	};

	// Keep the root below its 16-bit child limit and reduce copies under grassShapeLock
	class GrassNodeShards
	{
	public:
		static void Install();

	private:
		inline static constexpr std::uint16_t ShardCapacity = 8192;

		static RE::NiNode* GrassNode();

		// Caller holds mutex, detached shards must not be reused
		static RE::NiNode* FreeShard(RE::NiNode* root);

		static void AttachChild(RE::NiNode* node, RE::NiAVObject* child, bool firstAvail);

		// Route root-node detaches to the shard holding the shape
		static RE::NiNode* Owner(RE::NiNode* node, RE::NiAVObject* child);

		static void DetachChildOut(RE::NiNode* node, RE::NiAVObject* child, RE::NiPointer<RE::NiAVObject>& out);
		static void DetachChild(RE::NiNode* node, RE::NiAVObject* child);

		inline static std::mutex mutex;
		inline static std::vector<RE::NiPointer<RE::NiNode>> shards;
		inline static REL::Relocation<decltype(&AttachChild)> attachChild;
		inline static REL::Relocation<decltype(&DetachChildOut)> detachChildOut;
		inline static REL::Relocation<decltype(&DetachChild)> detachChild;
	};

	struct RetiredGrass
	{
		std::vector<RE::CellGrassData*> entries;
		std::chrono::steady_clock::time_point deadline;
	};

	inline constexpr auto RetiredGrassTimeout = std::chrono::seconds(3);

	struct CreationState
	{
		// Initialize raycast state first so it outlives the workers
		CreationState();

		std::mutex retiredMutex;
		std::unordered_map<RE::TESObjectCELL*, RetiredGrass> retiredGrass;
		std::atomic<std::uint64_t> swaps{ 0 };
		// Destroy prefetch and workers before retired grass
		GrassWorkers workers;
		GrassModelPrefetch modelPrefetch;
	};

	inline CreationState& GetCreationState()
	{
		static CreationState state;
		return state;
	}

	void OnGrassCreated(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort);

	void QueueGrassCreation(RE::TESObjectCELL* cell);

	// Caller holds grassCreateLock, reattach retired entries before native removal
	void ReattachRetiredGrass(RE::TESObjectCELL* cell);

	// Main thread only, after the engine updates fade alphas
	void RetireReplacedGrass();

	void ReportStatistics();
}
