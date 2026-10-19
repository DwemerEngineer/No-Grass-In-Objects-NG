#include "GrassControl/AddCellGrassTask.h"

namespace RE
{
	NiPointer<AddCellGrassTask> AddCellGrassTask::Create(TESObjectCELL* cell)
	{
		const auto* coordinates = cell->GetCoordinates();
		if (!coordinates)
			return {};

		auto* task = static_cast<AddCellGrassTask*>(malloc(RuntimeDataOffset() + sizeof(RUNTIME_DATA)));
		if (!task)
			return {};

		constexpr std::uint8_t GrassTaskPriority = 4;
		static REL::Relocation<void* (*)(void*, std::uint8_t, void*)> queuedFileConstructor{ RELOCATION_ID(73994, 75733) };
		queuedFileConstructor(task, GrassTaskPriority, nullptr);
		stl::emplace_vtable(task);

		task->GetRuntimeData() = { cell, coordinates->cellX, coordinates->cellY, false };
		return NiPointer(task);
	}

	AddCellGrassTask* AddCellGrassTask::FromAbort(bool* abort)
	{
		return reinterpret_cast<AddCellGrassTask*>(reinterpret_cast<std::byte*>(abort) - RuntimeDataOffset() - offsetof(RUNTIME_DATA, abort));
	}

	NiPointer<AddCellGrassTask> AddCellGrassTask::Get(TESObjectCELL* cell)
	{
		BSSpinLockGuard taskLock(cell->grassTaskLock);
		const auto* data = cell->extraList.GetByType<ExtraCellGrassData>();
		return data ? data->addGrassTask : NiPointer<AddCellGrassTask>();
	}

	void AddCellGrassTask::Set(TESObjectCELL* cell, AddCellGrassTask* task)
	{
		BSSpinLockGuard taskLock(cell->grassTaskLock);
		auto* data = cell->extraList.GetByType<ExtraCellGrassData>();
		if (!data) {
			if (!task)
				return;

			data = BSExtraData::Create<ExtraCellGrassData>();
			cell->extraList.Add(data);
		}

		data->addGrassTask.reset(task);
	}

	void AddCellGrassTask::DecRefCount()
	{
		if (InterlockedDecrement(reinterpret_cast<volatile long*>(&refCount)) == 0)
			delete static_cast<BSTask*>(this);
	}
}
