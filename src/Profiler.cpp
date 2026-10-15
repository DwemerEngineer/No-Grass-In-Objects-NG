#include "GrassControl/Profiler.h"

#include "GrassControl/Config.h"
#include "GrassControl/GrassCellCreation.h"
#include "GrassControl/PhysicsRaycast.h"

namespace GrassControl::Profiler
{
	static constexpr std::size_t MaxSamples = 1000;

	static std::mutex mutex;
	static std::deque<double> samples;

	void Record(std::chrono::steady_clock::duration time)
	{
		if (!Config::ProfilerReport)
			return;

		std::scoped_lock lock(mutex);
		samples.push_back(std::chrono::duration<double, std::milli>(time).count());
		if (samples.size() > MaxSamples)
			samples.pop_front();
	}

	Scope::~Scope()
	{
		Record(std::chrono::steady_clock::now() - started);
	}

	void Report()
	{
		std::vector<double> all;
		{
			std::scoped_lock lock(mutex);
			all.assign(samples.begin(), samples.end());
		}

		std::string message = "No samples have been gathered yet.";
		if (!all.empty()) {
			std::ranges::sort(all);
			const auto middle = all.size() / 2;
			const double median = all.size() % 2 == 0 ? (all[middle - 1] + all[middle]) / 2.0 : all[middle];
			const double average = std::accumulate(all.begin(), all.end(), 0.0) / static_cast<double>(all.size());
			message = fmt::format("Samples = {}; Average = {:.3f}ms; Median = {:.3f}ms; Min = {:.3f}ms; Max = {:.3f}ms", all.size(), average, median, all.front(), all.back());
		}

		logger::info("Grass generation: {}", message);
		PhysicsRaycast::GetSingleton().ReportStatistics();
		DistantGrass::ReportStatistics();
		RE::SendHUDMessage::ShowHUDMessage(message.c_str(), nullptr, true);
	}
}
