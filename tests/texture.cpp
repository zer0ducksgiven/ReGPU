#include <algorithm>
#include <cstdio>
#include <vector>

#include "gpu/texture.h"

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition)
        std::fprintf(stderr, "%s\n", message);
    return condition;
}
}

int main()
{
    TextureFetch fetch{};
    fetch.dwords[0] = 2;
    fetch.dwords[1] = 24;
    fetch.dwords[2] = 2;
    fetch.dwords[3] = 0x688 << 1;
    fetch.dwords[5] = 1 << 9;

    const uint8_t source[] = { 0x00, 0x00, 0x00, 0x80, 0xFF, 0xFF };
    std::vector<uint8_t> rgba;

    bool ok = true;
    ok &= Check(TextureFormatSupported(24), "R16 format is not supported");
    ok &= Check(TextureBCKind(24) == 7, "R16 format is not classified for native upload");
    ok &= Check(TextureSourceSize(fetch) == sizeof(source), "R16 source size is not two bytes per texel");
    ok &= Check(DecodeTexture(fetch, source, rgba), "R16 fallback decode failed");
    ok &= Check(rgba.size() == 12, "R16 fallback decode returned the wrong size");
    if (rgba.size() == 12)
    {
        const uint8_t expected[] = {
            0, 0, 0, 255,
            128, 128, 128, 255,
            255, 255, 255, 255,
        };
        ok &= Check(std::equal(rgba.begin(), rgba.end(), expected), "R16 fallback decode did not round UNORM values");
    }
    return ok ? 0 : 1;
}
