#include "GrassControl/Profiler.h"
#include "GrassControl/Util.h"

#include <algorithm>
#include <numeric>

namespace GrassControl
{
	void Profiler::Begin()
	{
		int who = GetCurrentThreadId();
		long long when = this->Timer.elapsed<stopwatch::ms>();

		{
			std::scoped_lock lock(this->Locker);
			for (int i = 0; i < this->Progress.size(); i++) {
				auto& [fst, snd] = this->Progress[i];
				if (fst == who) {
					this->Progress[i] = std::pair{ who, when };
					return;
				}
			}

			this->Progress.emplace_back(std::pair{ who, when });
		}
	}

	void Profiler::End()
	{
		long long when = this->Timer.elapsed<stopwatch::ms>();
		int who = GetCurrentThreadId();

		{
			std::scoped_lock lock(this->Locker);
			for (int i = 0; i < this->Progress.size(); i++) {
				auto& [fst, snd] = this->Progress[i];
				if (fst == who) {
					long long diff = when - snd;
					this->Progress.erase(this->Progress.begin() + i);

					this->Times.push_back(diff);
					if (this->Times.size() > 1000) {
						this->Times.erase(this->Times.begin());
					}
					return;
				}
			}
		}
	}

	void Profiler::Report()
	{
		std::vector<double> all;

		{
			std::lock_guard lock(this->Locker);
			for (auto diff : this->Times) {
				all.push_back(static_cast<double>(diff));
			}
		}

		std::string message;
		if (all.empty()) {
			message = "No samples have been gathered yet.";
		} else {
			if (all.size() > 1) {
				std::ranges::sort(all);
			}

			double sum = std::accumulate(all.begin(), all.end(), 0.0);
			double avg = sum / all.size();
			const auto middle = all.size() / 2;
			double med = all.size() % 2 == 0 ? (all[middle - 1] + all[middle]) / 2.0 : all[middle];
			double min = all[0];
			double max = all[all.size() - 1];

			std::vector<std::string> things;
			auto timeTemplate = fmt::runtime("{:.3}ms");
			things.emplace_back("Samples = " + std::to_string(all.size()));
			things.emplace_back("Average = " + format(timeTemplate, avg));
			things.emplace_back("Median = " + format(timeTemplate, med));
			things.emplace_back("Min = " + format(timeTemplate, min));
			things.emplace_back("Max = " + format(timeTemplate, max));

			message = Util::StringHelpers::Join(things, "; ");
		}
		RE::SendHUDMessage::ShowHUDMessage(message.c_str(), nullptr, true);
	}
}
