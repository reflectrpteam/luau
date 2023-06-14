// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/DenseHash.h"
#include "Luau/FileResolver.h"
#include "Luau/Location.h"

#include <string>
#include <vector>

namespace Luau
{

class AstStat;
class AstExpr;
class AstStatBlock;
struct AstLocal;

struct RequireListEntry
{
    ModuleName name;

    Location location;

    std::string_view typeName{}; // e.g. require or include
};

struct RequireTraceResult
{
    DenseHashMap<const AstExpr*, ModuleInfo> exprs{nullptr};

    std::vector<RequireListEntry> requireList;
};

RequireTraceResult traceRequires(FileResolver* fileResolver, AstStatBlock* root, const ModuleName& currentModuleName, const char* fnName = "require");

} // namespace Luau
