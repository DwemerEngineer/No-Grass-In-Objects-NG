#pragma once

namespace GrassControl::DistantGrass
{
	class GrassWorkers
	{
	public:
		inline static constexpr std::size_t ThreadCount = 4;

		static GrassWorkers& Get();
		~GrassWorkers();

		// Cells sharing engine grass shapes must not build concurrently.
		void AcquireBlock(const RE::TESObjectCELL* cell);

		void ReleaseBlock(const RE::TESObjectCELL* cell);

		// Retains the native task so its abort flag remains valid until the job finishes.
		bool Submit(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort);

		[[nodiscard]] bool Busy();

	private:
		struct Job
		{
			RE::BGSGrassManager* manager;
			RE::TESObjectCELL* cell;
			bool* abort;
			std::int64_t block;
			int x;
			int y;
		};

		std::int64_t GrassBlockOf(const RE::TESObjectCELL* cell) const;

		// Release under the queue lock to prevent missed work when every block was busy.
		void ReleaseBlock(std::int64_t block);

		// Caller holds mutex; reserves the nearest available block.
		std::optional<Job> Take();

		bool Dispatch();
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

	// Prefetch models to reduce serial model and texture IO inside LoadGrassType's grass manager lock.
	// Model streams require serial loads under the grass manager lock.
	class GrassModelPrefetch
	{
	public:
		static GrassModelPrefetch& Get();

		// The cell's land must remain loaded during this call.
		void Request(RE::TESObjectCELL* cell);

		// Grass types retain models still in use after prefetch references are released.
		void Reset();

	private:
		void Work(std::stop_token stop);

		std::mutex mutex;
		std::condition_variable_any ready;
		std::deque<std::string> jobs;
		std::unordered_set<std::string> requested;
		std::vector<RE::NiPointer<RE::NiNode>> loaded;
		std::uint64_t generation = 0;
		std::jthread thread;  // Declared last so the worker stops before the queue it reads.
	};

	// Shards keep the root below its 16-bit child limit and avoid repeated child-array copies under grassShapeLock.
	class GrassNodeShards
	{
	public:
		static void Install();

	private:
		inline static constexpr std::uint16_t ShardCapacity = 8192;

		static RE::NiNode* GrassNode();

		// Caller holds mutex; detached shards must not be reused.
		static RE::NiNode* FreeShard(RE::NiNode* root);

		static void AttachChild(RE::NiNode* node, RE::NiAVObject* child, bool firstAvail);

		// Engine code detaches grass shapes from the root node; forward those to the shard that holds them.
		static RE::NiNode* Owner(RE::NiNode* node, RE::NiAVObject* child);

		static void DetachChildOut(RE::NiNode* node, RE::NiAVObject* child, RE::NiPointer<RE::NiAVObject>& out);
		static void DetachChild(RE::NiNode* node, RE::NiAVObject* child);

		inline static std::mutex mutex;
		inline static std::vector<RE::NiPointer<RE::NiNode>> shards;
		inline static REL::Relocation<decltype(&AttachChild)> attachChild;
		inline static REL::Relocation<decltype(&DetachChildOut)> detachChildOut;
		inline static REL::Relocation<decltype(&DetachChild)> detachChild;
	};

	void OnGrassCreated(RE::BGSGrassManager* manager, RE::TESObjectCELL* cell, bool* abort);

	void QueueGrassCreation(RE::TESObjectCELL* cell);

	// Caller holds grassCreateLock; reattach before native removal so it frees retired entries too.
	void ReattachRetiredGrass(RE::TESObjectCELL* cell);

	// Main thread only, after the engine advances fade alphas.
	void RetireReplacedGrass();

	void ResetGrassModelPrefetch();

	void ReportStatistics();
}
