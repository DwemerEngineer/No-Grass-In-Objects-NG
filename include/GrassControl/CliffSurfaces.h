#pragma once

namespace GrassControl::CliffSurfaces
{
	struct Surface
	{
		RE::NiPoint3 center;
		float radius;
		RE::FormID object;
		RE::TESLandTexture* landTexture;  // Matches the cliff's diffuse texture and supplies its grass types.
	};

	// Cache results only after every eligible cliff has loaded its 3D.
	std::vector<Surface> Get(RE::TESObjectCELL* cell);

	void Forget(RE::FormID cell);

	void Clear();
}
