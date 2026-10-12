#pragma once

#include <mutex>
#include <utility>
#include <vector>

#include <GrassControl/Stopwatch.hpp>

namespace GrassControl
{
	class Profiler final
	{
	public:
		void Begin();

		void End();

		void Report();

	private:
		std::vector<long long> Times;

		std::vector<std::pair<int, long long>> Progress;

		std::mutex Locker;

		stopwatch::Stopwatch Timer;
	};
}
