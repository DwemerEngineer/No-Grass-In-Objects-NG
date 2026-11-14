#pragma once

#include "GrassControl/Util.h"

namespace GrassControl::CollisionGrid
{
	struct Bounds
	{
		RE::NiPoint3 min{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		RE::NiPoint3 max{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };

		void Add(const RE::NiPoint3& point);
		[[nodiscard]] bool Valid() const { return min.x <= max.x; }
		[[nodiscard]] Bounds Transformed(const RE::NiTransform& transform) const;
	};

	struct Model
	{
		struct Body
		{
			const RE::hkpShape* shape = nullptr;
			RE::NiTransform toModel;
			bool hasOffset = false;
			RE::NiMatrix3 offsetRotation;
			RE::NiPoint3 offsetTranslation;
			std::uint32_t layer = 0;
			Bounds bounds;
		};

		struct Surface
		{
			std::string shape;
			std::string texture;
			RE::NiPoint3 center;
			float radius = 0.0f;
			std::uint32_t index = 0;
		};

		std::string rootName;
		std::vector<Body> bodies;
		RE::NiPointer<RE::NiNode> root;

		const Util::ShapeSnapshot& Shapes() const;
		const std::vector<Surface>& Surfaces() const;

	private:
		mutable std::once_flag shapesOnce;
		mutable Util::ShapeSnapshot shapes;
		mutable std::once_flag surfacesOnce;
		mutable std::vector<Surface> surfaces;
	};

	class ModelCache
	{
	public:
		static ModelCache& Get()
		{
			static ModelCache instance;
			return instance;
		}

		std::shared_ptr<const Model> Load(std::string_view requestedPath);

	private:
		inline static constexpr std::size_t Capacity = 2048;

		struct Entry
		{
			std::shared_ptr<const Model> model;
			std::list<std::string>::iterator order;
		};

		std::shared_ptr<const Model> LoadModel(const std::string& path) const;
		void TrimUnusedModels();

		std::mutex mutex;
		std::unordered_map<std::string, Entry> entries;
		std::list<std::string> useOrder;
		std::unordered_map<std::string, std::shared_future<std::shared_ptr<const Model>>> pendingLoads;
		mutable std::atomic<std::uint32_t> loadFailureCount{ 0 };
	};

	struct Placement
	{
		RE::FormID reference = 0;
		RE::FormID base = 0;
		RE::NiPoint3 position;
		RE::NiPoint3 angle;
		float scale = 1.0f;
	};

	using Placements = std::vector<Placement>;

	namespace PlacementReader
	{
		// Returns std::nullopt until persistent references are available
		std::optional<Placements> Read(RE::TESWorldSpace* world, int x, int y);

		void Clear();
	}
}
