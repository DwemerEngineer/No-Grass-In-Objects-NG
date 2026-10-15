#include "GrassControl/Events.h"
#include "GrassControl/CliffSurfaces.h"
#include "GrassControl/Config.h"
#include "GrassControl/DistantGrass.h"
#include "GrassControl/Main.h"
#include "GrassControl/PhysicsRaycast.h"

namespace GrassControl::Events
{
	RE::BSEventNotifyControl MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
	{
		if (a_event) {
			if (a_event->menuName == RE::MainMenu::MENU_NAME) {
				if (a_event->opening) {
					Plugin::OnMainMenuOpen();
				} else {
					auto* ui = RE::UI::GetSingleton();
					PhysicsRaycast::GetSingleton().SetQueriesDisabled(!ui);
				}
			} else if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
				auto* ui = RE::UI::GetSingleton();
				PhysicsRaycast::GetSingleton().SetQueriesDisabled(!ui || ui->IsMenuOpen(RE::MainMenu::MENU_NAME));
				if (a_event->opening) {
					CliffSurfaces::Clear();
					if (Config::ExtendGrassDistance)
						DistantGrass::ResetGridUpdates();
				}
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}

}
