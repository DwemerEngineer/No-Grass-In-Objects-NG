#pragma once

#include <GrassControl/Config.h>
#include <GrassControl/ObjectHandler.h>
#include <GrassControl/Util.h>
#include <cstdint>
#include <string>
#include <vector>

namespace GrassControl
{
	class RaycastHelper;
}

namespace Raycast
{
	class CdBodyPairCollector
	{
	public:
		struct HitResult
		{
			const RE::hkpCdBody* body;
			RE::TESObjectREFR* hitObject;
			bool hitCliff;
		};

		CdBodyPairCollector();
		virtual ~CdBodyPairCollector() = default;

		virtual void addCdBodyPair(const RE::hkpCdBody& bodyA, const RE::hkpCdBody& bodyB);

		const std::vector<HitResult>& GetHits();

		virtual void Reset();

	private:
		bool earlyOut = false;
		char pad09[7];

		std::vector<HitResult> hits{};

	public:
		bool ignoreCliff = false;
		const GrassControl::RaycastHelper* settingsCache = nullptr;
	};

	class RayCollector : public RE::hkpClosestRayHitCollector
	{
	public:
		struct HitResult
		{
			glm::vec3 normal;
			float hitFraction;
			const RE::hkpCdBody* body;
			bool hitCliff;
		};

		RayCollector();
		~RayCollector() override = default;

		void AddRayHit(const RE::hkpCdBody& body, const RE::hkpShapeRayCastCollectorOutput& hitInfo) override;

		const std::vector<HitResult>& GetHits();

		void Reset();

	private:
		std::vector<HitResult> hits{};

	public:
		bool ignoreCliff = false;
		const GrassControl::RaycastHelper* settingsCache = nullptr;
	};

#pragma warning(push)
#pragma warning(disable: 26495)

	struct RayResult
	{
		// True if the trace hit something before reaching it's end position
		bool hit = false;
		// If the ray hits a havok object, this will point to its reference
		RE::TESObjectREFR* hitObject = nullptr;

		// The position the ray hit, in world space
		glm::vec4 hitPos;
		// A vector of hits to be iterated in original code
		std::vector<RayCollector::HitResult> hitArray{};
		std::vector<CdBodyPairCollector::HitResult> cdBodyHitArray{};

		bool hitCliff = false;
	};

#pragma warning(pop)

	void HandleErrorMessage(std::string_view errorSource);

	// Cast a ray from 'start' to 'end', returning the first thing it hits
	// This variant collides with pretty much any solid geometry
	//	Params:
	//		glm::vec4 start:     Starting position for the trace in world space
	//		glm::vec4 end:       End position for the trace in world space
	//
	// Returns:
	//	RayResult:
	//		A structure holding the results of the ray cast.
	//		If the ray hit something, result.hit will be true.
	RayResult hkpCastRay(const glm::vec4& start, const glm::vec4& end, const GrassControl::RaycastHelper* cache, bool ignoreCliff = false) noexcept;

	RayResult hkpPhantomCast(glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, RE::GrassParam* param, const GrassControl::RaycastHelper* cache, bool ignoreCliff = false) noexcept;

	inline std::shared_ptr<RE::hkpSimpleShapePhantom> phantom = nullptr;

	inline RE::bhkShape* currentShape = nullptr;

	inline bool createdShape = false;

	inline float oldRadius = -1.0f;

	inline int RaycastErrorCount = 0;

	inline bool shownError = false;

	inline RE::TESObjectCELL* lastCell = nullptr;

	inline std::shared_ptr<RE::hkpAabbPhantom> AabbPhantom = nullptr;
}

namespace GrassControl
{
	class RaycastHelper
	{
	public:
		virtual ~RaycastHelper() = default;

		RaycastHelper(int version, float rayHeight, float rayDepth, const std::string& layers, std::unique_ptr<Util::CachedFormList> ignored, std::unique_ptr<Util::CachedFormList> textures, std::unique_ptr<Util::CachedFormList> cliffs, std::unique_ptr<Util::CachedFormList> grassTypes);

		const int Version = 0;

		const float RayHeight = 0.0f;

		const float RayDepth = 0.0f;

		unsigned long long RaycastMask = 0;

		std::unique_ptr<Util::CachedFormList> const Ignore;

		std::unique_ptr<Util::CachedFormList> const Textures;

		std::unique_ptr<Util::CachedFormList> const Cliffs;

		std::unique_ptr<Util::CachedFormList> const Grasses;

		std::unordered_map<RE::FormID, CliffObject> cliffObjects;

		std::unordered_map<RE::FormID, PartIgnoredObject> partIgnoredObjects;

		Raycast::RayCollector* GetRayCollector() const;

		Raycast::CdBodyPairCollector* GetBodyPairCollector() const;

		mutable volatile int64_t lastPhantomTestTime = 0;
		mutable volatile int64_t lastRaycastTime = 0;

		bool CanPlaceGrass(RE::TESObjectLAND* land, float x, float y, float z, RE::GrassParam* param, bool& hitCliff, bool& falseCliff) const;
		float CreateGrassCliff(float x, float y, float z, glm::vec3& Normal, RE::GrassParam* param, bool& falseCliff) const;

	private:
		bool IsCliffObject(const Raycast::RayResult& r) const;
	};
}
