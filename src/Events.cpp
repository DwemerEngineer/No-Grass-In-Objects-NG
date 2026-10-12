#include "GrassControl/Events.h"
#include "GrassControl/Config.h"
#include "GrassControl/DistantGrass.h"
#include "GrassControl/Main.h"
#include "GrassControl/PhysicsRaycast.h"
#include "GrassControl/RaycastHelper.h"

namespace GrassControl::Events
{
	RE::BSEventNotifyControl CellLoadEventHandler::ProcessEvent(const RE::TESCellAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*)
	{
		if (a_event) {
			auto refr = a_event->reference;
			if (!refr)
				return RE::BSEventNotifyControl::kContinue;

			auto cell = refr->GetParentCell();
			if (!cell)
				return RE::BSEventNotifyControl::kContinue;

			if (cell->IsInteriorCell()) {
				if (Config::OnlyLoadFromCache && Config::ExtendGrassDistance) {
					DistantGrass::UnloadAll();
				}
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
	{
		if (a_event) {
			if (a_event->menuName == RE::MainMenu::MENU_NAME) {
				if (a_event->opening) {
					Plugin::OnMainMenuOpen();
				}
			} else if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
				Raycast::SetSuspended(a_event->opening);
				if (a_event->opening && Config::ExtendGrassDistance) {
					DistantGrass::ResetGridUpdates();
				}
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	void Register()
	{
		static MenuOpenCloseEventHandler menuEvents;
		static CellLoadEventHandler cellEvents;

		RE::UI::GetSingleton()->AddEventSink(&menuEvents);
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(&cellEvents);
	}
}
