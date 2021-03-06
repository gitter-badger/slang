//------------------------------------------------------------------------------
// Compilation.cpp
// Central manager for compilation processes.
//
// File is under the MIT license; see LICENSE for details.
//------------------------------------------------------------------------------
#include "Compilation.h"

#include "parsing/SyntaxTree.h"
#include "symbols/ASTVisitor.h"

#include "BuiltInSubroutines.h"

namespace {

using namespace slang;

// This visitor is used to touch every node in the AST to ensure that all lazily
// evaluated members have been realized and we have recorded every diagnostic.
struct DiagnosticVisitor : public ASTVisitor<DiagnosticVisitor> {
    void handle(const ValueSymbol& value) { value.getType(); }
    void handle(const ExplicitImportSymbol& symbol) { symbol.importedSymbol(); }
    void handle(const WildcardImportSymbol& symbol) { symbol.getPackage(); }
    void handle(const SubroutineSymbol& symbol) { symbol.returnType.get(); }
    void handle(const VariableSymbol& symbol) { symbol.type.get(); symbol.initializer.get(); }
};

}

namespace slang {

Compilation::Compilation() :
    bitType(ScalarType::Bit),
    logicType(ScalarType::Logic),
    regType(ScalarType::Reg),
    signedBitType(ScalarType::Bit, true),
    signedLogicType(ScalarType::Logic, true),
    signedRegType(ScalarType::Reg, true),
    shortIntType(PredefinedIntegerType::ShortInt),
    intType(PredefinedIntegerType::Int),
    longIntType(PredefinedIntegerType::LongInt),
    byteType(PredefinedIntegerType::Byte),
    integerType(PredefinedIntegerType::Integer),
    timeType(PredefinedIntegerType::Time),
    realType(FloatingType::Real),
    realTimeType(FloatingType::RealTime),
    shortRealType(FloatingType::ShortReal)
{
    // Register built-in types for lookup by syntax kind.
    knownTypes[SyntaxKind::ShortIntType] = &shortIntType;
    knownTypes[SyntaxKind::IntType] = &intType;
    knownTypes[SyntaxKind::LongIntType] = &longIntType;
    knownTypes[SyntaxKind::ByteType] = &byteType;
    knownTypes[SyntaxKind::BitType] = &bitType;
    knownTypes[SyntaxKind::LogicType] = &logicType;
    knownTypes[SyntaxKind::RegType] = &regType;
    knownTypes[SyntaxKind::IntegerType] = &integerType;
    knownTypes[SyntaxKind::TimeType] = &timeType;
    knownTypes[SyntaxKind::RealType] = &realType;
    knownTypes[SyntaxKind::RealTimeType] = &realTimeType;
    knownTypes[SyntaxKind::ShortRealType] = &shortRealType;
    knownTypes[SyntaxKind::StringType] = &stringType;
    knownTypes[SyntaxKind::CHandleType] = &chandleType;
    knownTypes[SyntaxKind::VoidType] = &voidType;
    knownTypes[SyntaxKind::NullLiteralExpression] = &nullType;
    knownTypes[SyntaxKind::EventType] = &eventType;
    knownTypes[SyntaxKind::Unknown] = &errorType;

    knownNetTypes[TokenKind::WireKeyword] = std::make_unique<NetType>(NetType::Wire);
    knownNetTypes[TokenKind::WAndKeyword] = std::make_unique<NetType>(NetType::WAnd);
    knownNetTypes[TokenKind::WOrKeyword] = std::make_unique<NetType>(NetType::WOr);
    knownNetTypes[TokenKind::TriKeyword] = std::make_unique<NetType>(NetType::Tri);
    knownNetTypes[TokenKind::TriAndKeyword] = std::make_unique<NetType>(NetType::TriAnd);
    knownNetTypes[TokenKind::TriOrKeyword] = std::make_unique<NetType>(NetType::TriOr);
    knownNetTypes[TokenKind::Tri0Keyword] = std::make_unique<NetType>(NetType::Tri0);
    knownNetTypes[TokenKind::Tri1Keyword] = std::make_unique<NetType>(NetType::Tri1);
    knownNetTypes[TokenKind::TriRegKeyword] = std::make_unique<NetType>(NetType::TriReg);
    knownNetTypes[TokenKind::Supply0Keyword] = std::make_unique<NetType>(NetType::Supply0);
    knownNetTypes[TokenKind::Supply1Keyword] = std::make_unique<NetType>(NetType::Supply1);
    knownNetTypes[TokenKind::UWireKeyword] = std::make_unique<NetType>(NetType::UWire);
    knownNetTypes[TokenKind::Unknown] = std::make_unique<NetType>(NetType::Unknown);
    wireNetType = knownNetTypes[TokenKind::WireKeyword].get();

    // Scalar types are indexed by bit flags.
    auto registerScalar = [this](auto& type) { scalarTypeTable[type.getIntegralFlags().bits() & 0x7] = &type; };
    registerScalar(bitType);
    registerScalar(logicType);
    registerScalar(regType);
    registerScalar(signedBitType);
    registerScalar(signedLogicType);
    registerScalar(signedRegType);

    root = std::make_unique<RootSymbol>(*this);

    // Register all system functions.
#define REGISTER(name) addSystemSubroutine(std::make_unique<Builtins::name##Subroutine>())
    REGISTER(Clog2);
    REGISTER(Bits);
    REGISTER(Low);
    REGISTER(High);
    REGISTER(Left);
    REGISTER(Right);
    REGISTER(Size);
    REGISTER(Increment);
#undef REGISTER
}

void Compilation::addSyntaxTree(std::shared_ptr<SyntaxTree> tree) {
    if (finalized)
        throw std::logic_error("The compilation has already been finalized");

    if (&tree->sourceManager() != sourceManager) {
        if (!sourceManager)
            sourceManager = &tree->sourceManager();
        else
            throw std::logic_error("All syntax trees added to the compilation must use the same source manager");
    }

    auto unit = emplace<CompilationUnitSymbol>(*this);
    const SyntaxNode& node = tree->root();
    NameSet instances;

    if (node.kind == SyntaxKind::CompilationUnit) {
        for (auto member : node.as<CompilationUnitSyntax>().members) {
            unit->addMembers(*member);

            // Because of the requirement that we look at uninstantiated branches of generate blocks,
            // we need to look at the syntax nodes instead of any bound symbols.
            if (member->kind == SyntaxKind::ModuleDeclaration) {
                SmallVectorSized<NameSet, 2> scopeStack;
                findInstantiations(member->as<MemberSyntax>(), scopeStack, instances);
            }
        }
    }
    else {
        unit->addMembers(node);

        if (node.kind == SyntaxKind::ModuleDeclaration) {
            SmallVectorSized<NameSet, 2> scopeStack;
            findInstantiations(node.as<MemberSyntax>(), scopeStack, instances);
        }
    }

    // Merge found instantiations into the global list. This is done separately instead of
    // just passing instantiatedNames into findInstantiations to make it easy in the future
    // to make this method thread safe by throwing a lock around this stuff.
    for (auto entry : instances)
        instantiatedNames.emplace(entry);

    root->addMember(*unit);
    compilationUnits.push_back(unit);
    syntaxTrees.emplace_back(std::move(tree));
    forcedDiagnostics = false;
}

span<const std::shared_ptr<SyntaxTree>> Compilation::getSyntaxTrees() const {
    return syntaxTrees;
}

const RootSymbol& Compilation::getRoot() {
    if (!finalized) {
        // Find modules that have no instantiations.
        SmallVectorSized<const ModuleInstanceSymbol*, 4> topList;
        for (auto& [key, definition] : definitionMap) {
            (void)key;
            const auto& syntax = definition->syntax;

            if (syntax.kind == SyntaxKind::ModuleDeclaration && instantiatedNames.count(definition->name) == 0) {
                // TODO: check for no parameters here
                const auto& instance = ModuleInstanceSymbol::instantiate(*this, definition->name,
                                                                         syntax.header.name.location(), *definition);
                root->addMember(instance);
                topList.append(&instance);
            }
        }

        // Sort the list of instances so that we get deterministic ordering of instances;
        // the order is otherwise dependent on iterating over a hash table.
        std::sort(topList.begin(), topList.end(), [](auto a, auto b) { return a->name < b->name; });

        root->topInstances = topList.copy(*this);
        root->compilationUnits = compilationUnits;
        finalized = true;
    }
    return *root;
}

const Definition* Compilation::getDefinition(string_view lookupName, const Scope& scope) const {
    const Scope* searchScope = &scope;
    while (true) {
        auto it = definitionMap.find(std::make_tuple(lookupName, searchScope));
        if (it != definitionMap.end())
            return it->second.get();

        if (searchScope->asSymbol().kind == SymbolKind::Root)
            return nullptr;

        searchScope = searchScope->getParent();
    }
}

void Compilation::addDefinition(const ModuleDeclarationSyntax& syntax, const Scope& scope) {
    SmallVectorSized<Definition::ParameterDecl, 8> parameters;
    bool hasPortParams = syntax.header.parameters;
    if (hasPortParams) {
        bool lastLocal = false;
        for (auto declaration : syntax.header.parameters->declarations) {
            // It's legal to leave off the parameter keyword in the parameter port list.
            // If you do so, we "inherit" the parameter or localparam keyword from the previous entry.
            // This isn't allowed in a module body, but the parser will take care of the error for us.
            if (declaration->keyword)
                lastLocal = declaration->keyword.kind == TokenKind::LocalParamKeyword;
            getParamDecls(*declaration, true, lastLocal, parameters);
        }
    }

    // Also search through immediate members in the body of the definition for any parameters, as they may
    // be overridable at instantiation time.
    for (auto member : syntax.members) {
        if (member->kind == SyntaxKind::ParameterDeclarationStatement) {
            const auto& declaration = member->as<ParameterDeclarationStatementSyntax>().parameter;
            bool isLocal = hasPortParams || declaration.keyword.kind == TokenKind::LocalParamKeyword;
            getParamDecls(declaration, false, isLocal, parameters);
        }
    }

    auto definition = std::make_unique<Definition>(syntax);
    definition->parameters = parameters.copy(*this);

    // Record that the given scope contains this definition. If the scope is a compilation unit, add it to
    // the root scope instead so that lookups from other compilation units will find it.
    if (scope.asSymbol().kind == SymbolKind::CompilationUnit)
        definitionMap.emplace(std::make_tuple(definition->name, root.get()), std::move(definition));
    else
        definitionMap.emplace(std::make_tuple(definition->name, &scope), std::move(definition));
}

void Compilation::getParamDecls(const ParameterDeclarationSyntax& syntax, bool isPort, bool isLocal,
                                SmallVector<Definition::ParameterDecl>& parameters) {
    for (const VariableDeclaratorSyntax* decl : syntax.declarators) {
        Definition::ParameterDecl param;
        param.name = decl->name.valueText();
        param.location = decl->name.location();
        param.type = &syntax.type;
        param.isLocal = isLocal;
        param.isPort = isPort;

        if (decl->initializer)
            param.initializer = &decl->initializer->expr;
        else if (!isPort)
            addError(DiagCode::BodyParamNoInitializer, param.location);
        else if (isLocal)
            addError(DiagCode::LocalParamNoInitializer, param.location);

        parameters.append(param);
    }
}

const PackageSymbol* Compilation::getPackage(string_view lookupName) const {
    auto it = packageMap.find(lookupName);
    if (it == packageMap.end())
        return nullptr;
    return it->second;
}

void Compilation::addPackage(const PackageSymbol& package) {
    packageMap.emplace(package.name, &package);
}

void Compilation::addSystemSubroutine(std::unique_ptr<SystemSubroutine> subroutine) {
    subroutineMap.emplace(subroutine->name, std::move(subroutine));
}

const SystemSubroutine* Compilation::getSystemSubroutine(string_view name) const {
    auto it = subroutineMap.find(name);
    if (it == subroutineMap.end())
        return nullptr;
    return it->second.get();
}

const NameSyntax& Compilation::parseName(string_view name) {
    SourceManager& sourceMan = SyntaxTree::getDefaultSourceManager();
    Preprocessor preprocessor(sourceMan, *this, diags);
    preprocessor.pushSource(sourceMan.assignText(name));

    Parser parser(preprocessor);
    return parser.parseName();
}

CompilationUnitSymbol& Compilation::createScriptScope() {
    auto unit = emplace<CompilationUnitSymbol>(*this);
    root->addMember(*unit);
    return *unit;
}

Diagnostics Compilation::getParseDiagnostics() {
    Diagnostics results;
    for (const auto& tree : syntaxTrees)
        results.appendRange(tree->diagnostics());

    if (sourceManager)
        results.sort(*sourceManager);
    return results;
}

Diagnostics Compilation::getSemanticDiagnostics() {
    // If we haven't already done so, touch every symbol, scope, statement,
    // and expression tree so that we can be sure we have all the diagnostics.
    if (!forcedDiagnostics) {
        forcedDiagnostics = true;
        DiagnosticVisitor visitor;
        getRoot().visit(visitor);
    }

    Diagnostics results;
    results.appendRange(diags);

    if (sourceManager)
        results.sort(*sourceManager);

    return results;
}

Diagnostics Compilation::getAllDiagnostics() {
    Diagnostics results = getParseDiagnostics();
    results.appendRange(getSemanticDiagnostics());

    if (sourceManager)
        results.sort(*sourceManager);
    return results;
}

void Compilation::addDiagnostics(const Diagnostics& diagnostics) {
    diags.appendRange(diagnostics);
}

const Type& Compilation::getType(SyntaxKind typeKind) const {
    auto it = knownTypes.find(typeKind);
    return it == knownTypes.end() ? errorType : *it->second;
}

const Type& Compilation::getType(const DataTypeSyntax& node, LookupLocation location, const Scope& parent,
                                 bool allowNetType) {
    const Type& result = Type::fromSyntax(*this, node, location, parent);
    if (!allowNetType && result.isNetType()) {
        addError(DiagCode::NetTypeNotAllowed, node.sourceRange()) << result.name;
        return errorType;
    }
    return result;
}

const PackedArrayType& Compilation::getType(bitwidth_t width, bitmask<IntegralFlags> flags) {
    ASSERT(width > 0);
    uint32_t key = width;
    key |= uint32_t(flags.bits()) << SVInt::BITWIDTH_BITS;
    auto it = vectorTypeCache.find(key);
    if (it != vectorTypeCache.end())
        return *it->second;

    auto type = emplace<PackedArrayType>(getScalarType(flags), ConstantRange { int32_t(width - 1), 0 });
    vectorTypeCache.emplace_hint(it, key, type);
    return *type;
}

const ScalarType& Compilation::getScalarType(bitmask<IntegralFlags> flags) {
    ScalarType* ptr = scalarTypeTable[flags.bits() & 0x7];
    ASSERT(ptr);
    return *ptr;
}

const NetType& Compilation::getNetType(TokenKind kind) const {
    auto it = knownNetTypes.find(kind);
    return it == knownNetTypes.end() ? *knownNetTypes.find(TokenKind::Unknown)->second : *it->second;
}

Scope::DeferredMemberData& Compilation::getOrAddDeferredData(Scope::DeferredMemberIndex& index) {
    if (index == Scope::DeferredMemberIndex::Invalid)
        index = deferredData.emplace();
    return deferredData[index];
}

void Compilation::trackImport(Scope::ImportDataIndex& index, const WildcardImportSymbol& import) {
    if (index != Scope::ImportDataIndex::Invalid)
        importData[index].push_back(&import);
    else
        index = importData.add({ &import });
}

span<const WildcardImportSymbol*> Compilation::queryImports(Scope::ImportDataIndex index) {
    if (index == Scope::ImportDataIndex::Invalid)
        return {};
    return importData[index];
}

bool Compilation::checkNoUnknowns(const SVInt& value, SourceRange range) {
    if (value.hasUnknown()) {
        addError(DiagCode::ValueMustNotBeUnknown, range);
        return false;
    }
    return true;
}

bool Compilation::checkPositive(const SVInt& value, SourceRange range) {
    if (value.isSigned() && value.isNegative()) {
        addError(DiagCode::ValueMustBePositive, range);
        return false;
    }
    return true;
}

optional<bitwidth_t> Compilation::checkValidBitWidth(const SVInt& value, SourceRange range) {
    auto result = value.as<bitwidth_t>();
    if (!result)
        addError(DiagCode::ValueExceedsMaxBitWidth, range) << (int)SVInt::MAX_BITS;
    return result;
}

void Compilation::findInstantiations(const ModuleDeclarationSyntax& module, SmallVector<NameSet>& scopeStack,
                                     NameSet& found) {
    // If there are nested modules that shadow global module names, we need to
    // ignore them when considering instantiations.
    NameSet* localDefs = nullptr;
    for (auto member : module.members) {
        switch (member->kind) {
            case SyntaxKind::ModuleDeclaration:
            case SyntaxKind::InterfaceDeclaration:
            case SyntaxKind::ProgramDeclaration: {
                // ignore empty names
                string_view name = member->as<ModuleDeclarationSyntax>().header.name.valueText();
                if (!name.empty()) {
                    // create new scope entry lazily
                    if (!localDefs) {
                        scopeStack.emplace();
                        localDefs = &scopeStack.back();
                    }
                    localDefs->insert(name);
                }
                break;
            }
            default:
                break;
        }
    }

    // now traverse all children
    for (auto member : module.members)
        findInstantiations(*member, scopeStack, found);

    if (localDefs)
        scopeStack.pop();
}

static bool containsName(const SmallVector<flat_hash_set<string_view>>& scopeStack, string_view name) {
    for (const auto& set : scopeStack) {
        if (set.find(name) != set.end())
            return true;
    }
    return false;
}

void Compilation::findInstantiations(const MemberSyntax& node, SmallVector<NameSet>& scopeStack,
                                     NameSet& found) {
    switch (node.kind) {
        case SyntaxKind::HierarchyInstantiation: {
            // Determine whether this is a local or global module we're instantiating;
            // don't worry about local instantiations right now, they can't be root.
            const auto& his = node.as<HierarchyInstantiationSyntax>();
            string_view name = his.type.valueText();
            if (!name.empty() && !containsName(scopeStack, name))
                found.insert(name);
            break;
        }
        case SyntaxKind::ModuleDeclaration:
        case SyntaxKind::InterfaceDeclaration:
        case SyntaxKind::ProgramDeclaration:
            findInstantiations(node.as<ModuleDeclarationSyntax>(), scopeStack, found);
            break;
        case SyntaxKind::GenerateRegion:
            for (auto& child : node.as<GenerateRegionSyntax>().members)
                findInstantiations(*child, scopeStack, found);
            break;
        case SyntaxKind::GenerateBlock:
            for (auto& child : node.as<GenerateBlockSyntax>().members)
                findInstantiations(*child, scopeStack, found);
            break;
        case SyntaxKind::LoopGenerate:
            findInstantiations(node.as<LoopGenerateSyntax>().block, scopeStack, found);
            break;
        case SyntaxKind::CaseGenerate:
            for (auto& item : node.as<CaseGenerateSyntax>().items) {
                switch (item->kind) {
                    case SyntaxKind::DefaultCaseItem:
                        findInstantiations(item->as<DefaultCaseItemSyntax>().clause.as<MemberSyntax>(),
                                           scopeStack, found);
                        break;
                    case SyntaxKind::StandardCaseItem:
                        findInstantiations(item->as<StandardCaseItemSyntax>().clause.as<MemberSyntax>(),
                                           scopeStack, found);
                        break;
                    default:
                        break;
                }
            }
            break;
        case SyntaxKind::IfGenerate: {
            const auto& ifGen = node.as<IfGenerateSyntax>();
            findInstantiations(ifGen.block, scopeStack, found);
            if (ifGen.elseClause)
                findInstantiations(ifGen.elseClause->clause.as<MemberSyntax>(), scopeStack, found);
            break;
        }
        default:
            break;
    }
}

}
