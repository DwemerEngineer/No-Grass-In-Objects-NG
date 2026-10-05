#pragma once

#include "GrassControl/Main.h"

namespace GrassControl
{
	class GidFileGenerationTask;
	class GidFileWorldGenerateTask;
	class GidFileCellGenerateTask;

	struct case_insensitive_unordered_set
	{
		struct comp
		{
			bool operator()(const std::string& Left, const std::string& Right) const noexcept
			{
				return Left.size() == Right.size() && std::equal(Left.begin(), Left.end(), Right.begin(),
														  [](char a, char b) {
															  return tolower(a) == tolower(b);
														  });
			}
		};
		struct hash
		{
			size_t operator()(const std::string& Keyval) const noexcept
			{
				size_t h = 0;
				std::ranges::for_each(Keyval.begin(), Keyval.end(), [&](char c) {
					h += tolower(c);
				});
				return h;
			}
		};
	};

	static std::mutex ProgressLocker;

	class GidFileCache final
	{
	public:
		static void FixFileFormat(bool only_load);

		static inline bool HelperInstalled = false;
	};

	class GidFileGenerationTask final
	{
	public:
		GidFileGenerationTask();

		static inline uintptr_t addr_GrassMgr = RELOCATION_ID(514292, 400452).address();
		static inline uintptr_t addr_uGrids = RELOCATION_ID(501244, 359675).address();

		static int getChosenGrassGridRadius();

	private:
		static int _chosenGrassGridRadius;

	public:
		static void run_freeze_check();

		static void apply();

		static void IncrementHook();

		static void ExchangeHook();

		static inline int DoneWS = 0;
		static inline int TotalWS = 0;

	private:
		static inline std::ofstream FileStream;

	public:
		static inline volatile int64_t queued_grass_counter = 0;

		static inline volatile int64_t queued_grass_mode = 0;

		static inline int cur_state = 0;

		static inline bool IsApplying = false;

		static inline std::unique_ptr<GidFileGenerationTask> cur_instance = nullptr;

	private:
		bool IsResuming;

		std::unordered_set<std::string, case_insensitive_unordered_set::hash> ProgressDone;

	public:
		static inline const std::string KeyWS = "ws";

		static inline const std::string KeyCell = "cell";

		static inline volatile int64_t _lastDidSomething = -1;

		static void InstallHooks()
		{
			Hooks::Install();
		}

	private:
		void Init();

		static void Free();

	public:
		[[nodiscard]] bool HasDone(const std::string& key, const std::string& wsName, int x = INT_MIN, int y = INT_MIN) const;

		[[nodiscard]] std::string GenerateProgressKey(const std::string& key, const std::string& wsName, int x = INT_MIN, int y = INT_MIN) const;

		void WriteProgressFile(const std::string& key, const std::string& wsName, int x = INT_MIN, int y = INT_MIN);

	private:
		std::vector<std::unique_ptr<GidFileWorldGenerateTask>> WorldTodo;

		void Begin();

		static void End();

		static inline int _istate = 0;

	public:
		static void Update();

	private:
		bool RunOne();

	protected:
		struct Hooks
		{
			struct PathFileNameSave
			{
				static int thunk(char* a_arg, const char* Buffer, int a_arg3)
				{
					auto str = std::string(Buffer);
					size_t pos = str.find(".gid");
					if (pos != std::string::npos) {
						str.replace(pos, std::string(".gid").size(), ".cgid");
					}

					return func(a_arg, str.c_str(), a_arg3);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct PathFileNameLoad
			{
				static int thunk(uintptr_t a_arg, const char* Buffer, uintptr_t a_arg3)
				{
					// Use a different file extension because we don't want to load the broken .gid files from BSA.
					auto str = std::string(Buffer);
					size_t pos = str.find(".gid");
					if (pos != std::string::npos) {
						str.replace(pos, std::string(".gid").size(), ".cgid");
					}
					return func(a_arg, str.c_str(), a_arg3);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct GrassCountIncrement
			{
				static void thunk(RE::TESObjectCELL* cell)
				{
					InterlockedIncrement64(&queued_grass_counter);
					func(cell);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct WriteProgress
			{
				static void thunk(uintptr_t GrassMgr, RE::TESObjectCELL* cell, uintptr_t unk)
				{
					func(GrassMgr, cell, unk);
					if (IsApplying) {
						if (cell != nullptr) {
							auto ws = cell->GetRuntimeData().worldSpace;
							if (ws != nullptr) {
								std::string wsn = ws->editorID.c_str();
								int x = cell->GetCoordinates()->cellX;
								int y = cell->GetCoordinates()->cellY;

								cur_instance->WriteProgressFile(KeyCell, wsn, x, y);
							}
						}
						InterlockedDecrement64(&queued_grass_counter);
					}
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			static void Install()
			{
				if (Config::UseGrassCache) {
					if (!GidFileCache::HelperInstalled) {
						stl::write_thunk_call<PathFileNameLoad>(RELOCATION_ID(15206, 15374).address() + REL::Relocate(0xE5, 0xE5));
					}

					if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99) {
						stl::write_thunk_call<PathFileNameSave>(RELOCATION_ID(15204, 15372).address() + 0x6B3);
					} else {
						stl::write_thunk_call<PathFileNameSave>(RELOCATION_ID(15204, 15372).address() + REL::Relocate(0x6A7, 0x6A3));
					}
			
					if (exists(std::filesystem::path(Util::GetProgressFilePath()))) {
						stl::write_thunk_call<GrassCountIncrement>(RELOCATION_ID(13190, 13335).address() + REL::Relocate(0xD40 - 0xC70, 0xD0));
						stl::write_thunk_jump<WriteProgress>(RELOCATION_ID(13138, 13278).address() + REL::Relocate(0xF, 0xF));
					}
				}
			}
		};
	};

	class GidFileWorldGenerateTask final
	{
	public:
		GidFileWorldGenerateTask(GidFileGenerationTask* parent, RE::TESWorldSpace* ws, const std::string& wsName);

		GidFileGenerationTask* Parent;

		RE::TESWorldSpace* WorldSpace;

		std::string Name;

	private:
		std::list<GidFileCellGenerateTask*> CellTodo{};

		std::vector<GidFileCellGenerateTask*> _grid{};

	public:
		int TotalCellDo = 0;

		int DidCellDo = 0;

	private:
		static void Init();

		void Free();

		void Begin();

		void End() const;

		int _istate = 0;

		[[nodiscard]] GidFileCellGenerateTask* GetGrid(int x, int y) const;

		int uhalf = 0;
		int ugrid = 0;

		[[nodiscard]] GidFileCellGenerateTask* FindBestTodo() const;

	public:
		bool RunOne();

		void Remove(GidFileCellGenerateTask*& t);
	};

	class GidFileCellGenerateTask final
	{
	public:
		GidFileCellGenerateTask(GidFileWorldGenerateTask* parent, int x, int y);

		GidFileWorldGenerateTask* Parent;

		int X;

		int Y;

	private:
		RE::TESObjectCELL* Cell{};

		static void Init();

		static void Free();

		bool Begin();

		void End();

		bool Process();

	public:
		bool RunOne();
	};
}
