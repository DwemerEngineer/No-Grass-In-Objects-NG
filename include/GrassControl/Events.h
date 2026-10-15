#pragma once

namespace GrassControl::Events
{
	class MenuOpenCloseEventHandler final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent*, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
	};

	void Register();
}
