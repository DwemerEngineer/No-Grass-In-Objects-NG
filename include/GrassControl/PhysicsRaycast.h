#pragma once

#include <RE/B/BSPointerHandle.h>
#include <RE/B/bhkShape.h>
#include <RE/H/hkVector4.h>
#include <RE/H/hkpRayHitCollector.h>
#include <RE/H/hkpSimpleShapePhantom.h>
#include <RE/N/NiPoint3.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace RE
{
	struct GrassParam;

	class TESObjectCELL;
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
		};

		struct ShapeNames
		{
			std::string shape;
			std::string root;
		};

		static PhysicsRaycast& GetSingleton();

		RayResult CastRay(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, const RaycastHelper& settings, bool ignoreCliff = false);

		RayResult CastVolume(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, RE::GrassParam* param, const RaycastHelper& settings, bool ignoreCliff = false);

		std::optional<ShapeNames> GetShapeNames(RE::ObjectRefHandle reference, RE::NiPoint3 position);

		static bool ValidateSettings(const RaycastHelper& settings);

		void SetQueriesDisabled(bool disabled);

		void ReportAllocationFailure();

		// DebugMessageBox must be called on the game thread.
		void Update();

	private:
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
		// CommonLib only forward-declares the interface.
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

			bool PrepareShape(const RaycastHelper& settings, float halfWidthX, float halfWidthY, float radius, float height);

			// Must remain unattached so dimensions can change without updating broadphase pairs.
			std::unique_ptr<RE::hkpSimpleShapePhantom, HavokDelete> shapePhantom;
			RE::NiPointer<RE::bhkShape> shape;
			int shapeMode = -1;
		};

		PhysicsRaycast() = default;

		RayResult Query(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, const RaycastHelper& settings, bool ignoreCliff, RE::GrassParam* param, bool volumeQuery);

		RayResult CastInWorld(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL& cell, const RaycastHelper& settings, bool ignoreCliff, RE::GrassParam* param, bool volumeQuery);

		static bool CopyHit(const RE::hkpCdBody& body, const RaycastHelper& settings, bool ignoreCliff, HitResult& hit, bool& invalid);

		static RE::hkpSimpleShapePhantom* AllocatePhantom();

		std::mutex queryMutex;
		QueryResources resources;
		bool queriesDisabled = true;
		RE::FormID errorCell = 0;
		unsigned errors = 0;
		bool warningPending = false;
	};
}
