// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/TypeChecker2.h"

#include "Luau/Ast.h"
#include "Luau/AstQuery.h"
#include "Luau/Clone.h"
#include "Luau/Common.h"
#include "Luau/DcrLogger.h"
#include "Luau/Error.h"
#include "Luau/Instantiation.h"
#include "Luau/Metamethods.h"
#include "Luau/Normalize.h"
#include "Luau/ToString.h"
#include "Luau/TxnLog.h"
#include "Luau/Type.h"
#include "Luau/TypePack.h"
#include "Luau/TypeUtils.h"
#include "Luau/Unifier.h"
#include "Luau/TypeFamily.h"
#include "Luau/VisitType.h"

#include <algorithm>

LUAU_FASTFLAG(DebugLuauMagicTypes)

namespace Luau
{

// TypeInfer.h
// TODO move these
using PrintLineProc = void (*)(const std::string&);
extern PrintLineProc luauPrintLine;

/* Push a scope onto the end of a stack for the lifetime of the StackPusher instance.
 * TypeChecker2 uses this to maintain knowledge about which scope encloses every
 * given AstNode.
 */
struct StackPusher
{
    std::vector<NotNull<Scope>>* stack;
    NotNull<Scope> scope;

    explicit StackPusher(std::vector<NotNull<Scope>>& stack, Scope* scope)
        : stack(&stack)
        , scope(scope)
    {
        stack.push_back(NotNull{scope});
    }

    ~StackPusher()
    {
        if (stack)
        {
            LUAU_ASSERT(stack->back() == scope);
            stack->pop_back();
        }
    }

    StackPusher(const StackPusher&) = delete;
    StackPusher&& operator=(const StackPusher&) = delete;

    StackPusher(StackPusher&& other)
        : stack(std::exchange(other.stack, nullptr))
        , scope(other.scope)
    {
    }
};

static std::optional<std::string> getIdentifierOfBaseVar(AstExpr* node)
{
    if (AstExprGlobal* expr = node->as<AstExprGlobal>())
        return expr->name.value;

    if (AstExprLocal* expr = node->as<AstExprLocal>())
        return expr->local->name.value;

    if (AstExprIndexExpr* expr = node->as<AstExprIndexExpr>())
        return getIdentifierOfBaseVar(expr->expr);

    if (AstExprIndexName* expr = node->as<AstExprIndexName>())
        return getIdentifierOfBaseVar(expr->expr);

    return std::nullopt;
}

template<typename T>
bool areEquivalent(const T& a, const T& b)
{
    if (a.family != b.family)
        return false;

    if (a.typeArguments.size() != b.typeArguments.size() || a.packArguments.size() != b.packArguments.size())
        return false;

    for (size_t i = 0; i < a.typeArguments.size(); ++i)
    {
        if (follow(a.typeArguments[i]) != follow(b.typeArguments[i]))
            return false;
    }

    for (size_t i = 0; i < a.packArguments.size(); ++i)
    {
        if (follow(a.packArguments[i]) != follow(b.packArguments[i]))
            return false;
    }

    return true;
}

struct FamilyFinder : TypeOnceVisitor
{
    DenseHashSet<TypeId> mentionedFamilies{nullptr};
    DenseHashSet<TypePackId> mentionedFamilyPacks{nullptr};

    bool visit(TypeId ty, const TypeFamilyInstanceType&) override
    {
        mentionedFamilies.insert(ty);
        return true;
    }

    bool visit(TypePackId tp, const TypeFamilyInstanceTypePack&) override
    {
        mentionedFamilyPacks.insert(tp);
        return true;
    }
};

struct InternalFamilyFinder : TypeOnceVisitor
{
    DenseHashSet<TypeId> internalFamilies{nullptr};
    DenseHashSet<TypePackId> internalPackFamilies{nullptr};
    DenseHashSet<TypeId> mentionedFamilies{nullptr};
    DenseHashSet<TypePackId> mentionedFamilyPacks{nullptr};

    InternalFamilyFinder(std::vector<TypeId>& declStack)
    {
        FamilyFinder f;
        for (TypeId fn : declStack)
            f.traverse(fn);

        mentionedFamilies = std::move(f.mentionedFamilies);
        mentionedFamilyPacks = std::move(f.mentionedFamilyPacks);
    }

    bool visit(TypeId ty, const TypeFamilyInstanceType& tfit) override
    {
        bool hasGeneric = false;

        for (TypeId p : tfit.typeArguments)
        {
            if (get<GenericType>(follow(p)))
            {
                hasGeneric = true;
                break;
            }
        }

        for (TypePackId p : tfit.packArguments)
        {
            if (get<GenericTypePack>(follow(p)))
            {
                hasGeneric = true;
                break;
            }
        }

        if (hasGeneric)
        {
            for (TypeId mentioned : mentionedFamilies)
            {
                const TypeFamilyInstanceType* mentionedTfit = get<TypeFamilyInstanceType>(mentioned);
                LUAU_ASSERT(mentionedTfit);
                if (areEquivalent(tfit, *mentionedTfit))
                {
                    return true;
                }
            }

            internalFamilies.insert(ty);
        }

        return true;
    }

    bool visit(TypePackId tp, const TypeFamilyInstanceTypePack& tfitp) override
    {
        bool hasGeneric = false;

        for (TypeId p : tfitp.typeArguments)
        {
            if (get<GenericType>(follow(p)))
            {
                hasGeneric = true;
                break;
            }
        }

        for (TypePackId p : tfitp.packArguments)
        {
            if (get<GenericTypePack>(follow(p)))
            {
                hasGeneric = true;
                break;
            }
        }

        if (hasGeneric)
        {
            for (TypePackId mentioned : mentionedFamilyPacks)
            {
                const TypeFamilyInstanceTypePack* mentionedTfitp = get<TypeFamilyInstanceTypePack>(mentioned);
                LUAU_ASSERT(mentionedTfitp);
                if (areEquivalent(tfitp, *mentionedTfitp))
                {
                    return true;
                }
            }

            internalPackFamilies.insert(tp);
        }

        return true;
    }
};

struct TypeChecker2
{
    NotNull<BuiltinTypes> builtinTypes;
    DcrLogger* logger;
    NotNull<InternalErrorReporter> ice;
    const SourceModule* sourceModule;
    Module* module;
    TypeArena testArena;

    std::vector<NotNull<Scope>> stack;
    std::vector<TypeId> functionDeclStack;

    DenseHashSet<TypeId> noTypeFamilyErrors{nullptr};

    Normalizer normalizer;

    TypeChecker2(NotNull<BuiltinTypes> builtinTypes, NotNull<UnifierSharedState> unifierState, DcrLogger* logger, const SourceModule* sourceModule,
        Module* module)
        : builtinTypes(builtinTypes)
        , logger(logger)
        , ice(unifierState->iceHandler)
        , sourceModule(sourceModule)
        , module(module)
        , normalizer{&testArena, builtinTypes, unifierState, /* cacheInhabitance */ true}
    {
    }

    std::optional<StackPusher> pushStack(AstNode* node)
    {
        if (Scope** scope = module->astScopes.find(node))
            return StackPusher{stack, *scope};
        else
            return std::nullopt;
    }

    void checkForInternalFamily(TypeId ty, Location location)
    {
        InternalFamilyFinder finder(functionDeclStack);
        finder.traverse(ty);

        for (TypeId internal : finder.internalFamilies)
            reportError(WhereClauseNeeded{internal}, location);

        for (TypePackId internal : finder.internalPackFamilies)
            reportError(PackWhereClauseNeeded{internal}, location);
    }

    TypeId checkForFamilyInhabitance(TypeId instance, Location location)
    {
        if (noTypeFamilyErrors.find(instance))
            return instance;

        TxnLog fake{};
        ErrorVec errors =
            reduceFamilies(instance, location, NotNull{&testArena}, builtinTypes, stack.back(), NotNull{&normalizer}, &fake, true).errors;

        if (errors.empty())
            noTypeFamilyErrors.insert(instance);

        reportErrors(std::move(errors));
        return instance;
    }

    TypePackId lookupPack(AstExpr* expr)
    {
        // If a type isn't in the type graph, it probably means that a recursion limit was exceeded.
        // We'll just return anyType in these cases.  Typechecking against any is very fast and this
        // allows us not to think about this very much in the actual typechecking logic.
        TypePackId* tp = module->astTypePacks.find(expr);
        if (tp)
            return follow(*tp);
        else
            return builtinTypes->anyTypePack;
    }

    TypeId lookupType(AstExpr* expr)
    {
        // If a type isn't in the type graph, it probably means that a recursion limit was exceeded.
        // We'll just return anyType in these cases.  Typechecking against any is very fast and this
        // allows us not to think about this very much in the actual typechecking logic.
        TypeId* ty = module->astTypes.find(expr);
        if (ty)
            return checkForFamilyInhabitance(follow(*ty), expr->location);

        TypePackId* tp = module->astTypePacks.find(expr);
        if (tp)
            return checkForFamilyInhabitance(flattenPack(*tp), expr->location);

        return builtinTypes->anyType;
    }

    TypeId lookupAnnotation(AstType* annotation)
    {
        if (FFlag::DebugLuauMagicTypes)
        {
            if (auto ref = annotation->as<AstTypeReference>(); ref && ref->name == "_luau_print" && ref->parameters.size > 0)
            {
                if (auto ann = ref->parameters.data[0].type)
                {
                    TypeId argTy = lookupAnnotation(ref->parameters.data[0].type);
                    luauPrintLine(format(
                        "_luau_print (%d, %d): %s\n", annotation->location.begin.line, annotation->location.begin.column, toString(argTy).c_str()));
                    return follow(argTy);
                }
            }
        }

        TypeId* ty = module->astResolvedTypes.find(annotation);
        LUAU_ASSERT(ty);
        return checkForFamilyInhabitance(follow(*ty), annotation->location);
    }

    TypePackId lookupPackAnnotation(AstTypePack* annotation)
    {
        TypePackId* tp = module->astResolvedTypePacks.find(annotation);
        LUAU_ASSERT(tp);
        return follow(*tp);
    }

    TypeId lookupExpectedType(AstExpr* expr)
    {
        if (TypeId* ty = module->astExpectedTypes.find(expr))
            return follow(*ty);

        return builtinTypes->anyType;
    }

    TypePackId lookupExpectedPack(AstExpr* expr, TypeArena& arena)
    {
        if (TypeId* ty = module->astExpectedTypes.find(expr))
            return arena.addTypePack(TypePack{{follow(*ty)}, std::nullopt});

        return builtinTypes->anyTypePack;
    }

    TypePackId reconstructPack(AstArray<AstExpr*> exprs, TypeArena& arena)
    {
        if (exprs.size == 0)
            return arena.addTypePack(TypePack{{}, std::nullopt});

        std::vector<TypeId> head;

        for (size_t i = 0; i < exprs.size - 1; ++i)
        {
            head.push_back(lookupType(exprs.data[i]));
        }

        TypePackId tail = lookupPack(exprs.data[exprs.size - 1]);
        return arena.addTypePack(TypePack{head, tail});
    }

    Scope* findInnermostScope(Location location)
    {
        Scope* bestScope = module->getModuleScope().get();
        Location bestLocation = module->scopes[0].first;

        for (size_t i = 0; i < module->scopes.size(); ++i)
        {
            auto& [scopeBounds, scope] = module->scopes[i];
            if (scopeBounds.encloses(location))
            {
                if (scopeBounds.begin > bestLocation.begin || scopeBounds.end < bestLocation.end)
                {
                    bestScope = scope.get();
                    bestLocation = scopeBounds;
                }
            }
        }

        return bestScope;
    }

    void visit(AstStat* stat)
    {
        auto pusher = pushStack(stat);

        if (auto s = stat->as<AstStatBlock>())
            return visit(s);
        else if (auto s = stat->as<AstStatIf>())
            return visit(s);
        else if (auto s = stat->as<AstStatWhile>())
            return visit(s);
        else if (auto s = stat->as<AstStatRepeat>())
            return visit(s);
        else if (auto s = stat->as<AstStatBreak>())
            return visit(s);
        else if (auto s = stat->as<AstStatContinue>())
            return visit(s);
        else if (auto s = stat->as<AstStatReturn>())
            return visit(s);
        else if (auto s = stat->as<AstStatExpr>())
            return visit(s);
        else if (auto s = stat->as<AstStatLocal>())
            return visit(s);
        else if (auto s = stat->as<AstStatFor>())
            return visit(s);
        else if (auto s = stat->as<AstStatForIn>())
            return visit(s);
        else if (auto s = stat->as<AstStatAssign>())
            return visit(s);
        else if (auto s = stat->as<AstStatCompoundAssign>())
            return visit(s);
        else if (auto s = stat->as<AstStatFunction>())
            return visit(s);
        else if (auto s = stat->as<AstStatLocalFunction>())
            return visit(s);
        else if (auto s = stat->as<AstStatTypeAlias>())
            return visit(s);
        else if (auto s = stat->as<AstStatDeclareFunction>())
            return visit(s);
        else if (auto s = stat->as<AstStatDeclareGlobal>())
            return visit(s);
        else if (auto s = stat->as<AstStatDeclareClass>())
            return visit(s);
        else if (auto s = stat->as<AstStatError>())
            return visit(s);
        else
            LUAU_ASSERT(!"TypeChecker2 encountered an unknown node type");
    }

    void visit(AstStatBlock* block)
    {
        auto StackPusher = pushStack(block);

        for (AstStat* statement : block->body)
            visit(statement);
    }

    void visit(AstStatIf* ifStatement)
    {
        visit(ifStatement->condition, ValueContext::RValue);
        visit(ifStatement->thenbody);
        if (ifStatement->elsebody)
            visit(ifStatement->elsebody);
    }

    void visit(AstStatWhile* whileStatement)
    {
        visit(whileStatement->condition, ValueContext::RValue);
        visit(whileStatement->body);
    }

    void visit(AstStatRepeat* repeatStatement)
    {
        visit(repeatStatement->body);
        visit(repeatStatement->condition, ValueContext::RValue);
    }

    void visit(AstStatBreak*) {}

    void visit(AstStatContinue*) {}

    void visit(AstStatReturn* ret)
    {
        Scope* scope = findInnermostScope(ret->location);
        TypePackId expectedRetType = scope->returnType;

        TypeArena* arena = &testArena;
        TypePackId actualRetType = reconstructPack(ret->list, *arena);

        Unifier u{NotNull{&normalizer}, stack.back(), ret->location, Covariant};
        u.hideousFixMeGenericsAreActuallyFree = true;

        u.tryUnify(actualRetType, expectedRetType);
        const bool ok = u.errors.empty() && u.log.empty();

        if (!ok)
        {
            for (const TypeError& e : u.errors)
                reportError(e);
        }

        for (AstExpr* expr : ret->list)
            visit(expr, ValueContext::RValue);
    }

    void visit(AstStatExpr* expr)
    {
        visit(expr->expr, ValueContext::RValue);
    }

    void visit(AstStatLocal* local)
    {
        size_t count = std::max(local->values.size, local->vars.size);
        for (size_t i = 0; i < count; ++i)
        {
            AstExpr* value = i < local->values.size ? local->values.data[i] : nullptr;
            const bool isPack = value && (value->is<AstExprCall>() || value->is<AstExprVarargs>());

            if (value)
                visit(value, ValueContext::RValue);

            if (i != local->values.size - 1 || !isPack)
            {
                AstLocal* var = i < local->vars.size ? local->vars.data[i] : nullptr;

                if (var && var->annotation)
                {
                    TypeId annotationType = lookupAnnotation(var->annotation);
                    TypeId valueType = value ? lookupType(value) : nullptr;
                    if (valueType)
                    {
                        ErrorVec errors = tryUnify(stack.back(), value->location, valueType, annotationType);
                        if (!errors.empty())
                            reportErrors(std::move(errors));
                    }

                    visit(var->annotation);
                }
            }
            else if (value)
            {
                TypePackId valuePack = lookupPack(value);
                TypePack valueTypes;
                if (i < local->vars.size)
                    valueTypes = extendTypePack(module->internalTypes, builtinTypes, valuePack, local->vars.size - i);

                Location errorLocation;
                for (size_t j = i; j < local->vars.size; ++j)
                {
                    if (j - i >= valueTypes.head.size())
                    {
                        errorLocation = local->vars.data[j]->location;
                        break;
                    }

                    AstLocal* var = local->vars.data[j];
                    if (var->annotation)
                    {
                        TypeId varType = lookupAnnotation(var->annotation);
                        ErrorVec errors = tryUnify(stack.back(), value->location, valueTypes.head[j - i], varType);
                        if (!errors.empty())
                            reportErrors(std::move(errors));

                        visit(var->annotation);
                    }
                }

                if (valueTypes.head.size() < local->vars.size - i)
                {
                    reportError(
                        CountMismatch{
                            // We subtract 1 here because the final AST
                            // expression is not worth one value.  It is worth 0
                            // or more depending on valueTypes.head
                            local->values.size - 1 + valueTypes.head.size(),
                            std::nullopt,
                            local->vars.size,
                            local->values.data[local->values.size - 1]->is<AstExprCall>() ? CountMismatch::FunctionResult
                                                                                          : CountMismatch::ExprListResult,
                        },
                        errorLocation);
                }
            }
        }
    }

    void visit(AstStatFor* forStatement)
    {
        NotNull<Scope> scope = stack.back();

        if (forStatement->var->annotation)
        {
            visit(forStatement->var->annotation);
            reportErrors(tryUnify(scope, forStatement->var->location, builtinTypes->numberType, lookupAnnotation(forStatement->var->annotation)));
        }

        auto checkNumber = [this, scope](AstExpr* expr) {
            if (!expr)
                return;

            visit(expr, ValueContext::RValue);
            reportErrors(tryUnify(scope, expr->location, lookupType(expr), builtinTypes->numberType));
        };

        checkNumber(forStatement->from);
        checkNumber(forStatement->to);
        checkNumber(forStatement->step);

        visit(forStatement->body);
    }

    void visit(AstStatForIn* forInStatement)
    {
        for (AstLocal* local : forInStatement->vars)
        {
            if (local->annotation)
                visit(local->annotation);
        }

        for (AstExpr* expr : forInStatement->values)
            visit(expr, ValueContext::RValue);

        visit(forInStatement->body);

        // Rule out crazy stuff.  Maybe possible if the file is not syntactically valid.
        if (!forInStatement->vars.size || !forInStatement->values.size)
            return;

        NotNull<Scope> scope = stack.back();
        TypeArena& arena = testArena;

        std::vector<TypeId> variableTypes;
        for (AstLocal* var : forInStatement->vars)
        {
            std::optional<TypeId> ty = scope->lookup(var);
            LUAU_ASSERT(ty);
            variableTypes.emplace_back(*ty);
        }

        AstExpr* firstValue = forInStatement->values.data[0];

        // we need to build up a typepack for the iterators/values portion of the for-in statement.
        std::vector<TypeId> valueTypes;
        std::optional<TypePackId> iteratorTail;

        // since the first value may be the only iterator (e.g. if it is a call), we want to
        // look to see if it has a resulting typepack as our iterators.
        TypePackId* retPack = module->astTypePacks.find(firstValue);
        if (retPack)
        {
            auto [head, tail] = flatten(*retPack);
            valueTypes = head;
            iteratorTail = tail;
        }
        else
        {
            valueTypes.emplace_back(lookupType(firstValue));
        }

        // if the initial and expected types from the iterator unified during constraint solving,
        // we'll have a resolved type to use here, but we'll only use it if either the iterator is
        // directly present in the for-in statement or if we have an iterator state constraining us
        TypeId* resolvedTy = module->astOverloadResolvedTypes.find(firstValue);
        if (resolvedTy && (!retPack || valueTypes.size() > 1))
            valueTypes[0] = *resolvedTy;

        for (size_t i = 1; i < forInStatement->values.size - 1; ++i)
        {
            valueTypes.emplace_back(lookupType(forInStatement->values.data[i]));
        }

        // if we had more than one value, the tail from the first value is no longer appropriate to use.
        if (forInStatement->values.size > 1)
        {
            auto [head, tail] = flatten(lookupPack(forInStatement->values.data[forInStatement->values.size - 1]));
            valueTypes.insert(valueTypes.end(), head.begin(), head.end());
            iteratorTail = tail;
        }

        // and now we can put everything together to get the actual typepack of the iterators.
        TypePackId iteratorPack = arena.addTypePack(valueTypes, iteratorTail);

        // ... and then expand it out to 3 values (if possible)
        TypePack iteratorTypes = extendTypePack(arena, builtinTypes, iteratorPack, 3);
        if (iteratorTypes.head.empty())
        {
            reportError(GenericError{"for..in loops require at least one value to iterate over.  Got zero"}, getLocation(forInStatement->values));
            return;
        }
        TypeId iteratorTy = follow(iteratorTypes.head[0]);

        auto checkFunction = [this, &arena, &scope, &forInStatement, &variableTypes](
                                 const FunctionType* iterFtv, std::vector<TypeId> iterTys, bool isMm) {
            if (iterTys.size() < 1 || iterTys.size() > 3)
            {
                if (isMm)
                    reportError(GenericError{"__iter metamethod must return (next[, table[, state]])"}, getLocation(forInStatement->values));
                else
                    reportError(GenericError{"for..in loops must be passed (next[, table[, state]])"}, getLocation(forInStatement->values));

                return;
            }

            // It is okay if there aren't enough iterators, but the iteratee must provide enough.
            TypePack expectedVariableTypes = extendTypePack(arena, builtinTypes, iterFtv->retTypes, variableTypes.size());
            if (expectedVariableTypes.head.size() < variableTypes.size())
            {
                if (isMm)
                    reportError(
                        GenericError{"__iter metamethod's next() function does not return enough values"}, getLocation(forInStatement->values));
                else
                    reportError(GenericError{"next() does not return enough values"}, forInStatement->values.data[0]->location);
            }

            for (size_t i = 0; i < std::min(expectedVariableTypes.head.size(), variableTypes.size()); ++i)
                reportErrors(tryUnify(scope, forInStatement->vars.data[i]->location, variableTypes[i], expectedVariableTypes.head[i]));

            // nextFn is going to be invoked with (arrayTy, startIndexTy)

            // It will be passed two arguments on every iteration save the
            // first.

            // It may be invoked with 0 or 1 argument on the first iteration.
            // This depends on the types in iterateePack and therefore
            // iteratorTypes.

            // If the iteratee is an error type, then we can't really say anything else about iteration over it.
            // After all, it _could've_ been a table.
            if (get<ErrorType>(follow(flattenPack(iterFtv->argTypes))))
                return;

            // If iteratorTypes is too short to be a valid call to nextFn, we have to report a count mismatch error.
            // If 2 is too short to be a valid call to nextFn, we have to report a count mismatch error.
            // If 2 is too long to be a valid call to nextFn, we have to report a count mismatch error.
            auto [minCount, maxCount] = getParameterExtents(TxnLog::empty(), iterFtv->argTypes, /*includeHiddenVariadics*/ true);

            TypePack flattenedArgTypes = extendTypePack(arena, builtinTypes, iterFtv->argTypes, 2);
            size_t firstIterationArgCount = iterTys.empty() ? 0 : iterTys.size() - 1;
            size_t actualArgCount = expectedVariableTypes.head.size();
            if (firstIterationArgCount < minCount)
            {
                if (isMm)
                    reportError(GenericError{"__iter metamethod must return (next[, table[, state]])"}, getLocation(forInStatement->values));
                else
                    reportError(CountMismatch{2, std::nullopt, firstIterationArgCount, CountMismatch::Arg}, forInStatement->values.data[0]->location);
            }

            else if (actualArgCount < minCount)
            {
                if (isMm)
                    reportError(GenericError{"__iter metamethod must return (next[, table[, state]])"}, getLocation(forInStatement->values));
                else
                    reportError(CountMismatch{2, std::nullopt, firstIterationArgCount, CountMismatch::Arg}, forInStatement->values.data[0]->location);
            }


            if (iterTys.size() >= 2 && flattenedArgTypes.head.size() > 0)
            {
                size_t valueIndex = forInStatement->values.size > 1 ? 1 : 0;
                reportErrors(tryUnify(scope, forInStatement->values.data[valueIndex]->location, iterTys[1], flattenedArgTypes.head[0]));
            }

            if (iterTys.size() == 3 && flattenedArgTypes.head.size() > 1)
            {
                size_t valueIndex = forInStatement->values.size > 2 ? 2 : 0;
                reportErrors(tryUnify(scope, forInStatement->values.data[valueIndex]->location, iterTys[2], flattenedArgTypes.head[1]));
            }
        };

        /*
         * If the first iterator argument is a function
         *  * There must be 1 to 3 iterator arguments.  Name them (nextTy,
         *    arrayTy, startIndexTy)
         *  * The return type of nextTy() must correspond to the variables'
         *    types and counts.  HOWEVER the first iterator will never be nil.
         *  * The first return value of nextTy must be compatible with
         *    startIndexTy.
         *  * The first argument to nextTy() must be compatible with arrayTy if
         *    present.  nil if not.
         *  * The second argument to nextTy() must be compatible with
         *    startIndexTy if it is present.  Else, it must be compatible with
         *    nil.
         *  * nextTy() must be callable with only 2 arguments.
         */
        if (const FunctionType* nextFn = get<FunctionType>(iteratorTy))
        {
            checkFunction(nextFn, iteratorTypes.head, false);
        }
        else if (const TableType* ttv = get<TableType>(iteratorTy))
        {
            if ((forInStatement->vars.size == 1 || forInStatement->vars.size == 2) && ttv->indexer)
            {
                reportErrors(tryUnify(scope, forInStatement->vars.data[0]->location, variableTypes[0], ttv->indexer->indexType));
                if (variableTypes.size() == 2)
                    reportErrors(tryUnify(scope, forInStatement->vars.data[1]->location, variableTypes[1], ttv->indexer->indexResultType));
            }
            else
                reportError(GenericError{"Cannot iterate over a table without indexer"}, forInStatement->values.data[0]->location);
        }
        else if (get<AnyType>(iteratorTy) || get<ErrorType>(iteratorTy) || get<NeverType>(iteratorTy))
        {
            // nothing
        }
        else if (isOptional(iteratorTy))
        {
            reportError(OptionalValueAccess{iteratorTy}, forInStatement->values.data[0]->location);
        }
        else if (std::optional<TypeId> iterMmTy =
                     findMetatableEntry(builtinTypes, module->errors, iteratorTy, "__iter", forInStatement->values.data[0]->location))
        {
            Instantiation instantiation{TxnLog::empty(), &arena, TypeLevel{}, scope};

            if (std::optional<TypeId> instantiatedIterMmTy = instantiation.substitute(*iterMmTy))
            {
                if (const FunctionType* iterMmFtv = get<FunctionType>(*instantiatedIterMmTy))
                {
                    TypePackId argPack = arena.addTypePack({iteratorTy});
                    reportErrors(tryUnify(scope, forInStatement->values.data[0]->location, argPack, iterMmFtv->argTypes));

                    TypePack mmIteratorTypes = extendTypePack(arena, builtinTypes, iterMmFtv->retTypes, 3);

                    if (mmIteratorTypes.head.size() == 0)
                    {
                        reportError(GenericError{"__iter must return at least one value"}, forInStatement->values.data[0]->location);
                        return;
                    }

                    TypeId nextFn = follow(mmIteratorTypes.head[0]);

                    if (std::optional<TypeId> instantiatedNextFn = instantiation.substitute(nextFn))
                    {
                        std::vector<TypeId> instantiatedIteratorTypes = mmIteratorTypes.head;
                        instantiatedIteratorTypes[0] = *instantiatedNextFn;

                        if (const FunctionType* nextFtv = get<FunctionType>(*instantiatedNextFn))
                        {
                            checkFunction(nextFtv, instantiatedIteratorTypes, true);
                        }
                        else
                        {
                            reportError(CannotCallNonFunction{*instantiatedNextFn}, forInStatement->values.data[0]->location);
                        }
                    }
                    else
                    {
                        reportError(UnificationTooComplex{}, forInStatement->values.data[0]->location);
                    }
                }
                else
                {
                    // TODO: This will not tell the user that this is because the
                    // metamethod isn't callable. This is not ideal, and we should
                    // improve this error message.

                    // TODO: This will also not handle intersections of functions or
                    // callable tables (which are supported by the runtime).
                    reportError(CannotCallNonFunction{*iterMmTy}, forInStatement->values.data[0]->location);
                }
            }
            else
            {
                reportError(UnificationTooComplex{}, forInStatement->values.data[0]->location);
            }
        }
        else
        {
            reportError(CannotCallNonFunction{iteratorTy}, forInStatement->values.data[0]->location);
        }
    }

    void visit(AstStatAssign* assign)
    {
        size_t count = std::min(assign->vars.size, assign->values.size);

        for (size_t i = 0; i < count; ++i)
        {
            AstExpr* lhs = assign->vars.data[i];
            visit(lhs, ValueContext::LValue);
            TypeId lhsType = lookupType(lhs);

            AstExpr* rhs = assign->values.data[i];
            visit(rhs, ValueContext::RValue);
            TypeId rhsType = lookupType(rhs);

            if (get<NeverType>(lhsType))
                continue;

            if (!isSubtype(rhsType, lhsType, stack.back()))
            {
                reportError(TypeMismatch{lhsType, rhsType}, rhs->location);
            }
        }
    }

    void visit(AstStatCompoundAssign* stat)
    {
        AstExprBinary fake{stat->location, stat->op, stat->var, stat->value};
        TypeId resultTy = visit(&fake, stat);
        TypeId varTy = lookupType(stat->var);

        reportErrors(tryUnify(stack.back(), stat->location, resultTy, varTy));
    }

    void visit(AstStatFunction* stat)
    {
        visit(stat->name, ValueContext::LValue);
        visit(stat->func);
    }

    void visit(AstStatLocalFunction* stat)
    {
        visit(stat->func);
    }

    void visit(const AstTypeList* typeList)
    {
        for (AstType* ty : typeList->types)
            visit(ty);

        if (typeList->tailType)
            visit(typeList->tailType);
    }

    void visit(AstStatTypeAlias* stat)
    {
        visitGenerics(stat->generics, stat->genericPacks);
        visit(stat->type);
    }

    void visit(AstTypeList types)
    {
        for (AstType* type : types.types)
            visit(type);
        if (types.tailType)
            visit(types.tailType);
    }

    void visit(AstStatDeclareFunction* stat)
    {
        visitGenerics(stat->generics, stat->genericPacks);
        visit(stat->params);
        visit(stat->retTypes);
    }

    void visit(AstStatDeclareGlobal* stat)
    {
        visit(stat->type);
    }

    void visit(AstStatDeclareClass* stat)
    {
        for (const AstDeclaredClassProp& prop : stat->props)
            visit(prop.ty);
    }

    void visit(AstStatError* stat)
    {
        for (AstExpr* expr : stat->expressions)
            visit(expr, ValueContext::RValue);

        for (AstStat* s : stat->statements)
            visit(s);
    }

    void visit(AstExpr* expr, ValueContext context)
    {
        auto StackPusher = pushStack(expr);

        if (auto e = expr->as<AstExprGroup>())
            return visit(e, context);
        else if (auto e = expr->as<AstExprConstantNil>())
            return visit(e);
        else if (auto e = expr->as<AstExprConstantBool>())
            return visit(e);
        else if (auto e = expr->as<AstExprConstantNumber>())
            return visit(e);
        else if (auto e = expr->as<AstExprConstantString>())
            return visit(e);
        else if (auto e = expr->as<AstExprLocal>())
            return visit(e);
        else if (auto e = expr->as<AstExprGlobal>())
            return visit(e);
        else if (auto e = expr->as<AstExprVarargs>())
            return visit(e);
        else if (auto e = expr->as<AstExprCall>())
            return visit(e);
        else if (auto e = expr->as<AstExprIndexName>())
            return visit(e, context);
        else if (auto e = expr->as<AstExprIndexExpr>())
            return visit(e, context);
        else if (auto e = expr->as<AstExprFunction>())
            return visit(e);
        else if (auto e = expr->as<AstExprTable>())
            return visit(e);
        else if (auto e = expr->as<AstExprUnary>())
            return visit(e);
        else if (auto e = expr->as<AstExprBinary>())
        {
            visit(e);
            return;
        }
        else if (auto e = expr->as<AstExprTypeAssertion>())
            return visit(e);
        else if (auto e = expr->as<AstExprIfElse>())
            return visit(e);
        else if (auto e = expr->as<AstExprInterpString>())
            return visit(e);
        else if (auto e = expr->as<AstExprError>())
            return visit(e);
        else
            LUAU_ASSERT(!"TypeChecker2 encountered an unknown expression type");
    }

    void visit(AstExprGroup* expr, ValueContext context)
    {
        visit(expr->expr, context);
    }

    void visit(AstExprConstantNil* expr)
    {
        NotNull<Scope> scope = stack.back();
        TypeId actualType = lookupType(expr);
        TypeId expectedType = builtinTypes->nilType;
        LUAU_ASSERT(isSubtype(actualType, expectedType, scope));
    }

    void visit(AstExprConstantBool* expr)
    {
        NotNull<Scope> scope = stack.back();
        TypeId actualType = lookupType(expr);
        TypeId expectedType = builtinTypes->booleanType;
        LUAU_ASSERT(isSubtype(actualType, expectedType, scope));
    }

    void visit(AstExprConstantNumber* expr)
    {
        NotNull<Scope> scope = stack.back();
        TypeId actualType = lookupType(expr);
        TypeId expectedType = builtinTypes->numberType;
        LUAU_ASSERT(isSubtype(actualType, expectedType, scope));
    }

    void visit(AstExprConstantString* expr)
    {
        NotNull<Scope> scope = stack.back();
        TypeId actualType = lookupType(expr);
        TypeId expectedType = builtinTypes->stringType;
        LUAU_ASSERT(isSubtype(actualType, expectedType, scope));
    }

    void visit(AstExprLocal* expr)
    {
        // TODO!
    }

    void visit(AstExprGlobal* expr)
    {
        // TODO!
    }

    void visit(AstExprVarargs* expr)
    {
        // TODO!
    }

    // Note: this is intentionally separated from `visit(AstExprCall*)` for stack allocation purposes.
    void visitCall(AstExprCall* call)
    {
        TypePackId expectedRetType = lookupExpectedPack(call, testArena);
        TypePack args;
        std::vector<Location> argLocs;
        argLocs.reserve(call->args.size + 1);

        auto maybeOriginalCallTy = module->astOriginalCallTypes.find(call);
        if (!maybeOriginalCallTy)
            return;

        TypeId originalCallTy = follow(*maybeOriginalCallTy);
        std::vector<TypeId> overloads = flattenIntersection(originalCallTy);

        if (get<AnyType>(originalCallTy) || get<ErrorType>(originalCallTy) || get<NeverType>(originalCallTy))
            return;
        else if (std::optional<TypeId> callMm = findMetatableEntry(builtinTypes, module->errors, originalCallTy, "__call", call->func->location))
        {
            if (get<FunctionType>(follow(*callMm)))
            {
                args.head.push_back(originalCallTy);
                argLocs.push_back(call->func->location);
            }
            else
            {
                // TODO: This doesn't flag the __call metamethod as the problem
                // very clearly.
                reportError(CannotCallNonFunction{*callMm}, call->func->location);
                return;
            }
        }
        else if (get<FunctionType>(originalCallTy))
        {
            // ok.
        }
        else if (get<IntersectionType>(originalCallTy))
        {
            auto norm = normalizer.normalize(originalCallTy);
            if (!norm)
                return reportError(CodeTooComplex{}, call->location);

            // NormalizedType::hasFunction returns true if its' tops component is `unknown`, but for soundness we want the reverse.
            if (get<UnknownType>(norm->tops) || !norm->hasFunctions())
                return reportError(CannotCallNonFunction{originalCallTy}, call->func->location);
        }
        else if (auto utv = get<UnionType>(originalCallTy))
        {
            // Sometimes it's okay to call a union of functions, but only if all of the functions are the same.
            // Another scenario we might run into it is if the union has a nil member. In this case, we want to throw an error
            if (isOptional(originalCallTy))
            {
                reportError(OptionalValueAccess{originalCallTy}, call->location);
                return;
            }
            std::optional<TypeId> fst;
            for (TypeId ty : utv)
            {
                if (!fst)
                    fst = follow(ty);
                else if (fst != follow(ty))
                {
                    reportError(CannotCallNonFunction{originalCallTy}, call->func->location);
                    return;
                }
            }

            if (!fst)
                ice->ice("UnionType had no elements, so fst is nullopt?");

            originalCallTy = follow(*fst);
            if (!get<FunctionType>(originalCallTy))
            {
                reportError(CannotCallNonFunction{originalCallTy}, call->func->location);
                return;
            }
        }
        else
        {
            reportError(CannotCallNonFunction{originalCallTy}, call->func->location);
            return;
        }

        if (call->self)
        {
            AstExprIndexName* indexExpr = call->func->as<AstExprIndexName>();
            if (!indexExpr)
                ice->ice("method call expression has no 'self'");

            args.head.push_back(lookupType(indexExpr->expr));
            argLocs.push_back(indexExpr->expr->location);
        }

        for (size_t i = 0; i < call->args.size; ++i)
        {
            AstExpr* arg = call->args.data[i];
            argLocs.push_back(arg->location);
            TypeId* argTy = module->astTypes.find(arg);
            if (argTy)
                args.head.push_back(*argTy);
            else if (i == call->args.size - 1)
            {
                TypePackId* argTail = module->astTypePacks.find(arg);
                if (argTail)
                    args.tail = *argTail;
                else
                    args.tail = builtinTypes->anyTypePack;
            }
            else
                args.head.push_back(builtinTypes->anyType);
        }

        TypePackId expectedArgTypes = testArena.addTypePack(args);

        if (auto maybeSelectedOverload = module->astOverloadResolvedTypes.find(call))
        {
            // This overload might not work still: the constraint solver will
            // pass the type checker an instantiated function type that matches
            // in arity, but not in subtyping, in order to allow the type
            // checker to report better error messages.

            TypeId selectedOverload = follow(*maybeSelectedOverload);
            const FunctionType* ftv;

            if (get<AnyType>(selectedOverload) || get<ErrorType>(selectedOverload) || get<NeverType>(selectedOverload))
            {
                return;
            }
            else if (const FunctionType* overloadFtv = get<FunctionType>(selectedOverload))
            {
                ftv = overloadFtv;
            }
            else
            {
                reportError(CannotCallNonFunction{selectedOverload}, call->func->location);
                return;
            }

            TxnLog fake{};

            LUAU_ASSERT(ftv);
            reportErrors(tryUnify(stack.back(), call->location, ftv->retTypes, expectedRetType, CountMismatch::Context::Return, /* genericsOkay */ true));
            reportErrors(
                reduceFamilies(ftv->retTypes, call->location, NotNull{&testArena}, builtinTypes, stack.back(), NotNull{&normalizer}, &fake, true)
                    .errors);

            auto it = begin(expectedArgTypes);
            size_t i = 0;
            std::vector<TypeId> slice;
            for (TypeId arg : ftv->argTypes)
            {
                if (it == end(expectedArgTypes))
                {
                    slice.push_back(arg);
                    continue;
                }

                TypeId expectedArg = *it;

                Location argLoc = argLocs.at(i >= argLocs.size() ? argLocs.size() - 1 : i);

                reportErrors(tryUnify(stack.back(), argLoc, expectedArg, arg, CountMismatch::Context::Arg, /* genericsOkay */ true));
                reportErrors(reduceFamilies(arg, argLoc, NotNull{&testArena}, builtinTypes, stack.back(), NotNull{&normalizer}, &fake, true).errors);

                ++it;
                ++i;
            }

            if (slice.size() > 0 && it == end(expectedArgTypes))
            {
                if (auto tail = it.tail())
                {
                    TypePackId remainingArgs = testArena.addTypePack(TypePack{std::move(slice), std::nullopt});
                    reportErrors(tryUnify(stack.back(), argLocs.back(), *tail, remainingArgs, CountMismatch::Context::Arg, /* genericsOkay */ true));
                    reportErrors(reduceFamilies(
                        remainingArgs, argLocs.back(), NotNull{&testArena}, builtinTypes, stack.back(), NotNull{&normalizer}, &fake, true)
                                     .errors);
                }
            }
        }
        else
        {
            // No overload worked, even when instantiated. We need to filter the
            // set of overloads to those that match the arity of the incoming
            // argument set, and then report only those as not matching.

            std::vector<TypeId> arityMatchingOverloads;
            ErrorVec empty;
            for (TypeId overload : overloads)
            {
                overload = follow(overload);
                if (const FunctionType* ftv = get<FunctionType>(overload))
                {
                    if (size(ftv->argTypes) == size(expectedArgTypes))
                    {
                        arityMatchingOverloads.push_back(overload);
                    }
                }
                else if (const std::optional<TypeId> callMm = findMetatableEntry(builtinTypes, empty, overload, "__call", call->location))
                {
                    if (const FunctionType* ftv = get<FunctionType>(follow(*callMm)))
                    {
                        if (size(ftv->argTypes) == size(expectedArgTypes))
                        {
                            arityMatchingOverloads.push_back(overload);
                        }
                    }
                    else
                    {
                        reportError(CannotCallNonFunction{}, call->location);
                    }
                }
            }

            if (arityMatchingOverloads.size() == 0)
            {
                reportError(
                    GenericError{"No overload for function accepts " + std::to_string(size(expectedArgTypes)) + " arguments."}, call->location);
            }
            else
            {
                // We have handled the case of a singular arity-matching
                // overload above, in the case where an overload was selected.
                // LUAU_ASSERT(arityMatchingOverloads.size() > 1);
                reportError(GenericError{"None of the overloads for function that accept " + std::to_string(size(expectedArgTypes)) +
                                         " arguments are compatible."},
                    call->location);
            }

            std::string s;
            std::vector<TypeId>& stringifyOverloads = arityMatchingOverloads.size() == 0 ? overloads : arityMatchingOverloads;
            for (size_t i = 0; i < stringifyOverloads.size(); ++i)
            {
                TypeId overload = follow(stringifyOverloads[i]);

                if (i > 0)
                    s += "; ";

                if (i > 0 && i == stringifyOverloads.size() - 1)
                    s += "and ";

                s += toString(overload);
            }

            reportError(ExtraInformation{"Available overloads: " + s}, call->func->location);
        }
    }

    void visit(AstExprCall* call)
    {
        visit(call->func, ValueContext::RValue);

        for (AstExpr* arg : call->args)
            visit(arg, ValueContext::RValue);

        visitCall(call);
    }

    std::optional<TypeId> tryStripUnionFromNil(TypeId ty)
    {
        if (const UnionType* utv = get<UnionType>(ty))
        {
            if (!std::any_of(begin(utv), end(utv), isNil))
                return ty;

            std::vector<TypeId> result;

            for (TypeId option : utv)
            {
                if (!isNil(option))
                    result.push_back(option);
            }

            if (result.empty())
                return std::nullopt;

            return result.size() == 1 ? result[0] : module->internalTypes.addType(UnionType{std::move(result)});
        }

        return std::nullopt;
    }

    TypeId stripFromNilAndReport(TypeId ty, const Location& location)
    {
        ty = follow(ty);

        if (auto utv = get<UnionType>(ty))
        {
            if (!std::any_of(begin(utv), end(utv), isNil))
                return ty;
        }

        if (std::optional<TypeId> strippedUnion = tryStripUnionFromNil(ty))
        {
            reportError(OptionalValueAccess{ty}, location);
            return follow(*strippedUnion);
        }

        return ty;
    }

    void visitExprName(AstExpr* expr, Location location, const std::string& propName, ValueContext context, TypeId astIndexExprTy)
    {
        visit(expr, ValueContext::RValue);
        TypeId leftType = stripFromNilAndReport(lookupType(expr), location);
        checkIndexTypeFromType(leftType, propName, location, context, astIndexExprTy);
    }

    void visit(AstExprIndexName* indexName, ValueContext context)
    {
        // If we're indexing like _.foo - foo could either be a prop or a string.
        visitExprName(indexName->expr, indexName->location, indexName->index.value, context, builtinTypes->stringType);
    }

    void visit(AstExprIndexExpr* indexExpr, ValueContext context)
    {
        if (auto str = indexExpr->index->as<AstExprConstantString>())
        {
            TypeId astIndexExprType = lookupType(indexExpr->index);
            const std::string stringValue(str->value.data, str->value.size);
            visitExprName(indexExpr->expr, indexExpr->location, stringValue, context, astIndexExprType);
            return;
        }

        // TODO!
        visit(indexExpr->expr, ValueContext::LValue);
        visit(indexExpr->index, ValueContext::RValue);

        NotNull<Scope> scope = stack.back();

        TypeId exprType = lookupType(indexExpr->expr);
        TypeId indexType = lookupType(indexExpr->index);

        if (auto tt = get<TableType>(exprType))
        {
            if (tt->indexer)
                reportErrors(tryUnify(scope, indexExpr->index->location, indexType, tt->indexer->indexType));
            else
                reportError(CannotExtendTable{exprType, CannotExtendTable::Indexer, "indexer??"}, indexExpr->location);
        }
        else if (auto cls = get<ClassType>(exprType); cls && cls->indexer)
            reportErrors(tryUnify(scope, indexExpr->index->location, indexType, cls->indexer->indexType));
        else if (get<UnionType>(exprType) && isOptional(exprType))
            reportError(OptionalValueAccess{exprType}, indexExpr->location);
    }

    void visit(AstExprFunction* fn)
    {
        auto StackPusher = pushStack(fn);

        visitGenerics(fn->generics, fn->genericPacks);

        TypeId inferredFnTy = lookupType(fn);
        functionDeclStack.push_back(inferredFnTy);

        const NormalizedType* normalizedFnTy = normalizer.normalize(inferredFnTy);
        if (!normalizedFnTy)
        {
            reportError(CodeTooComplex{}, fn->location);
        }
        else if (get<ErrorType>(normalizedFnTy->errors))
        {
            // Nothing
        }
        else if (!normalizedFnTy->hasFunctions())
        {
            ice->ice("Internal error: Lambda has non-function type " + toString(inferredFnTy), fn->location);
        }
        else
        {
            if (1 != normalizedFnTy->functions.parts.size())
                ice->ice("Unexpected: Lambda has unexpected type " + toString(inferredFnTy), fn->location);

            const FunctionType* inferredFtv = get<FunctionType>(normalizedFnTy->functions.parts.front());
            LUAU_ASSERT(inferredFtv);

            // There is no way to write an annotation for the self argument, so we
            // cannot do anything to check it.
            auto argIt = begin(inferredFtv->argTypes);
            if (fn->self)
                ++argIt;

            for (const auto& arg : fn->args)
            {
                if (argIt == end(inferredFtv->argTypes))
                    break;

                if (arg->annotation)
                {
                    TypeId inferredArgTy = *argIt;
                    TypeId annotatedArgTy = lookupAnnotation(arg->annotation);

                    if (!isSubtype(inferredArgTy, annotatedArgTy, stack.back()))
                    {
                        reportError(TypeMismatch{inferredArgTy, annotatedArgTy}, arg->location);
                    }
                }

                ++argIt;
            }
        }

        visit(fn->body);

        functionDeclStack.pop_back();
    }

    void visit(AstExprTable* expr)
    {
        // TODO!
        for (const AstExprTable::Item& item : expr->items)
        {
            if (item.key)
                visit(item.key, ValueContext::LValue);
            visit(item.value, ValueContext::RValue);
        }
    }

    void visit(AstExprUnary* expr)
    {
        visit(expr->expr, ValueContext::RValue);

        NotNull<Scope> scope = stack.back();
        TypeId operandType = lookupType(expr->expr);
        TypeId resultType = lookupType(expr);

        if (get<AnyType>(operandType) || get<ErrorType>(operandType) || get<NeverType>(operandType))
            return;

        if (auto it = kUnaryOpMetamethods.find(expr->op); it != kUnaryOpMetamethods.end())
        {
            std::optional<TypeId> mm = findMetatableEntry(builtinTypes, module->errors, operandType, it->second, expr->location);
            if (mm)
            {
                if (const FunctionType* ftv = get<FunctionType>(follow(*mm)))
                {
                    if (std::optional<TypeId> ret = first(ftv->retTypes))
                    {
                        if (expr->op == AstExprUnary::Op::Len)
                        {
                            reportErrors(tryUnify(scope, expr->location, follow(*ret), builtinTypes->numberType));
                        }
                    }
                    else
                    {
                        reportError(GenericError{format("Metamethod '%s' must return a value", it->second)}, expr->location);
                    }

                    std::optional<TypeId> firstArg = first(ftv->argTypes);
                    if (!firstArg)
                    {
                        reportError(GenericError{"__unm metamethod must accept one argument"}, expr->location);
                        return;
                    }

                    TypePackId expectedArgs = testArena.addTypePack({operandType});
                    TypePackId expectedRet = testArena.addTypePack({resultType});

                    TypeId expectedFunction = testArena.addType(FunctionType{expectedArgs, expectedRet});

                    ErrorVec errors = tryUnify(scope, expr->location, *mm, expectedFunction);
                    if (!errors.empty())
                    {
                        reportError(TypeMismatch{*firstArg, operandType}, expr->location);
                        return;
                    }
                }

                return;
            }
        }

        if (expr->op == AstExprUnary::Op::Len)
        {
            DenseHashSet<TypeId> seen{nullptr};
            int recursionCount = 0;


            if (!hasLength(operandType, seen, &recursionCount))
            {
                if (isOptional(operandType))
                    reportError(OptionalValueAccess{operandType}, expr->location);
                else
                    reportError(NotATable{operandType}, expr->location);
            }
        }
        else if (expr->op == AstExprUnary::Op::Minus)
        {
            reportErrors(tryUnify(scope, expr->location, operandType, builtinTypes->numberType));
        }
        else if (expr->op == AstExprUnary::Op::Not)
        {
        }
        else
        {
            LUAU_ASSERT(!"Unhandled unary operator");
        }
    }

    TypeId visit(AstExprBinary* expr, AstNode* overrideKey = nullptr)
    {
        visit(expr->left, ValueContext::LValue);
        visit(expr->right, ValueContext::LValue);

        NotNull<Scope> scope = stack.back();

        bool isEquality = expr->op == AstExprBinary::Op::CompareEq || expr->op == AstExprBinary::Op::CompareNe;
        bool isComparison = expr->op >= AstExprBinary::Op::CompareEq && expr->op <= AstExprBinary::Op::CompareGe;
        bool isLogical = expr->op == AstExprBinary::Op::And || expr->op == AstExprBinary::Op::Or;

        TypeId leftType = lookupType(expr->left);
        TypeId rightType = lookupType(expr->right);
        TypeId expectedResult = lookupType(expr);

        if (get<TypeFamilyInstanceType>(expectedResult))
        {
            checkForInternalFamily(expectedResult, expr->location);
            return expectedResult;
        }

        if (expr->op == AstExprBinary::Op::Or)
        {
            leftType = stripNil(builtinTypes, testArena, leftType);
        }

        bool isStringOperation = isString(leftType) && isString(rightType);

        if (get<AnyType>(leftType) || get<ErrorType>(leftType) || get<NeverType>(leftType))
            return leftType;
        else if (get<AnyType>(rightType) || get<ErrorType>(rightType) || get<NeverType>(rightType))
            return rightType;

        if ((get<BlockedType>(leftType) || get<FreeType>(leftType) || get<GenericType>(leftType)) && !isEquality && !isLogical)
        {
            auto name = getIdentifierOfBaseVar(expr->left);
            reportError(CannotInferBinaryOperation{expr->op, name,
                            isComparison ? CannotInferBinaryOperation::OpKind::Comparison : CannotInferBinaryOperation::OpKind::Operation},
                expr->location);
            return leftType;
        }

        bool typesHaveIntersection = normalizer.isIntersectionInhabited(leftType, rightType);
        if (auto it = kBinaryOpMetamethods.find(expr->op); it != kBinaryOpMetamethods.end())
        {
            std::optional<TypeId> leftMt = getMetatable(leftType, builtinTypes);
            std::optional<TypeId> rightMt = getMetatable(rightType, builtinTypes);
            bool matches = leftMt == rightMt;


            if (isEquality && !matches)
            {
                auto testUnion = [&matches, builtinTypes = this->builtinTypes](const UnionType* utv, std::optional<TypeId> otherMt) {
                    for (TypeId option : utv)
                    {
                        if (getMetatable(follow(option), builtinTypes) == otherMt)
                        {
                            matches = true;
                            break;
                        }
                    }
                };

                if (const UnionType* utv = get<UnionType>(leftType); utv && rightMt)
                {
                    testUnion(utv, rightMt);
                }

                if (const UnionType* utv = get<UnionType>(rightType); utv && leftMt && !matches)
                {
                    testUnion(utv, leftMt);
                }

                // If either left or right has no metatable (or both), we need to consider if
                // there are values in common that could possibly inhabit the type (and thus equality could be considered)
                if (!leftMt.has_value() || !rightMt.has_value())
                {
                    matches = matches || typesHaveIntersection;
                }
            }

            if (!matches && isComparison)
            {
                reportError(GenericError{format("Types %s and %s cannot be compared with %s because they do not have the same metatable",
                                toString(leftType).c_str(), toString(rightType).c_str(), toString(expr->op).c_str())},
                    expr->location);

                return builtinTypes->errorRecoveryType();
            }

            std::optional<TypeId> mm;
            if (std::optional<TypeId> leftMm = findMetatableEntry(builtinTypes, module->errors, leftType, it->second, expr->left->location))
                mm = leftMm;
            else if (std::optional<TypeId> rightMm = findMetatableEntry(builtinTypes, module->errors, rightType, it->second, expr->right->location))
            {
                mm = rightMm;
                std::swap(leftType, rightType);
            }

            if (mm)
            {
                AstNode* key = expr;
                if (overrideKey != nullptr)
                    key = overrideKey;

                TypeId instantiatedMm = module->astOverloadResolvedTypes[key];
                if (!instantiatedMm)
                {
                    // reportError(CodeTooComplex{}, expr->location);
                    // was handled by a type family
                    return expectedResult;
                }

                else if (const FunctionType* ftv = get<FunctionType>(follow(instantiatedMm)))
                {
                    TypePackId expectedArgs;
                    // For >= and > we invoke __lt and __le respectively with
                    // swapped argument ordering.
                    if (expr->op == AstExprBinary::Op::CompareGe || expr->op == AstExprBinary::Op::CompareGt)
                    {
                        expectedArgs = testArena.addTypePack({rightType, leftType});
                    }
                    else
                    {
                        expectedArgs = testArena.addTypePack({leftType, rightType});
                    }

                    TypePackId expectedRets;
                    if (expr->op == AstExprBinary::CompareEq || expr->op == AstExprBinary::CompareNe || expr->op == AstExprBinary::CompareGe ||
                        expr->op == AstExprBinary::CompareGt || expr->op == AstExprBinary::Op::CompareLe || expr->op == AstExprBinary::Op::CompareLt)
                    {
                        expectedRets = testArena.addTypePack({builtinTypes->booleanType});
                    }
                    else
                    {
                        expectedRets = testArena.addTypePack({testArena.freshType(scope, TypeLevel{})});
                    }

                    TypeId expectedTy = testArena.addType(FunctionType(expectedArgs, expectedRets));

                    reportErrors(tryUnify(scope, expr->location, follow(*mm), expectedTy));

                    std::optional<TypeId> ret = first(ftv->retTypes);
                    if (ret)
                    {
                        if (isComparison)
                        {
                            if (!isBoolean(follow(*ret)))
                            {
                                reportError(GenericError{format("Metamethod '%s' must return a boolean", it->second)}, expr->location);
                            }

                            return builtinTypes->booleanType;
                        }
                        else
                        {
                            return follow(*ret);
                        }
                    }
                    else
                    {
                        if (isComparison)
                        {
                            reportError(GenericError{format("Metamethod '%s' must return a boolean", it->second)}, expr->location);
                        }
                        else
                        {
                            reportError(GenericError{format("Metamethod '%s' must return a value", it->second)}, expr->location);
                        }

                        return builtinTypes->errorRecoveryType();
                    }
                }
                else
                {
                    reportError(CannotCallNonFunction{*mm}, expr->location);
                }

                return builtinTypes->errorRecoveryType();
            }
            // If this is a string comparison, or a concatenation of strings, we
            // want to fall through to primitive behavior.
            else if (!isEquality && !(isStringOperation && (expr->op == AstExprBinary::Op::Concat || isComparison)))
            {
                if ((leftMt && !isString(leftType)) || (rightMt && !isString(rightType)))
                {
                    if (isComparison)
                    {
                        reportError(GenericError{format(
                                        "Types '%s' and '%s' cannot be compared with %s because neither type's metatable has a '%s' metamethod",
                                        toString(leftType).c_str(), toString(rightType).c_str(), toString(expr->op).c_str(), it->second)},
                            expr->location);
                    }
                    else
                    {
                        reportError(GenericError{format(
                                        "Operator %s is not applicable for '%s' and '%s' because neither type's metatable has a '%s' metamethod",
                                        toString(expr->op).c_str(), toString(leftType).c_str(), toString(rightType).c_str(), it->second)},
                            expr->location);
                    }

                    return builtinTypes->errorRecoveryType();
                }
                else if (!leftMt && !rightMt && (get<TableType>(leftType) || get<TableType>(rightType)))
                {
                    if (isComparison)
                    {
                        reportError(GenericError{format("Types '%s' and '%s' cannot be compared with %s because neither type has a metatable",
                                        toString(leftType).c_str(), toString(rightType).c_str(), toString(expr->op).c_str())},
                            expr->location);
                    }
                    else
                    {
                        reportError(GenericError{format("Operator %s is not applicable for '%s' and '%s' because neither type has a metatable",
                                        toString(expr->op).c_str(), toString(leftType).c_str(), toString(rightType).c_str())},
                            expr->location);
                    }

                    return builtinTypes->errorRecoveryType();
                }
            }
        }

        switch (expr->op)
        {
        case AstExprBinary::Op::Add:
        case AstExprBinary::Op::Sub:
        case AstExprBinary::Op::Mul:
        case AstExprBinary::Op::Div:
        case AstExprBinary::Op::Pow:
        case AstExprBinary::Op::Mod:
            reportErrors(tryUnify(scope, expr->left->location, leftType, builtinTypes->numberType));
            reportErrors(tryUnify(scope, expr->right->location, rightType, builtinTypes->numberType));

            return builtinTypes->numberType;
        case AstExprBinary::Op::Concat:
            reportErrors(tryUnify(scope, expr->left->location, leftType, builtinTypes->stringType));
            reportErrors(tryUnify(scope, expr->right->location, rightType, builtinTypes->stringType));

            return builtinTypes->stringType;
        case AstExprBinary::Op::CompareGe:
        case AstExprBinary::Op::CompareGt:
        case AstExprBinary::Op::CompareLe:
        case AstExprBinary::Op::CompareLt:
        {
            const NormalizedType* leftTyNorm = normalizer.normalize(leftType);
            if (leftTyNorm && leftTyNorm->isExactlyNumber())
            {
                reportErrors(tryUnify(scope, expr->right->location, rightType, builtinTypes->numberType));
                return builtinTypes->numberType;
            }
            else if (leftTyNorm && leftTyNorm->isSubtypeOfString())
            {
                reportErrors(tryUnify(scope, expr->right->location, rightType, builtinTypes->stringType));
                return builtinTypes->stringType;
            }
            else
            {
                reportError(GenericError{format("Types '%s' and '%s' cannot be compared with relational operator %s", toString(leftType).c_str(),
                                toString(rightType).c_str(), toString(expr->op).c_str())},
                    expr->location);
                return builtinTypes->errorRecoveryType();
            }
        }

        case AstExprBinary::Op::And:
        case AstExprBinary::Op::Or:
        case AstExprBinary::Op::CompareEq:
        case AstExprBinary::Op::CompareNe:
            // Ugly case: we don't care about this possibility, because a
            // compound assignment will never exist with one of these operators.
            return builtinTypes->anyType;
        default:
            // Unhandled AstExprBinary::Op possibility.
            LUAU_ASSERT(false);
            return builtinTypes->errorRecoveryType();
        }
    }

    void visit(AstExprTypeAssertion* expr)
    {
        visit(expr->expr, ValueContext::RValue);
        visit(expr->annotation);

        TypeId annotationType = lookupAnnotation(expr->annotation);
        TypeId computedType = lookupType(expr->expr);

        // Note: As an optimization, we try 'number <: number | string' first, as that is the more likely case.
        if (isSubtype(annotationType, computedType, stack.back(), true))
            return;

        if (isSubtype(computedType, annotationType, stack.back(), true))
            return;

        reportError(TypesAreUnrelated{computedType, annotationType}, expr->location);
    }

    void visit(AstExprIfElse* expr)
    {
        // TODO!
        visit(expr->condition, ValueContext::RValue);
        visit(expr->trueExpr, ValueContext::RValue);
        visit(expr->falseExpr, ValueContext::RValue);
    }

    void visit(AstExprInterpString* interpString)
    {
        for (AstExpr* expr : interpString->expressions)
            visit(expr, ValueContext::RValue);
    }

    void visit(AstExprError* expr)
    {
        // TODO!
        for (AstExpr* e : expr->expressions)
            visit(e, ValueContext::RValue);
    }

    /** Extract a TypeId for the first type of the provided pack.
     *
     * Note that this may require modifying some types.  I hope this doesn't cause problems!
     */
    TypeId flattenPack(TypePackId pack)
    {
        pack = follow(pack);

        if (auto fst = first(pack, /*ignoreHiddenVariadics*/ false))
            return *fst;
        else if (auto ftp = get<FreeTypePack>(pack))
        {
            TypeId result = testArena.addType(FreeType{ftp->scope});
            TypePackId freeTail = testArena.addTypePack(FreeTypePack{ftp->scope});

            TypePack& resultPack = asMutable(pack)->ty.emplace<TypePack>();
            resultPack.head.assign(1, result);
            resultPack.tail = freeTail;

            return result;
        }
        else if (get<Unifiable::Error>(pack))
            return builtinTypes->errorRecoveryType();
        else if (finite(pack) && size(pack) == 0)
            return builtinTypes->nilType; // `(f())` where `f()` returns no values is coerced into `nil`
        else
            ice->ice("flattenPack got a weird pack!");
    }

    void visitGenerics(AstArray<AstGenericType> generics, AstArray<AstGenericTypePack> genericPacks)
    {
        DenseHashSet<AstName> seen{AstName{}};

        for (const auto& g : generics)
        {
            if (seen.contains(g.name))
                reportError(DuplicateGenericParameter{g.name.value}, g.location);
            else
                seen.insert(g.name);

            if (g.defaultValue)
                visit(g.defaultValue);
        }

        for (const auto& g : genericPacks)
        {
            if (seen.contains(g.name))
                reportError(DuplicateGenericParameter{g.name.value}, g.location);
            else
                seen.insert(g.name);

            if (g.defaultValue)
                visit(g.defaultValue);
        }
    }

    void visit(AstType* ty)
    {
        TypeId* resolvedTy = module->astResolvedTypes.find(ty);
        if (resolvedTy)
            checkForFamilyInhabitance(follow(*resolvedTy), ty->location);

        if (auto t = ty->as<AstTypeReference>())
            return visit(t);
        else if (auto t = ty->as<AstTypeTable>())
            return visit(t);
        else if (auto t = ty->as<AstTypeFunction>())
            return visit(t);
        else if (auto t = ty->as<AstTypeTypeof>())
            return visit(t);
        else if (auto t = ty->as<AstTypeUnion>())
            return visit(t);
        else if (auto t = ty->as<AstTypeIntersection>())
            return visit(t);
    }

    void visit(AstTypeReference* ty)
    {
        // No further validation is necessary in this case. The main logic for
        // _luau_print is contained in lookupAnnotation.
        if (FFlag::DebugLuauMagicTypes && ty->name == "_luau_print")
            return;

        for (const AstTypeOrPack& param : ty->parameters)
        {
            if (param.type)
                visit(param.type);
            else
                visit(param.typePack);
        }

        Scope* scope = findInnermostScope(ty->location);
        LUAU_ASSERT(scope);

        std::optional<TypeFun> alias =
            (ty->prefix) ? scope->lookupImportedType(ty->prefix->value, ty->name.value) : scope->lookupType(ty->name.value);

        if (alias.has_value())
        {
            size_t typesRequired = alias->typeParams.size();
            size_t packsRequired = alias->typePackParams.size();

            bool hasDefaultTypes = std::any_of(alias->typeParams.begin(), alias->typeParams.end(), [](auto&& el) {
                return el.defaultValue.has_value();
            });

            bool hasDefaultPacks = std::any_of(alias->typePackParams.begin(), alias->typePackParams.end(), [](auto&& el) {
                return el.defaultValue.has_value();
            });

            if (!ty->hasParameterList)
            {
                if ((!alias->typeParams.empty() && !hasDefaultTypes) || (!alias->typePackParams.empty() && !hasDefaultPacks))
                {
                    reportError(GenericError{"Type parameter list is required"}, ty->location);
                }
            }

            size_t typesProvided = 0;
            size_t extraTypes = 0;
            size_t packsProvided = 0;

            for (const AstTypeOrPack& p : ty->parameters)
            {
                if (p.type)
                {
                    if (packsProvided != 0)
                    {
                        reportError(GenericError{"Type parameters must come before type pack parameters"}, ty->location);
                        continue;
                    }

                    if (typesProvided < typesRequired)
                    {
                        typesProvided += 1;
                    }
                    else
                    {
                        extraTypes += 1;
                    }
                }
                else if (p.typePack)
                {
                    TypePackId tp = lookupPackAnnotation(p.typePack);

                    if (typesProvided < typesRequired && size(tp) == 1 && finite(tp) && first(tp))
                    {
                        typesProvided += 1;
                    }
                    else
                    {
                        packsProvided += 1;
                    }
                }
            }

            if (extraTypes != 0 && packsProvided == 0)
            {
                // Extra types are only collected into a pack if a pack is expected
                if (packsRequired != 0)
                    packsProvided += 1;
                else
                    typesProvided += extraTypes;
            }

            for (size_t i = typesProvided; i < typesRequired; ++i)
            {
                if (alias->typeParams[i].defaultValue)
                {
                    typesProvided += 1;
                }
            }

            for (size_t i = packsProvided; i < packsRequired; ++i)
            {
                if (alias->typePackParams[i].defaultValue)
                {
                    packsProvided += 1;
                }
            }

            if (extraTypes == 0 && packsProvided + 1 == packsRequired)
            {
                packsProvided += 1;
            }

            if (typesProvided != typesRequired || packsProvided != packsRequired)
            {
                reportError(IncorrectGenericParameterCount{
                                /* name */ ty->name.value,
                                /* typeFun */ *alias,
                                /* actualParameters */ typesProvided,
                                /* actualPackParameters */ packsProvided,
                            },
                    ty->location);
            }
        }
        else
        {
            if (scope->lookupPack(ty->name.value))
            {
                reportError(
                    SwappedGenericTypeParameter{
                        ty->name.value,
                        SwappedGenericTypeParameter::Kind::Type,
                    },
                    ty->location);
            }
            else
            {
                std::string symbol = "";
                if (ty->prefix)
                {
                    symbol += (*(ty->prefix)).value;
                    symbol += ".";
                }
                symbol += ty->name.value;

                reportError(UnknownSymbol{symbol, UnknownSymbol::Context::Type}, ty->location);
            }
        }
    }

    void visit(AstTypeTable* table)
    {
        // TODO!

        for (const AstTableProp& prop : table->props)
            visit(prop.type);

        if (table->indexer)
        {
            visit(table->indexer->indexType);
            visit(table->indexer->resultType);
        }
    }

    void visit(AstTypeFunction* ty)
    {
        visitGenerics(ty->generics, ty->genericPacks);
        visit(ty->argTypes);
        visit(ty->returnTypes);
    }

    void visit(AstTypeTypeof* ty)
    {
        visit(ty->expr, ValueContext::RValue);
    }

    void visit(AstTypeUnion* ty)
    {
        // TODO!
        for (AstType* type : ty->types)
            visit(type);
    }

    void visit(AstTypeIntersection* ty)
    {
        // TODO!
        for (AstType* type : ty->types)
            visit(type);
    }

    void visit(AstTypePack* pack)
    {
        if (auto p = pack->as<AstTypePackExplicit>())
            return visit(p);
        else if (auto p = pack->as<AstTypePackVariadic>())
            return visit(p);
        else if (auto p = pack->as<AstTypePackGeneric>())
            return visit(p);
    }

    void visit(AstTypePackExplicit* tp)
    {
        // TODO!
        for (AstType* type : tp->typeList.types)
            visit(type);

        if (tp->typeList.tailType)
            visit(tp->typeList.tailType);
    }

    void visit(AstTypePackVariadic* tp)
    {
        // TODO!
        visit(tp->variadicType);
    }

    void visit(AstTypePackGeneric* tp)
    {
        Scope* scope = findInnermostScope(tp->location);
        LUAU_ASSERT(scope);

        std::optional<TypePackId> alias = scope->lookupPack(tp->genericName.value);
        if (!alias.has_value())
        {
            if (scope->lookupType(tp->genericName.value))
            {
                reportError(
                    SwappedGenericTypeParameter{
                        tp->genericName.value,
                        SwappedGenericTypeParameter::Kind::Pack,
                    },
                    tp->location);
            }
            else
            {
                reportError(UnknownSymbol{tp->genericName.value, UnknownSymbol::Context::Type}, tp->location);
            }
        }
    }

    template<typename TID>
    bool isSubtype(TID subTy, TID superTy, NotNull<Scope> scope, bool genericsOkay = false)
    {
        TypeArena arena;
        Unifier u{NotNull{&normalizer}, scope, Location{}, Covariant};
        u.hideousFixMeGenericsAreActuallyFree = genericsOkay;
        u.enableScopeTests();

        u.tryUnify(subTy, superTy);
        const bool ok = u.errors.empty() && u.log.empty();
        return ok;
    }

    template<typename TID>
    ErrorVec tryUnify(NotNull<Scope> scope, const Location& location, TID subTy, TID superTy, CountMismatch::Context context = CountMismatch::Arg,
        bool genericsOkay = false)
    {
        Unifier u{NotNull{&normalizer}, scope, location, Covariant};
        u.ctx = context;
        u.hideousFixMeGenericsAreActuallyFree = genericsOkay;
        u.enableScopeTests();
        u.tryUnify(subTy, superTy);

        return std::move(u.errors);
    }

    void reportError(TypeErrorData data, const Location& location)
    {
        if (auto utk = get_if<UnknownProperty>(&data))
            diagnoseMissingTableKey(utk, data);

        module->errors.emplace_back(location, module->name, std::move(data));

        if (logger)
            logger->captureTypeCheckError(module->errors.back());
    }

    void reportError(TypeError e)
    {
        reportError(std::move(e.data), e.location);
    }

    void reportErrors(ErrorVec errors)
    {
        for (TypeError e : errors)
            reportError(std::move(e));
    }

    // If the provided type does not have the named property, report an error.
    void checkIndexTypeFromType(TypeId tableTy, const std::string& prop, const Location& location, ValueContext context, TypeId astIndexExprType)
    {
        const NormalizedType* norm = normalizer.normalize(tableTy);
        if (!norm)
        {
            reportError(NormalizationTooComplex{}, location);
            return;
        }

        bool foundOneProp = false;
        std::vector<TypeId> typesMissingTheProp;

        auto fetch = [&](TypeId ty) {
            if (!normalizer.isInhabited(ty))
                return;

            std::unordered_set<TypeId> seen;
            bool found = hasIndexTypeFromType(ty, prop, location, seen, astIndexExprType);
            foundOneProp |= found;
            if (!found)
                typesMissingTheProp.push_back(ty);
        };

        fetch(norm->tops);
        fetch(norm->booleans);

        for (const auto& [ty, _negations] : norm->classes.classes)
        {
            fetch(ty);
        }
        fetch(norm->errors);
        fetch(norm->nils);
        fetch(norm->numbers);
        if (!norm->strings.isNever())
            fetch(builtinTypes->stringType);
        fetch(norm->threads);
        for (TypeId ty : norm->tables)
            fetch(ty);
        if (norm->functions.isTop)
            fetch(builtinTypes->functionType);
        else if (!norm->functions.isNever())
        {
            if (norm->functions.parts.size() == 1)
                fetch(norm->functions.parts.front());
            else
            {
                std::vector<TypeId> parts;
                parts.insert(parts.end(), norm->functions.parts.begin(), norm->functions.parts.end());
                fetch(testArena.addType(IntersectionType{std::move(parts)}));
            }
        }
        for (const auto& [tyvar, intersect] : norm->tyvars)
        {
            if (get<NeverType>(intersect->tops))
            {
                TypeId ty = normalizer.typeFromNormal(*intersect);
                fetch(testArena.addType(IntersectionType{{tyvar, ty}}));
            }
            else
                fetch(tyvar);
        }

        if (!typesMissingTheProp.empty())
        {
            if (foundOneProp)
                reportError(MissingUnionProperty{tableTy, typesMissingTheProp, prop}, location);
            // For class LValues, we don't want to report an extension error,
            // because classes come into being with full knowledge of their
            // shape. We instead want to report the unknown property error of
            // the `else` branch.
            else if (context == ValueContext::LValue && !get<ClassType>(tableTy))
                reportError(CannotExtendTable{tableTy, CannotExtendTable::Property, prop}, location);
            else
                reportError(UnknownProperty{tableTy, prop}, location);
        }
    }

    bool hasIndexTypeFromType(TypeId ty, const std::string& prop, const Location& location, std::unordered_set<TypeId>& seen, TypeId astIndexExprType)
    {
        // If we have already encountered this type, we must assume that some
        // other codepath will do the right thing and signal false if the
        // property is not present.
        const bool isUnseen = seen.insert(ty).second;
        if (!isUnseen)
            return true;

        if (get<ErrorType>(ty) || get<AnyType>(ty) || get<NeverType>(ty))
            return true;

        if (isString(ty))
        {
            std::optional<TypeId> mtIndex = Luau::findMetatableEntry(builtinTypes, module->errors, builtinTypes->stringType, "__index", location);
            LUAU_ASSERT(mtIndex);
            ty = *mtIndex;
        }

        if (auto tt = getTableType(ty))
        {
            if (findTablePropertyRespectingMeta(builtinTypes, module->errors, ty, prop, location))
                return true;

            if (tt->indexer)
            {
                TypeId indexType = follow(tt->indexer->indexType);
                if (isPrim(indexType, PrimitiveType::String))
                    return true;
                // If the indexer looks like { [any] : _} - the prop lookup should be allowed!
                else if (get<AnyType>(indexType) || get<UnknownType>(indexType))
                    return true;
            }

            return false;
        }
        else if (const ClassType* cls = get<ClassType>(ty))
        {
            // If the property doesn't exist on the class, we consult the indexer
            // We need to check if the type of the index expression foo (x[foo])
            // is compatible with the indexer's indexType
            // Construct the intersection and test inhabitedness!
            if (auto property = lookupClassProp(cls, prop))
                return true;
            if (cls->indexer)
            {
                TypeId inhabitatedTestType = testArena.addType(IntersectionType{{cls->indexer->indexType, astIndexExprType}});
                return normalizer.isInhabited(inhabitatedTestType);
            }
            return false;
        }
        else if (const UnionType* utv = get<UnionType>(ty))
            return std::all_of(begin(utv), end(utv), [&](TypeId part) {
                return hasIndexTypeFromType(part, prop, location, seen, astIndexExprType);
            });
        else if (const IntersectionType* itv = get<IntersectionType>(ty))
            return std::any_of(begin(itv), end(itv), [&](TypeId part) {
                return hasIndexTypeFromType(part, prop, location, seen, astIndexExprType);
            });
        else
            return false;
    }

    void diagnoseMissingTableKey(UnknownProperty* utk, TypeErrorData& data) const
    {
        std::string_view sv(utk->key);
        std::set<Name> candidates;

        auto accumulate = [&](const TableType::Props& props) {
            for (const auto& [name, ty] : props)
            {
                if (sv != name && equalsLower(sv, name))
                    candidates.insert(name);
            }
        };

        if (auto ttv = getTableType(utk->table))
            accumulate(ttv->props);
        else if (auto ctv = get<ClassType>(follow(utk->table)))
        {
            while (ctv)
            {
                accumulate(ctv->props);

                if (!ctv->parent)
                    break;

                ctv = get<ClassType>(*ctv->parent);
                LUAU_ASSERT(ctv);
            }
        }

        if (!candidates.empty())
            data = TypeErrorData(UnknownPropButFoundLikeProp{utk->table, utk->key, candidates});
    }
};

void check(NotNull<BuiltinTypes> builtinTypes, NotNull<UnifierSharedState> unifierState, DcrLogger* logger, const SourceModule& sourceModule, Module* module)
{
    TypeChecker2 typeChecker{builtinTypes, unifierState, logger, &sourceModule, module};

    typeChecker.visit(sourceModule.root);

    unfreeze(module->interfaceTypes);
    copyErrors(module->errors, module->interfaceTypes);
    freeze(module->interfaceTypes);
}

} // namespace Luau
