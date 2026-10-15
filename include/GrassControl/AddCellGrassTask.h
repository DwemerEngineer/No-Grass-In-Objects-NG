#pragma once

namespace RE
{
	// The QueuedFile base precedes the cell, coordinates and abort flag.
	// VR adds 8 bytes to QueuedFile, requiring runtime field offsets.
	class AddCellGrassTask final
	{
	public:
		AddCellGrassTask() = delete;
		AddCellGrassTask(const AddCellGrassTask&) = delete;
		AddCellGrassTask& operator=(const AddCellGrassTask&) = delete;

		// Returns an unqueued task with one reference owned by the caller.
		static AddCellGrassTask* Create(TESObjectCELL* cell);

		static AddCellGrassTask* FromAbort(bool* abort);

		// Adds a reference for the caller when a task exists.
		static AddCellGrassTask* Get(const ExtraDataList& extraList);

		static void Set(const ExtraDataList& extraList, AddCellGrassTask* task);

		static void Queue(TESObjectCELL* cell);

		bool* Abort() { return reinterpret_cast<bool*>(Field(0x10)); }

		void AddReference() { InterlockedIncrement(RefCount()); }

		void Release();

	private:
		static std::size_t CellOffset() { return REL::Relocate(0x38, 0x38, 0x40); }

		std::uintptr_t Field(std::size_t offset) { return reinterpret_cast<std::uintptr_t>(this) + CellOffset() + offset; }
		volatile long* RefCount() { return reinterpret_cast<volatile long*>(reinterpret_cast<std::uintptr_t>(this) + 0x8); }
	};
}
