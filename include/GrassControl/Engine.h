#pragma once

namespace RE
{
	inline void AddCellGrass(BGSGrassManager* manager, TESObjectCELL* cell, bool* abort)
	{
		static REL::Relocation<void (*)(BGSGrassManager*, TESObjectCELL*, bool*)> func{ RELOCATION_ID(15204, 15372) };
		func(manager, cell, abort);
	}

	inline void RemoveCellGrass(BGSGrassManager* manager, TESObjectCELL* cell)
	{
		static REL::Relocation<void (*)(BGSGrassManager*, TESObjectCELL*)> func{ RELOCATION_ID(15207, 15375) };
		func(manager, cell);
	}

	inline void LoadCellGrassFile(BGSGrassManager* manager, TESObjectCELL* cell)
	{
		static REL::Relocation<void (*)(BGSGrassManager*, TESObjectCELL*)> func{ RELOCATION_ID(15206, 15374) };
		func(manager, cell);
	}

	// Writes the returned number of instances to manager->instanceData.
	inline std::uint32_t CreateGrassPatch(BGSGrassManager* manager, CreateGrassParams* params)
	{
		static REL::Relocation<std::uint32_t (*)(BGSGrassManager*, CreateGrassParams*)> func{ RELOCATION_ID(15212, 15381) };
		return func(manager, params);
	}

	// Creates missing shapes; cells with the same keys share a shape.
	inline BSMultiStreamInstanceTriShape* LoadGrassType(BGSGrassManager* manager, GrassParam* param, std::uint32_t cellKeyX, std::uint32_t cellKeyY, std::uint64_t* typeKey, BSFixedString* modelKey)
	{
		static REL::Relocation<BSMultiStreamInstanceTriShape* (*)(BGSGrassManager*, GrassParam*, std::uint32_t, std::uint32_t, std::uint64_t*, BSFixedString*)> func{ RELOCATION_ID(15214, 15383) };
		return func(manager, param, cellKeyX, cellKeyY, typeKey, modelKey);
	}

	// Contains 512 entries indexed by the engine's slope-angle conversion.
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

	inline TESObjectLAND* GetLand(TESObjectCELL* cell)
	{
		static REL::Relocation<TESObjectLAND* (*)(TESObjectCELL*)> func{ RELOCATION_ID(18513, 18970) };
		return func(cell);
	}

	inline void LoadLand(TESObjectLAND* land)
	{
		static REL::Relocation<void (*)(TESObjectLAND*, int, int)> func{ RELOCATION_ID(18331, 18747) };
		func(land, 0, 1);
	}

	// Returns false when the point is outside the land's loaded data.
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

	// Creates missing cells; caller must hold the TES lock.
	inline TESObjectCELL* GetWorldCell(TESWorldSpace* world, int x, int y)
	{
		static REL::Relocation<TESObjectCELL* (*)(TESWorldSpace*, std::int32_t, std::int32_t)> func{ RELOCATION_ID(20026, 20460) };
		return func(world, x, y);
	}

	// Creates missing cells without loading or resetting their references.
	inline TESObjectCELL* CreateCellAt(TESDataHandler* dataHandler, int x, int y, TESWorldSpace* world)
	{
		static REL::Relocation<TESObjectCELL* (*)(TESDataHandler*, int, int, TESWorldSpace*, bool)> func{ RELOCATION_ID(13620, 13718) };
		return func(dataHandler, x, y, world, true);
	}

	inline void QueueCellLoad(TESWorldSpace* world, int x, int y)
	{
		static REL::Relocation<void (*)(std::uintptr_t, TESWorldSpace*, int, int, int)> func{ RELOCATION_ID(18150, 18541) };
		static REL::Relocation<std::uintptr_t*> loader{ RELOCATION_ID(514741, 400899) };
		func(*loader, world, x, y, 0);
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

	inline void ShowMessageBox(const char* text, void (*callback)(std::uint8_t), const char* yes, const char* no)
	{
		static REL::Relocation<void (*)(const char*, void (*)(std::uint8_t), std::uint32_t, std::uint32_t, std::uint32_t, const char*, const char*, const char*)> func{ RELOCATION_ID(51420, 52269) };
		func(text, callback, 0, 0x19, 10, yes, no, nullptr);
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

	// Anniversary Edition's box constructor leaves dimensions unset.
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

	// Event types match BSThread: 0 for start, 1 for exit.
	inline void SendHavokThreadEvent(const void* event)
	{
		static REL::Relocation<void (*)(void*, const void*)> func{ RELOCATION_ID(77215, 79103) };
		func(nullptr, event);
	}
}
