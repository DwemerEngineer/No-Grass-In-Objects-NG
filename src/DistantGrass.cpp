#include "GrassControl/DistantGrass.h"
using namespace Xbyak;

namespace GrassControl
{
	static bool AE = REL::Module::IsAE();

	DistantGrass::cell_info::cell_info(const int _x, const int _y) :
		x(_x), y(_y)
	{
	}

	DistantGrass::cell_info::cell_info() :
		x(0), y(0)
	{
	}

	bool DistantGrass::cell_info::checkHasFile(const std::string& wsName) const
	{
		if (wsName.empty()) {
			return false;
		}

		std::string x_str = x < 0 ? std::string(3, '0').append(std::to_string(x)) : std::string(4, '0').append(std::to_string(x));
		std::string y_str = y < 0 ? std::string(3, '0').append(std::to_string(y)) : std::string(4, '0').append(std::to_string(y));
		std::string fpath = "Data/Grass/" + wsName + "x" + x_str + "y" + y_str + ".cgid";

		bool has = false;
		try {
			if (std::filesystem::exists(fpath)) {
				has = true;
			}
		} catch (...) {
			logger::error("Error occured while checking for cache files for cell");
		}

		return has;
	}

	DistantGrass::CellInfoContainer::CellInfoContainer() :
		grid(std::vector<std::shared_ptr<cell_info>>(16641))  // 129 * 129
	{
		for (int x = 0; x <= 128; x++) {
			for (int y = 0; y <= 128; y++) {
				grid[x * 129 + y] = std::make_shared<cell_info>(x - 64, y - 64);
			}
		}
	}

	std::shared_ptr<DistantGrass::cell_info> DistantGrass::CellInfoContainer::GetFromGrid(int x, int y) const
	{
		if (x < -64 || y < -64 || x > 64 || y > 64)
			return {};

		x += 64;
		y += 64;
		return grid[x * 129 + y];
	}

	void DistantGrass::CellInfoContainer::unsafe_ForeachWithState(const std::function<bool(std::shared_ptr<cell_info>)>& action)
	{
		try {
			std::erase_if(this->map, [&](const auto& pair) {
				auto& [fst, snd] = pair;
				if (fst != nullptr) {
					if (!action(snd)) {
						return true;
					}
				}
				return false; 
			});
		} catch (...) {
			logger::error("Exception occurred while trying to erase cell from loaded reference map. Attempting to continue.");
			//Terrible idea but fuck it. Erase keeps throwing a read access violation in Debug. Release should be fine, but just in case.
			//Probably because of Debug Iterators
		}
	}

	std::shared_ptr<DistantGrass::cell_info> DistantGrass::CellInfoContainer::FindByCell(RE::TESObjectCELL* cell)
	{
		{
			std::scoped_lock lock(cellMapMutex);

			auto it = this->map.find(cell);
			if (it != this->map.end()) {
				return it->second;
			}
			return {};
		}
	}

	int DistantGrass::LoadOnlyCellInfoContainer2::GetCount() const
	{
		int count = 0;
		{
			std::scoped_lock lock(cellMapMutex);
			for (const auto& val : map | std::views::values) {
				if (val->State != _cell_data::_cell_states::None) {
					count++;
				}
			}
		}
		return count;
	}

	std::string DistantGrass::LoadOnlyCellInfoContainer2::MakeKey(const std::string& ws, int x, int y)
	{
		std::string rtn;
		try {
			rtn = fmt::format("{}_{}_{}", x, y, ws);
		} catch (fmt::format_error& e) {
			logger::error("Error occured while extending Grass Distance:");
			logger::error(fmt::runtime(e.what()));
		}

		return rtn;
	}

	void DistantGrass::LoadOnlyCellInfoContainer2::UpdatePositionWithRemove(RE::TESWorldSpace* ws, int addType, int nowX, int nowY, int grassRadius)
	{
		unsigned int wsId = ws != nullptr ? ws->formID : 0;

		std::vector<std::pair<std::string, std::shared_ptr<_cell_data>>> queueDelete;

		{
			std::scoped_lock lock(cellMapMutex);
			for (const auto& [key, val] : this->map) {
				if (!val || val->State == _cell_data::_cell_states::None) {
					continue;
				}

				bool shouldKeep = addType >= 0 && val->WsId == wsId && std::abs(val->X - nowX) <= grassRadius && std::abs(val->Y - nowY) <= grassRadius;

				if (!shouldKeep) {
					queueDelete.push_back({ key, val });
				}
			}

			for (auto& pair : queueDelete) {
				bool erase = _DoUnload(pair.second);
				if (erase) {
					this->map.erase(pair.first);
				}
			}
		}
	}

	void DistantGrass::LoadOnlyCellInfoContainer2::QueueLoad(RE::TESWorldSpace* ws, int x, int y)
	{
		if (ws == nullptr)
			return;

		std::string key = MakeKey(ws->editorID.c_str(), x, y);
		if (key.empty())
			return;

		{
			std::scoped_lock lock(cellMapMutex);

			std::shared_ptr<_cell_data> d;

			if (!this->map.contains(key)) {
				d = std::make_shared<_cell_data>();
				this->map.insert_or_assign(key, d);

				d->X = x;
				d->Y = y;
				d->WsId = ws != nullptr ? ws->formID : 0;
			}

			if (d == nullptr)
				return;

			{
				if (d->State != _cell_data::_cell_states::None) {
					return;
				}

				d->State = _cell_data::_cell_states::Loading;

				if (d->DummyCell_Ptr == nullptr) {
					char* tempPtr = nullptr;
					if (REL::Relocate(false, REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_6_629)) {
						tempPtr = new char[0x148];
						memset(tempPtr, 0, 0x148);
					} else {
						tempPtr = new char[0x140];
						memset(tempPtr, 0, 0x140);
					}

					d->DummyCell_Ptr = reinterpret_cast<RE::TESObjectCELL*>(tempPtr);

					auto exteriorData = new RE::EXTERIOR_DATA();
					exteriorData->cellX = x;
					exteriorData->cellY = y;

					d->DummyCell_Ptr->GetRuntimeData().cellData.exterior = exteriorData;
					d->DummyCell_Ptr->GetRuntimeData().worldSpace = ws;
				}

				using func_t = void (*)(RE::TESObjectCELL*);
				REL::Relocation<func_t> func{ RELOCATION_ID(13137, 13277) };

				func(d->DummyCell_Ptr);
			}
		}
	}

	bool DistantGrass::LoadOnlyCellInfoContainer2::_DoUnload(std::shared_ptr<_cell_data>& cellData)
	{
		if (cellData == nullptr)
			return false;

		{
			std::scoped_lock lock(cellMapMutex);

			if (!cellData || cellData->State == _cell_data::_cell_states::None || cellData->State == _cell_data::_cell_states::Abort) {
				return false;
			}

			if (cellData->State == _cell_data::_cell_states::Loading) {
				cellData->State = _cell_data::_cell_states::Abort;
				return false;
			}

			if (cellData->DummyCell_Ptr) {
				REL::Relocation<void (*)(RE::BGSGrassManager*, RE::TESObjectCELL*)> func{ RELOCATION_ID(15207, 15375) };
				func(RE::BGSGrassManager::GetSingleton(), cellData->DummyCell_Ptr);

				REL::Relocation<void (*)(RE::TESObjectCELL*, uintptr_t)> func2{ RELOCATION_ID(11932, 12071) };
				func2(cellData->DummyCell_Ptr, 0);

				delete cellData->DummyCell_Ptr->GetRuntimeData().cellData.exterior;
			}

			cellData->DummyCell_Ptr = nullptr;
			cellData->State = _cell_data::_cell_states::None;

			return true;
		}
	}

	void DistantGrass::LoadOnlyCellInfoContainer2::Unload(const RE::TESWorldSpace* ws, const int x, const int y)
	{
		if (ws == nullptr)
			return;

		std::string key = MakeKey(ws->editorID.c_str(), x, y);
		std::shared_ptr<_cell_data> data;

		{
			std::scoped_lock lock(cellMapMutex);

			if (!this->map.empty()) {
				auto it = this->map.find(key);
				data = it == this->map.end() ? nullptr : it->second;
			}

			if (data == nullptr)
				return;

			if (_DoUnload(data)) {
				this->map.erase(key);
			}
		}
	}

	void DistantGrass::LoadOnlyCellInfoContainer2::_DoLoad(const RE::TESWorldSpace* ws, const int x, const int y) const
	{
		if (ws == nullptr)
			return;

		std::string key = MakeKey(ws->editorID.c_str(), x, y);
		std::shared_ptr<_cell_data> d;
		{
			std::scoped_lock lock(cellMapMutex);
			auto it = this->map.find(key);
			d = it == this->map.end() ? nullptr : it->second;
		}

		if (d == nullptr)
			return;

		{
			std::scoped_lock lock(cellMapMutex);
			if (d->State == _cell_data::_cell_states::Loaded) {
				// This shouldn't happen
			} else if (d->State == _cell_data::_cell_states::Loading) {
				REL::Relocation<void (*)(RE::BGSGrassManager*, RE::TESObjectCELL*)> func{ RELOCATION_ID(15206, 15374) };

				if (d->DummyCell_Ptr == nullptr)
					return;

				func(RE::BGSGrassManager::GetSingleton(), d->DummyCell_Ptr);
				d->State = _cell_data::_cell_states::Loaded;
			} else if (d->DummyCell_Ptr != nullptr) {
				d->DummyCell_Ptr = nullptr;
				d->State = _cell_data::_cell_states::None;
			} else {
				d->State = _cell_data::_cell_states::None;  // this shouldn't happen?
			}
		}
	}

	void DistantGrass::LoadOnlyCellInfoContainer2::UnloadAll()
	{
		for (auto& pair : this->map) {
			_DoUnload(pair.second);
		}

		this->map.clear();
	}

	void DistantGrass::RemoveGrassHook(RE::TESObjectCELL* cell, uintptr_t arg_1)
	{
		if (cell != nullptr) {
			auto c = Map->FindByCell(cell);
			if (c && (c->self_data & 0xFF) != 0) {
				logger::debug("RemoveGrassDueToAdd({}, {})", c->x, c->y);

				REL::Relocation<void (*)(uintptr_t, RE::TESObjectCELL*)> func{ RELOCATION_ID(15207, 15375) };
				func(arg_1, cell);
			}

			if (!c) {
				logger::debug("RemoveGrassDueToAdd(null): warning: c == null");
			}
		} else if (Config::DebugLogEnable) {
			logger::debug("RemoveGrassDueToAdd(null)");
		}
	}

	void DistantGrass::CellUnloadHook(bool did, RE::TESObjectCELL* cellObj)
	{
		logger::debug("DataHandler::UnloadCell({}, {}) did: {}", cellObj->GetCoordinates()->cellX, cellObj->GetCoordinates()->cellY, did);
	}

	static void ThrowOurMethodException(const uint8_t ourMethod)
	{
		stl::report_and_fail("GrassControl.dll: unexpected ourMethod: " + std::to_string(ourMethod));
	}

	unsigned char DistantGrass::CellLoadHook(const int x, const int y)
	{
		unsigned char isOurMethod = 0;
		auto c = Map->GetFromGrid(x, y);
		if (c && InterlockedCompareExchange64(&c->furtherLoad, 2, 1) == 1) {
			isOurMethod = 1;
		}
		return isOurMethod;
	}

	void DistantGrass::ReplaceGrassGrid(const bool _loadOnly)
	{
		DidApply = true;
		load_only = _loadOnly;

		if (load_only) {
			LO2Map = std::make_unique<LoadOnlyCellInfoContainer2>();
			logger::debug("LO2Map created at {:x}", reinterpret_cast<uintptr_t>(LO2Map.get()));
		} else {
			Map = std::make_unique<CellInfoContainer>();
			logger::debug("Map created at {:x}", reinterpret_cast<uintptr_t>(Map.get()));
		}

		// Disable grass fade.
		if (!load_only) {
			auto addr = RELOCATION_ID(38141, 39098).address();
			Memory::Internal::write<uint8_t>(addr, 0xC3, true);

			// There's a chance this below isn't actually used so if it fails no problem.
			try {
				if (addr = (RELOCATION_ID(13240, 13391).address() + (0x55E - 0x480)); REL::make_pattern<"E8">().match(addr)) {
					REL::safe_write(addr + 5, REL::NOP5, 5);
				}
			} catch (...) {
			}
		}

		// Auto use correct iGrassCellRadius.
		if (!load_only) {
			int radius = getChosenGrassGridRadius();
			auto setting = RE::INISettingCollection::GetSingleton()->GetSetting("iGrassCellRadius:Grass");
			setting->data.i = radius;
		}

		// Unload old grass, load new grass. But we have replaced how the grid works now.
		auto addr = RELOCATION_ID(13148, 13288).address() + REL::Relocate(0xA06 - 0x220, 0xA1D);
		struct Patch : CodeGenerator
		{
			Patch(std::uintptr_t a_func, uintptr_t a_target, Reg a_Y, Reg a_X)
			{
				Label funcLabel;
				Label retnLabel;

				push(rdx);
				push(rcx);

				mov(r9d, 0);
				mov(r8d, a_Y);  // movedY
				mov(edx, a_X);  // movedX
				mov(rcx, rbx);

				sub(rsp, 0x20);
				call(ptr[rip + funcLabel]);
				add(rsp, 0x20);

				pop(rdx);
				pop(rcx);
				jmp(ptr[rip + retnLabel]);

				L(funcLabel);
				dq(a_func);

				L(retnLabel);
				dq(a_target);
			}
		};
		Patch patch(reinterpret_cast<uintptr_t>(UpdateGrassGridNow), addr + REL::Relocate(0x159, 0x17D), Reg32(REL::Relocate(Reg::R14D, Reg::EDI)), Reg32(REL::Relocate(Reg::EBP, Reg::R14D)));
		patch.ready();

		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_branch<6>(addr, trampoline.allocate(patch));

		// cell dtor
		if (!load_only) {
			addr = RELOCATION_ID(18446, 18877).address() + (0xC4 - 0x50);
			struct Patch4 : CodeGenerator
			{
				Patch4(uintptr_t a_func, uintptr_t a_target)
				{
					Label funcLabel;
					Label retnLabel;
					Label NotIf;
					Label addr_ClearGrassHandles;

					mov(rcx, rdi);

					sub(rsp, 0x20);
					call(ptr[rip + funcLabel]);
					add(rsp, 0x20);

					test(rax, rax);
					je(NotIf);
					lea(rcx, ptr[rdi + 0x48]);

					sub(rsp, 0x20);
					call(ptr[rip + addr_ClearGrassHandles]);
					add(rsp, 0x20);
					jmp(NotIf);

					L(NotIf);
					jmp(ptr[rip + retnLabel]);

					L(funcLabel);
					dq(a_func);

					L(addr_ClearGrassHandles);
					dq(RELOCATION_ID(11931, 12070).address());

					L(retnLabel);
					dq(a_target + 0x5 + (0xE9 - 0xC9));
				}
			};
			Patch4 patch4(reinterpret_cast<uintptr_t>(Handle_RemoveGrassFromCell_Call), addr);
			patch4.ready();

			Util::NopBlock(addr, 0xE9 - 0xC9, 5);
			trampoline.write_branch<5>(addr, trampoline.allocate(patch4));
		}

		// unloading cell
		if (!load_only) {
			addr = RELOCATION_ID(13623, 13721).address() + REL::Relocate(0xC0F8 - 0xBF90, 0x1E8);
			REL::safe_write(addr, REL::NOP5, 5);
		}

		// remove just before read grass so there's not so noticeable fade-in / fade-out, there is still somewhat noticeable effect though
		if (!load_only) {
			addr = RELOCATION_ID(15204, 15372).address() + REL::Relocate(0x8F - 0x10, 0x7D);
			struct Patch5 : CodeGenerator
			{
				Patch5(std::uintptr_t a_func, uintptr_t a_target)
				{
					Label funcLabel;
					Label funcLabel2;
					Label retnLabel;

					Label NotIf;

					sub(rsp, 0x20);
					call(ptr[rip + funcLabel2]);
					add(rsp, 0x20);

					mov(rcx, rsi);
					mov(rdx, r13);

					sub(rsp, 0x20);
					call(ptr[rip + funcLabel]);
					add(rsp, 0x20);

					jmp(ptr[rip + retnLabel]);

					L(funcLabel);
					dq(a_func);

					L(funcLabel2);
					dq(RELOCATION_ID(12210, 13129).address());

					L(retnLabel);
					dq(a_target + 0x5);
				}
			};
			Patch5 patch5(reinterpret_cast<uintptr_t>(RemoveGrassHook), addr);
			patch5.ready();

			trampoline.write_branch<5>(addr, trampoline.allocate(patch5));
		}

		addr = RELOCATION_ID(15214, 15383).address() + REL::Relocate(0x78B7 - 0x7830, 0x7E);
		struct Patch9 : CodeGenerator
		{
			explicit Patch9(uintptr_t a_target, Reg a_X, uintptr_t rsp_offset, uintptr_t rsp_stackOffset)
			{
				Label retnLabel;

				mov(ptr[rsp + rsp_offset + 4], a_X.cvt16());
				mov(ptr[rsp + rsp_offset + 6], bx);

				movsx(eax, a_X.cvt16());  // x
				mov(a_X, ecx);
				cdq();
				mov(ecx, 12);  // x/12
				idiv(ecx);

				mov(ecx, a_X);
				mov(a_X.cvt32(), eax);

				movsx(eax, bx);  // y
				mov(ebx, ecx);
				cdq();
				mov(ecx, 12);  // y/12
				idiv(ecx);
				mov(ecx, ebx);
				mov(ebx, eax);

				mov(ptr[rsp + rsp_stackOffset + 0x58], a_X.cvt32());  // x
				mov(ptr[rsp + rsp_stackOffset + 0x60], ebx);          // y

				jmp(ptr[rip + retnLabel]);

				L(retnLabel);
				dq(a_target + 0xB);
			}
		};
		Patch9 patch9(addr, Reg32(REL::Relocate(Reg::R15, Reg::R14)), REL::Relocate(0x48, 0x50), REL::Relocate(0x258, 0x288));
		patch9.ready();

		Util::NopBlock(addr, 0xB, 0);
		trampoline.write_branch<5>(addr, trampoline.allocate(patch9));

		// Exterior cell buffer must be extended if grass radius is outside of ugrids.
		// Reason: cell may get deleted while it still has grass and we can not keep grass there then.
		if (!load_only) {
			addr = RELOCATION_ID(13233, 13384).address() + (0xB2 - 0x60);
			int ugrids = Memory::Internal::read<int>(addr_uGrids + 8);
			int ggrids = getChosenGrassGridRadius() * 2 + 1;
			int Max = std::max(ugrids, ggrids);
			struct Patch10 : CodeGenerator
			{
				explicit Patch10(uintptr_t a_target, int max)
				{
					Label retnLabel;

					mov(rax, max);

					jmp(ptr[rip + retnLabel]);

					L(retnLabel);
					dq(a_target + 0x6);
				}
			};
			Patch10 patch10(addr, Max);
			patch10.ready();
			trampoline.write_branch<6>(addr, trampoline.allocate(patch10));
		}

		// Allow grass distance to extend beyond uGrids * 4096 units (20480).
		addr = RELOCATION_ID(15202, 15370).address() + REL::Relocate(0xB06 - 0x890, 0x279);
		Memory::Internal::write<uint8_t>(addr + 2, 16, true);

		addr = RELOCATION_ID(528751, 15379).address() + REL::Relocate(0xFE - 0xE0, 0x1E);
		Memory::Internal::write<uint8_t>(addr + 2, 16, true);

		// Allow creating grass without a land mesh. This is still necessary!
		if (!load_only) {
			addr = RELOCATION_ID(15204, 15372).address() + (0xED2 - 0xD10);
			struct Patch11 : CodeGenerator
			{
				explicit Patch11(uintptr_t a_target)
				{
					Label retnLabel;

					mov(rax, 1);
					jmp(ptr[rip + retnLabel]);

					L(retnLabel);
					dq(a_target + 0x5);
				}
			};
			Patch11 patch11(addr);
			patch11.ready();

			trampoline.write_branch<5>(addr, trampoline.allocate(patch11));
		}

		// Cell unload should clear queued task. Otherwise, it will crash or not allow creating grass again later.
		if (!load_only) {
			addr = RELOCATION_ID(18655, 19130).address() + REL::Relocate(0xC888 - 0xC7C0, 0xCA);
			struct Patch12 : CodeGenerator
			{
				Patch12(std::uintptr_t a_func, uintptr_t b_func, uintptr_t a_target)
				{
					Label funcLabel;
					Label funcLabel2;
					Label retnLabel;

					mov(r15, rax);
					mov(rcx, rdi);

					sub(rsp, 0x20);
					call(ptr[rip + funcLabel2]);
					add(rsp, 0x20);

					movzx(ecx, al);
					mov(rdx, rdi);

					sub(rsp, 0x20);
					call(ptr[rip + funcLabel]);
					add(rsp, 0x20);

					mov(rax, r15);

					jmp(ptr[rip + retnLabel]);

					L(funcLabel);
					dq(a_func);

					L(funcLabel2);
					dq(b_func);

					L(retnLabel);
					dq(a_target + 0x5);
				}
			};
			Patch12 patch12(reinterpret_cast<uintptr_t>(CellUnloadHook), reinterpret_cast<uintptr_t>(ClearCellAddGrassTask), addr);
			patch12.ready();

			trampoline.write_branch<5>(addr, trampoline.allocate(patch12));
		}

		// Create custom way to load cell.
		if (!load_only) {
			addr = RELOCATION_ID(18137, 18527).address() + REL::Relocate(0x17, 0x25);
			struct Patch13 : CodeGenerator
			{
				Patch13(std::uintptr_t a_func, std::uintptr_t b_func, uintptr_t a_rbxWorldSpaceOffset, uintptr_t a_targetReturn, uintptr_t a_targetSkip)
				{
					Label funcLabel;
					Label funcLabel2;
					Label retnLabel;
					Label skip;
					Label Call;

					Label j_else;
					Label notIf;
					Label include;

					mov(al, ptr[rbx + a_rbxWorldSpaceOffset + 0x1E]);  //if ourMethod == 1
					cmp(al, 1);
					jne(j_else);

					mov(rcx, ptr[rbx + a_rbxWorldSpaceOffset]);         // ws
					mov(edx, ptr[rbx + a_rbxWorldSpaceOffset + 0x10]);  // x
					mov(r8d, ptr[rbx + a_rbxWorldSpaceOffset + 0x14]);  // y

					sub(rsp, 0x20);
					call(ptr[rip + funcLabel]);
					add(rsp, 0x20);

					jmp(ptr[rip + skip]);

					L(j_else);
					cmp(al, 0);
					je(notIf);

					mov(cl, al);

					sub(rsp, 0x20);
					call(ptr[rip + funcLabel2]);  // exception
					add(rsp, 0x20);

					jmp(include);

					L(notIf);
					jmp(include);

					L(include);
					if (AE) {
						call(ptr[rip + Call]);
					} else {
						mov(rcx, ptr[rbx + a_rbxWorldSpaceOffset - 0x8]);  // lock
						lea(rdx, ptr[rbx + a_rbxWorldSpaceOffset]);        // worldspace
					}
					jmp(ptr[rip + retnLabel]);

					L(funcLabel);
					dq(a_func);

					L(funcLabel2);
					dq(b_func);

					L(retnLabel);
					dq(a_targetReturn);

					L(Call);
					dq(RELOCATION_ID(18637, 19111).address());

					L(skip);
					dq(a_targetSkip);
				}
			};
			Patch13 patch13(reinterpret_cast<uintptr_t>(CellLoadNow_Our), reinterpret_cast<uintptr_t>(ThrowOurMethodException), REL::Relocate(0x20, 0x20, 0x28), addr + REL::Relocate(0x8, 0x5), addr + REL::Relocate(0x8 + (0xA9 - 0x8F), 0x141));
			patch13.ready();

			if (!AE) {
				REL::safe_write(addr + 5, REL::NOP3, 3);
			}
			trampoline.write_branch<5>(addr, trampoline.allocate(patch13));

			addr = RELOCATION_ID(18150, 18541).address() + REL::Relocate(0xB094 - 0xAF20, 0x1CA, 0x177);
			struct Patch14 : CodeGenerator
			{
				Patch14(uintptr_t a_func, uintptr_t a_target, uintptr_t rbx_offset)
				{
					Label funcLabel;
					Label retnLabel;

					if (AE)
						movzx(eax, ptr[rsp + 0xA0]);
					else
						mov(esi, ptr[rsp + 0x30]);
					mov(ptr[rbx + rbx_offset + 0xd], al);

					mov(rcx, ptr[rbx + rbx_offset]);        // x
					mov(rdx, ptr[rbx + rbx_offset + 0x4]);  // y

					sub(rsp, 0x20);
					call(ptr[rip + funcLabel]);
					add(rsp, 0x20);

					mov(byte[rbx + rbx_offset + 0xe], al);

					jmp(ptr[rip + retnLabel]);

					L(funcLabel);
					dq(a_func);

					L(retnLabel);
					dq(a_target);
				}
			};
			Patch14 patch14(reinterpret_cast<uintptr_t>(CellLoadHook), addr + REL::Relocate(0x7, 0xB), REL::Relocate(0x30, 0x30, 0x38));
			patch14.ready();
			if (!AE) {
				REL::safe_write(addr + 5, REL::NOP2, 2);
			} else {
				REL::safe_write(addr + 5, REL::NOP6, 6);
			}
			trampoline.write_branch<5>(addr, trampoline.allocate(patch14));

			addr = RELOCATION_ID(18149, 18540).address() + REL::Relocate(0xE1B - 0xCC0, 0x167, 0x15E);
			struct Patch15 : CodeGenerator
			{
				explicit Patch15(uintptr_t a_target, uintptr_t rbx_offset, Reg a_source)
				{
					Label retnLabel;
					mov(byte[rbx + rbx_offset + 0x2], 0);

					mov(byte[rbx + rbx_offset], 1);

					mov(ptr[rbx + rbx_offset + 0x1], a_source);
					jmp(ptr[rip + retnLabel]);

					L(retnLabel);
					dq(a_target + 0x8);
				}
			};
			Patch15 patch15(addr, REL::Relocate(0x3c, 0x3c, 0x44), Reg8(REL::Relocate(Reg::R15B, Reg::BPL)));
			patch15.ready();

			REL::safe_write(addr + 6, REL::NOP2, 2);
			trampoline.write_branch<5>(addr, trampoline.allocate(patch15));

			addr = RELOCATION_ID(13148, 13288).address() + REL::Relocate(0x2630 - 0x2220, 0x4D0);
			struct Patch16 : CodeGenerator
			{
				Patch16(std::uintptr_t a_func, uintptr_t a_target, Reg a_movedY, uintptr_t a_retn_offset)
				{
					Label funcLabel;
					Label retnLabel;
					Label notIf;
					Label jump;
					Label jumpAd;

					push(r9);
					push(rdx);
					push(rax);
					push(rsi);
					if (AE) {
						movsx(r8d, r12w);  //movedX
					} else {
						mov(r8d, ptr[rsp + 0x90]);  //movedX
					}
					movsx(r9d, a_movedY);  //movedY
					mov(esi, r8d);

					mov(rax, ptr[rbx + 0x140]);  // ws
					test(rax, rax);
					jne(notIf);

					pop(r9);
					pop(rdx);
					pop(rax);
					pop(rsi);
					cmp(esi, 0);
					jmp(ptr[rip + retnLabel]);

					L(notIf);
					mov(ecx, ptr[rbx + 0xB8]);  // prevX
					mov(edx, ptr[rbx + 0xBC]);  // prevY

					sub(rsp, 0x20);
					call(ptr[rip + funcLabel]);
					add(rsp, 0x20);

					pop(r9);
					pop(rdx);
					pop(rax);
					pop(rsi);
					if (AE) {
						test(r12w, r12w);
						jz(jump);
					} else
						cmp(esi, 0);
					jmp(ptr[rip + retnLabel]);

					L(funcLabel);
					dq(a_func);

					L(jumpAd);
					dq(a_target + 0x2C);

					L(jump);
					jmp(ptr[rip + jumpAd]);

					L(retnLabel);
					dq(a_target + a_retn_offset);
				}
			};
			Patch16 patch16(reinterpret_cast<uintptr_t>(UpdateGrassGridQueue), addr, Reg16(REL::Relocate(Reg16::R13W, Reg16::R15W)), REL::Relocate(0x9, 0x6));
			patch16.ready();

			if (AE) {
				trampoline.write_branch<6>(addr, trampoline.allocate(patch16));
			} else {
				REL::safe_write(addr + 5, REL::NOP4, 4);
				trampoline.write_branch<5>(addr, trampoline.allocate(patch16));
			}
		}
	}

	float DistantGrass::getChosenGrassFadeRange()
	{
		float r = static_cast<float>(Config::OverwriteGrassFadeRange);
		if (r < 0.0f) {
			auto setting = RE::INISettingCollection::GetSingleton()->GetSetting("fGrassFadeRange:Grass");
			r = setting->data.f;
		}

		return r;
	}

	int DistantGrass::getChosenGrassGridRadius()
	{
		int r = _chosenGrassGridRadius;
		if (r < 0) {
			float dist = static_cast<float>(Config::OverwriteGrassDistance);
			if (dist < 0.0f) {
				auto addr = RELOCATION_ID(501108, 359413).address();
				dist = Memory::Internal::read<float>(addr + 8);
			}

			float range = getChosenGrassFadeRange();

			float total = std::max(0.0f, dist) + std::max(0.0f, range);
			float cells = total / 4096.0f;

			int icells = static_cast<int>(std::ceil(cells));

			switch (Config::DynDOLODGrassMode) {
			case 1:
				icells = Memory::Internal::read<int>(addr_uGrids + 8) / 2;
				break;

			case 2:
				icells = Memory::Internal::read<int>(addr_uLargeRef + 8) / 2;
				break;
			default:
				break;
			}

			if (icells < 2) {
				r = 2;
			} else if (icells > 32) {
				logger::info("GrassControl: calculated iGrassCellRadius is {}! Using 32 instead. This probably means fGrassFadeRange or fGrassFadeStartDistance is very strange value.", icells);
				r = 32;
			} else {
				r = icells;
			}

			int imin = 0;
			if (Config::OverwriteMinGrassSize >= 0) {
				imin = Config::OverwriteMinGrassSize;
			} else {
				try {
					imin = Memory::Internal::read<int>(RELOCATION_ID(501113, 359421).address() + 8);
				} catch (...) {
				}
			}

			logger::debug("ChosenGrassGridRadius(null) chose: {}, dist: {}, range: {}, iMinGrassSize: {}", r, dist, range, imin);

			logger::info("ChosenGrassSettings: grid: {}, dist: {}, range: {}, iMinGrassSize: {}", r, dist, range, imin);

			_chosenGrassGridRadius = r;
		}

		return r;
	}

	int DistantGrass::_chosenGrassGridRadius = -1;
	bool DistantGrass::DidApply = false;

	void DistantGrass::CellLoadNow_Our(uintptr_t ws, int x, int y)
	{
		auto c = Map->GetFromGrid(x, y);
		if (!c || InterlockedCompareExchange64(&c->furtherLoad, 0, 2) != 2) {
			return;
		}

		if (ws == 0)
			return;

		REL::Relocation<RE::TESObjectCELL* (*)(uintptr_t, int, int)> func{ RELOCATION_ID(20026, 20460) };

		auto cell = func(ws, x, y);
		if (IsValidLoadedCell(cell, false)) {
			if (*cell->GetName()) {
				logger::debug(fmt::runtime("FurtherLoadSuccessFirst({}, {}) Name: " + std::string(cell->GetName()) + ", FormId: {:x}"), x, y, cell->formID);
			} else {
				logger::debug(fmt::runtime("FurtherLoadSuccessFirst({}, {}) Name: Wilderness, FormId: {:x}"), x, y, cell->formID);
			}
		} else if (cell != nullptr) {
			auto landPtr = cell->GetRuntimeData().cellLand;
			if (landPtr != nullptr && (Memory::Internal::read<uint8_t>(&(landPtr->data.flags)) & 8) == 0) {
				REL::Relocation<void (*)(RE::TESObjectLAND*, int, int)> fnc{ RELOCATION_ID(18331, 18747) };

				fnc(landPtr, 0, 1);

				if (IsValidLoadedCell(cell, false)) {
					logger::debug(fmt::runtime("FurtherLoadSuccessSecond({}, {}) {}"), c->x, c->y, cell->formID);
				}
			}
		}
	}

	bool DistantGrass::ClearCellAddGrassTask(const RE::TESObjectCELL* cell)
	{
		if (cell == nullptr)
			return false;

		void* task = nullptr;

		REL::Relocation<void (*)(const RE::ExtraDataList&, void**)> GetAddGrassTask{ RELOCATION_ID(11933, 12072) };
		GetAddGrassTask(cell->extraList, &task);

		if (task == nullptr)
			return false;

		Memory::Internal::write<uint8_t>(reinterpret_cast<uintptr_t>(task) + 0x48, 1);
		auto refPtr = reinterpret_cast<volatile long*>(reinterpret_cast<uintptr_t>(task) + 8);
		if (InterlockedDecrement(refPtr) == 0) {
			auto vtable = *reinterpret_cast<void***>(task);
			using Dtor = void(__fastcall*)(void*, bool);
			auto dtor = reinterpret_cast<Dtor>(vtable[0]);
			dtor(task, 1);
		}

		REL::Relocation<void (*)(const RE::ExtraDataList&, void*)> SetAddGrassTask{ RELOCATION_ID(11932, 12071) };

		SetAddGrassTask(cell->extraList, nullptr);

		return true;
	}

	bool DistantGrass::IsValidLoadedCell(RE::TESObjectCELL* cell, bool quickLoad)
	{
		if (cell == nullptr)
			return false;

		if (quickLoad)
			return true;

		auto land = cell->GetRuntimeData().cellLand;
		if (land == nullptr)
			return false;

		auto data = land->loadedData;
		if (data == nullptr)
			return false;

		return true;
	}

	RE::TESObjectCELL* DistantGrass::GetCurrentWorldspaceCell(const RE::TES* tes, RE::TESWorldSpace* ws, int x, int y, bool quickLoad, bool allowLoadNow)
	{
		auto data = Memory::Internal::read<uintptr_t>(addr_DataHandler);
		if (data == 0 || ws == nullptr)
			return nullptr;

		using func_t = RE::TESObjectCELL* (*)(const RE::TES*, int, int);
		REL::Relocation<func_t> func{ RELOCATION_ID(13174, 13319) };

		RE::TESObjectCELL* cell = func(tes, x, y);
		if (IsValidLoadedCell(cell, quickLoad)) {
			if (*cell->GetName()) {
				logger::debug(fmt::runtime("add_GetCell({}, {}) (0): Name: " + std::string(cell->GetName()) + ", FormId: {:X}"), x, y, cell->formID);
			} else {
				logger::debug(fmt::runtime("add_GetCell({}, {}) (0): Name: Wilderness, FormId: {:X}"), x, y, cell->formID);
			}

			return cell;
		}

		if (allowLoadNow) {
			auto c = Map->GetFromGrid(x, y);
			if (c) {
				InterlockedCompareExchange64(&c->furtherLoad, 0, 2);
			}
			{
				tes->lock.Lock();
				try {
					REL::Relocation<RE::TESObjectCELL* (*)(RE::TESWorldSpace*, int16_t, int16_t)> Func2{ RELOCATION_ID(20026, 20460) };
					cell = Func2(ws, x, y);
					if (IsValidLoadedCell(cell, quickLoad)) {
						if (*cell->GetName()) {
							logger::debug(fmt::runtime("add_GetCell({}, {}) (0): Name: " + std::string(cell->GetName()) + ", FormId: {:X}"), x, y, cell->formID);
						} else {
							logger::debug(fmt::runtime("add_GetCell({}, {}) (0): Name: Wilderness, FormId: {:X}"), x, y, cell->formID);
						}
						return cell;
					}
				} catch (...) {
				}
				tes->lock.Unlock();
			}
			if (cell != nullptr) {
				REL::Relocation<RE::TESObjectLAND* (*)(RE::TESObjectCELL*)> func2{ RELOCATION_ID(18513, 18970) };

				auto landPtr = func2(cell);
				if (landPtr != nullptr && (Memory::Internal::read<uint8_t>(&(landPtr->data.flags)) & 8) == 0) {
					REL::Relocation<void (*)(RE::TESObjectLAND*, int, int)> Func{ RELOCATION_ID(18331, 18747) };

					Func(landPtr, 0, 1);

					if (IsValidLoadedCell(cell, false)) {
						if (*cell->GetName()) {
							logger::debug(fmt::runtime("add_GetCell({}, {}) (0): Name: " + std::string(cell->GetName()) + ", FormId: {:X}"), x, y, cell->formID);
						} else {
							logger::debug(fmt::runtime("add_GetCell({}, {}) (0): Name: Wilderness, FormId: {:X}"), x, y, cell->formID);
						}

						return cell;
					}
				}
			}
		}

		std::string failData = "null";
		if (cell != nullptr) {
			auto cellStates = cell->cellState.get();
			failData = "loadState: " + std::to_string(static_cast<int>(cellStates)) + " hasTemp: " + std::to_string((Memory::Internal::read<uint8_t>(cell + 0x40) & 0x10) != 0 ? 1 : 0);
		}
		logger::debug(fmt::runtime("add_GetCell({}, {}) FAIL " + failData), x, y);

		return nullptr;
	}

	void DistantGrass::Call_AddGrassNow(RE::BGSGrassManager* GrassMgr, RE::TESObjectCELL* cell, uintptr_t customArg)
	{
		if (Memory::Internal::read<uint8_t>(customArg) != 0)
			return;

		{
			std::scoped_lock lock(cellMapMutex);
			auto c = Map->FindByCell(cell);
			if (!c) {
				logger::debug("AddedGrass(null): warning: c == null");
				return;
			}

			int d = c->self_data;
			int tg = d >> 8 & 0xFF;

			bool quickLoad = (d >> 16 & 0xFF) != 0;
			if (IsValidLoadedCell(cell, quickLoad)) {
				if (*cell->GetName()) {
					logger::debug(fmt::runtime("AddingGrass({}, {}) ca: {}, cell: Name: " + std::string(cell->GetName()) + ", FormId: {:X}"), c->x, c->y, Memory::Internal::read<uint8_t>(customArg), cell->formID);
				} else {
					logger::debug(fmt::runtime("AddingGrass({}, {}) ca: {}, cell: Name: Wilderness, FormId: {:X}"), c->x, c->y, Memory::Internal::read<uint8_t>(customArg), cell->formID);
				}

				using func_t = void (*)(RE::BGSGrassManager*, RE::TESObjectCELL*, uintptr_t);
				REL::Relocation<func_t> func{ RELOCATION_ID(15204, 15372) };

				func(GrassMgr, cell, customArg);

				logger::debug(fmt::runtime("AddedGrass({}, {})"), c->x, c->y);
			} else {
				logger::debug(fmt::runtime("AddedGrassFAIL({}, {}) <NotValidLoadedCell>"), c->x, c->y);
			}

			// Set this anyway otherwise we get stuck.
			c->self_data = tg;
		}
	}
	GrassStates DistantGrass::GetWantState(const std::shared_ptr<cell_info>& c, const int curX, const int curY, const int uGrid, const int grassRadius, const bool canLoadFromFile, const std::string& wsName)
	{
		int diffX = std::abs(curX - c->x);
		int diffY = std::abs(curY - c->y);

		if (diffX > grassRadius || diffY > grassRadius) {
			return GrassStates::None;
		}

		if (load_only) {
			return GrassStates::Active;
		}

		int uHalf = uGrid / 2;
		if (diffX > uHalf || diffY > uHalf) {
			// Special case: if we are loading and not generating anyway and already have active file we can take active instead of lod
			if (canLoadFromFile && c->checkHasFile(wsName)) {
				return GrassStates::Active;
			}

			return GrassStates::Lod;
		}

		return GrassStates::Active;
	}

	void DistantGrass::Handle_RemoveGrassFromCell_Call(RE::TESObjectCELL* cell)
	{
		auto grassMgr = RE::BGSGrassManager::GetSingleton();
		std::string why = "celldtor";
		if (cell == nullptr) {
			logger::debug(fmt::runtime("RemoveGrassOther(null): <- " + why));
			return;
		}
		{
			std::scoped_lock lock(cellMapMutex);
			auto c = Map->FindByCell(cell);
			if (c) {
				if (c->self_data >> 24 != 0) {
					logger::debug(fmt::runtime("RemoveGrassOther({}, {}) <- " + why + " warning: busy"), c->x, c->y);
				} else {
					logger::debug(fmt::runtime("RemoveGrassOther({}, {}) <- " + why), c->x, c->y);
				}

				c->self_data = 0;
				c->cell = nullptr;
				Map->map.erase(cell);
				ClearCellAddGrassTask(cell);
			} else {
				if (!cell->IsInteriorCell()) {
					logger::debug(fmt::runtime("cell_dtor({}, {})"), cell->GetCoordinates()->cellX, cell->GetCoordinates()->cellY);
				}
			}
		}
		REL::Relocation<void (*)(RE::BGSGrassManager*, RE::TESObjectCELL*)> func{ RELOCATION_ID(15207, 15375) };

		func(grassMgr, cell);
	}

	unsigned char DistantGrass::CalculateLoadState(const int nowX, const int nowY, const int x, const int y, const int ugrid, const int ggrid)
	{
		int diffX = std::abs(x - nowX);
		int diffY = std::abs(y - nowY);

		if (diffX <= ugrid && diffY <= ugrid)
			return 2;

		if (diffX <= ggrid && diffY <= ggrid)
			return 1;

		return 0;
	}

	void DistantGrass::UpdateGrassGridEnsureLoad(RE::TESWorldSpace* ws, const int nowX, const int nowY)
	{
		if (!Map)
			return;

		if (ws == nullptr)
			return;

		int grassRadius = getChosenGrassGridRadius();
		int uGrids = Memory::Internal::read<int>(addr_uGrids + 8);
		int uHalf = uGrids / 2;
		int bigSide = std::max(grassRadius, uHalf);
		bool canLoadGrass = Memory::Internal::read<uint8_t>(addr_AllowLoadFile + 8) != 0;

		std::string wsName = ws->editorID.c_str();

		for (int x = nowX - bigSide; x <= nowX + bigSide; x++) {
			for (int y = nowY - bigSide; y <= nowY + bigSide; y++) {
				unsigned char wantState = CalculateLoadState(nowX, nowY, x, y, uHalf, grassRadius);
				if (wantState != 1)
					continue;

				auto c = Map->GetFromGrid(x, y);
				if (!c)
					continue;

				auto xs = static_cast<short>(x);
				auto ys = static_cast<short>(y);

				auto xsu = static_cast<unsigned short>(xs);

				auto ysu = static_cast<unsigned short>(ys);

				unsigned int mask = static_cast<unsigned int>(xsu) << 16;
				mask |= ysu;

				REL::Relocation<RE::TESObjectCELL* (*)(RE::TESWorldSpace*, uintptr_t)> func{ RELOCATION_ID(13549, 13641) };

				auto cell = func(ws, mask);
				if (IsValidLoadedCell(cell, false))
					continue;

				bool quickLoad = false;
				if (canLoadGrass && c->checkHasFile(wsName)) {
					quickLoad = true;
				}

				if (quickLoad && IsValidLoadedCell(cell, true))
					continue;

				InterlockedCompareExchange64(&c->furtherLoad, 0, 2);

				REL::Relocation<RE::TESObjectCELL* (*)(RE::TESWorldSpace*, int, int)> func2{ RELOCATION_ID(20026, 20460) };

				cell = func2(ws, x, y);
				if (IsValidLoadedCell(cell, false)) {
					logger::debug("InstantLoadSuccessFirst({}, {}) {}", x, y, cell->GetFormEditorID());
				} else if (cell != nullptr) {
					REL::Relocation<RE::TESObjectLAND* (*)(RE::TESObjectCELL*)> Func{ RELOCATION_ID(18513, 18970) };

					auto landPtr = Func(cell);
					if (landPtr != nullptr && (Memory::Internal::read<uint8_t>(&(landPtr->data.flags)) & 8) == 0) {
						REL::Relocation<void (*)(RE::TESObjectLAND*, int, int)> fnc{ RELOCATION_ID(18331, 18747) };

						fnc(landPtr, 0, 1);

						if (IsValidLoadedCell(cell, false)) {
							logger::debug("InstantLoadSuccessSecond({}, {}) {}", c->x, c->y, cell->GetFormEditorID());
						}
					}
				}
			}
		}
	}

	void DistantGrass::UpdateGrassGridQueue(const int prevX, const int prevY, const int movedX, const int movedY)
	{
		auto wsObj = RE::TES::GetSingleton()->GetRuntimeData2().worldSpace;

		if (wsObj == nullptr)
			return;

		int grassRadius = getChosenGrassGridRadius();
		int uGrids = Memory::Internal::read<int>(addr_uGrids + 8);
		int uHalf = uGrids / 2;
		int bigSide = std::max(grassRadius, uHalf);
		bool canLoadGrass = Memory::Internal::read<uint8_t>(addr_AllowLoadFile + 8) != 0;

		std::string wsName = wsObj->editorID.c_str();

		int nowX = prevX + movedX;
		int nowY = prevY + movedY;

		for (int x = nowX - bigSide; x <= nowX + bigSide; x++) {
			for (int y = nowY - bigSide; y <= nowY + bigSide; y++) {
				unsigned char wantState = CalculateLoadState(nowX, nowY, x, y, uHalf, grassRadius);
				if (wantState != 1)
					continue;

				auto c = Map->GetFromGrid(x, y);
				if (!c)
					continue;

				auto xs = static_cast<short>(x);
				auto ys = static_cast<short>(y);

				auto xsu = static_cast<unsigned short>(xs);

				auto ysu = static_cast<unsigned short>(ys);

				unsigned int mask = static_cast<unsigned int>(xsu) << 16;
				mask |= ysu;

				REL::Relocation<RE::TESObjectCELL* (*)(RE::TESWorldSpace*, intptr_t)> func{ RELOCATION_ID(13549, 13641) };

				auto cell = func(wsObj, mask);
				if (IsValidLoadedCell(cell, false))
					continue;

				bool quickLoad = false;
				if (canLoadGrass && c->checkHasFile(wsName))
					quickLoad = true;

				if (quickLoad && IsValidLoadedCell(cell, true))
					continue;

				if (InterlockedCompareExchange64(&c->furtherLoad, 1, 0) == 0) {
					REL::Relocation<void (*)(uintptr_t, RE::TESWorldSpace*, int, int, int)> Func{ RELOCATION_ID(18150, 18541) };

					Func(Memory::Internal::read<uintptr_t>(addr_QueueLoadCellUnkGlobal), wsObj, x, y, 0);
				}
			}
		}
	}

	void DistantGrass::UpdateGrassGridNow(const RE::TES* tes, int movedX, int movedY, int addType)
	{
		if (addType == 1) {
			_canUpdateGridNormal = true;
		} else if (addType == -1) {
			_canUpdateGridNormal = false;
		} else if (addType == 0) {
			if (!_canUpdateGridNormal) {
				return;
			}
		}

		if (load_only) {
			UpdateGrassGridNow_LoadOnly(tes, movedX, movedY, addType);
			return;
		}

		logger::debug("UpdateGrassGridNowBegin({}, {}) type: {}", movedX, movedY, addType);

		int grassRadius = getChosenGrassGridRadius();
		int uGrids = Memory::Internal::read<int>(addr_uGrids + 8);
		int uHalf = uGrids / 2;
		int bigSide = std::max(grassRadius, uHalf);
		auto ws = tes->GetRuntimeData2().worldSpace;
		auto grassMgr = RE::BGSGrassManager::GetSingleton();
		bool canLoadGrass = Memory::Internal::read<uint8_t>(addr_AllowLoadFile + 8) != 0;
		std::string wsName;
		if (ws != nullptr) {
			wsName = ws->editorID.c_str();
		}

		int nowX = tes->currentGridX;
		int nowY = tes->currentGridY;

		auto invokeList = std::vector<RE::TESObjectCELL*>();
		if (addType <= 0) {
			{
				std::scoped_lock lock(cellMapMutex);
				Map->unsafe_ForeachWithState([&](std::shared_ptr<cell_info> c) {
					auto want = addType < 0 ? GrassStates::None : GetWantState(c, nowX, nowY, uGrids, grassRadius, false, "");
					if (want == GrassStates::None) {
						auto cell = c->cell;
						c->cell = nullptr;
						c->self_data = 0;
						ClearCellAddGrassTask(cell);

						logger::debug("RemoveGrassGrid({}, {})", c->x, c->y);

						invokeList.push_back(cell);
						return false;
					}

					return true;
				});
			}

			if (!invokeList.empty()) {
				for (auto cell : invokeList) {
					REL::Relocation<void (*)(RE::BGSGrassManager*, RE::TESObjectCELL*)> func{ RELOCATION_ID(15207, 15375) };

					func(grassMgr, cell);
				}

				invokeList.clear();
			}
		}

		if (addType >= 0) {
			{
				std::scoped_lock lock(cellMapMutex);
				int minX = nowX - bigSide;
				int maxX = nowX + bigSide;
				int minY = nowY - bigSide;
				int maxY = nowY + bigSide;

				logger::debug("X: ({}, {})", minX, maxX);
				logger::debug("NowX: ({}, {})", nowX, 0);
				logger::debug("Bigside: ({}, {})", bigSide, 0);
				logger::debug("X: ({}, {})", minX, maxX);
				logger::debug("NowX: ({}, {})", nowX, 0);
				logger::debug("Y: ({}, {})", minY, maxY);
				logger::debug("NowY: ({}, {})", nowY, 0);
				logger::debug("GrassRadius: ({}, {})", grassRadius, 0);
				logger::debug("uHalf: ({}, {})", uHalf, 0);

				// Add grass after.
				for (int x = minX; x <= maxX; x++) {
					for (int y = minY; y <= maxY; y++) {
						auto c = Map->GetFromGrid(x, y);
						if (!c) {
							continue;
						}

						int d = c->self_data;
						bool busy = d >> 24 != 0;

						auto want = GetWantState(c, nowX, nowY, uGrids, grassRadius, canLoadGrass, wsName);
						if (want > static_cast<GrassStates>(d & 0xFF))  // this check is using > because there's no need to set lod grass if we already have active grass
						{
							bool canQuickLoad = want == GrassStates::Active;
							auto cellPtr = GetCurrentWorldspaceCell(tes, ws, x, y, canQuickLoad, addType > 0);

							if (cellPtr != nullptr) {
								if (c->cell != cellPtr) {
									if (c->cell != nullptr) {
										logger::debug("c.cell({}, {}) warning: already had cell!", c->x, c->y);
									}
									c->cell = cellPtr;
									Map->map.try_emplace(cellPtr, c);
								}

								// set busy and target
								c->self_data = 0x01000000 | (canQuickLoad ? 0x00010000 : 0) | (static_cast<int>(want) << 8) | (d & 0xFF);

								logger::debug("QueueGrass({}, {})", x, y);

								if (!busy)
									invokeList.push_back(cellPtr);
							}
						}
					}
				}
			}

			if (!invokeList.empty()) {
				for (auto cell : invokeList) {
					// The way game does it is do it directly if generate grass files is enabled, But we don't want that because it causes a lot of stuttering.
					REL::Relocation<void (*)(RE::TESObjectCELL*)> func{ RELOCATION_ID(13137, 13277) };

					func(cell);
				}
			}
		}

		logger::debug("UpdateGrassGridNowEnd({}, {}) type: {}", movedX, movedY, addType);
	}

	void DistantGrass::UpdateGrassGridNow_LoadOnly(const RE::TES* tes, int movedX, int movedY, int addType)
	{
		logger::debug("UpdateGrassGridNowBegin_LoadOnly({}, {}) type: {}", movedX, movedY, addType);

		int grassRadius = getChosenGrassGridRadius();
		auto ws = tes->GetRuntimeData2().worldSpace;
		auto grassMgr = RE::BGSGrassManager::GetSingleton();
		int nowX = tes->currentGridX;
		int nowY = tes->currentGridY;

		if (addType <= 0) {
			LO2Map->UpdatePositionWithRemove(ws, addType, nowX, nowY, grassRadius);
		}

		if (addType >= 0) {
			auto minX = static_cast<short>(nowX - grassRadius);
			auto maxX = static_cast<short>(nowX + grassRadius);
			auto minY = static_cast<short>(nowY - grassRadius);
			auto maxY = static_cast<short>(nowY + grassRadius);

			// Add grass after.
			for (short x = minX; x <= maxX; x++) {
				for (short y = minY; y <= maxY; y++) {
					if (LO2Map)
						LO2Map->QueueLoad(ws, x, y);
				}
			}
		}

		logger::debug("UpdateGrassGridNowEnd_LoadOnly({}, {}) type: {}; c_count: {}; shapeCount: {}", movedX, movedY, addType, LO2Map->GetCount(), Memory::Internal::read<int>((uintptr_t)grassMgr + 0x58));
	}
}
