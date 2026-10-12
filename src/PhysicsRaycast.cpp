#include "GrassControl/PhysicsRaycast.h"
#include "GrassControl/RaycastHelper.h"

#include <cmath>
#include <memory>
#include <mutex>
#include <new>
#include <optional>

namespace
{
	inline constexpr float HavokScale = 0.0142875f;
	inline constexpr double MaxDimension = 65536.0;

	inline bool ValidDimension(double value) { return std::isfinite(value) && value > 0.001 && value <= MaxDimension; }

	inline bool ValidSettingsValues(int mode, double height, double depth, double width, double multiplier, double textureWidth)
	{
		return mode >= 0 && mode <= 2 && std::isfinite(height) && height >= 0 && height <= MaxDimension &&
		       std::isfinite(depth) && depth >= 0 && depth <= MaxDimension && ValidDimension(height + depth) &&
		       std::isfinite(width) && width >= 0 && width <= MaxDimension &&
		       std::isfinite(multiplier) && multiplier > 0 && multiplier <= 100 &&
		       std::isfinite(textureWidth) && textureWidth >= 0 && textureWidth <= MaxDimension;
	}

	inline bool ValidPoint(float x, float y, float z)
	{
		return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
		       std::abs(x) <= 100000000.0f && std::abs(y) <= 100000000.0f && std::abs(z) <= 100000000.0f;
	}

	inline bool ValidHit(float fraction, float x, float y, float z)
	{
		const double lengthSquared = double(x) * x + double(y) * y + double(z) * z;

		return std::isfinite(fraction) && fraction >= 0 && fraction <= 1 &&
		       std::isfinite(lengthSquared) && lengthSquared > 0.000001 && lengthSquared < 4.0;
	}

	inline void SetQueryShape(RE::bhkShape* wrapper, int mode, float halfWidthX, float halfWidthY, float radius, float height)
	{
		if (mode == 1) {
			auto* capsule = static_cast<RE::hkpCapsuleShape*>(wrapper->referencedObject.get());
			const float scaledRadius = radius * HavokScale;
			capsule->radius = scaledRadius;

			// Havok caches the radius in both endpoint W components.
			capsule->vertexA = RE::hkVector4(0.0f, 0.0f, 0.0f, scaledRadius);
			capsule->vertexB = RE::hkVector4(0.0f, 0.0f, height * HavokScale, scaledRadius);
		} else {
			auto* box = static_cast<RE::hkpBoxShape*>(wrapper->referencedObject.get());
			const float halfExtentX = halfWidthX * HavokScale;
			const float halfExtentY = halfWidthY * HavokScale;
			const float halfExtentZ = std::abs(height) * HavokScale / 2.0f;

			// Havok caches the smallest half extent in W.
			box->halfExtents = RE::hkVector4(halfExtentX, halfExtentY, halfExtentZ, std::min({ halfExtentX, halfExtentY, halfExtentZ }));
		}
	}
}

namespace
{
	using namespace GrassControl;
	using namespace GrassControl::Raycast;

	constexpr std::size_t MaxHits = 4096;

	// The deleting destructor returns this storage to the current Havok heap.
	RE::hkpSimpleShapePhantom* AllocatePhantom()
	{
		auto* router = RE::hkMemoryRouter::GetInstancePtr();
		return router && router->Heap ? static_cast<RE::hkpSimpleShapePhantom*>(router->Heap->BlockAlloc(sizeof(RE::hkpSimpleShapePhantom))) : nullptr;
	}

	bool CopyHit(const RE::hkpCdBody& body, const RaycastHelper& settings, bool ignoreCliff, HitResult& hit, bool& invalid)
	{
		const auto* root = &body;
		std::size_t depth = 0;

		while (root->parent) {
			if (++depth > 64) {
				invalid = true;
				return false;
			}

			root = root->parent;
		}

		const auto* collidable = static_cast<const RE::hkpCollidable*>(root);

		// The remaining bits encode part/flags/system group, not a layer.
		const auto layer = collidable->broadPhaseHandle.collisionFilterInfo.filter & 0x7F;
		if (layer >= 64 || !(settings.RaycastMask & (1ULL << layer)))
			return false;

		RE::NiPointer<RE::TESObjectREFR> reference(RE::TESHavokUtilities::FindCollidableRef(*collidable));
		if (reference) {
			if (const auto* form = reference->GetBaseObject())
				hit.formID = form->formID;

			hit.reference = reference->GetHandle();
		}

		if (settings.Ignore && settings.Ignore->Contains(hit.formID))
			return false;

		hit.hitCliff = (settings.Cliffs && settings.Cliffs->Contains(hit.formID)) || settings.cliffObjects.contains(hit.formID);
		if (ignoreCliff && hit.hitCliff)
			return false;

		return true;
	}

	std::mutex queryMutex;
	QueryResources resources;
	bool suspended = true;
	RE::FormID errorCell = 0;
	unsigned errors = 0;
	bool warningPending = false;

	bool ValidRay(const glm::vec4& start, const glm::vec4& end)
	{
		const auto difference = end - start;
		return ValidPoint(start.x, start.y, start.z) && ValidPoint(end.x, end.y, end.z) &&
		       ValidDimension(glm::length(glm::vec3(difference)));
	}

	RayResult Cast(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL& cell, const RaycastHelper& settings, bool ignoreCliff, RE::GrassParam* param, bool volumeQuery)
	{
		if (suspended || !cell.IsAttached() || cell.IsInteriorCell())
			return {};

		// queryMutex prevents loading suspension while the borrowed world wrapper is retained.
		RE::NiPointer<RE::bhkWorld> owner(cell.GetbhkWorld());
		if (!owner)
			return {};

		const auto* router = RE::hkMemoryRouter::GetInstancePtr();
		if (!router || !router->Heap)
			return {};

		RE::BSReadLockGuard worldLock(owner->worldLock);

		RE::hkRefPtr<RE::hkpWorld> world(owner->GetWorld1());
		if (!world || !cell.IsAttached() || cell.GetbhkWorld() != owner.get())
			return {};

		const auto havokStart = start * HavokScale;
		const auto havokEnd = end * HavokScale;

		if (volumeQuery) {
			if (!world->collisionInput)
				return {};

			float halfWidthX = 20, halfWidthY = 20, radius = 20;
			if (param) {
				if (auto* grass = RE::TESForm::LookupByID<RE::TESGrass>(param->grassFormID)) {
					const auto& bounds = grass->boundData;
					if (bounds.boundMax.x != 0 && bounds.boundMin.x != 0) {
						halfWidthX = std::abs(float(bounds.boundMax.x - bounds.boundMin.x)) / 2;
						halfWidthY = std::abs(float(bounds.boundMax.y - bounds.boundMin.y)) / 2;
						radius = std::max(halfWidthX, halfWidthY) / 2;
					}

					radius *= static_cast<float>(settings.WidthMultiplier);
				}
			}

			if (settings.Width > 0)
				halfWidthX = halfWidthY = radius = static_cast<float>(settings.Width / 2);

			const float height = end.z - start.z;
			if (!ValidDimension(height) ||
				(settings.Mode == 1 ? !ValidDimension(radius) : (!ValidDimension(halfWidthX) || !ValidDimension(halfWidthY))))
				return { QueryStatus::InvalidInput, {} };

			if (!resources.PrepareShape(settings, halfWidthX, halfWidthY, radius, height))
				return { QueryStatus::Failed, {} };

			resources.shapePhantom->SetPosition(RE::hkVector4(havokStart.x, havokStart.y, settings.Mode == 1 ? havokStart.z : (havokStart.z + havokEnd.z) / 2, 1));

			BodyCollector collector(settings, ignoreCliff, resources.shapePhantom->GetCollidable());

			REL::Relocation<void (*)(const RE::hkpWorld*, const RE::hkpCollidable*, const RE::hkpCollisionInput&, RE::hkpCdBodyPairCollector&)> getPenetrations{ RELOCATION_ID(60558, 61406) };
			getPenetrations(world.get(), resources.shapePhantom->GetCollidable(), *world->collisionInput, *reinterpret_cast<RE::hkpCdBodyPairCollector*>(&collector));
			if (collector.invalid)
				return { QueryStatus::Failed, {} };

			return { QueryStatus::Success, std::move(collector.hits) };
		}

		RayCollector collector(settings, ignoreCliff);

		RE::hkpWorldRayCastInput input{};
		input.from = RE::hkVector4(havokStart.x, havokStart.y, havokStart.z, 1);
		input.to = RE::hkVector4(havokEnd.x, havokEnd.y, havokEnd.z, 1);

		// Use the collector overload so ignored hits cannot hide other blockers.
		REL::Relocation<void (*)(const RE::hkpWorld*, const RE::hkpWorldRayCastInput&, RE::hkpRayHitCollector&)> cast{ RELOCATION_ID(60552, 61400) };
		cast(world.get(), input, collector);
		if (collector.invalid)
			return { QueryStatus::Failed, {} };

		return { QueryStatus::Success, std::move(collector.hits) };
	}

	RayResult Query(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, const RaycastHelper& settings, bool ignoreCliff, RE::GrassParam* param, bool volumeQuery)
	{
		if (!settings.Valid || !ValidRay(start, end) || (volumeQuery && settings.Mode == 0))
			return { QueryStatus::InvalidInput, {} };

		if (!cell)
			return {};

		std::scoped_lock lock(queryMutex);

		if (errorCell != cell->formID) {
			errorCell = cell->formID;
			errors = 0;
		}

		RayResult result;
		try {
			result = Cast(start, end, *cell, settings, ignoreCliff, param, volumeQuery);
		} catch (const std::bad_alloc&) {
			result = { QueryStatus::Failed, {} };
			resources.Reset();
		}

		if (result.status == QueryStatus::Failed || result.status == QueryStatus::InvalidInput) {
			if (errors < 20) {
				++errors;
				logger::warn("Raycast query failed in cell 0x{:x}, status {}", cell->formID, static_cast<int>(result.status));
				if (errors == 20)
					warningPending = true;
			}
		}

		return result;
	}
}

namespace GrassControl::Raycast
{
	void HavokDelete::operator()(RE::hkReferencedObject* object) const
	{
		if (object)
			object->RemoveReference();
	}

	RayCollector::RayCollector(const RaycastHelper& settings, bool ignoreCliff) : settings(settings), ignoreCliff(ignoreCliff) { earlyOutHitFraction = 1.0f; }

	void RayCollector::AddRayHit(const RE::hkpCdBody& body, const RE::hkpShapeRayCastCollectorOutput& output)
	{
		if (invalid)
			return;

		HitResult hit{};
		hit.hitFraction = output.hitFraction;
		hit.normal = { output.normal.quad.m128_f32[0], output.normal.quad.m128_f32[1], output.normal.quad.m128_f32[2] };

		if (!ValidHit(hit.hitFraction, hit.normal.x, hit.normal.y, hit.normal.z)) {
			invalid = true;
			return;
		}

		hit.normal = glm::normalize(hit.normal);

		if (CopyHit(body, settings, ignoreCliff, hit, invalid)) {
			if (hits.size() >= MaxHits) {
				invalid = true;
				return;
			}

			try {
				hits.push_back(std::move(hit));
			} catch (const std::bad_alloc&) {
				// Never unwind a plugin allocation failure through Havok callbacks.
				invalid = true;
				earlyOutHitFraction = 0.0f;
			}
		}
	}

	BodyCollector::BodyCollector(const RaycastHelper& settings, bool ignoreCliff, const RE::hkpCdBody* queryBody) :
		settings(settings), ignoreCliff(ignoreCliff), queryBody(queryBody)
	{}

	BodyCollector::~BodyCollector() = default;

	void BodyCollector::addCdBodyPair(const RE::hkpCdBody& a, const RE::hkpCdBody& b)
	{
		if (invalid)
			return;

		HitResult hit{};
		if (CopyHit(&a == queryBody ? b : a, settings, ignoreCliff, hit, invalid)) {
			if (hits.size() >= MaxHits) {
				invalid = true;
				return;
			}

			try {
				hits.push_back(std::move(hit));
			} catch (const std::bad_alloc&) {
				invalid = true;
				earlyOut = true;
			}
		}
	}

	void BodyCollector::Reset()
	{
		earlyOut = false;
		hits.clear();
		invalid = false;
	}

	void QueryResources::Reset()
	{
		shapePhantom.reset();
		shape.reset();
		shapeMode = -1;
	}

	bool QueryResources::PrepareShape(const RaycastHelper& settings, float halfWidthX, float halfWidthY, float radius, float height)
	{
		if (shapeMode != settings.Mode)
			Reset();

		if (!shape) {
			auto* storage = RE::malloc<RE::bhkShape>(0x28);
			if (!storage)
				return false;

			RE::bhkShape* constructed = nullptr;
			if (settings.Mode == 1) {
				RE::NiPoint3 bottom(0, 0, 0);
				RE::NiPoint3 top(0, 0, height);

				REL::Relocation<RE::bhkShape* (*)(RE::bhkShape*, RE::NiPoint3&, RE::NiPoint3&, float)> constructCapsule{ RELOCATION_ID(76924, 78799) };
				constructed = constructCapsule(storage, bottom, top, radius);
			} else {
				RE::NiPoint3 extents(halfWidthX, halfWidthY, height / 2);

				REL::Relocation<RE::bhkShape* (*)(RE::bhkShape*, RE::NiPoint3*)> constructBox{ RELOCATION_ID(38677, 78820) };
				constructed = constructBox(storage, &extents);

				if (constructed && REL::Module::IsAE()) {
					BoxInfo info{};
					info.halfExtents = RE::hkVector4(halfWidthX * HavokScale, halfWidthY * HavokScale, height * HavokScale / 2, 0);
					REL::Relocation<void (*)(RE::bhkShape*, const BoxInfo*)> initializeBox{ RELOCATION_ID(76940, 78815) };
					initializeBox(constructed, &info);
				}
			}

			if (!constructed) {
				RE::free(storage);
				return false;
			}

			shape.reset(constructed);
			shapeMode = settings.Mode;
		}

		if (!shape->referencedObject)
			return false;

		SetQueryShape(shape.get(), settings.Mode, halfWidthX, halfWidthY, radius, height);

		if (!shapePhantom) {
			auto* storage = AllocatePhantom();
			if (!storage)
				return false;

			RE::hkTransform identityTransform{};
			identityTransform.rotation.col0 = RE::hkVector4(1, 0, 0, 0);
			identityTransform.rotation.col1 = RE::hkVector4(0, 1, 0, 0);
			identityTransform.rotation.col2 = RE::hkVector4(0, 0, 1, 0);
			identityTransform.translation = RE::hkVector4(0, 0, 0, 1);

			REL::Relocation<RE::hkpSimpleShapePhantom* (*)(RE::hkpShapePhantom*, RE::hkpShape*, const RE::hkTransform&, std::uint32_t)> constructPhantom{ RELOCATION_ID(60675, 61535) };
			auto* constructed = constructPhantom(storage, static_cast<RE::hkpShape*>(shape->referencedObject.get()), identityTransform, 0);
			if (!constructed) {
				RE::hkMemoryRouter::GetInstance().Heap->BlockFree(storage, sizeof(RE::hkpSimpleShapePhantom));
				return false;
			}

			shapePhantom.reset(constructed);
		}

		return true;
	}

	RayResult hkpCastRay(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, const RaycastHelper& settings, bool ignoreCliff)
	{
		return Query(start, end, cell, settings, ignoreCliff, nullptr, false);
	}

	RayResult hkpPhantomCast(const glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, RE::GrassParam* param, const RaycastHelper& settings, bool ignoreCliff)
	{
		return Query(start, end, cell, settings, ignoreCliff, param, true);
	}

	bool ValidateSettings(const RaycastHelper& settings)
	{
		return ValidSettingsValues(settings.Mode, settings.RayHeight, settings.RayDepth, settings.Width, settings.WidthMultiplier, settings.TextureWidth);
	}

	void SetSuspended(bool value)
	{
		std::scoped_lock lock(queryMutex);

		suspended = value;
		resources.Reset();
		errors = 0;
		warningPending = false;
	}

	void ReportAllocationFailure()
	{
		std::scoped_lock lock(queryMutex);

		warningPending = true;
	}

	void Update()
	{
		bool warn = false;

		{
			std::scoped_lock lock(queryMutex);

			warn = std::exchange(warningPending, false);
		}

		if (warn && Config::RayCastError)
			RE::DebugMessageBox("NGIO could not complete repeated physics queries. Grass placement used its fallback behavior. See the NGIO log for the affected cell and query status.");
	}

	std::optional<ShapeNames> GetShapeNames(RE::ObjectRefHandle handle, RE::NiPoint3 position)
	{
		std::scoped_lock lock(queryMutex);

		if (suspended)
			return std::nullopt;

		const auto reference = handle.get();
		if (!reference)
			return std::nullopt;

		const auto* cell = reference->GetParentCell();
		if (!cell || !cell->IsAttached())
			return std::nullopt;

		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return std::nullopt;

		renderer->Lock();
		const SKSE::stl::scope_exit rendererLock([renderer] { renderer->Unlock(); });

		RE::NiPointer<RE::NiAVObject> root(reference->Get3D());
		if (!root)
			return std::nullopt;

		if (auto shape = Util::ClosestShape(root.get(), position))
			return ShapeNames{ std::move(*shape), root->name.c_str() };

		return std::nullopt;
	}
}
