#pragma once

namespace GrassControl::GrassTasks
{
	// VR's BSTask is eight bytes larger, shifting the task's runtime data.
	inline std::size_t RuntimeDataOffset() { return REL::Relocate<std::size_t>(0x38, 0x38, 0x40); }

	RE::NiPointer<RE::AddCellGrassTask> Create(RE::TESObjectCELL* cell);

	RE::AddCellGrassTask* FromCancel(bool* cancel);

	RE::NiPointer<RE::AddCellGrassTask> Get(RE::TESObjectCELL* cell);

	void Set(RE::TESObjectCELL* cell, RE::AddCellGrassTask* task);
}
