#pragma once

namespace RE
{
	// Completes CommonLib's forward declaration, so ExtraCellGrassData::addGrassTask holds this type.
	class AddCellGrassTask final : public QueuedFile
	{
	public:
		inline static constexpr auto RTTI = RTTI_AddCellGrassTask;
		inline static constexpr auto VTABLE = VTABLE_AddCellGrassTask;

		struct RUNTIME_DATA
		{
			TESObjectCELL* cell;  // 00
			std::int32_t cellX;   // 08
			std::int32_t cellY;   // 0C
			bool abort;           // 10
		};

		AddCellGrassTask() = delete;
		AddCellGrassTask(const AddCellGrassTask&) = delete;
		AddCellGrassTask& operator=(const AddCellGrassTask&) = delete;
		~AddCellGrassTask() override;  // 00

		// VR's BSTask is 8 bytes larger, which moves the task's own fields.
		RUNTIME_DATA_ACCESSOR(RUNTIME_DATA, 0x38, 0x40);

		static std::size_t RuntimeDataOffset() { return REL::Relocate<std::size_t>(0x38, 0x38, 0x40); }

		/** @brief Creates an unqueued task for the cell. */
		static NiPointer<AddCellGrassTask> Create(TESObjectCELL* cell);

		static AddCellGrassTask* FromAbort(bool* abort);

		/** @brief The task stored in the cell's grass data, or null. */
		static NiPointer<AddCellGrassTask> Get(TESObjectCELL* cell);

		/** @brief Stores the task in the cell's grass data; null clears it. */
		static void Set(TESObjectCELL* cell, AddCellGrassTask* task);

		static void Queue(TESObjectCELL* cell)
		{
			static REL::Relocation<void (*)(TESObjectCELL*)> func{ RELOCATION_ID(13137, 13277) };
			func(cell);
		}

		bool* Abort() { return &GetRuntimeData().abort; }

		// The reference count NiPointer uses; the last release runs the engine's deleting destructor.
		void IncRefCount() { InterlockedIncrement(reinterpret_cast<volatile long*>(&refCount)); }
		void DecRefCount();
	};
}
