#pragma once

namespace RE
{
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

		// Returns an unqueued task with one reference owned by the caller.
		static AddCellGrassTask* Create(TESObjectCELL* cell);

		static AddCellGrassTask* FromAbort(bool* abort);

		// Adds a reference for the caller when a task exists.
		static AddCellGrassTask* Get(const ExtraDataList& extraList)
		{
			static REL::Relocation<void (*)(const ExtraDataList&, AddCellGrassTask**)> func{ RELOCATION_ID(11933, 12072) };
			AddCellGrassTask* task = nullptr;
			func(extraList, &task);
			return task;
		}

		static void Set(const ExtraDataList& extraList, AddCellGrassTask* task)
		{
			static REL::Relocation<void (*)(const ExtraDataList&, AddCellGrassTask*)> func{ RELOCATION_ID(11932, 12071) };
			func(extraList, task);
		}

		static void Queue(TESObjectCELL* cell)
		{
			static REL::Relocation<void (*)(TESObjectCELL*)> func{ RELOCATION_ID(13137, 13277) };
			func(cell);
		}

		bool* Abort() { return &GetRuntimeData().abort; }

		void AddReference() { InterlockedIncrement(reinterpret_cast<volatile long*>(&refCount)); }

		void Release();
	};
}
