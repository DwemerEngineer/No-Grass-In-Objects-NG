#pragma once

namespace GrassControl::Profiler
{
	inline constexpr std::size_t MaxSamples = 1000;

	inline std::mutex mutex;
	inline std::deque<double> samples;

	void Record(std::chrono::steady_clock::duration time);

	void Report();

	class Scope final
	{
	public:
		Scope() = default;
		~Scope();

		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	};
}
