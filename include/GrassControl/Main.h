#pragma once

namespace GrassControl
{
	class RaycastHelper;

	namespace Plugin
	{
		inline std::atomic<std::shared_ptr<const RaycastHelper>> raycastSettings;
		inline int mainMenuUpdates = 0;

		void Initialize();

		void InstallHooks();

		void OnMainMenuOpen();

		void Update();

		[[nodiscard]] std::shared_ptr<const RaycastHelper> GetRaycastSettings();
	}
}
