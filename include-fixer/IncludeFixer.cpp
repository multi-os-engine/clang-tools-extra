//===-- IncludeFixer.cpp - Include inserter based on sema callbacks -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "IncludeFixer.h"
#include "clang/Format/Format.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/ParseAST.h"
#include "clang/Sema/ExternalSemaSource.h"
#include "clang/Sema/Sema.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "include-fixer"

using namespace clang;

namespace clang {
namespace include_fixer {
namespace {

/// Manages the parse, gathers include suggestions.
class Action : public clang::ASTFrontendAction,
               public clang::ExternalSemaSource {
public:
  explicit Action(SymbolIndexManager &SymbolIndexMgr, bool MinimizeIncludePaths)
      : SymbolIndexMgr(SymbolIndexMgr),
        MinimizeIncludePaths(MinimizeIncludePaths) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &Compiler,
                    StringRef InFile) override {
    Filename = InFile;
    return llvm::make_unique<clang::ASTConsumer>();
  }

  void ExecuteAction() override {
    clang::CompilerInstance *Compiler = &getCompilerInstance();
    assert(!Compiler->hasSema() && "CI already has Sema");

    // Set up our hooks into sema and parse the AST.
    if (hasCodeCompletionSupport() &&
        !Compiler->getFrontendOpts().CodeCompletionAt.FileName.empty())
      Compiler->createCodeCompletionConsumer();

    clang::CodeCompleteConsumer *CompletionConsumer = nullptr;
    if (Compiler->hasCodeCompletionConsumer())
      CompletionConsumer = &Compiler->getCodeCompletionConsumer();

    Compiler->createSema(getTranslationUnitKind(), CompletionConsumer);
    Compiler->getSema().addExternalSource(this);

    clang::ParseAST(Compiler->getSema(), Compiler->getFrontendOpts().ShowStats,
                    Compiler->getFrontendOpts().SkipFunctionBodies);
  }

  /// Callback for incomplete types. If we encounter a forward declaration we
  /// have the fully qualified name ready. Just query that.
  bool MaybeDiagnoseMissingCompleteType(clang::SourceLocation Loc,
                                        clang::QualType T) override {
    // Ignore spurious callbacks from SFINAE contexts.
    if (getCompilerInstance().getSema().isSFINAEContext())
      return false;

    clang::ASTContext &context = getCompilerInstance().getASTContext();
    std::string QueryString =
        T.getUnqualifiedType().getAsString(context.getPrintingPolicy());
    DEBUG(llvm::dbgs() << "Query missing complete type '" << QueryString
                       << "'");
    query(QueryString, "", tooling::Range());
    return false;
  }

  /// Callback for unknown identifiers. Try to piece together as much
  /// qualification as we can get and do a query.
  clang::TypoCorrection CorrectTypo(const DeclarationNameInfo &Typo,
                                    int LookupKind, Scope *S, CXXScopeSpec *SS,
                                    CorrectionCandidateCallback &CCC,
                                    DeclContext *MemberContext,
                                    bool EnteringContext,
                                    const ObjCObjectPointerType *OPT) override {
    // Ignore spurious callbacks from SFINAE contexts.
    if (getCompilerInstance().getSema().isSFINAEContext())
      return clang::TypoCorrection();

    // We currently ignore the unidentified symbol which is not from the
    // main file.
    //
    // However, this is not always true due to templates in a non-self contained
    // header, consider the case:
    //
    //   // header.h
    //   template <typename T>
    //   class Foo {
    //     T t;
    //   };
    //
    //   // test.cc
    //   // We need to add <bar.h> in test.cc instead of header.h.
    //   class Bar;
    //   Foo<Bar> foo;
    //
    // FIXME: Add the missing header to the header file where the symbol comes
    // from.
    if (!getCompilerInstance().getSourceManager().isWrittenInMainFile(
            Typo.getLoc()))
      return clang::TypoCorrection();

    std::string TypoScopeString;
    if (S) {
      // FIXME: Currently we only use namespace contexts. Use other context
      // types for query.
      for (const auto *Context = S->getEntity(); Context;
           Context = Context->getParent()) {
        if (const auto *ND = dyn_cast<NamespaceDecl>(Context)) {
          if (!ND->getName().empty())
            TypoScopeString = ND->getNameAsString() + "::" + TypoScopeString;
        }
      }
    }

    auto ExtendNestedNameSpecifier = [this](CharSourceRange Range) {
      StringRef Source =
          Lexer::getSourceText(Range, getCompilerInstance().getSourceManager(),
                               getCompilerInstance().getLangOpts());

      // Skip forward until we find a character that's neither identifier nor
      // colon. This is a bit of a hack around the fact that we will only get a
      // single callback for a long nested name if a part of the beginning is
      // unknown. For example:
      //
      // llvm::sys::path::parent_path(...)
      // ^~~~  ^~~
      //    known
      //            ^~~~
      //      unknown, last callback
      //                  ^~~~~~~~~~~
      //                  no callback
      //
      // With the extension we get the full nested name specifier including
      // parent_path.
      // FIXME: Don't rely on source text.
      const char *End = Source.end();
      while (isIdentifierBody(*End) || *End == ':')
        ++End;

      return std::string(Source.begin(), End);
    };

    /// If we have a scope specification, use that to get more precise results.
    std::string QueryString;
    tooling::Range SymbolRange;
    const auto &SM = getCompilerInstance().getSourceManager();
    auto CreateToolingRange = [&QueryString, &SM](SourceLocation BeginLoc) {
      return tooling::Range(SM.getDecomposedLoc(BeginLoc).second,
                            QueryString.size());
    };
    if (SS && SS->getRange().isValid()) {
      auto Range = CharSourceRange::getTokenRange(SS->getRange().getBegin(),
                                                  Typo.getLoc());

      QueryString = ExtendNestedNameSpecifier(Range);
      SymbolRange = CreateToolingRange(Range.getBegin());
    } else if (Typo.getName().isIdentifier() && !Typo.getLoc().isMacroID()) {
      auto Range =
          CharSourceRange::getTokenRange(Typo.getBeginLoc(), Typo.getEndLoc());

      QueryString = ExtendNestedNameSpecifier(Range);
      SymbolRange = CreateToolingRange(Range.getBegin());
    } else {
      QueryString = Typo.getAsString();
      SymbolRange = CreateToolingRange(Typo.getLoc());
    }

    DEBUG(llvm::dbgs() << "TypoScopeQualifiers: " << TypoScopeString << "\n");
    query(QueryString, TypoScopeString, SymbolRange);

    // FIXME: We should just return the name we got as input here and prevent
    // clang from trying to correct the typo by itself. That may change the
    // identifier to something that's not wanted by the user.
    return clang::TypoCorrection();
  }

  StringRef filename() const { return Filename; }

  /// Get the minimal include for a given path.
  std::string minimizeInclude(StringRef Include,
                              const clang::SourceManager &SourceManager,
                              clang::HeaderSearch &HeaderSearch) {
    if (!MinimizeIncludePaths)
      return Include;

    // Get the FileEntry for the include.
    StringRef StrippedInclude = Include.trim("\"<>");
    const FileEntry *Entry =
        SourceManager.getFileManager().getFile(StrippedInclude);

    // If the file doesn't exist return the path from the database.
    // FIXME: This should never happen.
    if (!Entry)
      return Include;

    bool IsSystem;
    std::string Suggestion =
        HeaderSearch.suggestPathToFileForDiagnostics(Entry, &IsSystem);

    return IsSystem ? '<' + Suggestion + '>' : '"' + Suggestion + '"';
  }

  /// Get the include fixer context for the queried symbol.
  IncludeFixerContext
  getIncludeFixerContext(const clang::SourceManager &SourceManager,
                         clang::HeaderSearch &HeaderSearch) {
    std::vector<find_all_symbols::SymbolInfo> SymbolCandidates;
    for (const auto &Symbol : MatchedSymbols) {
      std::string FilePath = Symbol.getFilePath().str();
      std::string MinimizedFilePath = minimizeInclude(
          ((FilePath[0] == '"' || FilePath[0] == '<') ? FilePath
                                                      : "\"" + FilePath + "\""),
          SourceManager, HeaderSearch);
      SymbolCandidates.emplace_back(Symbol.getName(), Symbol.getSymbolKind(),
                                    MinimizedFilePath, Symbol.getLineNumber(),
                                    Symbol.getContexts(),
                                    Symbol.getNumOccurrences());
    }
    return IncludeFixerContext(QuerySymbol, SymbolScopedQualifiers,
                               SymbolCandidates, QuerySymbolRange);
  }

private:
  /// Query the database for a given identifier.
  bool query(StringRef Query, StringRef ScopedQualifiers, tooling::Range Range) {
    assert(!Query.empty() && "Empty query!");

    // Skip other identifiers once we have discovered an identfier successfully.
    if (!MatchedSymbols.empty())
      return false;

    DEBUG(llvm::dbgs() << "Looking up '" << Query << "' at ");
    DEBUG(getCompilerInstance()
              .getSourceManager()
              .getLocForStartOfFile(
                  getCompilerInstance().getSourceManager().getMainFileID())
              .getLocWithOffset(Range.getOffset())
              .print(llvm::dbgs(), getCompilerInstance().getSourceManager()));
    DEBUG(llvm::dbgs() << " ...");

    QuerySymbol = Query.str();
    QuerySymbolRange = Range;
    SymbolScopedQualifiers = ScopedQualifiers;

    // Query the symbol based on C++ name Lookup rules.
    // Firstly, lookup the identifier with scoped namespace contexts;
    // If that fails, falls back to look up the identifier directly.
    //
    // For example:
    //
    // namespace a {
    // b::foo f;
    // }
    //
    // 1. lookup a::b::foo.
    // 2. lookup b::foo.
    std::string QueryString = ScopedQualifiers.str() + Query.str();
    MatchedSymbols = SymbolIndexMgr.search(QueryString);
    if (MatchedSymbols.empty() && !ScopedQualifiers.empty())
      MatchedSymbols = SymbolIndexMgr.search(Query);
    DEBUG(llvm::dbgs() << "Having found " << MatchedSymbols.size()
                       << " symbols\n");
    return !MatchedSymbols.empty();
  }

  /// The client to use to find cross-references.
  SymbolIndexManager &SymbolIndexMgr;

  /// The absolute path to the file being processed.
  std::string Filename;

  /// The symbol being queried.
  std::string QuerySymbol;

  /// The scoped qualifiers of QuerySymbol. It is represented as a sequence of
  /// names and scope resolution operatiors ::, ending with a scope resolution
  /// operator (e.g. a::b::). Empty if the symbol is not in a specific scope.
  std::string SymbolScopedQualifiers;

  /// The replacement range of the first discovered QuerySymbol.
  tooling::Range QuerySymbolRange;

  /// All symbol candidates which match QuerySymbol. We only include the first
  /// discovered identifier to avoid getting caught in results from error
  /// recovery.
  std::vector<find_all_symbols::SymbolInfo> MatchedSymbols;

  /// Whether we should use the smallest possible include path.
  bool MinimizeIncludePaths = true;
};

} // namespace

IncludeFixerActionFactory::IncludeFixerActionFactory(
    SymbolIndexManager &SymbolIndexMgr, IncludeFixerContext &Context,
    StringRef StyleName, bool MinimizeIncludePaths)
    : SymbolIndexMgr(SymbolIndexMgr), Context(Context),
      MinimizeIncludePaths(MinimizeIncludePaths) {}

IncludeFixerActionFactory::~IncludeFixerActionFactory() = default;

bool IncludeFixerActionFactory::runInvocation(
    clang::CompilerInvocation *Invocation, clang::FileManager *Files,
    std::shared_ptr<clang::PCHContainerOperations> PCHContainerOps,
    clang::DiagnosticConsumer *Diagnostics) {
  assert(Invocation->getFrontendOpts().Inputs.size() == 1);

  // Set up Clang.
  clang::CompilerInstance Compiler(PCHContainerOps);
  Compiler.setInvocation(Invocation);
  Compiler.setFileManager(Files);

  // Create the compiler's actual diagnostics engine. We want to drop all
  // diagnostics here.
  Compiler.createDiagnostics(new clang::IgnoringDiagConsumer,
                             /*ShouldOwnClient=*/true);
  Compiler.createSourceManager(*Files);

  // We abort on fatal errors so don't let a large number of errors become
  // fatal. A missing #include can cause thousands of errors.
  Compiler.getDiagnostics().setErrorLimit(0);

  // Run the parser, gather missing includes.
  auto ScopedToolAction =
      llvm::make_unique<Action>(SymbolIndexMgr, MinimizeIncludePaths);
  Compiler.ExecuteAction(*ScopedToolAction);

  Context = ScopedToolAction->getIncludeFixerContext(
      Compiler.getSourceManager(),
      Compiler.getPreprocessor().getHeaderSearchInfo());

  // Technically this should only return true if we're sure that we have a
  // parseable file. We don't know that though. Only inform users of fatal
  // errors.
  return !Compiler.getDiagnostics().hasFatalErrorOccurred();
}

llvm::Expected<tooling::Replacements>
createInsertHeaderReplacements(StringRef Code, StringRef FilePath,
                               StringRef Header,
                               const clang::format::FormatStyle &Style) {
  if (Header.empty())
    return tooling::Replacements();
  std::string IncludeName = "#include " + Header.str() + "\n";
  // Create replacements for the new header.
  clang::tooling::Replacements Insertions = {
      tooling::Replacement(FilePath, UINT_MAX, 0, IncludeName)};

  auto CleanReplaces = cleanupAroundReplacements(Code, Insertions, Style);
  if (!CleanReplaces)
    return CleanReplaces;
  return formatReplacements(Code, *CleanReplaces, Style);
}

} // namespace include_fixer
} // namespace clang
