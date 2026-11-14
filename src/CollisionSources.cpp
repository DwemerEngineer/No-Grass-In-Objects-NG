#include "GrassControl/CollisionSources.h"

#include "GrassControl/CliffSurfaces.h"
#include "GrassControl/PhysicsRaycast.h"

namespace GrassControl::CollisionGrid
{
	constexpr std::size_t MaxObjects = 8192;
	constexpr std::size_t MaxDepth = 128;
	constexpr std::size_t MaxShapeVertices = 65536;
	constexpr float HavokScale = 0.0142875f;

	// The reference transform replaces the model root's transform
	template <class F>
	static void VisitModel(RE::NiAVObject* root, F&& visit)
	{
		struct PendingNode
		{
			RE::NiAVObject* object;
			RE::NiTransform nodeToModel;
			std::size_t depth;
		};

		std::vector<PendingNode> pendingNodes{ { root, {}, 0 } };
		std::size_t visitedNodes = 0;

		while (!pendingNodes.empty() && visitedNodes++ < MaxObjects) {
			auto [object, nodeToModel, depth] = pendingNodes.back();
			pendingNodes.pop_back();
			if (!object)
				continue;

			visit(object, nodeToModel);

			auto* node = object->AsNode();
			if (!node || depth >= MaxDepth)
				continue;

			for (const auto& child : node->GetChildren()) {
				if (child)
					pendingNodes.push_back({ child.get(), nodeToModel * child->local, depth + 1 });
			}
		}
	}

	static RE::NiPoint3 ToPoint3(const RE::hkVector4& vector)
	{
		return { vector.quad.m128_f32[0], vector.quad.m128_f32[1], vector.quad.m128_f32[2] };
	}

	// bhkRigidBodyT derives privately from bhkRigidBody, so netimmerse_cast cannot reach it.
	static const RE::bhkRigidBodyT* AsRigidBodyT(RE::bhkWorldObject* body)
	{
		static REL::Relocation<const RE::NiRTTI*> rtti{ RE::bhkRigidBodyT::Ni_RTTI };
		return body->GetRTTI() == rtti.get() ? reinterpret_cast<const RE::bhkRigidBodyT*>(body) : nullptr;
	}

	static std::optional<Model::Body> ReadBody(RE::NiAVObject* object, const RE::NiTransform& nodeToModel)
	{
		auto* collision = object->collisionObject ? object->collisionObject->AsBhkNiCollisionObject() : nullptr;
		auto* collisionBody = collision ? collision->body.get() : nullptr;

		auto* rigidBody = collisionBody ? netimmerse_cast<RE::bhkRigidBody*>(collisionBody) : nullptr;
		auto* havokBody = rigidBody ? rigidBody->GetRigidBody() : nullptr;
		const auto* shape = havokBody ? havokBody->collidable.GetShape() : nullptr;

		if (!shape)
			return std::nullopt;

		Model::Body capturedBody;
		capturedBody.shape = shape;
		capturedBody.toModel = nodeToModel;
		capturedBody.layer = havokBody->collidable.broadPhaseHandle.collisionFilterInfo.filter & 0x7F;

		if (const auto* transformedBody = AsRigidBodyT(collisionBody)) {
			capturedBody.hasOffset = true;
			const auto& rotation = transformedBody->rotation.vec.quad.m128_f32;
			capturedBody.offsetRotation = RE::NiQuaternion(rotation[3], rotation[0], rotation[1], rotation[2]).ToRotation();
			capturedBody.offsetTranslation = ToPoint3(transformedBody->translation);
		}

		RE::hkTransform identityTransform{};
		identityTransform.rotation.col0 = RE::hkVector4(1, 0, 0, 0);
		identityTransform.rotation.col1 = RE::hkVector4(0, 1, 0, 0);
		identityTransform.rotation.col2 = RE::hkVector4(0, 0, 1, 0);
		identityTransform.translation = RE::hkVector4(0, 0, 0, 1);

		RE::hkAabb shapeBounds{};
		shape->GetAabbImpl(identityTransform, 0.0f, shapeBounds);

		const auto boundsMin = ToPoint3(shapeBounds.min);
		const auto boundsMax = ToPoint3(shapeBounds.max);
		for (int corner = 0; corner < 8; ++corner) {
			RE::NiPoint3 point{ corner & 1 ? boundsMax.x : boundsMin.x, corner & 2 ? boundsMax.y : boundsMin.y, corner & 4 ? boundsMax.z : boundsMin.z };
			if (capturedBody.hasOffset)
				point = capturedBody.offsetRotation * point + capturedBody.offsetTranslation;

			capturedBody.bounds.Add(nodeToModel * (point / HavokScale));
		}

		if (!capturedBody.bounds.Valid() || !std::isfinite(capturedBody.bounds.min.x) || !std::isfinite(capturedBody.bounds.max.x))
			return std::nullopt;

		return capturedBody;
	}

	static const char* DiffusePath(RE::BSLightingShaderProperty* property)
	{
		if (auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material); material && material->textureSet) {
			if (const char* path = material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse); path && *path)
				return path;
		}

		const auto* texture = property->GetBaseTexture();

		return texture && !texture->name.empty() ? texture->name.c_str() : nullptr;
	}

	void Bounds::Add(const RE::NiPoint3& point)
	{
		min = { std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z) };
		max = { std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z) };
	}

	Bounds Bounds::Transformed(const RE::NiTransform& transform) const
	{
		if (!Valid())
			return {};

		Bounds result;
		for (int corner = 0; corner < 8; ++corner) {
			const RE::NiPoint3 point{ corner & 1 ? max.x : min.x, corner & 2 ? max.y : min.y, corner & 4 ? max.z : min.z };
			result.Add(transform * point);
		}

		return result;
	}

	const Util::ShapeSnapshot& Model::Shapes() const
	{
		std::call_once(shapesOnce, [&] {
			shapes.root = rootName;
			std::size_t capturedVertexCount = 0;

			VisitModel(root.get(), [&](RE::NiAVObject* object, const RE::NiTransform& nodeToModel) {
				auto* shape = object->AsTriShape();
				if (!shape || capturedVertexCount >= MaxShapeVertices)
					return;

				const auto geometryType = shape->GetType().get();
				const bool staticShape = geometryType == RE::BSGeometry::Type::kTriShape || geometryType == RE::BSGeometry::Type::kMeshLODTriShape || geometryType == RE::BSGeometry::Type::kSubIndexTriShape;
				const auto& runtimeData = shape->GetGeometryRuntimeData();
				const auto* rendererData = runtimeData.rendererData;

				if (!staticShape || runtimeData.skinInstance || !rendererData || !rendererData->rawVertexData)
					return;

				auto vertexLayout = rendererData->vertexDesc;
				const auto stride = vertexLayout.GetSize();
				const auto positionOffset = vertexLayout.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
				if (stride < sizeof(RE::NiPoint3) || stride > 1024 || positionOffset > stride - sizeof(RE::NiPoint3))
					return;

				const std::size_t vertexCount = shape->GetTrishapeRuntimeData().vertexCount;
				const auto remainingVertices = MaxShapeVertices - capturedVertexCount;
				const auto sampleBudget = std::max<std::size_t>(1, remainingVertices / 4);
				const auto vertexStep = std::max<std::size_t>(1, vertexCount / sampleBudget);
				Util::ShapeSnapshot::Shape capturedShape{ object->name.c_str(), {} };

				for (std::size_t index = 0; index < vertexCount; index += vertexStep) {
					RE::NiPoint3 vertex;
					std::memcpy(&vertex, rendererData->rawVertexData + stride * index + positionOffset, sizeof(vertex));
					capturedShape.vertices.push_back(nodeToModel * vertex);
				}

				capturedVertexCount += capturedShape.vertices.size();
				if (!capturedShape.vertices.empty())
					shapes.shapes.push_back(std::move(capturedShape));
			});
		});

		return shapes;
	}

	const std::vector<Model::Surface>& Model::Surfaces() const
	{
		std::call_once(surfacesOnce, [&] {
			std::uint32_t nextShapeIndex = 0;
			const auto collectSurfaces = [&](const auto& recurse, RE::NiAVObject* object, const RE::NiTransform& nodeToModel, std::size_t depth) -> void {
				if (!object || depth >= MaxDepth)
					return;

				if (auto* node = object->AsNode()) {
					for (const auto& child : node->GetChildren()) {
						if (child)
							recurse(recurse, child.get(), nodeToModel * child->local, depth + 1);
					}
					return;
				}

				auto* geometry = object->AsGeometry();
				if (!geometry)
					return;

				const auto shapeIndex = nextShapeIndex++;
				auto* shader = CliffSurfaces::GetSurfaceShader(*geometry);
				if (!shader)
					return;

				const auto& bound = geometry->GetModelData().modelBound;
				if (!(bound.radius > 0.0f))
					return;

				const char* diffuseTexture = DiffusePath(shader);
				surfaces.push_back({ object->name.c_str(), diffuseTexture ? diffuseTexture : "", nodeToModel * bound.center, bound.radius * nodeToModel.scale, shapeIndex });
			};
			collectSurfaces(collectSurfaces, root.get(), {}, 0);
		});

		return surfaces;
	}

	std::shared_ptr<const Model> ModelCache::LoadModel(const std::string& path) const
	{
		if (!PhysicsRaycast::EnsureThreadMemory())
			return nullptr;

		RE::NiPointer<RE::NiNode> root;
		RE::BSModelDB::DBTraits::ArgsType args{};
		const auto result = RE::BSModelDB::Demand(path.c_str(), root, args);
		if (result != RE::BSResource::ErrorCode::kNone || !root) {
			if (loadFailureCount.fetch_add(1) < 20)
				logger::warn("Could not load {} for collision preloading (error {})", path, static_cast<int>(result));
			return nullptr;
		}

		auto model = std::make_shared<Model>();
		model->rootName = root->name.c_str();

		VisitModel(root.get(), [&](RE::NiAVObject* object, const RE::NiTransform& nodeToModel) {
			if (auto body = ReadBody(object, nodeToModel))
				model->bodies.push_back(std::move(*body));
		});

		model->root = std::move(root);
		return model;
	}

	std::shared_ptr<const Model> ModelCache::Load(std::string_view requestedPath)
	{
		std::string path(requestedPath);
		std::ranges::transform(path, path.begin(), [](unsigned char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(c)); });

		std::optional<std::promise<std::shared_ptr<const Model>>> loadPromise;
		std::shared_future<std::shared_ptr<const Model>> pendingLoad;
		{
			std::scoped_lock lock(mutex);
			if (const auto it = entries.find(path); it != entries.end()) {
				useOrder.splice(useOrder.end(), useOrder, it->second.order);
				return it->second.model;
			}
			if (const auto it = pendingLoads.find(path); it != pendingLoads.end()) {
				pendingLoad = it->second;
			} else {
				auto& promise = loadPromise.emplace();
				pendingLoads.emplace(path, promise.get_future().share());
			}
		}

		if (pendingLoad.valid())
			return pendingLoad.get();

		std::shared_ptr<const Model> model;
		try {
			model = LoadModel(path);
		} catch (const std::exception& e) {
			logger::warn("Could not read collision of {}: {}", path, e.what());
		}

		{
			std::scoped_lock lock(mutex);
			pendingLoads.erase(path);
			useOrder.push_back(path);
			entries.insert_or_assign(path, Entry{ model, std::prev(useOrder.end()) });
			TrimUnusedModels();
		}

		loadPromise->set_value(model);
		return model;
	}

	void ModelCache::TrimUnusedModels()
	{
		// Keep models pinned by cell geometry
		for (auto oldest = useOrder.begin(); entries.size() > Capacity && oldest != useOrder.end();) {
			const auto entry = entries.find(*oldest);
			if (entry != entries.end() && entry->second.model && entry->second.model.use_count() > 1) {
				++oldest;
				continue;
			}

			if (entry != entries.end())
				entries.erase(entry);

			oldest = useOrder.erase(oldest);
		}
	}
}

namespace GrassControl::CollisionGrid::PlacementReader
{
	static constexpr std::uint32_t Tag(const char (&name)[5])
	{
		return std::uint32_t(name[0]) | std::uint32_t(name[1]) << 8 | std::uint32_t(name[2]) << 16 | std::uint32_t(name[3]) << 24;
	}

	constexpr std::uint32_t RecordDeleted = 0x20;
	constexpr std::uint32_t RecordInitiallyDisabled = 0x800;
	constexpr std::uint32_t CellChildrenGroup = 6;
	constexpr std::uint32_t PersistentChildrenGroup = 8;
	constexpr std::uint32_t TemporaryChildrenGroup = 9;

	struct ReferenceRecord
	{
		Placement placement;
		std::uint32_t flags;
	};

	using PlacementsByCell = std::unordered_map<std::uint64_t, Placements>;

	static std::mutex persistentMutex;
	static std::unordered_map<RE::FormID, std::shared_ptr<const PlacementsByCell>> persistentPlacementsByWorld;

	static bool HasModel(RE::FormID baseID)
	{
		const auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(baseID);
		const auto* model = base ? base->As<RE::TESModel>() : nullptr;
		const char* path = model ? model->GetModel() : nullptr;
		return path && *path;
	}

	static bool IsCellChild(const RE::FORM& form)
	{
		if (form.form == Tag("GRUP"))
			return form.formID == PersistentChildrenGroup || form.formID == TemporaryChildrenGroup;

		switch (form.form) {
		case Tag("REFR"):
		case Tag("ACHR"):
		case Tag("PGRE"):
		case Tag("PMIS"):
		case Tag("PARW"):
		case Tag("PBAR"):
		case Tag("PBEA"):
		case Tag("PCON"):
		case Tag("PFLA"):
		case Tag("PHZD"):
		case Tag("NAVM"):
		case Tag("LAND"):
			return true;
		default:
			return false;
		}
	}

	static std::optional<ReferenceRecord> ReadReference(RE::TESFile& file, const RE::TESFile& owner)
	{
		ReferenceRecord record{ {}, file.currentform.flags };
		RE::FormID rawBaseID = 0;

		do {
			switch (file.GetCurrentSubRecordType()) {
			case Tag("NAME"):
				file.ReadData(&rawBaseID, sizeof(rawBaseID));
				break;
			case Tag("DATA"):
				{
					float positionAndRotation[6];
					if (file.GetCurrentSubRecordSize() >= sizeof(positionAndRotation) && file.ReadData(positionAndRotation, sizeof(positionAndRotation))) {
						record.placement.position = { positionAndRotation[0], positionAndRotation[1], positionAndRotation[2] };
						record.placement.angle = { positionAndRotation[3], positionAndRotation[4], positionAndRotation[5] };
					}
					break;
				}
			case Tag("XSCL"):
				file.ReadData(&record.placement.scale, sizeof(float));
				break;
			default:
				break;
			}
		} while (file.SeekNextSubrecord());

		if (!rawBaseID)
			return std::nullopt;
		record.placement.base = owner.GetRuntimeFormID(rawBaseID);

		return record;
	}

	static void ReadCellReferences(RE::TESWorldSpace* world, RE::TESFile* source, int x, int y, std::unordered_map<RE::FormID, ReferenceRecord>& records)
	{
		auto* reader = source->Duplicate();
		if (!reader || !reader->SeekCell(world, x, y))
			return;

		if (!reader->SeekNextForm(true) || reader->currentform.form != Tag("GRUP") || reader->currentform.formID != CellChildrenGroup)
			return;

		while (reader->SeekNextForm(true) && IsCellChild(reader->currentform)) {
			if (reader->currentform.form != Tag("REFR"))
				continue;

			// Later plugins replace earlier reference records
			const auto formID = reader->currentform.formID;
			if (auto record = ReadReference(*reader, *source)) {
				record->placement.reference = formID;
				records.insert_or_assign(formID, *record);
			}
		}
	}

	static std::uint64_t CellKey(int x, int y)
	{
		const auto cellX = std::uint64_t(std::uint32_t(x)) << 32;
		const auto cellY = std::uint32_t(y);

		return cellX | cellY;
	}

	// Persistent references must be copied on the main thread
	static std::shared_ptr<const PlacementsByCell> GetPersistentPlacements(RE::TESWorldSpace* world)
	{
		{
			std::scoped_lock lock(persistentMutex);
			if (const auto it = persistentPlacementsByWorld.find(world->GetFormID()); it != persistentPlacementsByWorld.end())
				return it->second;
		}

		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks)
			return nullptr;

		auto placementPromise = std::make_shared<std::promise<PlacementsByCell>>();
		auto placementFuture = placementPromise->get_future();

		tasks->AddTask([placementPromise, world] {
			PlacementsByCell placementsByCell;
			if (auto* persistentCell = world->persistentCell) {
				persistentCell->ForEachReference([&](RE::TESObjectREFR* reference) {
					const auto* base = reference->GetBaseObject();
					if (!base || reference->IsDisabled() || reference->IsDeleted() || reference->Is(RE::FormType::ActorCharacter) || !HasModel(base->GetFormID()))
						return RE::BSContainer::ForEachResult::kContinue;

					const auto position = reference->GetPosition();
					const int cellX = static_cast<int>(std::floor(position.x / 4096.0f));
					const int cellY = static_cast<int>(std::floor(position.y / 4096.0f));
					placementsByCell[CellKey(cellX, cellY)].push_back({ reference->GetFormID(), base->GetFormID(), position, reference->GetAngle(), reference->GetScale() });

					return RE::BSContainer::ForEachResult::kContinue;
				});
			}

			placementPromise->set_value(std::move(placementsByCell));
		});

		if (placementFuture.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
			return nullptr;

		auto placementsByCell = std::make_shared<const PlacementsByCell>(placementFuture.get());
		std::scoped_lock lock(persistentMutex);

		return persistentPlacementsByWorld.try_emplace(world->GetFormID(), std::move(placementsByCell)).first->second;
	}

	std::optional<Placements> Read(RE::TESWorldSpace* world, int x, int y)
	{
		const auto persistent = world ? GetPersistentPlacements(world) : nullptr;
		if (!persistent)
			return std::nullopt;

		std::unordered_map<RE::FormID, ReferenceRecord> records;
		if (const auto* sourceFiles = world->sourceFiles.array) {
			for (auto* source : *sourceFiles) {
				if (source)
					ReadCellReferences(world, source, x, y, records);
			}
		}

		Placements placements;
		for (const auto& [formID, record] : records) {
			if (!(record.flags & (RecordDeleted | RecordInitiallyDisabled)) && HasModel(record.placement.base))
				placements.push_back(record.placement);
		}

		if (const auto it = persistent->find(CellKey(x, y)); it != persistent->end()) {
			for (const auto& placement : it->second) {
				if (!records.contains(placement.reference))
					placements.push_back(placement);
			}
		}

		return placements;
	}

	void Clear()
	{
		std::scoped_lock lock(persistentMutex);
		persistentPlacementsByWorld.clear();
	}
}
