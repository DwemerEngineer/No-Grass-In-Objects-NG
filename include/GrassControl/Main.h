#pragma once

namespace GrassControl
{
	class RaycastHelper;
}

namespace GrassControl::Plugin
{
	void Initialize();

	void InstallHooks();

	void OnMainMenuOpen();

	void Update();

	[[nodiscard]] std::shared_ptr<const RaycastHelper> GetRaycastSettings();
}
