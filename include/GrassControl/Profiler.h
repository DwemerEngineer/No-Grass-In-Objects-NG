#pragma once

// Cell timings are recorded only when Profiler-report is enabled.
namespace GrassControl::Profiler
{
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
