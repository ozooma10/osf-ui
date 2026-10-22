#include "World/ScreenFormId.h"

#include <cstdio>
#include <cstdlib>

namespace F = OSFUI::ScreenFormId;

unsigned checks{};
void Check(bool condition, const char* name)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAILED: %s\n", name); std::exit(1); }
}
void Composes(F::Tier tier, std::uint32_t slot, std::uint32_t local, std::uint32_t expected, const char* name)
{
    const auto composed = F::Compose(tier, slot, local);
    Check(composed && *composed == expected, name);
}
void Refuses(F::Tier tier, std::uint32_t slot, std::uint32_t local, const char* name)
{
    Check(!F::Compose(tier, slot, local), name);
}

int main()
{
    // Full masters spend the high byte on the compile index and keep a 24-bit local.
    Composes(F::Tier::Full, 0x00, 0x000800, 0x00000800, "full master at index zero");
    Composes(F::Tier::Full, 0x05, 0x000800, 0x05000800, "full master composes its compile index");
    Composes(F::Tier::Full, 0xFC, 0xFFFFFF, 0xFCFFFFFF, "full master at its widest slot and local");
    Refuses(F::Tier::Full, 0xFD, 0x000800, "full master cannot occupy the medium prefix");
    Refuses(F::Tier::Full, 0x05, 0x1000000, "full master local stays within 24 bits");

    // Medium masters live under 0xFD with an 8-bit slot and a 16-bit local.
    Composes(F::Tier::Medium, 0x00, 0x0800, 0xFD000800, "medium master at slot zero");
    Composes(F::Tier::Medium, 0x03, 0x1234, 0xFD031234, "medium master composes its tier slot");
    Composes(F::Tier::Medium, 0xFF, 0xFFFF, 0xFDFFFFFF, "medium master at its widest slot and local");
    Refuses(F::Tier::Medium, 0x100, 0x0800, "medium slot stays within 8 bits");
    Refuses(F::Tier::Medium, 0x03, 0x10000, "medium local stays within 16 bits");

    // Small masters live under 0xFE with a 12-bit slot and a 12-bit local.
    Composes(F::Tier::Small, 0x000, 0x800, 0xFE000800, "small master at slot zero");
    Composes(F::Tier::Small, 0x012, 0x7FF, 0xFE0127FF, "small master composes its tier slot");
    Composes(F::Tier::Small, 0xFFF, 0xFFF, 0xFEFFFFFF, "small master at its widest slot and local");
    Refuses(F::Tier::Small, 0x1000, 0x800, "small slot stays within 12 bits");
    Refuses(F::Tier::Small, 0x012, 0x1000, "small local stays within 12 bits");

    // A local ID valid for a full master must not silently truncate into a
    // narrower tier: that would bind a different form than the author wrote.
    Refuses(F::Tier::Small, 0x012, 0x1800, "an oversized local is refused, not masked");
    Refuses(F::Tier::Medium, 0x003, 0x18000, "an oversized medium local is refused, not masked");

    // Tier slots never bleed into the prefix byte.
    Check((*F::Compose(F::Tier::Small, 0xFFF, 0x000) >> 24) == 0xFE, "small composition keeps its prefix");
    Check((*F::Compose(F::Tier::Medium, 0xFF, 0x0000) >> 24) == 0xFD, "medium composition keeps its prefix");

    std::printf("screen form id tests passed (%u checks)\n", checks);
    return 0;
}
