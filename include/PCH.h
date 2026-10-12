#pragma once

#define WIN32_LEAN_AND_MEAN

#define NOMINMAX
#define _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define GLM_FORCE_INLINE
#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
#define GLM_FORCE_INTRINSICS
#define GLM_FORCE_LEFT_HANDED
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_QUAT_DATA_WXYZ
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#include "Version.h"

#include <Windows.h>

#include <CLibUtil/String.hpp>
#include <CasualLibrary.hpp>
#include <SimpleIni.h>
#include <xbyak/xbyak.h>

#include <spdlog/sinks/basic_file_sink.h>

namespace logger = SKSE::log;

using namespace std::literals;

namespace stl
{
	using namespace SKSE::stl;

	template <class T>
	void write_thunk_jump(std::uintptr_t a_src)
	{
		auto& trampoline = SKSE::GetTrampoline();
		T::func = trampoline.write_branch<5>(a_src, T::thunk);
	}

	template <class T, size_t size = 5>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = SKSE::GetTrampoline();
		T::func = trampoline.write_call<size>(a_src, T::thunk);
	}
}

extern "C"
{
	int WINAPI MessageBoxTimeoutA(IN HWND hWnd, IN LPCSTR lpText, IN LPCSTR lpCaption, IN UINT uType, IN WORD wLanguageId, IN DWORD dwMilliseconds);
};
