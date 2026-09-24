#pragma once

#include "Core/Ids.h"
#include "Views/ViewManifest.h"
#include "World/WorldAssets.h"

#include <algorithm>
#include <span>

namespace OSFUI
{
	inline bool IsEligibleWorldView(const ViewManifest& a_view, bool a_developerMode)
	{
		return a_view.kind == ViewKind::World && (!a_view.debugOnly || a_developerMode);
	}

	inline const ViewManifest* FindWorldBindingConflict(const ViewManifest& a_view,
		std::span<const ViewManifest> a_views, bool a_developerMode)
	{
		const auto conflict = std::ranges::find_if(a_views, [&](const auto& other) {
			return &other != &a_view && IsEligibleWorldView(other, a_developerMode) &&
			       (Ids::EqualsCaseInsensitiveAscii(other.id, a_view.id) || WorldAssets::KeyForPath(other.texture) == WorldAssets::KeyForPath(a_view.texture));
		});
		return conflict != a_views.end() ? &*conflict : nullptr;
	}
}
