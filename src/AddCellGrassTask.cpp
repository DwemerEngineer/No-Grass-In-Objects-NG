#include "GrassControl/AddCellGrassTask.h"

namespace RE
{
	AddCellGrassTask* AddCellGrassTask::Create(TESObjectCELL* cell)
	{
		const auto* coordinates = cell->GetCoordinates();
		if (!coordinates)
			return nullptr;

		auto* task = static_cast<AddCellGrassTask*>(malloc(CellOffset() + 0x18));
		if (!task)
			return nullptr;

		constexpr std::uint8_t GrassTaskPriority = 4;
		static REL::Relocation<void* (*)(void*, std::uint8_t, void*)> queuedFileCtor{ RELOCATION_ID(73994, 75733) };
		queuedFileCtor(task, GrassTaskPriority, nullptr);
		*reinterpret_cast<std::uintptr_t*>(task) = VTABLE_AddCellGrassTask[0].address();

		*reinterpret_cast<TESObjectCELL**>(task->Field(0)) = cell;
		*reinterpret_cast<std::int32_t*>(task->Field(0x8)) = coordinates->cellX;
		*reinterpret_cast<std::int32_t*>(task->Field(0xC)) = coordinates->cellY;
		*task->Abort() = false;
		task->AddReference();
		return task;
	}

	AddCellGrassTask* AddCellGrassTask::FromAbort(bool* abort)
	{
		return reinterpret_cast<AddCellGrassTask*>(reinterpret_cast<std::uintptr_t>(abort) - CellOffset() - 0x10);
	}

	AddCellGrassTask* AddCellGrassTask::Get(const ExtraDataList& extraList)
	{
		static REL::Relocation<void (*)(const ExtraDataList&, AddCellGrassTask**)> func{ RELOCATION_ID(11933, 12072) };
		AddCellGrassTask* task = nullptr;
		func(extraList, &task);
		return task;
	}

	void AddCellGrassTask::Set(const ExtraDataList& extraList, AddCellGrassTask* task)
	{
		static REL::Relocation<void (*)(const ExtraDataList&, AddCellGrassTask*)> func{ RELOCATION_ID(11932, 12071) };
		func(extraList, task);
	}

	void AddCellGrassTask::Queue(TESObjectCELL* cell)
	{
		static REL::Relocation<void (*)(TESObjectCELL*)> func{ RELOCATION_ID(13137, 13277) };
		func(cell);
	}

	void AddCellGrassTask::Release()
	{
		if (InterlockedDecrement(RefCount()) == 0) {
			using Destructor = void(__fastcall*)(void*, bool);
			reinterpret_cast<Destructor>((*reinterpret_cast<void***>(this))[0])(this, true);
		}
	}
}
