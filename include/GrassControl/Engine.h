#pragma once

namespace RE
{
	// Writes the returned instance count to manager->instanceData
	inline std::uint32_t CreateGrassPatch(BGSGrassManager* manager, CreateGrassParams* params)
	{
		static REL::Relocation<std::uint32_t (*)(BGSGrassManager*, CreateGrassParams*)> func{ RELOCATION_ID(15212, 15381) };
		return func(manager, params);
	}

	inline BSMultiStreamInstanceTriShape* LoadGrassType(BGSGrassManager* manager, GrassParam* param, std::uint32_t cellKeyX, std::uint32_t cellKeyY, std::uint64_t* typeKey, BSFixedString* modelKey)
	{
		static REL::Relocation<BSMultiStreamInstanceTriShape* (*)(BGSGrassManager*, GrassParam*, std::uint32_t, std::uint32_t, std::uint64_t*, BSFixedString*)> func{ RELOCATION_ID(15214, 15383) };
		return func(manager, param, cellKeyX, cellKeyY, typeKey, modelKey);
	}

	// Indexed by the engine's slope-angle conversion
	inline const float* GrassSlopeTable()
	{
		static REL::Relocation<const float*> table{ RELOCATION_ID(523934, 410514) };
		return table.get();
	}

	inline void ClearGrassHandles(ExtraDataList* extraList)
	{
		static REL::Relocation<void (*)(ExtraDataList*)> func{ RELOCATION_ID(11931, 12070) };
		func(extraList);
	}

	// Returns false outside the land's loaded data
	inline bool GetLandCoordData(TESObjectLAND* land, void* coordData, const NiPoint3* position, std::uint32_t flags)
	{
		static REL::Relocation<bool (*)(TESObjectLAND*, void*, const NiPoint3*, std::uint32_t)> func{ RELOCATION_ID(18338, 18754) };
		return func(land, coordData, position, flags);
	}

	inline void GetLandData(TESObjectLAND* land, void* coordData, float* height, NiPoint3* interpolatedNormal, NiPoint3* normal, NiColorA* color)
	{
		static REL::Relocation<void (*)(TESObjectLAND*, void*, float*, NiPoint3*, NiPoint3*, NiColorA*)> func{ RELOCATION_ID(18353, 18772) };
		func(land, coordData, height, interpolatedNormal, normal, color);
	}

	// Bypass TES::GetLandTexture's loaded-cell lookup for distant terrain
	inline TESLandTexture* GetLandTexture(TESObjectLAND* land, const NiPoint3* position)
	{
		static REL::Relocation<TESLandTexture* (*)(TESObjectLAND*, const NiPoint3*)> func{ RELOCATION_ID(18310, 18714) };
		return func(land, position);
	}

	inline void GetLandNormal(TES* tes, const NiPoint3* position, NiPoint3* interpolatedNormal, NiPoint3* normal)
	{
		static REL::Relocation<void (*)(TES*, const NiPoint3*, NiPoint3*, NiPoint3*)> func{ RELOCATION_ID(13199, 13345) };
		func(tes, position, interpolatedNormal, normal);
	}

	inline void GetLandColor(TES* tes, const NiPoint3* position, NiColorA* color)
	{
		static REL::Relocation<void (*)(TES*, const NiPoint3*, NiColorA*)> func{ RELOCATION_ID(13200, 13346) };
		func(tes, position, color);
	}

	inline TESObjectCELL* GetLoadedCell(const TES* tes, int x, int y)
	{
		static REL::Relocation<TESObjectCELL* (*)(const TES*, int, int)> func{ RELOCATION_ID(13174, 13319) };
		return func(tes, x, y);
	}

	inline TESObjectCELL* CreateCellAt(TESDataHandler* dataHandler, int x, int y, TESWorldSpace* world)
	{
		static REL::Relocation<TESObjectCELL* (*)(TESDataHandler*, int, int, TESWorldSpace*, bool)> func{ RELOCATION_ID(13620, 13718) };
		return func(dataHandler, x, y, world, true);
	}

	inline BSResource::ID MakeGrassModelKey(const char* model)
	{
		static REL::Relocation<const char* (*)(char*, std::uint32_t, const char*, const char*)> prepareName{ RELOCATION_ID(68471, 69822) };
		static REL::Relocation<BSResource::ID* (*)(BSResource::ID*, const char*)> makeID{ RELOCATION_ID(68631, 69971) };
		static REL::Relocation<std::uint32_t*> grassDirectory{ RELOCATION_ID(514429, 400575) };

		char modelPath[260];
		BSResource::ID key;
		makeID(&key, prepareName(modelPath, sizeof(modelPath), model, "meshes\\"));
		key.dir = *grassDirectory;
		return key;
	}

	using ModelEntryDB = BSResource::EntryDBBase<BSResource::EntryDBTraits<BSModelDB::DBTraits, ModelDBHandle::U_EntryDB>>;

	inline ModelEntryDB* ModelEntryDatabase()
	{
		static REL::Relocation<ModelDBHandle::U_EntryDB**> database{ RELOCATION_ID(514145, 400273) };
		return *database;
	}

	inline ModelDBHandle::U_Entry* DemandGrassModel(const BSResource::ID& key)
	{
		ModelDBHandle::U_Entry* loadedEntry = nullptr;

		if (REL::Module::IsSE()) {
			static REL::Relocation<std::uint32_t (*)(const BSResource::ID*, ModelDBHandle::U_Entry**)> DemandModel{ RELOCATION_ID(15257, 0) };
			DemandModel(&key, &loadedEntry);

			return loadedEntry;
		}

		// DemandGrassModel is inlined in AE
		static REL::Relocation<bool (*)(ModelEntryDB*, const BSResource::ID*, ModelDBHandle::U_Entry**, bool)> acquire{ RELOCATION_ID(0, 15430) };
		static REL::Relocation<std::uint32_t (*)(ModelEntryDB*, std::uint32_t, ModelDBHandle::U_Entry*, ModelDBHandle::U_Entry**, const BSModelDB::DBTraits::ArgsType*)> load{ RELOCATION_ID(0, 15420) };

		auto* entryDatabase = ModelEntryDatabase();
		ModelDBHandle::U_Entry* cacheEntry = nullptr;

		if (!acquire(entryDatabase, &key, &cacheEntry, false) || !cacheEntry)
			return nullptr;

		const BSModelDB::DBTraits::ArgsType loadArgs{
			.LODmult = 0,
			.texLoadLevel = 3,
			.unk8 = false,
			.unk9 = false,
			.unkA = false,
			.postProcess = false
		};
		load(entryDatabase, 0, cacheEntry, &loadedEntry, &loadArgs);

		return loadedEntry;
	}

	inline void ReleaseGrassModel(ModelDBHandle::U_Entry* entry)
	{
		if (!entry)
			return;

		if (REL::Module::IsAE()) {
			static REL::Relocation<void (*)(ModelDBHandle::U_Entry*)> release{ RELOCATION_ID(11275, 15443) };
			release(entry);
		} else {
			static REL::Relocation<void (*)(ModelEntryDB*, ModelDBHandle::U_Entry*)> release{ RELOCATION_ID(11275, 15443) };
			release(ModelEntryDatabase(), entry);
		}
	}

	inline void PositionPlayer(PlayerCharacter* player, const float* position, const float* rotation, TESObjectCELL* cell)
	{
		static REL::Relocation<void (*)(PlayerCharacter*, const float*, const float*, TESObjectCELL*, int)> func{ RELOCATION_ID(39657, 40744) };
		func(player, position, rotation, cell, 0);
	}

	inline std::uint32_t WriteStream(std::uintptr_t stream, const void* data, std::uint32_t size)
	{
		static REL::Relocation<std::uint32_t (*)(std::uintptr_t, const void*, std::uint32_t)> func{ RELOCATION_ID(74621, 76352) };
		return func(stream, data, size);
	}

	inline void CastRay(const hkpWorld* world, const hkpWorldRayCastInput& input, hkpRayHitCollector& collector)
	{
		static REL::Relocation<void (*)(const hkpWorld*, const hkpWorldRayCastInput&, hkpRayHitCollector&)> func{ RELOCATION_ID(60552, 61400) };
		func(world, input, collector);
	}

	inline void GetPenetrations(const hkpWorld* world, const hkpCollidable* collidable, const hkpCollisionInput& input, void* collector)
	{
		static REL::Relocation<void (*)(const hkpWorld*, const hkpCollidable*, const hkpCollisionInput&, void*)> func{ RELOCATION_ID(60558, 61406) };
		func(world, collidable, input, collector);
	}

	inline bhkShape* ConstructCapsuleShape(bhkShape* storage, NiPoint3& bottom, NiPoint3& top, float radius)
	{
		static REL::Relocation<bhkShape* (*)(bhkShape*, NiPoint3&, NiPoint3&, float)> func{ RELOCATION_ID(76924, 78799) };
		return func(storage, bottom, top, radius);
	}

	inline bhkShape* ConstructBoxShape(bhkShape* storage, NiPoint3* halfExtents)
	{
		static REL::Relocation<bhkShape* (*)(bhkShape*, NiPoint3*)> func{ RELOCATION_ID(38677, 78820) };
		return func(storage, halfExtents);
	}

	// AE's box constructor leaves dimensions unset
	inline void InitializeBoxShape(bhkShape* shape, const void* boxInfo)
	{
		static REL::Relocation<void (*)(bhkShape*, const void*)> func{ RELOCATION_ID(76940, 78815) };
		func(shape, boxInfo);
	}

	inline hkpSimpleShapePhantom* ConstructSimpleShapePhantom(void* storage, hkpShape* shape, const hkTransform& transform, std::uint32_t filter)
	{
		static REL::Relocation<hkpSimpleShapePhantom* (*)(void*, hkpShape*, const hkTransform&, std::uint32_t)> func{ RELOCATION_ID(60675, 61535) };
		return func(storage, shape, transform, filter);
	}

	inline void SendHavokThreadEvent(const void* event)
	{
		static REL::Relocation<void (*)(void*, const void*)> func{ RELOCATION_ID(77215, 79103) };
		func(nullptr, event);
	}
}
