#pragma once

#include <RE/B/BSTEvent.h>
#include <RE/M/MenuOpenCloseEvent.h>
#include <RE/T/TESCellAttachDetachEvent.h>

namespace GrassControl::Events
{
	class CellLoadEventHandler final : public RE::BSTEventSink<RE::TESCellAttachDetachEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent*, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override;
	};

	class MenuOpenCloseEventHandler final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent*, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
	};

	void Register();
}
