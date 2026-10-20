#include "World/SurfaceCatalog.h"
#include "World/WorldAssets.h"

#include <cstdio>
#include <cstdlib>

namespace
{
	unsigned checks{};
	void Check(bool condition, const char* name)
	{
		++checks;
		if (!condition) {
			std::fprintf(stderr, "FAILED: %s\n", name);
			std::exit(1);
		}
	}

	OSFUI::WorldTexture::SurfaceDesc Feed(const char* id)
	{
		return {id, OSFUI::WorldAssets::TexturePath(id), 1280, 720};
	}
}

int main()
{
	using namespace OSFUI::WorldTexture;
	const auto first = Feed("alpha/screen");
	const auto second = Feed("beta/screen");
	auto malformed = Feed("broken/screen");
	malformed.texture = "x";
	auto renamed = Feed("renamed/screen");
	renamed.texture = first.texture;
	auto oversized = Feed("oversized/screen");
	oversized.width = 4097;
	const std::vector mixed{malformed, first, renamed, oversized, second};
	const auto selected = SelectSupportedSurfaces(mixed);
	Check(selected.accepted.size() == 2 && selected.rejected.size() == 3,
		"malformed, foreign and oversized feeds do not disable valid content");
	Check(selected.accepted[0].id == first.id && selected.accepted[1].id == second.id,
		"accepted feed indices preserve discovery order");
	Check(selected.accepted[0].texture == first.texture && selected.accepted[1].texture == second.texture,
		"accepted feed identities remain unchanged");
	Check(selected.rejected[0].id == malformed.id && selected.rejected[1].id == renamed.id &&
		selected.rejected[2].id == oversized.id, "rejections identify each offending view");

	const auto duplicate = Feed("ALPHA/screen");
	const std::vector repeated{first, duplicate, first, second};
	const auto unique = SelectSupportedSurfaces(repeated);
	Check(unique.accepted.size() == 2 && unique.rejected.size() == 2,
		"duplicate IDs and asset claims do not disable unrelated feeds");
	Check(unique.accepted[0].id == first.id && unique.accepted[1].id == second.id,
		"first valid claim wins without changing subsequent valid indices");

	const std::vector invalid{malformed, renamed, oversized};
	Check(SelectSupportedSurfaces(invalid).accepted.empty(), "all-invalid content admits no feeds");
	Check(SelectSupportedSurfaces({}).accepted.empty(), "empty content admits no feeds");
	std::printf("surface catalog: %u checks passed\n", checks);
}
