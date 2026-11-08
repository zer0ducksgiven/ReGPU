#pragma once
// Xenos microcode to HLSL (Shader Model 5.1).
//
// Vertex shaders fetch their own vertices: the host draws a non-indexed
// triangle list whose vertex IDs index a per-draw list of guest indices
// (already endian-swapped and expanded from fans, strips, quads and
// rectangles). Rectangle corners are marked and computed as v1 + v2 - v0.
//
// Bindings (one root signature for everything):
//   b0  per-draw constants for the vertex shader: c[256], bool constants,
//       vertex fetch table, index list offset, viewport mode
//   b1  per-draw constants for the pixel shader: c[256], bool constants,
//       alpha test, texture sizes / sampler modes
//   t0  guest data (vertex buffers and index lists) as a raw buffer
//   t1+ textures for fetch slots 0-31
//   s0 linear wrap, s1 linear clamp, s2 point clamp, s3 point wrap
#include <cstdint>
#include <string>
#include <array>
#include <vector>

#include "gpu/ucode.h"

struct TranslatedShader
{
    bool ok = false;
    std::string error;
    std::string hlsl;
    uint32_t usedTextureSlots = 0;  // bit per fetch slot (pixel shaders)
    bool memExport = false;         // a vertex shader writing eA/eM (memory export): not translated, its writes are lost
    std::vector<uint32_t> vertexFetchConstants; // vertex shaders
    std::vector<std::array<uint32_t, 4>> vertexFetchLayout; // vfl[k] for each vertex fetch (see hlsl.cpp)
    // Vertex shaders whose vertex fetch indices are simple arithmetic on the vertex index and constants
    // (index = (vertex + c252.x) * c251.y: four corners per particle): the index expressions, and each
    // fetch's {constant, stride, offset, expression}, so a draw can work out how much of a buffer its
    // vertices reach (EvalReachNode). Empty when an index depends on anything else.
    struct ReachNode
    {
        uint8_t op = 0; // ReachOp
        uint8_t comp = 0; // constants: component
        uint16_t a = 0, b = 0; // operands (node indices)
        uint16_t reg = 0; // constants: float constant
    };
    std::vector<ReachNode> reachNodes;
    std::vector<std::array<uint32_t, 4>> vertexFetchReach;
    // Float constants the code reads: c[0] up to this (exclusive), 256 with relative
    // addressing. Only these are uploaded per draw.
    uint32_t constantsUsed = 0;
    // The same as a bit per constant (all of them with relative addressing): a draw copies only
    // these (shaders read a few constants spread up to c[255]: ~440 per draw were copied).
    std::array<uint64_t, 4> constantMask{};
    // Vertex shaders: bit per interpolator (o0-o15) the code exports.
    uint32_t interpolatorsWritten = 0;
};

TranslatedShader TranslateShader(const ucode::Shader& shader);
enum ReachOp : uint8_t { ReachVertex, ReachConst, ReachAdd, ReachMul, ReachMax, ReachMin, ReachFloor, ReachTrunc, ReachNeg, ReachAbs, ReachRound };
// A reach expression's value for one vertex index; constant(reg, comp) reads a float constant.
template <typename Constant>
float EvalReachNode(const std::vector<TranslatedShader::ReachNode>& nodes, uint32_t n, float vertex, const Constant& constant)
{
    const TranslatedShader::ReachNode& r = nodes[n];
    auto at = [&](uint32_t i) { return EvalReachNode(nodes, i, vertex, constant); };
    auto floorf = [](float x) { const float t = float(int64_t(x)); return t > x ? t - 1.0f : t; };
    switch (r.op)
    {
    case ReachVertex: return vertex;
    case ReachConst: return constant(r.reg, r.comp);
    case ReachAdd: return at(r.a) + at(r.b);
    case ReachMul: { const float x = at(r.a), y = at(r.b); return x == 0.0f || y == 0.0f ? 0.0f : x * y; }
    case ReachMax: { const float x = at(r.a), y = at(r.b); return x > y ? x : y; }
    case ReachMin: { const float x = at(r.a), y = at(r.b); return x < y ? x : y; }
    case ReachFloor: return floorf(at(r.a));
    case ReachTrunc: return float(int64_t(at(r.a)));
    case ReachNeg: return -at(r.a);
    case ReachAbs: { const float x = at(r.a); return x < 0 ? -x : x; }
    case ReachRound: return floorf(at(r.a) + 0.5f);
    }
    return 0.0f;
}
// Only the vertex fetch layout and constants of a vertex shader, without generating HLSL:
// for copies of an already translated shader that differ only in their fetch fields (see
// VFetchK). False when a fetch would have to be baked into the code (translate in full).
bool TranslateVertexFetchLayout(const ucode::Shader& shader, TranslatedShader& out);
// Vertex fetch layouts written into each shader as literals (the default): the compiler folds the
// unpacking (FM2 port: 12.7 -> 10.0 ms of GPU time a frame on a GTX 970), at the cost of each patched
// copy of a shader compiling on its own. REGPU_DYNAMIC_VFETCH=1 reads them from the constants instead.
bool BakedVertexFetch();

// Shared HLSL declarations both stages compile against.
const char* HlslPreamble();
// REGPU_LEAN_FETCH=1: lean 2D pixel fetches through a sampler table (root parameter 4, space1).
bool LeanFetch();
