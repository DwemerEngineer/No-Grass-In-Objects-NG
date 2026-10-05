#include "GrassControl/GidFileCache.h"
#pragma comment(lib, "CasualLibrary.lib")

using namespace Xbyak;
namespace GrassControl
{
	void FixSaving(uintptr_t r8, uintptr_t ptrbuf, uintptr_t ptrThing)
	{
		REL::Relocation<void (*)(uintptr_t, uintptr_t, uint32_t)> func{ RELOCATION_ID(74621, 76352) };
		func(r8, ptrbuf, 0x24);
		auto ptrBuf = Memory::Internal::read<uintptr_t>(ptrThing + 0x8);
		auto ptrSize = Memory::Internal::read<uint32_t>(ptrThing + 0x10);
		func(r8, ptrBuf, ptrSize);
	}

	void GidFileCache::FixFileFormat(const bool only_load)
	{
		if (exists(std::filesystem::path(Util::GetProgressFilePath()))) {
			try {
				auto dir = std::filesystem::path("data/grass");
				if (!exists(dir)) {
					create_directories(dir);
				}
			} catch (...) {
				stl::report_and_fail("Error occured while trying to create or access data/grass folder");
			}
		}

		// Fix saving the GID files (because bethesda broke it in SE).
		auto addr = RELOCATION_ID(74601, 76329).address() + REL::Relocate(0xB90 - 0xAE0, 0xB0);
		struct Patch : CodeGenerator
		{
			Patch(uintptr_t a_target, uintptr_t b_target)
			{
				Label funcLabel;
				Label retnLabel;

				mov(rax, rcx);
				lea(rdx, ptr[rbp - 0x30]);
				lea(rcx, ptr[r8 + 0x8]);
				mov(r8, ptr[rax + 0x40]);  // ptrThing

				sub(rsp, 0x20);
				call(ptr[rip + funcLabel]);
				add(rsp, 0x20);

				jmp(ptr[rip + retnLabel]);

				L(funcLabel);
				dq(b_target);

				L(retnLabel);
				dq(a_target + 0x13);
			}
		};
		Patch patch(addr, reinterpret_cast<uintptr_t>(FixSaving));
		patch.ready();

		auto& trampoline = SKSE::GetTrampoline();
		Util::NopBlock(addr, 0x13, 0);
		trampoline.write_branch<5>(addr, trampoline.allocate(patch));

		// Set the ini stuff.
		auto setting = RE::INISettingCollection::GetSingleton()->GetSetting("bAllowLoadGrass:Grass");
		setting->data.b = true;

		setting = RE::INISettingCollection::GetSingleton()->GetSetting("bAllowCreateGrass:Grass");
		if (!only_load || Config::Updating) {
			setting->data.b = true;
		} else {
			setting->data.b = false;
		}

		if (Config::Updating) {
			logger::info("Cache Updating Mode Enabled.");
			setting = RE::INISettingCollection::GetSingleton()->GetSetting("bGenerateGrassDataFiles:Grass");
			setting->data.b = true;
		}

		// Disable grass console.
		if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99) {
			addr = RELOCATION_ID(15204, 15372).address() + 0x8A6;
		} else {
			addr = RELOCATION_ID(15204, 15372).address() + 0x896;
		}

		struct Patch2 : CodeGenerator
		{
			Patch2(const std::uintptr_t a_target)
			{
				Label retnLabel;

				mov(r15, 0);
				jmp(ptr[rip + retnLabel]);

				L(retnLabel);
				dq(a_target + 0x7 + 0x2D5);
			}
		};
		Patch2 patch2(addr);
		patch2.ready();

		trampoline.write_branch<5>(addr, trampoline.allocate(patch2));
		REL::safe_write(addr + 5, REL::NOP2, 2);
	}

	GidFileGenerationTask::GidFileGenerationTask() :
		IsResuming(false), ProgressDone(std::unordered_set<std::string, case_insensitive_unordered_set::hash>())
	{
		if (cur_instance != nullptr) {
			stl::report_and_fail("Only one instance of GidFileGenerationTask can be created at a time!");
		}

		FileStream.open(Util::GetProgressFilePath(), std::ios::app);
		//cur_instance = std::unique_ptr<GidFileGenerationTask>(this);
	}

	int GidFileGenerationTask::getChosenGrassGridRadius()
	{
		int r = _chosenGrassGridRadius;
		if (r < 0) {
			int ugrid = Memory::Internal::read<int>(addr_uGrids + 8);
			r = (ugrid - 1) / 2;

			_chosenGrassGridRadius = r;
		}

		return r;
	}

	int GidFileGenerationTask::_chosenGrassGridRadius = -1;

	void GidFileGenerationTask::run_freeze_check()
	{
		while (true) {
			if (cur_state != 1)
				break;

			uint64_t last = InterlockedCompareExchange64(&_lastDidSomething, 0, 0);
			uint64_t now = GetTickCount64();

			if (now - last < Config::FreezeTime) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				continue;
			}

			RE::DebugMessageBox("Grass generation appears to have frozen! Restart the game.");
			Util::ReportAndFailTimed("Grass generation appears to have frozen! Restart the game.");
		}
	}

	void GidFileGenerationTask::IncrementHook()
	{
		InterlockedIncrement64(&queued_grass_counter);
	}

	void GidFileGenerationTask::ExchangeHook()
	{
		InterlockedExchange64(&queued_grass_mode, 0);
	}

	void GidFileGenerationTask::apply()
	{
		unsigned long long addr;

		if (addr = (RELOCATION_ID(13148, 13288).address() + REL::Relocate(0x2B25 - 0x2220, 0xB29)); REL::make_pattern<"E8">().match(addr)) {
			REL::safe_write(addr, REL::NOP5, 5);
		}

		addr = RELOCATION_ID(13190, 13335).address() + REL::Relocate(0x106, 0x106);
		struct Patch : CodeGenerator
		{
			Patch(uintptr_t Exchange, const uintptr_t a_target)
			{
				Label retnLabel;
				Label exchange;

				mov(rdi, ptr[rsp + 0x50]);

				sub(rsp, 0x20);
				call(ptr[rip + exchange]);
				add(rsp, 0x20);

				jmp(ptr[rip + retnLabel]);

				L(retnLabel);
				dq(a_target + 0x5);

				L(exchange);
				dq(Exchange);
			}
		};
		Patch patch(reinterpret_cast<uintptr_t>(&ExchangeHook), addr);
		patch.ready();

		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_branch<5>(addr, trampoline.allocate(patch));

		addr = RELOCATION_ID(15202, 15370).address() + REL::Relocate(0xA0E - 0x890, 0x17D);
		struct Patch2 : CodeGenerator
		{
			explicit Patch2(const uintptr_t a_func, const uintptr_t a_target)
			{
				Label funcLabel;
				Label retnLabel;

				sub(rsp, 0x20);
				call(ptr[rip + funcLabel]);
				add(rsp, 0x20);

				jmp(ptr[rip + retnLabel]);

				L(retnLabel);
				dq(a_target + 0x6);

				L(funcLabel);
				dq(a_func);
			}
		};
		Patch2 patch2(reinterpret_cast<uintptr_t>(getChosenGrassGridRadius), addr);
		patch2.ready();
		trampoline.write_branch<6>(addr, trampoline.allocate(patch2));

		auto setting = RE::INISettingCollection::GetSingleton()->GetSetting("bGenerateGrassDataFiles:Grass");
		setting->data.b = true;

		IsApplying = true;

		// Allow game to be alt-tabbed and make sure it's processing in the background correctly.
		addr = RELOCATION_ID(35565, 36564).address() + REL::Relocate(0x216 - 0x1E0, 0x51, 0x4b);
		Memory::Internal::write<uint8_t>(addr, 0xEB, true);
	}

	void GidFileGenerationTask::Update()
	{
		if (!IsApplying)
			return;

		if (cur_state == 1) {
			if (InterlockedCompareExchange64(&queued_grass_counter, 0, 0) != 0)
				return;

			if (InterlockedExchange64(&queued_grass_mode, 1) != 0)
				return;

			InterlockedExchange64(&_lastDidSomething, GetTickCount64());

			if (!cur_instance->RunOne()) {
				cur_state = 2;

				RE::ConsoleLog::GetSingleton()->Print("Grass generation finished successfully!");
				stl::report_and_fail("Grass generation finished successfully!");
			}
		}
	}

	void GidFileGenerationTask::Init()
	{
		auto fi = std::filesystem::path(Util::GetProgressFilePath());
		if (exists(fi)) {
			{
				std::scoped_lock lock(ProgressLocker);

				std::ifstream fs;
				try {
					fs = std::ifstream(Util::GetProgressFilePath());
					if (!fs) {
						throw std::system_error(errno, std::system_category(), "failed to open " + Util::GetProgressFilePath());
					}
				} catch (std::system_error& e) {
					logger::error(fmt::runtime("Error reading PrecacheGrass.txt: " + std::string(e.what())));
					RE::DebugMessageBox(e.what());
				}

				std::string l;
				while (std::getline(fs, l)) {
					l = Util::StringHelpers::trim(l);

					if (l.empty())
						continue;
					ProgressDone.insert(l);
				}
			}

			IsResuming = !ProgressDone.empty();
		}

		if (ProgressDone.empty()) {
			auto dir = std::filesystem::path("Data/Grass");
			if (!exists(dir)) {
				create_directories(dir);
			}

			if (!Config::Updating) {
				for (const auto& entry : std::filesystem::directory_iterator(dir)) {
					if (entry.path().extension() == ".dgid" || entry.path().extension() == ".cgid" || entry.path().extension() == ".fail") {
						remove(entry.path());
					}
				}
			}
		}

		std::string tx;
		if (IsResuming) {
			auto entries = ProgressDone.size();
			tx = "Resuming grass cache generation now with " + std::to_string(entries) + " entries completed.\n\nThis will take a while !\n\nIf the game crashes you can run it again to resume.\n\nWhen all is finished the game will say.\n\nOpen console to see progress.";
		} else {
			tx = "Generating new grass cache now.\n\nThis will take a while!\n\nIf the game crashes you can run it again to resume.\n\nWhen all is finished the game will say.\n\nOpen console to see progress.";
		}
		logger::info(fmt::runtime(tx.c_str()));
		RE::DebugMessageBox(tx.c_str());
	}

	void GidFileGenerationTask::Free()
	{
		if (FileStream) {
			FileStream.close();
		}
	}

	bool GidFileGenerationTask::HasDone(const std::string& key, const std::string& wsName, const int x, const int y) const
	{
		std::string text = GenerateProgressKey(key, wsName, x, y);
		{
			std::scoped_lock lock(ProgressLocker);
			return ProgressDone.contains(text);
		}
	}

	std::string GidFileGenerationTask::GenerateProgressKey(const std::string& key, const std::string& wsName, const int x, const int y) const
	{
		std::string bld;
		if (!key.empty()) {
			bld.append(key);
		}
		if (!wsName.empty()) {
			if (!bld.empty()) {
				bld += '_';
			}
			bld.append(wsName);
		}
		if (x != INT_MIN) {
			if (!bld.empty()) {
				bld += '_';
			}
			bld.append(std::to_string(x));
		}
		if (y != INT_MIN) {
			if (!bld.empty()) {
				bld += '_';
			}
			bld.append(std::to_string(y));
		}

		return bld;
	}

	void GidFileGenerationTask::WriteProgressFile(const std::string& key, const std::string& wsName, const int x, const int y)
	{
		std::string text = GenerateProgressKey(key, wsName, x, y);
		if (text.empty())
			return;

		{
			std::scoped_lock lock(ProgressLocker);
			auto [fst, snd] = ProgressDone.insert(text);
			if (!snd)
				return;

			if (FileStream.is_open()) {
				FileStream << text << std::endl;
			}
		}
	}

	void GidFileGenerationTask::Begin()
	{
		char Char = ';';
		auto skip = Util::StringHelpers::split(Config::SkipPregenerateWorldSpaces, Char, true);
		auto skipSet = std::unordered_set<std::string, case_insensitive_unordered_set::hash, case_insensitive_unordered_set::comp>();
		for (auto& x : skip) {
			logger::info("Skipping: {}", x);
			skipSet.insert(x);
		}

		auto only = Util::StringHelpers::split(Config::OnlyPregenerateWorldSpaces, Char, true);
		auto onlySet = std::unordered_set<std::string, case_insensitive_unordered_set::hash, case_insensitive_unordered_set::comp>();
		for (const auto& x : only) {
			logger::info("Only Generating: {}", x);
			onlySet.insert(x);
		}

		auto& all = RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESWorldSpace>();
		for (auto f : all) {
			auto ws = f;

			std::string name = ws->editorID.c_str();

			if (!onlySet.empty()) {
				if (name.empty() || !onlySet.contains(name))
					continue;

			} else if (!name.empty() && skipSet.contains(name))
				continue;

			TotalWS++;

			if (HasDone(KeyWS, name)) {
				DoneWS++;
				continue;
			}

			auto t = new GidFileWorldGenerateTask(this, ws, name);
			WorldTodo.emplace_back(t);
		}
	}

	void GidFileGenerationTask::End()
	{
		if (FileStream) {
			FileStream.close();
		}

		auto fi = std::filesystem::path(Util::GetProgressFilePath());
		if (exists(fi)) {
			remove(fi);
		}
	}

	bool GidFileGenerationTask::RunOne()
	{
		if (_istate == 0) {
			_istate = 1;

			Init();

			Begin();
		}

		if (!WorldTodo.empty()) {
			auto& t = WorldTodo.back();

			if (!t->RunOne()) {
				WorldTodo.pop_back();
				DoneWS++;

				InterlockedExchange64(&queued_grass_mode, 0);
			}

			return true;
		}

		if (_istate == 1) {
			_istate = 2;

			End();

			Free();
		}

		return false;
	}

	GidFileWorldGenerateTask::GidFileWorldGenerateTask(GidFileGenerationTask* parent, RE::TESWorldSpace* ws, const std::string& wsName)
	{
		Parent = parent;
		WorldSpace = ws;
		Name = wsName;
		_grid = std::vector<GidFileCellGenerateTask*>(16641);  // 129 * 129

		ugrid = Memory::Internal::read<int>(GidFileGenerationTask::addr_uGrids + 8);
		uhalf = ugrid / 2;
	}

	void GidFileWorldGenerateTask::Init()
	{
	}

	void GidFileWorldGenerateTask::Free()
	{
		_grid.clear();
	}

	void GidFileWorldGenerateTask::Begin()
	{
		int min = -64;
		int max = 64;

		TotalCellDo = 129 * 129;

		for (int x = min; x <= max; x++) {
			for (int y = min; y <= max; y++) {
				if (Parent->HasDone(GidFileGenerationTask::KeyCell, Name, x, y)) {
					DidCellDo++;
					continue;
				}

				auto cg = new GidFileCellGenerateTask(this, x, y);
				CellTodo.emplace_back(cg);
				_grid[(x + 64) * 129 + (y + 64)] = cg;
			}
		}
	}

	void GidFileWorldGenerateTask::End() const
	{
		Parent->WriteProgressFile(GidFileGenerationTask::KeyWS, Name);
	}

	GidFileCellGenerateTask* GidFileWorldGenerateTask::GetGrid(int x, int y) const
	{
		x += 64;
		y += 64;
		if (x < 0 || x > 128 || y < 0 || y > 128) {
			return nullptr;
		}
		return _grid[x * 129 + y];
	}

	GidFileCellGenerateTask* GidFileWorldGenerateTask::FindBestTodo() const
	{
		GidFileCellGenerateTask* best = nullptr;
		int bestCount = 0;

		int ufull = ugrid * ugrid;

		for (auto& n : CellTodo) {
			int minx = n->X - uhalf;
			int maxx = n->X + uhalf;
			int miny = n->Y - uhalf;
			int maxy = n->Y + uhalf;
			int cur = 0;

			for (int x = minx; x <= maxx; x++) {
				for (int y = miny; y <= maxy; y++) {
					auto t = GetGrid(x, y);
					if (t != nullptr)
						cur++;
				}
			}

			if (cur == 0) {
				stl::report_and_fail("Grass generation has crashed!");
			}

			if (best == nullptr || cur > bestCount) {
				best = n;
				bestCount = cur;

				if (bestCount >= ufull)
					break;
			}
		}

		return best;
	}

	bool GidFileWorldGenerateTask::RunOne()
	{
		if (_istate == 0) {
			_istate = 1;

			Init();

			Begin();
		}

		while (!CellTodo.empty()) {
			auto t = FindBestTodo();
			if (t == nullptr)
				stl::report_and_fail("Grass generation has crashed!");

			if (!t->RunOne()) {
				Remove(t);
				Parent->WriteProgressFile(GidFileGenerationTask::KeyCell, Name, t->X, t->Y);
				DidCellDo++;
				continue;
			}

			int minx = t->X - uhalf;
			int maxx = t->X + uhalf;
			int miny = t->Y - uhalf;
			int maxy = t->Y + uhalf;
			for (int x = minx; x <= maxx; x++) {
				for (int y = miny; y <= maxy; y++) {
					auto tx = GetGrid(x, y);
					if (tx != nullptr) {
						Remove(tx);
						Parent->WriteProgressFile(GidFileGenerationTask::KeyCell, Name, x, y);
						DidCellDo++;
					}
				}
			}

			return true;
		}

		if (_istate == 1) {
			_istate = 2;

			End();

			Free();
		}

		return false;
	}

	void GidFileWorldGenerateTask::Remove(GidFileCellGenerateTask*& t)
	{
		CellTodo.remove(t);

		int x = t->X + 64;
		int y = t->Y + 64;
		int ix = x * 129 + y;
		if (_grid[ix] == t) {
			_grid[ix] = nullptr;
		} else {
			Util::ReportAndFailTimed("Grass generation has crashed!");
		}
	}

	GidFileCellGenerateTask::GidFileCellGenerateTask(GidFileWorldGenerateTask* parent, const int x, const int y)
	{
		Parent = parent;
		X = x;
		Y = y;
	}

	void GidFileCellGenerateTask::Init()
	{
	}

	void GidFileCellGenerateTask::Free()
	{
	}

	bool GidFileCellGenerateTask::Begin()
	{
		RE::TES::GetSingleton()->lock.Lock();

		RE::TESObjectCELL* cellPtr = nullptr;
		//Better to let this crash if it fails, so that it is logged.
		REL::Relocation<RE::TESObjectCELL* (*)(RE::TESWorldSpace*, int32_t, int32_t)> Func2{ RELOCATION_ID(20026, 20460) };
		cellPtr = Func2(this->Parent->WorldSpace, this->X, this->Y);

		RE::TES::GetSingleton()->lock.Unlock();

		if (cellPtr == nullptr)
			return false;

		Cell = cellPtr;

		return true;
	}

	void GidFileCellGenerateTask::End()
	{
		Cell = nullptr;
	}

	bool GidFileCellGenerateTask::Process()
	{
		if (Cell == nullptr)
			return false;

		const std::string fileKey = fmt::format(fmt::runtime("Data/Grass/{}x{:04}y{:04}.fail"), this->Parent->Name, this->X, this->Y);
		auto fails = 1;
		if (std::filesystem::exists(fileKey)) {
			std::ifstream failFile(fileKey);
			if (failFile.good()) {
				std::string failCount;
				std::getline(failFile, failCount);

				try {
					fails = stoi(failCount);
				} catch (std::exception e) {
					logger::error(fmt::runtime("Error occured while trying to rread fail count for " + fileKey + ". Error was " + e.what()));
				}

				failFile.close();

				if (fails >= Config::MaxFailures) {
					logger::info(fmt::runtime("{}({},{}) failed {} times; skipping"), this->Parent->Name, this->X, this->Y, fails);
					return false;
				} else {
					logger::info(fmt::runtime("{}({},{}) failed {} times; trying again"), this->Parent->Name, this->X, this->Y, fails);
					fails++;
				}
			}
		}

		// write failFile
		std::ofstream failFile(fileKey, std::ios::trunc);
		if (failFile.is_open()) {
			failFile << fails << std::endl;
			failFile.close();
		}

		double pct = 0.0;
		if (Parent->TotalCellDo > 0) {
			pct = std::max(0.0, std::min(static_cast<double>(Parent->DidCellDo) / static_cast<double>(Parent->TotalCellDo), 1.0)) * 100.0;
		}

		const std::string msg = "Generating grass for " + this->Parent->Name + "(" + std::to_string(this->X) + ", " + std::to_string(this->Y) + ") " + std::to_string(pct) + " pct, world " + std::to_string(GidFileGenerationTask::DoneWS + 1) + " out of " + std::to_string(GidFileGenerationTask::TotalWS);
		RE::ConsoleLog::GetSingleton()->Print(msg.c_str());
		logger::info(fmt::runtime(msg));
		{
			auto alloc = new char[0x20];
			memset(alloc, 0, 0x20);

			Memory::Internal::write<float>(reinterpret_cast<uintptr_t>(alloc), static_cast<float>(Cell->GetCoordinates()->cellX) * 4096.0f + 2048.0f);
			Memory::Internal::write<float>(reinterpret_cast<uintptr_t>(alloc) + 4, static_cast<float>(Cell->GetCoordinates()->cellY) * 4096.0f + 2048.0f);
			Memory::Internal::write<float>(reinterpret_cast<uintptr_t>(alloc) + 8, 0.0f);

			//Let this crash if it fails, so that it is logged.
			REL::Relocation<void (*)(RE::PlayerCharacter*, uintptr_t, uintptr_t, RE::TESObjectCELL*, int)> func{ RELOCATION_ID(39657, 40744) };
			func(RE::PlayerCharacter::GetSingleton(), reinterpret_cast<uintptr_t>(alloc), reinterpret_cast<uintptr_t>(alloc) + 0x10, Cell, 0);
		}

		if (std::filesystem::exists(fileKey)) {
			std::filesystem::remove(fileKey);
		}

		return true;
	}

	bool GidFileCellGenerateTask::RunOne()
	{
		Init();

		bool did = Begin();

		if (did && !Process()) {
			did = false;
		}

		End();

		Free();

		return did;
	}
}
