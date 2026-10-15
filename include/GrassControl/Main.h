#pragma once

namespace GrassControl
{
	class RaycastHelper;

	namespace Plugin
	{
		void Initialize();

		void InstallHooks();

		void OnMainMenuOpen();

		void Update();

		[[nodiscard]] std::shared_ptr<const RaycastHelper> GetRaycastSettings();
	}
}
