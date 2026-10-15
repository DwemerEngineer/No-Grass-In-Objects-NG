#include "GrassControl/AddCellGrassTask.h"

namespace RE
{
	AddCellGrassTask* AddCellGrassTask::Create(TESObjectCELL* cell)
	{
		const auto* coordinates = cell->GetCoordinates();
		if (!coordinates)
			return nullptr;

		auto* task = static_cast<AddCellGrassTask*>(malloc(RuntimeDataOffset() + sizeof(RUNTIME_DATA)));
		if (!task)
			return nullptr;

		constexpr std::uint8_t GrassTaskPriority = 4;
		static REL::Relocation<void* (*)(void*, std::uint8_t, void*)> queuedFileConstructor{ RELOCATION_ID(73994, 75733) };
		queuedFileConstructor(task, GrassTaskPriority, nullptr);
		stl::emplace_vtable(task);

		task->GetRuntimeData() = { cell, coordinates->cellX, coordinates->cellY, false };
		task->AddReference();
		return task;
	}

	AddCellGrassTask* AddCellGrassTask::FromAbort(bool* abort)
	{
		return reinterpret_cast<AddCellGrassTask*>(reinterpret_cast<std::byte*>(abort) - RuntimeDataOffset() - offsetof(RUNTIME_DATA, abort));
	}

	void AddCellGrassTask::Release()
	{
		// The engine's deleting destructor returns the task to its allocator.
		if (InterlockedDecrement(reinterpret_cast<volatile long*>(&refCount)) == 0)
			delete static_cast<BSTask*>(this);
	}
}
