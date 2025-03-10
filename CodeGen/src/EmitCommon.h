// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Label.h"

namespace Luau
{
namespace CodeGen
{

constexpr unsigned kTValueSizeLog2 = 4;
constexpr unsigned kLuaNodeSizeLog2 = 5;

// TKey.tt and TKey.next are packed together in a bitfield
constexpr unsigned kOffsetOfTKeyTagNext = 12;  // offsetof cannot be used on a bit field
constexpr unsigned kTKeyTagBits = 4;
constexpr unsigned kTKeyTagMask = (1 << kTKeyTagBits) - 1;

constexpr unsigned kOffsetOfInstructionC = 3;

// Leaf functions that are placed in every module to perform common instruction sequences
struct ModuleHelpers
{
    // A64/X64
    Label exitContinueVm;
    Label exitNoContinueVm;
    Label return_;

    // X64
    Label continueCallInVm;

    // A64
    Label reentry;   // x0: closure
    Label interrupt; // x0: pc offset, x1: return address, x2: interrupt
};

} // namespace CodeGen
} // namespace Luau
