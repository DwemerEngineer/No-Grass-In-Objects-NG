// Copyright Ingo Proff 2017.
// https://github.com/CrikeeIP/Stopwatch
// Distributed under the MIT Software License (X11 license).
// See licenses/LICENSE-Stopwatch.

#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace stopwatch
{

	class Stopwatch
	{
	public:
		enum TimeFormat
		{
			TICKS,
			NANOSECONDS,
			CUSTOMTICK,
			MICROSECONDS,
			MILLISECONDS,
			SECONDS
		};

		Stopwatch() : start_time(), laps({})
		{
			start();
		}

		void start()
		{
			start_time = std::chrono::high_resolution_clock::now();
			laps = { start_time };
		}

		template <TimeFormat fmt = TimeFormat::MILLISECONDS>
		std::uint64_t lap()
		{
			const auto t = std::chrono::high_resolution_clock::now();
			const auto last_r = laps.back();
			laps.push_back(t);
			return ticks<fmt>(last_r, t);
		}

		template <TimeFormat fmt = TimeFormat::MILLISECONDS>
		std::uint64_t elapsed()
		{
			const auto end_time = std::chrono::high_resolution_clock::now();
			return ticks<fmt>(start_time, end_time);
		}

		template <TimeFormat fmt_total = TimeFormat::MILLISECONDS, TimeFormat fmt_lap = fmt_total>
		std::pair<std::uint64_t, std::vector<std::uint64_t>> elapsed_laps()
		{
			std::vector<std::uint64_t> lap_times;
			lap_times.reserve(laps.size() - 1);

			for (std::size_t idx = 0; idx + 1 < laps.size(); idx++) {
				const auto lap_end = laps[idx + 1];
				const auto lap_start = laps[idx];
				lap_times.push_back(ticks<fmt_lap>(lap_start, lap_end));
			}

			return { ticks<fmt_total>(start_time, laps.back()), lap_times };
		}

	private:
		typedef std::chrono::time_point<std::chrono::high_resolution_clock> time_pt;
		time_pt start_time;
		std::vector<time_pt> laps;

		template <TimeFormat fmt = TimeFormat::MILLISECONDS>
		static std::uint64_t ticks(const time_pt& start_time, const time_pt& end_time)
		{
			const auto duration = end_time - start_time;
			const std::uint64_t ns_count = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

			switch (fmt) {
			case TimeFormat::TICKS:
				{
					std::uint64_t up = ((ns_count * 10) % 10 >= 5) ? 1 : 0;
					const auto tk_count = (ns_count * 100) + up;
					return tk_count;
				}

			case TimeFormat::NANOSECONDS:
				{
					return ns_count;
				}

			case TimeFormat::CUSTOMTICK:
				{
					std::uint64_t up = ((ns_count) % 10 >= 5) ? 1 : 0;
					const auto ct_count = (ns_count / 10) + up;
					return ct_count;
				}

			case TimeFormat::MICROSECONDS:
				{
					std::uint64_t up = ((ns_count / 100) % 10 >= 5) ? 1 : 0;
					const auto mus_count = (ns_count / 1000) + up;
					return mus_count;
				}

			case TimeFormat::MILLISECONDS:
				{
					std::uint64_t up = ((ns_count / 100000) % 10 >= 5) ? 1 : 0;
					const auto ms_count = (ns_count / 1000000) + up;
					return ms_count;
				}

			case TimeFormat::SECONDS:
				{
					std::uint64_t up = ((ns_count / 100000000) % 10 >= 5) ? 1 : 0;
					const auto s_count = (ns_count / 1000000000) + up;
					return s_count;
				}

			default:
				return ns_count;
			}
		}
	};

	constexpr Stopwatch::TimeFormat tk = Stopwatch::TimeFormat::TICKS;
	constexpr Stopwatch::TimeFormat ns = Stopwatch::TimeFormat::NANOSECONDS;
	constexpr Stopwatch::TimeFormat ct = Stopwatch::TimeFormat::CUSTOMTICK;
	constexpr Stopwatch::TimeFormat mus = Stopwatch::TimeFormat::MICROSECONDS;
	constexpr Stopwatch::TimeFormat ms = Stopwatch::TimeFormat::MILLISECONDS;
	constexpr Stopwatch::TimeFormat s = Stopwatch::TimeFormat::SECONDS;

	constexpr Stopwatch::TimeFormat ticks = Stopwatch::TimeFormat::TICKS;
	constexpr Stopwatch::TimeFormat nanoseconds = Stopwatch::TimeFormat::NANOSECONDS;
	constexpr Stopwatch::TimeFormat customtick = Stopwatch::TimeFormat::CUSTOMTICK;
	constexpr Stopwatch::TimeFormat microseconds = Stopwatch::TimeFormat::MICROSECONDS;
	constexpr Stopwatch::TimeFormat milliseconds = Stopwatch::TimeFormat::MILLISECONDS;
	constexpr Stopwatch::TimeFormat seconds = Stopwatch::TimeFormat::SECONDS;

	inline std::string show_times(const std::vector<std::uint64_t>& times)
	{
		std::string result("{");
		if (times.empty())
			return "{}";

		for (const auto& t : times) {
			result += std::to_string(t) + ",";
		}

		result.back() = static_cast<char>('}');
		return result;
	}

}
