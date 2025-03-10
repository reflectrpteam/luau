// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/Clone.h"

#include "Luau/RecursionCounter.h"
#include "Luau/TxnLog.h"
#include "Luau/TypePack.h"
#include "Luau/Unifiable.h"

LUAU_FASTFLAG(DebugLuauCopyBeforeNormalizing)
LUAU_FASTFLAG(LuauClonePublicInterfaceLess2)
LUAU_FASTFLAG(DebugLuauReadWriteProperties)

LUAU_FASTINTVARIABLE(LuauTypeCloneRecursionLimit, 300)
LUAU_FASTFLAGVARIABLE(LuauCloneCyclicUnions, false)

namespace Luau
{

namespace
{

Property clone(const Property& prop, TypeArena& dest, CloneState& cloneState)
{
    if (FFlag::DebugLuauReadWriteProperties)
    {
        std::optional<TypeId> cloneReadTy;
        if (auto ty = prop.readType())
            cloneReadTy = clone(*ty, dest, cloneState);

        std::optional<TypeId> cloneWriteTy;
        if (auto ty = prop.writeType())
            cloneWriteTy = clone(*ty, dest, cloneState);

        std::optional<Property> cloned = Property::create(cloneReadTy, cloneWriteTy);
        LUAU_ASSERT(cloned);
        cloned->deprecated = prop.deprecated;
        cloned->deprecatedSuggestion = prop.deprecatedSuggestion;
        cloned->location = prop.location;
        cloned->tags = prop.tags;
        cloned->documentationSymbol = prop.documentationSymbol;
        return *cloned;
    }
    else
    {
        return Property{
            clone(prop.type(), dest, cloneState),
            prop.deprecated,
            prop.deprecatedSuggestion,
            prop.location,
            prop.tags,
            prop.documentationSymbol,
        };
    }
}

static TableIndexer clone(const TableIndexer& indexer, TypeArena& dest, CloneState& cloneState)
{
    LUAU_ASSERT(FFlag::LuauTypecheckClassTypeIndexers);
    return TableIndexer{clone(indexer.indexType, dest, cloneState), clone(indexer.indexResultType, dest, cloneState)};
}

struct TypePackCloner;

/*
 * Both TypeCloner and TypePackCloner work by depositing the requested type variable into the appropriate 'seen' set.
 * They do not return anything because their sole consumer (the deepClone function) already has a pointer into this storage.
 */

struct TypeCloner
{
    TypeCloner(TypeArena& dest, TypeId typeId, CloneState& cloneState)
        : dest(dest)
        , typeId(typeId)
        , seenTypes(cloneState.seenTypes)
        , seenTypePacks(cloneState.seenTypePacks)
        , cloneState(cloneState)
    {
    }

    TypeArena& dest;
    TypeId typeId;
    SeenTypes& seenTypes;
    SeenTypePacks& seenTypePacks;
    CloneState& cloneState;

    template<typename T>
    void defaultClone(const T& t);

    void operator()(const FreeType& t);
    void operator()(const GenericType& t);
    void operator()(const BoundType& t);
    void operator()(const ErrorType& t);
    void operator()(const BlockedType& t);
    void operator()(const PendingExpansionType& t);
    void operator()(const PrimitiveType& t);
    void operator()(const SingletonType& t);
    void operator()(const FunctionType& t);
    void operator()(const TableType& t);
    void operator()(const MetatableType& t);
    void operator()(const ClassType& t);
    void operator()(const AnyType& t);
    void operator()(const UnionType& t);
    void operator()(const IntersectionType& t);
    void operator()(const LazyType& t);
    void operator()(const UnknownType& t);
    void operator()(const NeverType& t);
    void operator()(const NegationType& t);
    void operator()(const TypeFamilyInstanceType& t);
};

struct TypePackCloner
{
    TypeArena& dest;
    TypePackId typePackId;
    SeenTypes& seenTypes;
    SeenTypePacks& seenTypePacks;
    CloneState& cloneState;

    TypePackCloner(TypeArena& dest, TypePackId typePackId, CloneState& cloneState)
        : dest(dest)
        , typePackId(typePackId)
        , seenTypes(cloneState.seenTypes)
        , seenTypePacks(cloneState.seenTypePacks)
        , cloneState(cloneState)
    {
    }

    template<typename T>
    void defaultClone(const T& t)
    {
        TypePackId cloned = dest.addTypePack(TypePackVar{t});
        seenTypePacks[typePackId] = cloned;
    }

    void operator()(const FreeTypePack& t)
    {
        defaultClone(t);
    }
    void operator()(const GenericTypePack& t)
    {
        defaultClone(t);
    }
    void operator()(const ErrorTypePack& t)
    {
        defaultClone(t);
    }

    void operator()(const BlockedTypePack& t)
    {
        defaultClone(t);
    }

    // While we are a-cloning, we can flatten out bound Types and make things a bit tighter.
    // We just need to be sure that we rewrite pointers both to the binder and the bindee to the same pointer.
    void operator()(const Unifiable::Bound<TypePackId>& t)
    {
        TypePackId cloned = clone(t.boundTo, dest, cloneState);
        if (FFlag::DebugLuauCopyBeforeNormalizing)
            cloned = dest.addTypePack(TypePackVar{BoundTypePack{cloned}});
        seenTypePacks[typePackId] = cloned;
    }

    void operator()(const VariadicTypePack& t)
    {
        TypePackId cloned = dest.addTypePack(TypePackVar{VariadicTypePack{clone(t.ty, dest, cloneState), /*hidden*/ t.hidden}});
        seenTypePacks[typePackId] = cloned;
    }

    void operator()(const TypePack& t)
    {
        TypePackId cloned = dest.addTypePack(TypePack{});
        TypePack* destTp = getMutable<TypePack>(cloned);
        LUAU_ASSERT(destTp != nullptr);
        seenTypePacks[typePackId] = cloned;

        for (TypeId ty : t.head)
            destTp->head.push_back(clone(ty, dest, cloneState));

        if (t.tail)
            destTp->tail = clone(*t.tail, dest, cloneState);
    }

    void operator()(const TypeFamilyInstanceTypePack& t)
    {
        TypePackId cloned = dest.addTypePack(TypeFamilyInstanceTypePack{t.family, {}, {}});
        TypeFamilyInstanceTypePack* destTp = getMutable<TypeFamilyInstanceTypePack>(cloned);
        LUAU_ASSERT(destTp);
        seenTypePacks[typePackId] = cloned;

        destTp->typeArguments.reserve(t.typeArguments.size());
        for (TypeId ty : t.typeArguments)
            destTp->typeArguments.push_back(clone(ty, dest, cloneState));

        destTp->packArguments.reserve(t.packArguments.size());
        for (TypePackId tp : t.packArguments)
            destTp->packArguments.push_back(clone(tp, dest, cloneState));
    }
};

template<typename T>
void TypeCloner::defaultClone(const T& t)
{
    TypeId cloned = dest.addType(t);
    seenTypes[typeId] = cloned;
}

void TypeCloner::operator()(const FreeType& t)
{
    defaultClone(t);
}

void TypeCloner::operator()(const GenericType& t)
{
    defaultClone(t);
}

void TypeCloner::operator()(const Unifiable::Bound<TypeId>& t)
{
    TypeId boundTo = clone(t.boundTo, dest, cloneState);
    if (FFlag::DebugLuauCopyBeforeNormalizing)
        boundTo = dest.addType(BoundType{boundTo});
    seenTypes[typeId] = boundTo;
}

void TypeCloner::operator()(const Unifiable::Error& t)
{
    defaultClone(t);
}

void TypeCloner::operator()(const BlockedType& t)
{
    defaultClone(t);
}

void TypeCloner::operator()(const PendingExpansionType& t)
{
    TypeId res = dest.addType(PendingExpansionType{t.prefix, t.name, t.typeArguments, t.packArguments});
    PendingExpansionType* petv = getMutable<PendingExpansionType>(res);
    LUAU_ASSERT(petv);

    seenTypes[typeId] = res;

    std::vector<TypeId> typeArguments;
    for (TypeId arg : t.typeArguments)
        typeArguments.push_back(clone(arg, dest, cloneState));

    std::vector<TypePackId> packArguments;
    for (TypePackId arg : t.packArguments)
        packArguments.push_back(clone(arg, dest, cloneState));

    petv->typeArguments = std::move(typeArguments);
    petv->packArguments = std::move(packArguments);
}

void TypeCloner::operator()(const PrimitiveType& t)
{
    defaultClone(t);
}

void TypeCloner::operator()(const SingletonType& t)
{
    defaultClone(t);
}

void TypeCloner::operator()(const FunctionType& t)
{
    // FISHY: We always erase the scope when we clone things.  clone() was
    // originally written so that we could copy a module's type surface into an
    // export arena.  This probably dates to that.
    TypeId result = dest.addType(FunctionType{TypeLevel{0, 0}, {}, {}, nullptr, nullptr, t.definition, t.hasSelf});
    FunctionType* ftv = getMutable<FunctionType>(result);
    LUAU_ASSERT(ftv != nullptr);

    seenTypes[typeId] = result;

    for (TypeId generic : t.generics)
        ftv->generics.push_back(clone(generic, dest, cloneState));

    for (TypePackId genericPack : t.genericPacks)
        ftv->genericPacks.push_back(clone(genericPack, dest, cloneState));

    ftv->tags = t.tags;
    ftv->argTypes = clone(t.argTypes, dest, cloneState);
    ftv->argNames = t.argNames;
    ftv->retTypes = clone(t.retTypes, dest, cloneState);
    ftv->hasNoFreeOrGenericTypes = t.hasNoFreeOrGenericTypes;
}

void TypeCloner::operator()(const TableType& t)
{
    // If table is now bound to another one, we ignore the content of the original
    if (!FFlag::DebugLuauCopyBeforeNormalizing && t.boundTo)
    {
        TypeId boundTo = clone(*t.boundTo, dest, cloneState);
        seenTypes[typeId] = boundTo;
        return;
    }

    TypeId result = dest.addType(TableType{});
    TableType* ttv = getMutable<TableType>(result);
    LUAU_ASSERT(ttv != nullptr);

    *ttv = t;

    seenTypes[typeId] = result;

    ttv->level = TypeLevel{0, 0};

    if (FFlag::DebugLuauCopyBeforeNormalizing && t.boundTo)
        ttv->boundTo = clone(*t.boundTo, dest, cloneState);

    for (const auto& [name, prop] : t.props)
        ttv->props[name] = clone(prop, dest, cloneState);

    if (FFlag::LuauTypecheckClassTypeIndexers)
    {
        if (t.indexer)
            ttv->indexer = clone(*t.indexer, dest, cloneState);
    }
    else
    {
        if (t.indexer)
            ttv->indexer = TableIndexer{clone(t.indexer->indexType, dest, cloneState), clone(t.indexer->indexResultType, dest, cloneState)};
    }

    for (TypeId& arg : ttv->instantiatedTypeParams)
        arg = clone(arg, dest, cloneState);

    for (TypePackId& arg : ttv->instantiatedTypePackParams)
        arg = clone(arg, dest, cloneState);

    ttv->definitionModuleName = t.definitionModuleName;
    ttv->definitionLocation = t.definitionLocation;
    ttv->tags = t.tags;
}

void TypeCloner::operator()(const MetatableType& t)
{
    TypeId result = dest.addType(MetatableType{});
    MetatableType* mtv = getMutable<MetatableType>(result);
    seenTypes[typeId] = result;

    mtv->table = clone(t.table, dest, cloneState);
    mtv->metatable = clone(t.metatable, dest, cloneState);
}

void TypeCloner::operator()(const ClassType& t)
{
    TypeId result = dest.addType(ClassType{t.name, {}, std::nullopt, std::nullopt, t.tags, t.userData, t.definitionModuleName});
    ClassType* ctv = getMutable<ClassType>(result);

    seenTypes[typeId] = result;

    for (const auto& [name, prop] : t.props)
        ctv->props[name] = clone(prop, dest, cloneState);

    if (t.parent)
        ctv->parent = clone(*t.parent, dest, cloneState);

    if (t.metatable)
        ctv->metatable = clone(*t.metatable, dest, cloneState);

    if (FFlag::LuauTypecheckClassTypeIndexers)
    {
        if (t.indexer)
            ctv->indexer = clone(*t.indexer, dest, cloneState);
    }
}

void TypeCloner::operator()(const AnyType& t)
{
    defaultClone(t);
}

void TypeCloner::operator()(const UnionType& t)
{
    if (FFlag::LuauCloneCyclicUnions)
    {
        TypeId result = dest.addType(FreeType{nullptr});
        seenTypes[typeId] = result;

        std::vector<TypeId> options;
        options.reserve(t.options.size());

        for (TypeId ty : t.options)
            options.push_back(clone(ty, dest, cloneState));

        asMutable(result)->ty.emplace<UnionType>(std::move(options));
    }
    else
    {
        std::vector<TypeId> options;
        options.reserve(t.options.size());

        for (TypeId ty : t.options)
            options.push_back(clone(ty, dest, cloneState));

        TypeId result = dest.addType(UnionType{std::move(options)});
        seenTypes[typeId] = result;
    }
}

void TypeCloner::operator()(const IntersectionType& t)
{
    TypeId result = dest.addType(IntersectionType{});
    seenTypes[typeId] = result;

    IntersectionType* option = getMutable<IntersectionType>(result);
    LUAU_ASSERT(option != nullptr);

    for (TypeId ty : t.parts)
        option->parts.push_back(clone(ty, dest, cloneState));
}

void TypeCloner::operator()(const LazyType& t)
{
    if (TypeId unwrapped = t.unwrapped.load())
    {
        seenTypes[typeId] = clone(unwrapped, dest, cloneState);
    }
    else
    {
        defaultClone(t);
    }
}

void TypeCloner::operator()(const UnknownType& t)
{
    defaultClone(t);
}

void TypeCloner::operator()(const NeverType& t)
{
    defaultClone(t);
}

void TypeCloner::operator()(const NegationType& t)
{
    TypeId result = dest.addType(AnyType{});
    seenTypes[typeId] = result;

    TypeId ty = clone(t.ty, dest, cloneState);
    asMutable(result)->ty = NegationType{ty};
}

void TypeCloner::operator()(const TypeFamilyInstanceType& t)
{
    TypeId result = dest.addType(TypeFamilyInstanceType{
        t.family,
        {},
        {},
    });

    seenTypes[typeId] = result;

    TypeFamilyInstanceType* tfit = getMutable<TypeFamilyInstanceType>(result);
    LUAU_ASSERT(tfit != nullptr);

    tfit->typeArguments.reserve(t.typeArguments.size());
    for (TypeId p : t.typeArguments)
        tfit->typeArguments.push_back(clone(p, dest, cloneState));

    tfit->packArguments.reserve(t.packArguments.size());
    for (TypePackId p : t.packArguments)
        tfit->packArguments.push_back(clone(p, dest, cloneState));
}

} // anonymous namespace

TypePackId clone(TypePackId tp, TypeArena& dest, CloneState& cloneState)
{
    if (tp->persistent)
        return tp;

    RecursionLimiter _ra(&cloneState.recursionCount, FInt::LuauTypeCloneRecursionLimit);

    TypePackId& res = cloneState.seenTypePacks[tp];

    if (res == nullptr)
    {
        TypePackCloner cloner{dest, tp, cloneState};
        Luau::visit(cloner, tp->ty); // Mutates the storage that 'res' points into.
    }

    return res;
}

TypeId clone(TypeId typeId, TypeArena& dest, CloneState& cloneState)
{
    if (typeId->persistent)
        return typeId;

    RecursionLimiter _ra(&cloneState.recursionCount, FInt::LuauTypeCloneRecursionLimit);

    TypeId& res = cloneState.seenTypes[typeId];

    if (res == nullptr)
    {
        TypeCloner cloner{dest, typeId, cloneState};
        Luau::visit(cloner, typeId->ty); // Mutates the storage that 'res' points into.

        // Persistent types are not being cloned and we get the original type back which might be read-only
        if (!res->persistent)
        {
            asMutable(res)->documentationSymbol = typeId->documentationSymbol;
        }
    }

    return res;
}

TypeFun clone(const TypeFun& typeFun, TypeArena& dest, CloneState& cloneState)
{
    TypeFun result;

    for (auto param : typeFun.typeParams)
    {
        TypeId ty = clone(param.ty, dest, cloneState);
        std::optional<TypeId> defaultValue;

        if (param.defaultValue)
            defaultValue = clone(*param.defaultValue, dest, cloneState);

        result.typeParams.push_back({ty, defaultValue});
    }

    for (auto param : typeFun.typePackParams)
    {
        TypePackId tp = clone(param.tp, dest, cloneState);
        std::optional<TypePackId> defaultValue;

        if (param.defaultValue)
            defaultValue = clone(*param.defaultValue, dest, cloneState);

        result.typePackParams.push_back({tp, defaultValue});
    }

    result.type = clone(typeFun.type, dest, cloneState);

    return result;
}

} // namespace Luau
