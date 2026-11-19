// ucode::Parse reading the control flow past the end instruction (and what the translator makes of it):
// a jump target behind the end and the fall-through after it, code nothing reaches after the end, and an
// unconditional jump with no fall-through. Not exercised here: return, and loop start / loop end targets.
#include <cstdio>
#include <string>
#include <vector>

#include "gpu/hlsl.h"
#include "gpu/ucode.h"

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition)
        std::fprintf(stderr, "%s\n", message);
    return condition;
}

// One control flow instruction: 32 low bits and 16 high bits (the opcode in bits 12-15 of the high half).
struct Cf
{
    uint32_t lo = 0, hi = 0;
};
Cf Exec(ucode::CfOp op, uint32_t address, uint32_t count) { return { address | count << 12, uint32_t(op) << 12 }; }
Cf Jump(uint32_t target, bool unconditional) { return { target | uint32_t(unconditional) << 13, uint32_t(ucode::CfOp::CondJmp) << 12 }; }

// Microcode as the guest holds it (big-endian): the control flow instructions two to three dwords,
// then `aluSlots` instructions of three zero dwords (vector add, nothing written).
std::vector<uint32_t> Microcode(const std::vector<Cf>& cf, uint32_t aluSlots)
{
    std::vector<uint32_t> host;
    for (size_t i = 0; i < cf.size(); i += 2)
    {
        const Cf a = cf[i], b = i + 1 < cf.size() ? cf[i + 1] : Cf{};
        host.push_back(a.lo);
        host.push_back((a.hi & 0xFFFF) | b.lo << 16);
        host.push_back(b.lo >> 16 | b.hi << 16);
    }
    host.resize(host.size() + aluSlots * 3, 0);
    for (uint32_t& w : host)
        w = __builtin_bswap32(w);
    return host;
}

bool Contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
}

int main()
{
    using ucode::CfOp;
    bool ok = true;

    // Code behind the end that a jump reaches: 0 jumps (on a bool constant) to 3, 1 ends, 3 runs the code
    // and falls through to 4, which jumps back to 1. Instructions 3 and 4 lie after the end.
    {
        const std::vector<uint32_t> code = Microcode({ Jump(3, false), Exec(CfOp::ExecEnd, 3, 1), {}, Exec(CfOp::Exec, 3, 1),
                                                         Jump(1, true), {} },
            1);
        const ucode::Shader shader = ucode::Parse(code.data(), uint32_t(code.size()), false);
        ok &= Check(shader.valid, "jump past the end: the program does not parse");
        ok &= Check(shader.cf.size() >= 5, "jump past the end: the control flow was not read past the end instruction");
        if (shader.cf.size() >= 5)
            ok &= Check(shader.cf[3].op() == CfOp::Exec && shader.cf[4].op() == CfOp::CondJmp,
                "jump past the end: instructions 3 and 4 were not read as they are");
        const TranslatedShader t = TranslateShader(shader);
        ok &= Check(Contains(t.hlsl, "cfpc == 3u"), "jump past the end: no block for the jump target (instruction 3)");
        ok &= Check(Contains(t.hlsl, "cfpc == 4u"), "jump past the end: no block for the fall-through after it (instruction 4)");
        // The layout-only walk stops at the end, so it would miss fetches in the code behind it.
        TranslatedShader layout;
        ok &= Check(!TranslateVertexFetchLayout(shader, layout), "jump past the end: the layout-only walk accepted code behind the end");
    }

    // Nothing reaches what follows the end: the control flow stops there (the rest is padding or data).
    {
        const std::vector<uint32_t> code = Microcode({ Exec(CfOp::ExecEnd, 2, 1), {}, Exec(CfOp::Exec, 2, 1), Jump(0, true) }, 1);
        const ucode::Shader shader = ucode::Parse(code.data(), uint32_t(code.size()), false);
        ok &= Check(shader.valid, "unreachable after the end: the program does not parse");
        ok &= Check(shader.cf.size() == 2, "unreachable after the end: the control flow was read past the end instruction");
    }

    // An unconditional jump has no fall-through: 0 jumps to 2, 1 ends, 2 runs the code, 3 (the last
    // instruction of its triple) jumps back to 1 unconditionally. Instruction 4, in the next triple, is
    // reached by nothing, so that triple is not read.
    {
        const std::vector<uint32_t> code = Microcode({ Jump(2, false), Exec(CfOp::ExecEnd, 3, 1), Exec(CfOp::Exec, 3, 1), Jump(1, true),
                                                         Exec(CfOp::Exec, 3, 1), {} },
            1);
        const ucode::Shader shader = ucode::Parse(code.data(), uint32_t(code.size()), false);
        ok &= Check(shader.valid, "unconditional jump: the program does not parse");
        ok &= Check(shader.cf.size() == 4, "unconditional jump: the control flow was read past an unconditional jump nothing follows");
    }
    return ok ? 0 : 1;
}
