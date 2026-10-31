#include "GrassControl/AddCellGrassTask.h"

namespace GrassControl::GrassTasks
{
	RE::NiPointer<RE::AddCellGrassTask> Create(RE::TESObjectCELL* cell)
	{
		const auto* coordinates = cell->GetCoordinates();
		if (!coordinates)
			return {};

		auto* task = static_cast<RE::AddCellGrassTask*>(RE::malloc(RuntimeDataOffset() + sizeof(RE::AddCellGrassTask::RUNTIME_DATA)));
		if (!task)
			return {};

		constexpr std::uint8_t GrassTaskPriority = 4;
		static REL::Relocation<void* (*)(void*, std::uint8_t, void*)> queuedFileConstructor{ RELOCATION_ID(73994, 75733) };
		queuedFileConstructor(task, GrassTaskPriority, nullptr);
		stl::emplace_vtable(task);

		task->GetRuntimeData() = { cell, coordinates->cellX, coordinates->cellY, false, {} };
		return RE::NiPointer(task);
	}

	RE::AddCellGrassTask* FromCancel(bool* cancel)
	{
		return reinterpret_cast<RE::AddCellGrassTask*>(reinterpret_cast<std::byte*>(cancel) - RuntimeDataOffset() - offsetof(RE::AddCellGrassTask::RUNTIME_DATA, cancel));
	}

	RE::NiPointer<RE::AddCellGrassTask> Get(RE::TESObjectCELL* cell)
	{
		RE::BSSpinLockGuard taskLock(cell->grassTaskLock);
		const auto* data = cell->extraList.GetByType<RE::ExtraCellGrassData>();
		return data ? data->addGrassTask : RE::NiPointer<RE::AddCellGrassTask>();
	}

	void Set(RE::TESObjectCELL* cell, RE::AddCellGrassTask* task)
	{
		RE::BSSpinLockGuard taskLock(cell->grassTaskLock);
		auto* data = cell->extraList.GetByType<RE::ExtraCellGrassData>();
		if (!data) {
			if (!task)
				return;

			data = RE::BSExtraData::Create<RE::ExtraCellGrassData>();
			cell->extraList.Add(data);
		}

		data->addGrassTask.reset(task);
	}
}
