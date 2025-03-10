// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lua.h"
#include "lualib.h"

#include "Luau/CodeGen.h"
#include "Luau/Compiler.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Parser.h"
#include "Luau/TimeTrace.h"

#include "FileUtils.h"
#include "Flags.h"

#include <memory>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

LUAU_FASTFLAG(DebugLuauTimeTracing)

enum class CompileFormat
{
    Text,
    Binary,
    Remarks,
    Codegen,        // Prints annotated native code including IR and assembly
    CodegenAsm,     // Prints annotated native code assembly
    CodegenIr,      // Prints annotated native code IR
    CodegenVerbose, // Prints annotated native code including IR, assembly and outlined code
    CodegenNull,
    Null
};

struct GlobalOptions
{
    int optimizationLevel = 1;
    int debugLevel = 1;
} globalOptions;

static Luau::CompileOptions copts()
{
    Luau::CompileOptions result = {};
    result.optimizationLevel = globalOptions.optimizationLevel;
    result.debugLevel = globalOptions.debugLevel;

    return result;
}

static std::optional<CompileFormat> getCompileFormat(const char* name)
{
    if (strcmp(name, "text") == 0)
        return CompileFormat::Text;
    else if (strcmp(name, "binary") == 0)
        return CompileFormat::Binary;
    else if (strcmp(name, "text") == 0)
        return CompileFormat::Text;
    else if (strcmp(name, "remarks") == 0)
        return CompileFormat::Remarks;
    else if (strcmp(name, "codegen") == 0)
        return CompileFormat::Codegen;
    else if (strcmp(name, "codegenasm") == 0)
        return CompileFormat::CodegenAsm;
    else if (strcmp(name, "codegenir") == 0)
        return CompileFormat::CodegenIr;
    else if (strcmp(name, "codegenverbose") == 0)
        return CompileFormat::CodegenVerbose;
    else if (strcmp(name, "codegennull") == 0)
        return CompileFormat::CodegenNull;
    else if (strcmp(name, "null") == 0)
        return CompileFormat::Null;
    else
        return std::nullopt;
}

static void report(const char* name, const Luau::Location& location, const char* type, const char* message)
{
    fprintf(stderr, "%s(%d,%d): %s: %s\n", name, location.begin.line + 1, location.begin.column + 1, type, message);
}

static void reportError(const char* name, const Luau::ParseError& error)
{
    report(name, error.getLocation(), "SyntaxError", error.what());
}

static void reportError(const char* name, const Luau::CompileError& error)
{
    report(name, error.getLocation(), "CompileError", error.what());
}

static std::string getCodegenAssembly(const char* name, const std::string& bytecode, Luau::CodeGen::AssemblyOptions options)
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();

    if (luau_load(L, name, bytecode.data(), bytecode.size(), 0) == 0)
        return Luau::CodeGen::getAssembly(L, -1, options);

    fprintf(stderr, "Error loading bytecode %s\n", name);
    return "";
}

static void annotateInstruction(void* context, std::string& text, int fid, int instpos)
{
    Luau::BytecodeBuilder& bcb = *(Luau::BytecodeBuilder*)context;

    bcb.annotateInstruction(text, fid, instpos);
}

struct CompileStats
{
    size_t lines;
    size_t bytecode;
    size_t codegen;

    double readTime;
    double miscTime;
    double parseTime;
    double compileTime;
    double codegenTime;
};

static double recordDeltaTime(double& timer)
{
    double now = Luau::TimeTrace::getClock();
    double delta = now - timer;
    timer = now;
    return delta;
}

static bool compileFile(const char* name, CompileFormat format, CompileStats& stats)
{
    double currts = Luau::TimeTrace::getClock();

    std::optional<std::string> source = readFile(name);
    if (!source)
    {
        fprintf(stderr, "Error opening %s\n", name);
        return false;
    }

    stats.readTime += recordDeltaTime(currts);

    // NOTE: Normally, you should use Luau::compile or luau_compile (see lua_require as an example)
    // This function is much more complicated because it supports many output human-readable formats through internal interfaces

    try
    {
        Luau::BytecodeBuilder bcb;

        Luau::CodeGen::AssemblyOptions options;
        options.outputBinary = format == CompileFormat::CodegenNull;

        if (!options.outputBinary)
        {
            options.includeAssembly = format != CompileFormat::CodegenIr;
            options.includeIr = format != CompileFormat::CodegenAsm;
            options.includeOutlinedCode = format == CompileFormat::CodegenVerbose;
        }

        options.annotator = annotateInstruction;
        options.annotatorContext = &bcb;

        if (format == CompileFormat::Text)
        {
            bcb.setDumpFlags(Luau::BytecodeBuilder::Dump_Code | Luau::BytecodeBuilder::Dump_Source | Luau::BytecodeBuilder::Dump_Locals |
                             Luau::BytecodeBuilder::Dump_Remarks);
            bcb.setDumpSource(*source);
        }
        else if (format == CompileFormat::Remarks)
        {
            bcb.setDumpFlags(Luau::BytecodeBuilder::Dump_Source | Luau::BytecodeBuilder::Dump_Remarks);
            bcb.setDumpSource(*source);
        }
        else if (format == CompileFormat::Codegen || format == CompileFormat::CodegenAsm || format == CompileFormat::CodegenIr ||
                 format == CompileFormat::CodegenVerbose)
        {
            bcb.setDumpFlags(Luau::BytecodeBuilder::Dump_Code | Luau::BytecodeBuilder::Dump_Source | Luau::BytecodeBuilder::Dump_Locals |
                             Luau::BytecodeBuilder::Dump_Remarks);
            bcb.setDumpSource(*source);
        }

        stats.miscTime += recordDeltaTime(currts);

        Luau::Allocator allocator;
        Luau::AstNameTable names(allocator);
        Luau::ParseResult result = Luau::Parser::parse(source->c_str(), source->size(), names, allocator);

        if (!result.errors.empty())
            throw Luau::ParseErrors(result.errors);

        stats.lines += result.lines;
        stats.parseTime += recordDeltaTime(currts);

        Luau::compileOrThrow(bcb, result, names, copts());
        stats.bytecode += bcb.getBytecode().size();
        stats.compileTime += recordDeltaTime(currts);

        switch (format)
        {
        case CompileFormat::Text:
            printf("%s", bcb.dumpEverything().c_str());
            break;
        case CompileFormat::Remarks:
            printf("%s", bcb.dumpSourceRemarks().c_str());
            break;
        case CompileFormat::Binary:
            fwrite(bcb.getBytecode().data(), 1, bcb.getBytecode().size(), stdout);
            break;
        case CompileFormat::Codegen:
        case CompileFormat::CodegenAsm:
        case CompileFormat::CodegenIr:
        case CompileFormat::CodegenVerbose:
            printf("%s", getCodegenAssembly(name, bcb.getBytecode(), options).c_str());
            break;
        case CompileFormat::CodegenNull:
            stats.codegen += getCodegenAssembly(name, bcb.getBytecode(), options).size();
            stats.codegenTime += recordDeltaTime(currts);
            break;
        case CompileFormat::Null:
            break;
        }

        return true;
    }
    catch (Luau::ParseErrors& e)
    {
        for (auto& error : e.getErrors())
            reportError(name, error);
        return false;
    }
    catch (Luau::CompileError& e)
    {
        reportError(name, e);
        return false;
    }
}

static void displayHelp(const char* argv0)
{
    printf("Usage: %s [--mode] [options] [file list]\n", argv0);
    printf("\n");
    printf("Available modes:\n");
    printf("   binary, text, remarks, codegen\n");
    printf("\n");
    printf("Available options:\n");
    printf("  -h, --help: Display this usage message.\n");
    printf("  -O<n>: compile with optimization level n (default 1, n should be between 0 and 2).\n");
    printf("  -g<n>: compile with debug level n (default 1, n should be between 0 and 2).\n");
    printf("  --timetrace: record compiler time tracing information into trace.json\n");
}

static int assertionHandler(const char* expr, const char* file, int line, const char* function)
{
    printf("%s(%d): ASSERTION FAILED: %s\n", file, line, expr);
    return 1;
}

int main(int argc, char** argv)
{
    Luau::assertHandler() = assertionHandler;

    setLuauFlagsDefault();

    CompileFormat compileFormat = CompileFormat::Text;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            displayHelp(argv[0]);
            return 0;
        }
        else if (strncmp(argv[i], "-O", 2) == 0)
        {
            int level = atoi(argv[i] + 2);
            if (level < 0 || level > 2)
            {
                fprintf(stderr, "Error: Optimization level must be between 0 and 2 inclusive.\n");
                return 1;
            }
            globalOptions.optimizationLevel = level;
        }
        else if (strncmp(argv[i], "-g", 2) == 0)
        {
            int level = atoi(argv[i] + 2);
            if (level < 0 || level > 2)
            {
                fprintf(stderr, "Error: Debug level must be between 0 and 2 inclusive.\n");
                return 1;
            }
            globalOptions.debugLevel = level;
        }
        else if (strcmp(argv[i], "--timetrace") == 0)
        {
            FFlag::DebugLuauTimeTracing.value = true;
        }
        else if (strncmp(argv[i], "--fflags=", 9) == 0)
        {
            setLuauFlags(argv[i] + 9);
        }
        else if (argv[i][0] == '-' && argv[i][1] == '-' && getCompileFormat(argv[i] + 2))
        {
            compileFormat = *getCompileFormat(argv[i] + 2);
        }
        else if (argv[i][0] == '-')
        {
            fprintf(stderr, "Error: Unrecognized option '%s'.\n\n", argv[i]);
            displayHelp(argv[0]);
            return 1;
        }
    }

#if !defined(LUAU_ENABLE_TIME_TRACE)
    if (FFlag::DebugLuauTimeTracing)
    {
        fprintf(stderr, "To run with --timetrace, Luau has to be built with LUAU_ENABLE_TIME_TRACE enabled\n");
        return 1;
    }
#endif

    const std::vector<std::string> files = getSourceFiles(argc, argv);

#ifdef _WIN32
    if (compileFormat == CompileFormat::Binary)
        _setmode(_fileno(stdout), _O_BINARY);
#endif

    CompileStats stats = {};
    int failed = 0;

    for (const std::string& path : files)
        failed += !compileFile(path.c_str(), compileFormat, stats);

    if (compileFormat == CompileFormat::Null)
        printf("Compiled %d KLOC into %d KB bytecode (read %.2fs, parse %.2fs, compile %.2fs)\n", int(stats.lines / 1000), int(stats.bytecode / 1024),
            stats.readTime, stats.parseTime, stats.compileTime);
    else if (compileFormat == CompileFormat::CodegenNull)
        printf("Compiled %d KLOC into %d KB bytecode => %d KB native code (%.2fx) (read %.2fs, parse %.2fs, compile %.2fs, codegen %.2fs)\n",
            int(stats.lines / 1000), int(stats.bytecode / 1024), int(stats.codegen / 1024),
            stats.bytecode == 0 ? 0.0 : double(stats.codegen) / double(stats.bytecode), stats.readTime, stats.parseTime, stats.compileTime,
            stats.codegenTime);

    return failed ? 1 : 0;
}
