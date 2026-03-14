// Adapted from https://github.com/mwilsnd/SkyrimSE-SmoothCam/blob/master/SmoothCam/source/raycast.cpp
#include "GrassControl/RaycastHelper.h"

#include "CasualLibrary.hpp"

Raycast::RayCollector::RayCollector() = default;
Raycast::CdBodyPairCollector::CdBodyPairCollector() = default;

namespace RE
{
	float hkpWorldRayCaster::AddBroadPhaseHandle(const hkpBroadPhaseHandle* a_broadphaseHandle, std::int32_t a_castIndex)
	{
		using func_t = decltype(&hkpWorldRayCaster::AddBroadPhaseHandle);
		static REL::Relocation<func_t> func{ RELOCATION_ID(63651, 64640) };
		return func(this, a_broadphaseHandle, a_castIndex);
	}

	float hkpSimpleWorldRayCaster ::AddBroadPhaseHandle(const hkpBroadPhaseHandle* a_broadphaseHandle, std::int32_t a_castIndex)
	{
		using func_t = decltype(&hkpSimpleWorldRayCaster::AddBroadPhaseHandle);
		static REL::Relocation<func_t> func{ RELOCATION_ID(63647, 64636) };
		return func(this, a_broadphaseHandle, a_castIndex);
	}
}

void Raycast::HandleErrorMessage()
{
	if (RaycastErrorCount < 20) {
		logger::error("Exception occurred while attempting raycasting. Unless repeated within the same cell this unlikely to be a serious issue.");
		RaycastErrorCount++;
	} else if (!shownError) {
		RE::DebugMessageBox("NGIO has encountered more than 20 errors while attempting raycasting in this cell. There is likely an issue with your game, that could result in a crash. The cause is most likely a bad mesh, most likely with broken collision. Try using Sniff to locate whate mesh(es) could be bad. It is also recommended to check the mods that edit this cell for errors in SSEdit. Saving your game is recommended. If you do not experience crashes, this warning can be ignored and disabled in GrassControl.ini using the setting Ray-cast-error-message.");
		logger::error("Raycast error count for this cell exceeded 20. There is likely an issue with your game, that could result in crashes. The cause is most likely a bad mesh, most likely with broken collision. Try using Sniff to locate whate mesh(es) could be bad. It is also recommended to check the mods that edit this cell for errors in SSEdit. If you do not experience crashes, this warning can be ignored and disabled in GrassControl.ini using the setting Ray-cast-error-message.");
		shownError = true;
	}
};

void Raycast::RayCollector::AddRayHit(const RE::hkpCdBody& body, const RE::hkpShapeRayCastCollectorOutput& hitInfo)
{
	HitResult hit{};
	hit.hitFraction = hitInfo.hitFraction;
	hit.normal = {
		hitInfo.normal.quad.m128_f32[0],
		hitInfo.normal.quad.m128_f32[1],
		hitInfo.normal.quad.m128_f32[2]
	};

	const RE::hkpCdBody* obj = &body;
	while (obj) {
		if (!obj->parent)
			break;
		obj = obj->parent;
	}

	hit.body = obj;
	if (!hit.body)
		return;

	const auto collisionObj = static_cast<const RE::hkpCollidable*>(hit.body);
	const auto flags = collisionObj->broadPhaseHandle.collisionFilterInfo;

	const uint64_t mask = 1ULL << flags.filter;
	if (this->settingsCache->RaycastMask & mask) {
		auto rsTESForm = GrassControl::RaycastHelper::GetRaycastHitBaseForm(hit.body);
		if (this->settingsCache->Ignore != nullptr && this->settingsCache->Ignore->Contains(rsTESForm)) {
			if (GrassControl::Config::DebugLogEnable && rsTESForm) {
				auto cell = RE::PlayerCharacter::GetSingleton()->GetParentCell();
				logger::debug("Ignored 0x{:x} in {}", rsTESForm->formID, cell->GetFormEditorID() ? cell->GetFormEditorID() : cell->GetName());
			}

			return;
		}

		earlyOutHitFraction = hit.hitFraction;
		hits.push_back(hit);
	}
}

void Raycast::CdBodyPairCollector::addCdBodyPair(const RE::hkpCdBody& bodyA, const RE::hkpCdBody& bodyB)
{
	HitResult hit{};

	const RE::hkpCdBody* obj = &bodyA;

	//auto bhkPhantom = currentPhantom.load(std::memory_order_acquire);

	//auto hkpPhantom = reinterpret_cast<RE::hkpPhantom*>(bhkPhantom->referencedObject.get());

	if (obj == phantom->GetCollidable()) {
		obj = &bodyB;
	}

	while (obj) {
		if (!obj->parent)
			break;
		obj = obj->parent;
	}

	hit.body = obj;
	if (!hit.body)
		return;

	const auto collisionObj = static_cast<const RE::hkpCollidable*>(hit.body);
	const auto flags = collisionObj->broadPhaseHandle.collisionFilterInfo;

	const uint64_t mask = 1ULL << flags.filter;
	if (this->settingsCache->RaycastMask & mask) {
		hit.hitObject = hit.getAVObject() ? hit.getAVObject()->GetUserData() : nullptr;

		if (this->settingsCache->Ignore != nullptr && this->settingsCache->Ignore->Contains(hit.hitObject)) {
			if (GrassControl::Config::DebugLogEnable && hit.hitObject) {
				auto cell = RE::PlayerCharacter::GetSingleton()->GetParentCell();
				logger::debug("Ignored 0x{:x} in {}", hit.hitObject->formID, cell->GetFormEditorID() ? cell->GetFormEditorID() : cell->GetName());
			}

			return;
		}

		this->earlyOut = true;
		hits.push_back(hit);
	}
}

const std::vector<Raycast::RayCollector::HitResult>& Raycast::RayCollector::GetHits()
{
	return hits;
}

const std::vector<Raycast::CdBodyPairCollector::HitResult>& Raycast::CdBodyPairCollector::GetHits()
{
	return hits;
}

void Raycast::RayCollector::Reset()
{
	earlyOutHitFraction = 1.0f;
	hits.clear();
}

void Raycast::CdBodyPairCollector::Reset()
{
	earlyOut = false;
	hits.clear();
}

RE::NiAVObject* Raycast::RayCollector::HitResult::getAVObject()
{
	typedef RE::NiAVObject* (*_GetUserData)(const RE::hkpCdBody*);
	static auto getAVObject = REL::Relocation<_GetUserData>(RELOCATION_ID(76160, 77988));
	return body ? getAVObject(body) : nullptr;
}

RE::NiAVObject* Raycast::CdBodyPairCollector::HitResult::getAVObject()
{
	typedef RE::NiAVObject* (*_GetUserData)(const RE::hkpCdBody*);
	static auto getAVObject = REL::Relocation<_GetUserData>(RELOCATION_ID(76160, 77988));
	return body ? getAVObject(body) : nullptr;
}

RE::NiAVObject* Raycast::getAVObject(const RE::hkpCdBody* body)
{
	typedef RE::NiAVObject* (*_GetUserData)(const RE::hkpCdBody*);
	static auto getAVObject = REL::Relocation<_GetUserData>(RELOCATION_ID(76160, 77988));
	return body ? getAVObject(body) : nullptr;
}

Raycast::RayResult Raycast::hkpCastRay(const glm::vec4& start, const glm::vec4& end, const GrassControl::RaycastHelper* cache, bool forCliffs) noexcept
{
	constexpr auto hkpScale = 0.0142875f;
	const glm::vec4 dif = end - start;

	constexpr auto one = 1.0f;
	const auto from = start * hkpScale;
	const auto to = end * hkpScale;

	RE::hkpWorldRayCastInput pickRayInput{};
	pickRayInput.from = RE::hkVector4(from.x, from.y, from.z, one);
	pickRayInput.to = RE::hkVector4(to.x, to.y, to.z, one);

	cache->RayCollector->Reset();

	const auto ply = RE::PlayerCharacter::GetSingleton();
	auto cell = ply->GetParentCell();
	if (!cell)
		return {};

	RayCollector::HitResult best{};
	best.hitFraction = 1.0f;
	glm::vec4 bestPos = start;

	RayResult result;

	if (!lastCell) {
		lastCell = cell;
	} else if (lastCell->formID != cell->formID) {
		lastCell = cell;
		RaycastErrorCount = 0;
		shownError = false;
	}

	RE::hkAabb aabb;

	aabb.min = pickRayInput.from;
	aabb.max = pickRayInput.to;

	auto physicsWorld = cell->GetbhkWorld();

	auto hkpWorld = physicsWorld ? physicsWorld->GetWorld1() : nullptr;

	if (!AabbPhantom) {
		AabbPhantom = RE::malloc<RE::hkpAabbPhantom>(0x130);

		using _createhkpAABBPhantom = void (*)(RE::hkpAabbPhantom*, RE::hkAabb*, uint32_t);
		REL::Relocation<_createhkpAABBPhantom> CreatehkpAABBPhantom(RELOCATION_ID(60170, 60938));

		CreatehkpAABBPhantom(AabbPhantom, &aabb, 0);
	}

	if (!AabbPhantom->world) {
		physicsWorld->worldLock.LockForWrite();
		hkpWorld->AddPhantom(AabbPhantom);
		physicsWorld->worldLock.UnlockForWrite();
	}

	try {
		auto physicsWorld = cell->GetbhkWorld();
		if (physicsWorld) {
			physicsWorld->worldLock.LockForWrite();

			using _setAabb = void (*)(RE::hkpAabbPhantom*, RE::hkAabb*);
			REL::Relocation<_setAabb> SetAabb(RELOCATION_ID(60181, 60949));

			SetAabb(AabbPhantom, &aabb);

			physicsWorld->worldLock.UnlockForWrite();

			physicsWorld->worldLock.LockForRead();

			using _RayCastAABB = void (*)(RE::hkpAabbPhantom*, RE::hkpWorldRayCastInput*, RE::hkpRayHitCollector*);
			REL::Relocation<_RayCastAABB> raycastAABBPhantom(RELOCATION_ID(60173, 60941));

			raycastAABBPhantom(AabbPhantom, &pickRayInput, cache->RayCollector.get());

			physicsWorld->worldLock.UnlockForRead();
		}
	} catch (...) {
		HandleErrorMessage();
		return result;
	}

	for (auto& hit : cache->RayCollector->GetHits()) {
		const auto pos = dif * hit.hitFraction + start;
		if (best.body == nullptr) {
			best = hit;
			bestPos = pos;
			continue;
		}

		if (hit.hitFraction < best.hitFraction) {
			best = hit;
		}

		if (pos.z > bestPos.z) {
			bestPos = pos;
		}
	}

	result.hitArray = cache->RayCollector->GetHits();

	result.hitPos = bestPos;

	if (!best.body)
		return result;
	auto av = best.getAVObject();
	result.hit = av != nullptr;

	if (result.hit) {
		result.hitObject = av->GetUserData();
	}

	return result;
}

Raycast::RayResult Raycast::hkpPhantomCast(glm::vec4& start, const glm::vec4& end, RE::TESObjectCELL* cell, RE::GrassParam* param, const GrassControl::RaycastHelper* cache) noexcept
{
	if (!cell)
		return {};

	constexpr auto hkpScale = 0.0142875f;

	constexpr auto one = 1.0f;
	const auto from = start * hkpScale;

	const glm::vec4 dif = end - start;

	auto vecA = RE::hkVector4(from.x, from.y, from.z, one);

	auto bhkWorld = cell->GetbhkWorld();
	if (!bhkWorld)
		return {};

	auto hkWorld = bhkWorld->GetWorld1();
	if (!hkWorld)
		return {};

	RE::hkTransform transform;
	transform.translation = RE::hkVector4(0.0f, 0.0f, 0.0f, 1.0f);

	auto vecBottom = RE::NiPoint3(0.0f, 0.0f, 0.0f);
	auto vecTop = RE::NiPoint3(0.0f, 0.0f, dif.z);

	float radius = 20.0f;
	float widthX = 20.0f;
	float widthY = 20.0f;

	if (param) {
		auto grassForm = RE::TESForm::LookupByID<RE::TESGrass>(param->grassFormID);
		if (grassForm) {
			if (grassForm->boundData.boundMax.x != 0 && grassForm->boundData.boundMin.x != 0) {
				widthX = std::abs(static_cast<float>(grassForm->boundData.boundMax.x - grassForm->boundData.boundMin.x) * 0.5f);
				widthY = std::abs(static_cast<float>(grassForm->boundData.boundMax.y - grassForm->boundData.boundMin.y) * 0.5f);
				radius = std::max(widthX, widthY) * 0.5f;
			}

			radius *= GrassControl::Config::RayCastWidthMult;
		}
	}

	if (GrassControl::Config::RayCastWidth > 0.0f) {
		widthX = GrassControl::Config::RayCastWidth * 0.5f;
		widthY = GrassControl::Config::RayCastWidth * 0.5f;
		radius = GrassControl::Config::RayCastWidth * 0.5f;
	}

	bool newShape = false;
	newShape = std::abs(oldRadius - radius) > 1.0f;
	if (newShape) {
		if (GrassControl::Config::RayCastMode == 1 && currentShape) {
			Memory::Internal::write<RE::hkVector4>(reinterpret_cast<uintptr_t>(currentShape->referencedObject.get()) + 0x28, radius * hkpScale);  // Set Radius
			Memory::Internal::write<RE::hkVector4>(reinterpret_cast<uintptr_t>(currentShape->referencedObject.get()) + 0x40, vecTop);             // Set Top Vertex
		} else if (currentShape) {
			auto halfExtents = RE::hkVector4(widthX, widthY, dif.z * hkpScale / 2.0f, 0.0f);
			Memory::Internal::write<RE::hkVector4>(reinterpret_cast<uintptr_t>(currentShape->referencedObject.get()) + 0x30, halfExtents);  // Set halfExtent
		}

		oldRadius = radius;
	}

	if (!currentShape) {
		currentShape = RE::malloc<RE::bhkShape>(0x28);
		if (!currentShape)
			return {};
	}

	if (!createdShape) {
		if (GrassControl::Config::RayCastMode == 1) {
			using createCapsuleShape_t = RE::bhkShape* (*)(RE::bhkShape*, RE::NiPoint3&, RE::NiPoint3&, float);
			REL::Relocation<createCapsuleShape_t> createCapsuleShape{ RELOCATION_ID(76924, 78799) };

			currentShape = createCapsuleShape(currentShape, vecBottom, vecTop, radius);

		} else {
			float halfExtents[4] = { widthX, widthY, dif.z / 2.0f, 0.0f };

			using createBoxShape_t = RE::bhkShape* (*)(RE::bhkShape*, float*);
			REL::Relocation<createBoxShape_t> createBoxShape{ RELOCATION_ID(76945, 78820) };

			currentShape = createBoxShape(currentShape, halfExtents);
		}

		createdShape = true;
	}

	/*
	auto phantomInstance = currentPhantom.load(std::memory_order_acquire);

	if (!phantomInstance || !phantomInstance->referencedObject) {
		auto shapeParam = new bhkSimpleShapePhantomParam();
		shapeParam->unk10 = 2;
		shapeParam->unk18 = nullptr;
		shapeParam->unk20 = 0;
		shapeParam->unk24 = 0x80000000;
		shapeParam->transform30 = transform;
		shapeParam->HavokObject = currentShape->referencedObject;
		shapeParam->collisionFlags = 0;

		using createSimpleShapePhantom_t = RE::bhkShapePhantom* (*)(RE::bhkShapePhantom*, bhkSimpleShapePhantomParam*);
		REL::Relocation<createSimpleShapePhantom_t> createSimpleShapePhantom{ RELOCATION_ID(37690, 78778) };
		auto rawPhantom = RE::malloc<RE::bhkShapePhantom>(0x30);
		if (!rawPhantom)
			return {};

		rawPhantom = createSimpleShapePhantom(rawPhantom, shapeParam);

		delete shapeParam;

		if (rawPhantom && rawPhantom->referencedObject) {
			rawPhantom->IncRefCount();
			rawPhantom->MoveToWorld(bhkWorld);
		} else {
			RE::free(rawPhantom);
			return {};
		}

		currentPhantom.store(rawPhantom, std::memory_order_release);
		phantomInstance = currentPhantom.load(std::memory_order_acquire);

		if (!cache->phantomCreated) {
			cache->phantomCreated = true;
			cache->lastRaycastTime = GetTickCount64();
		}
	}
	*/

	if (!phantom) {
		using createSimpleShapePhantom_t = RE::hkpShapePhantom* (*)(RE::hkpShapePhantom*, RE::hkpShape*, const RE::hkTransform&, uint32_t);
		REL::Relocation<createSimpleShapePhantom_t> createSimpleShapePhantom{ RELOCATION_ID(60675, 61535) };
		phantom = RE::malloc<RE::hkpShapePhantom>(0x1C0);

		if (!phantom)
			return {};
		phantom = createSimpleShapePhantom(phantom, reinterpret_cast<RE::hkpShape*>(currentShape->referencedObject.get()), transform, 0);

		RE::hkpPhantom* returnPhantom = nullptr;
		if (phantom->GetShape()) {
			bhkWorld->worldLock.LockForWrite();
			returnPhantom = hkWorld->AddPhantom(phantom);
			bhkWorld->worldLock.UnlockForWrite();
		}

		if (!returnPhantom)
			return {};
	}

	if (phantom->world != hkWorld) {
		bhkWorld->worldLock.LockForWrite();
		phantom->world->RemovePhantom(phantom);
		hkWorld->AddPhantom(phantom);
		bhkWorld->worldLock.UnlockForWrite();
	}

	/*
	if (phantomInstance->GetWorld1() != hkWorld) {
		phantomInstance->RemoveFromCurrentWorld();
		phantomInstance->MoveToWorld(bhkWorld);
	}
	*/

	RayResult result;

	cache->BodyPairCollector->Reset();

	if (!phantom->GetShape()) {
		return result;
	}

	/*
	if (!phantomInstance->referencedObject) {
		return result;
	}
	*/

	if (!lastCell) {
		lastCell = cell;
	} else if (lastCell != cell) {
		lastCell = cell;
		RaycastErrorCount = 0;
		shownError = false;
	}

	bhkWorld->worldLock.LockForWrite();

	using SetPosition_t = void (*)(RE::hkpShapePhantom*, RE::hkVector4);
	REL::Relocation<SetPosition_t> SetPosition{ RELOCATION_ID(60791, 61653) };
	SetPosition(phantom, vecA);

	bhkWorld->worldLock.UnlockForWrite();

	try {
		/*
		using SetPositionSimpleShapePhantom_t = void (*)(RE::bhkShapePhantom*, RE::hkVector4);
		REL::Relocation<SetPositionSimpleShapePhantom_t> SetPosition{ RELOCATION_ID(76895, 78771) };
		SetPosition(phantomInstance, vecA);
		*/

		bhkWorld->worldLock.LockForRead();

		using GetPenetrations_t = void (*)(RE::hkpShapePhantom*, RE::hkpCdBodyPairCollector*, RE::hkpCollisionInput*);
		REL::Relocation<GetPenetrations_t> GetPenetrations{ RELOCATION_ID(60682, 61543) };

		GetPenetrations(phantom, reinterpret_cast<RE::hkpCdBodyPairCollector*>(cache->BodyPairCollector.get()), nullptr);

		/*
		using GetPenetrations_t = void (*)(RE::hkpShapePhantom*, RE::hkpCdBodyPairCollector*, RE::hkpCollisionInput*);
		REL::Relocation<GetPenetrations_t> GetPenetrations{ RELOCATION_ID(60682, 61543) };

		if (phantomInstance->referencedObject && hkWorld->collisionInput) {
			auto hkpPhantom = reinterpret_cast<RE::hkpShapePhantom*>(phantomInstance->referencedObject.get());

			GetPenetrations(hkpPhantom, reinterpret_cast<RE::hkpCdBodyPairCollector*>(cache->BodyPairCollector.get()), nullptr);
		}
		*/

	} catch (...) {
		HandleErrorMessage();
	}

	bhkWorld->worldLock.UnlockForRead();

	result.cdBodyHitArray = cache->BodyPairCollector->GetHits();

	cache->lastRaycastTime = GetTickCount64();

	return result;
}

namespace GrassControl
{
	RaycastHelper::RaycastHelper(int version, float rayHeight, float rayDepth, const std::string& layers, std::unique_ptr<Util::CachedFormList> ignored, std::unique_ptr<Util::CachedFormList> textures, std::unique_ptr<Util::CachedFormList> cliffs, std::unique_ptr<Util::CachedFormList> grassTypes) :
		Version(version), RayHeight(rayHeight), RayDepth(rayDepth), Ignore(std::move(ignored)), Textures(std::move(textures)), Cliffs(std::move(cliffs)), Grasses(std::move(grassTypes))
	{
		auto spl = Util::StringHelpers::Split_at_any(layers, { ' ', ',', '\t', '+' }, true);
		unsigned long long mask = 0;
		for (const auto& x : spl) {
			int y = std::stoi(x);
			if (y >= 0 && y < 64) {
				mask |= static_cast<unsigned long long>(1) << y;
			}
		}
		this->RaycastMask = mask;

		this->RayCollector = std::make_unique<Raycast::RayCollector>();
		this->RayCollector->settingsCache = this;

		this->BodyPairCollector = std::make_unique<Raycast::CdBodyPairCollector>();
		this->BodyPairCollector->settingsCache = this;
	}

	bool RaycastHelper::CanPlaceGrass(RE::TESObjectLAND* land, const float x, const float y, const float z, RE::GrassParam* param) const
	{
		static thread_local std::string cachedCellName;
		static thread_local RE::FormID cachedCellID = 0;

		auto cell = land->GetSaveParentCell();

		if (cell == nullptr) {
			return true;
		}

		if (cell->formID != cachedCellID) {
			cachedCellID = cell->formID;
			cachedCellName = cell->GetFormEditorID() ? cell->GetFormEditorID() : cell->GetName();
		}

		// Currently not dealing with this.
		if (cell->IsInteriorCell() || !cell->IsAttached()) {
			/*
			auto phantomInstance = Raycast::currentPhantom.load(std::memory_order_acquire);

			if (phantomInstance) {
				phantomInstance->RemoveFromCurrentWorld();
				phantomInstance = nullptr;
			}
			*/

			return true;
		}

		if (param) {
			if (this->Grasses != nullptr && this->Grasses->Contains(param->grassFormID))
				return true;
		}

		if (this->Textures != nullptr) {
			const auto width = static_cast<float>(Config::RayCastTextureWidth);
			auto tes = RE::TES::GetSingleton();

			if (width > 0.0f) {
				RE::TESLandTexture* txts[5];
				std::array pts = { RE::NiPoint3{ x, y, z }, RE::NiPoint3{ x + width, y, z }, RE::NiPoint3{ x - width, y, z }, RE::NiPoint3{ x, y + width, z }, RE::NiPoint3{ x, y - width, z } };

				for (int i = 0; i < 5; i++) {
					txts[i] = tes->GetLandTexture(pts[i]);
				}

				for (auto& txt : txts) {
					if (txt && this->Textures->Contains(cachedCellID)) {
						logger::debug("Detected hit with landscape texture in {} with 0x{:x}", cachedCellName, txt->GetFormID());
						return false;
					}
				}
			} else {
				if (auto txt = tes->GetLandTexture(RE::NiPoint3{ x, y, z })) {
					if (this->Textures->Contains(cachedCellID)) {
						logger::debug("Detected hit with landscape texture in {} with 0x{:x}", cachedCellName, txt->GetFormID());
						return false;
					}
				}
			}
		}

		auto begin = glm::vec4(x, y, z - this->RayDepth, 0.0f);
		auto end = glm::vec4(x, y, z + this->RayHeight, 0.0f);

		Raycast::RayResult rs;

		if (Config::RayCastMode >= 1) {
			rs = Raycast::hkpPhantomCast(begin, end, cell, param, this);

			if (Config::DebugLogEnable) {
				for (auto& [body, hitObject] : rs.cdBodyHitArray) {
					auto sTemplate = fmt::runtime("{} {}(0x{:x})");
					auto cellName = format(sTemplate, "cell", cachedCellName, cell->formID);
					auto hitObjectName = hitObject ? format(sTemplate, "with", hitObject->GetFormEditorID() ? hitObject->GetFormEditorID() : hitObject->GetName(), hitObject->formID) : "";
					logger::debug("{}({},{},{}) detected hit {}", cellName, x, y, z, hitObjectName);
				}
			}

			if (!rs.cdBodyHitArray.empty())
				return false;

		} else {
			rs = Raycast::hkpCastRay(begin, end, this);

			if (Config::DebugLogEnable) {
				for (auto& [normal, hitFraction, body] : rs.hitArray) {
					auto rsTESForm = GetRaycastHitBaseForm(body);

					auto sTemplate = fmt::runtime("{} {}(0x{:x})");
					auto cellName = format(sTemplate, "cell", cachedCellName, cell->formID);
					auto hitObjectName = rsTESForm ? format(sTemplate, "with", rsTESForm->GetFormEditorID() ? rsTESForm->GetFormEditorID() : rsTESForm->GetName(), rsTESForm->formID) : "";
					logger::debug("{}({},{},{}) detected hit {}", cellName, x, y, z, hitObjectName);
				}
			}

			if (!rs.hitArray.empty())
				return false;
		}

		return true;
	}

	float RaycastHelper::CreateGrassCliff(const float x, const float y, const float z, glm::vec3& Normal, RE::GrassParam* param) const
	{
		static thread_local std::string cachedCellName;
		static thread_local RE::FormID cachedCellID = 0;

		RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
		auto cell = player->GetParentCell();

		if (cell == nullptr) {
			return z;
		}

		if (cell->formID != cachedCellID) {
			cachedCellID = cell->formID;
			cachedCellName = cell->GetFormEditorID() ? cell->GetFormEditorID() : cell->GetName();
		}

		// Currently not dealing with this.
		if (cell->IsInteriorCell() || !cell->IsAttached()) {
			return z;
		}

		RE::TESGrass* grassForm = nullptr;

		if (param) {
			grassForm = RE::TESForm::LookupByID<RE::TESGrass>(param->grassFormID);
			if (grassForm) {
				if (grassForm->GetUnderwaterState() == RE::TESGrass::GRASS_WATER_STATE::kBelowOnlyAtLeast || grassForm->GetUnderwaterState() == RE::TESGrass::GRASS_WATER_STATE::kBelowOnlyAtMost) {
					return z;
				}
			}
		}

		auto begin = glm::vec4{ x, y, z, 0.0f };
		auto end = glm::vec4{ x, y, z + 300.0f, 0.0f };

		auto rs = Raycast::hkpCastRay(begin, end, this, true);
		float retn = z;

		if (!IsCliffObject(rs))
			return z;

		glm::vec4 bestPos = begin;

		for (auto& [normal, hitFraction, body] : rs.hitArray) {
			if (hitFraction >= 1.0f || body == nullptr) {
				continue;
			}

			const auto pos = (end - begin) * hitFraction + begin;

			if (this->Cliffs == nullptr) {
				return z;
			}

			if (this->Cliffs->Contains(GetRaycastHitBaseForm(body)) && pos.z > bestPos.z) {
				normal *= -1;

				if (grassForm) {
					if (std::acosf(normal.z) > grassForm->GetMaxSlope() || std::acosf(normal.z) < grassForm->GetMinSlope())
						return z;
				}

				Normal = normal;

				retn = pos.z;
				bestPos = pos;
			}
		}

		if (param)
			param->fitsToSlope = true;

		std::vector<glm::vec4> ptsBegin;
		std::vector<glm::vec4> ptsEnd;

		if (grassForm) {
			if (grassForm->boundData.boundMax.x != 0 && grassForm->boundData.boundMin.x != 0) {
				auto widthX = std::abs(static_cast<float>(grassForm->boundData.boundMax.x - grassForm->boundData.boundMin.x) / 2);
				auto widthY = std::abs(static_cast<float>(grassForm->boundData.boundMax.y - grassForm->boundData.boundMin.y) / 2);
				auto width = std::max(widthX, widthY) + 40.0f;
				ptsBegin = { glm::vec4{ x + width, y, retn - 30.0f, 0.0f }, glm::vec4{ x - width, y, retn - 30.0f, 0.0f }, glm::vec4{ x, y + width, retn - 30.0f, 0.0f }, glm::vec4{ x, y - width, retn - 30.0f, 0.0f } };
				ptsEnd = { glm::vec4{ x + width, y, retn + 30.0f, 0.0f }, glm::vec4{ x - width, y, retn + 30.0f, 0.0f }, glm::vec4{ x, y + width, retn + 30.0f, 0.0f }, glm::vec4{ x, y - width, retn + 30.0f, 0.0f } };
			}
		}

		if (ptsBegin.empty() && ptsEnd.empty()) {
			ptsBegin = { glm::vec4{ x + 80.0f, y, retn - 30.0f, 0.0f }, glm::vec4{ x - 80.0f, y, retn - 30.0f, 0.0f }, glm::vec4{ x, y + 80.0f, retn - 30.0f, 0.0f }, glm::vec4{ x, y - 80.0f, retn - 30.0f, 0.0f } };
			ptsEnd = { glm::vec4{ x + 80.0f, y, retn + 30.0f, 0.0f }, glm::vec4{ x - 80.0f, y, retn + 30.0f, 0.0f }, glm::vec4{ x, y + 80.0f, retn + 30.0f, 0.0f }, glm::vec4{ x, y - 80.0f, retn + 30.0f, 0.0f } };
		}

		for (int i = 0; i < ptsBegin.size(); i++) {
			rs = Raycast::hkpCastRay(ptsBegin[i], ptsEnd[i], this, true);

			if (!this->Cliffs->Contains(GetRaycastHitBaseForm(rs))) {
				return z;
			}
		}

		if (Config::DebugLogEnable && retn != z) {
			auto sTemplate = fmt::runtime("{} {}(0x{:x})");
			auto cellName = fmt::format(sTemplate, "cell", cachedCellName, cell->formID);
			auto rsTESForm = GetRaycastHitBaseForm(rs);
			auto hitObjectName = rsTESForm ? fmt::format(sTemplate, "onto", rsTESForm->GetFormEditorID() ? rsTESForm->GetFormEditorID() : rsTESForm->GetName(), rsTESForm->formID) : "";
			logger::debug("{}({},{},{}) moved grass patch {}. New height is {} and new normal (up) direction is ({},{},{})", cellName, x, y, z, hitObjectName, retn, Normal.x, Normal.y, Normal.z);
		}

		return retn;
	}

	RE::TESForm* RaycastHelper::GetRaycastHitBaseForm(const Raycast::RayResult& r)
	{
		RE::TESForm* result = nullptr;
		try {
			auto ref = r.hitObject;
			if (ref != nullptr) {
				auto bound = ref->GetBaseObject();
				if (bound != nullptr) {
					auto baseform = bound->As<RE::TESForm>();
					if (baseform != nullptr) {
						result = baseform;
						return result;
					}
				}
			}
		} catch (...) {
		}
		return result;
	}

	RE::TESForm* RaycastHelper::GetRaycastHitBaseForm(const RE::hkpCdBody* body)
	{
		RE::TESForm* result = nullptr;
		try {
			RE::TESObjectREFR* ref = nullptr;

			auto av = Raycast::getAVObject(body);
			if (av) {
				ref = av->GetUserData();
			}
			if (ref != nullptr) {
				auto bound = ref->GetBaseObject();
				if (bound != nullptr) {
					auto baseform = bound->As<RE::TESForm>();
					if (baseform != nullptr) {
						result = baseform;
						return result;
					}
				}
			}
		} catch (...) {
		}
		return result;
	}

	bool RaycastHelper::IsCliffObject(const Raycast::RayResult& r) const
	{
		return this->Cliffs->Contains(GetRaycastHitBaseForm(r));
	}

	void RaycastHelper::CheckInactivePhantom() const
	{
		uint64_t last = InterlockedCompareExchange64(&lastRaycastTime, 0, 0);
		uint64_t now = GetTickCount64();

		if (now - last < 60000) {
			return;
		}

		if (auto phantom = Raycast::currentPhantom.load(std::memory_order_acquire)) {
			if (phantom->GetWorld1()) {
				phantom->RemoveFromCurrentWorld();
			}
		}

		phantomCreated = false;
	}
}
