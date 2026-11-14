#pragma once

#include "GrassControl/CollisionSources.h"

namespace GrassControl::CollisionGrid
{
	struct Instance
	{
		std::shared_ptr<const Model> model;
		const Model::Body* body = nullptr;
		RE::FormID base = 0;
		RE::NiTransform toWorld;
		RE::NiTransform toLocal;
		// Render shapes use model space rather than body space
		RE::NiTransform toModel;
		Bounds bounds;
	};

	struct Object
	{
		std::shared_ptr<const Model> model;
		RE::FormID base = 0;
		RE::NiTransform transform;
	};

	struct Hit
	{
		const Instance* instance;
		float fraction;
		RE::NiPoint3 normal;
	};

	class CellGeometry
	{
	public:
		inline static constexpr float CellSize = 4096.0f;
		inline static constexpr float Margin = 256.0f;

		CellGeometry(int x, int y, std::vector<Instance> instances, std::vector<Object> objects);

		// Returns false when the thread has no Havok heap
		bool CastRay(const RE::NiPoint3& start, const RE::NiPoint3& end, std::uint64_t layerMask, std::vector<Hit>& hits) const;

		// Approximate the footprint with parallel rays and keep the nearest hit per instance
		bool CastVolume(const RE::NiPoint3& start, const RE::NiPoint3& end, float halfWidthX, float halfWidthY, std::uint64_t layerMask, std::vector<Hit>& hits) const;

		const std::vector<Object>& Objects() const { return objects; }

	private:
		inline static constexpr int BinCount = 36;
		inline static constexpr float BinSize = (CellSize + 2 * Margin) / BinCount;

		std::optional<Hit> Cast(const Instance& instance, const RE::NiPoint3& start, const RE::NiPoint3& end) const;

		template <class F>
		void ForCandidates(const RE::NiPoint3& start, const RE::NiPoint3& end, F&& visit) const;

		RE::NiPoint3 origin;
		std::vector<Instance> instances;
		std::vector<Object> objects;
		std::vector<std::vector<std::uint32_t>> bins;
	};

	class Grid
	{
	public:
		static Grid& Get()
		{
			static Grid instance;
			return instance;
		}

		void Start(int size);
		bool Running() const { return !workers.empty(); }

		void Follow(RE::FormID world, int x, int y);
		std::shared_ptr<const CellGeometry> Find(RE::FormID world, int x, int y) const;
		std::vector<std::pair<int, int>> TakeReady(RE::FormID world);
		void Clear();

	private:
		inline static constexpr unsigned LoaderThreads = 4;
		inline static constexpr std::size_t PlacementCapacity = 2048;
		inline static constexpr auto RetryDelay = std::chrono::milliseconds(100);

		struct Key
		{
			RE::FormID world;
			int x;
			int y;

			bool operator==(const Key&) const = default;
		};

		struct KeyHash
		{
			std::size_t operator()(const Key& key) const noexcept
			{
				const auto world = std::uint64_t(key.world) << 32;
				const auto x = std::uint64_t(std::uint16_t(key.x)) << 16;
				const auto y = std::uint16_t(key.y);
				return std::hash<std::uint64_t>()(world | x | y);
			}
		};

		struct Queued
		{
			Key key;
			std::chrono::steady_clock::time_point notBefore;
		};

		struct LoadedCell
		{
			std::shared_ptr<const CellGeometry> geometry;
			std::list<Key>::iterator order;
		};

		// Caller holds mutex
		bool InGrid(const Key& key) const;
		std::optional<Key> NextCell(std::stop_token stop);
		void LoadCells(std::stop_token stop);
		std::shared_ptr<const Placements> GetPlacements(RE::TESWorldSpace* world, const Key& key);
		std::shared_ptr<const CellGeometry> BuildCell(const Key& key);

		mutable std::mutex mutex;
		std::condition_variable_any wake;

		std::deque<Queued> loadQueue;
		std::unordered_set<Key, KeyHash> pendingCells;
		std::unordered_map<Key, LoadedCell, KeyHash> loadedCells;
		std::list<Key> loadedOrder;
		std::vector<Key> completedCells;

		std::size_t cellCapacity = 512;
		int radius = 0;
		RE::FormID centerWorld = 0;
		int centerX = 0;
		int centerY = 0;

		std::mutex placementMutex;
		std::unordered_map<Key, std::shared_ptr<const Placements>, KeyHash> placements;
		std::deque<Key> placementOrder;

		std::atomic<std::uint64_t> generation{ 0 };
		std::vector<std::jthread> workers;  // Stop loaders before destroying their state
	};

	// Call after game data loads
	void Start();

	// Main thread only
	std::vector<std::pair<int, int>> Update();

	void Clear();

	std::shared_ptr<const CellGeometry> Find(RE::TESObjectCELL& cell);
}
