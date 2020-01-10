#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#pragma once


#include "CodeWriter.h"
#include "ControlFlowAnalyzer.h"

#include "boomerang/core/BoomerangAPI.h"
#include "boomerang/ifc/ICodeGenerator.h"
#include "boomerang/ssl/statements/Statement.h"
#include "boomerang/ssl/type/Type.h"
#include "boomerang/util/Address.h"

#include <QStringList>

#include <list>
#include <map>
#include <unordered_set>


class Exp;
class LocationSet;
class BinaryImage;
class Statement;

struct SwitchInfo;

/// Operator precedence
/**
 * Operator Name            Associativity Operators
 * Primary scope resolution LTR           ::
 * Primary                  LTR           () [] . -> dynamic_cast typeid
 * Unary                    RTL           ++ -- + - ! ~ & * (type_name) sizeof new delete
 * C++ Pointer to Member    LTR           .* ->*
 * Multiplicative           LTR           * / %
 * Additive                 LTR           + -
 * Bitwise Shift            LTR           << >>
 * Relational               LTR           < > <= >=
 * Equality                 LTR           == !=
 * Bitwise AND              LTR           &
 * Bitwise Exclusive OR     LTR           ^
 * Bitwise Inclusive OR     LTR           |
 * Logical AND              LTR           &&
 * Logical OR               LTR           ||
 * Conditional              RTL           ? :
 * Assignment               RTL           = += -= *= /= <<= >>= %= &= ^= |=
 * Comma                    LTR           ,
 */
enum class OpPrec : uint8_t
{
    Invalid  = 0,
    Scope    = 1,  ///< (LTR) Primary scope resolution
    Prim     = 2,  ///< (LTR) Primary
    Unary    = 3,  ///< (RTL) Unary
    PtrMem   = 4,  ///< (LTR) C++ Pointer to Member
    Mult     = 5,  ///< (LTR) Multiplicative
    Add      = 6,  ///< (LTR) Additive
    BitShift = 7,  ///< (LTR) Bitwise Shift
    Comp3Way = 8,  ///< (LTR) 3-way comparison operator (C++20)
    Rel      = 9,  ///< (LTR) Relational
    Equal    = 10, ///< (LTR) Equality
    BitAnd   = 11, ///< (LTR) Bitwise AND
    BitXor   = 12, ///< (LTR) Bitwise Exclusive OR
    BitOr    = 13, ///< (LTR) Bitwise Inclusive OR
    LogAnd   = 14, ///< (LTR) Logical AND
    LogOr    = 15, ///< (LTR) Logical OR
    Cond     = 16, ///< (RTL) Ternary conditional operator
    Assign   = 16, ///< (RTL) Assignment (same precedence as ?: operator)
    Comma    = 17, ///< (LTR) Comma

    None = 0xFF, ///< Outer level (no parens required)
};


/**
 * Concrete class for the "C" high level language
 * This class provides methods which are specific for the C language binding.
 * I guess this will be the most popular output language unless we do C++.
 */
class BOOMERANG_PLUGIN_API CCodeGenerator : public ICodeGenerator
{
public:
    CCodeGenerator(Project *project);
    ~CCodeGenerator() override = default;

public:
    /// \copydoc ICodeGenerator::generateCode
    virtual void generateCode(const Prog *prog, Module *module = nullptr, UserProc *proc = nullptr,
                              bool intermixRTL = false) override;

private:
    /// Add an assignment statement at the current position.
    void addAssignmentStatement(const std::shared_ptr<const Assign> &assign);

    /**
     * Adds a call to the function \p proc.
     *
     * \param dest      The Proc the call is to.
     * \param name      The name the Proc has.
     * \param args      The arguments to the call.
     * \param results   The variable that will receive the return value of the function.
     *
     * \todo            Remove the \p name parameter and use Proc::getName()
     * \todo            Add assignment for when the function returns a struct.
     */
    void addCallStatement(const Function *dest, const QString &name, const StatementList &args,
                          const StatementList &results);

    /**
     * Adds an indirect call to \p exp.
     * \param results UNUSED
     *
     * \sa addCallStatement
     * \todo Add the use of \p results like AddCallStatement.
     */
    void addIndCallStatement(const SharedExp &exp, const StatementList &args,
                             const StatementList &results);

    /**
     * Adds a return statement and returns the first expression in \a rets.
     * \todo This should be returning a struct if more than one real return value.
     */
    void addReturnStatement(const StatementList *rets);

    /// Removes unused labels from the code.
    void removeUnusedLabels();

    /// Add a prototype (for forward declaration)
    void addPrototype(UserProc *proc);

    /// Generate code for a single procedure.
    void generateCode(UserProc *proc);

    /// Generate global variables from data sections.
    void generateDataSectionCode(const BinaryImage *image, QString sectionName,
                                 Address sectionStart, uint32_t sectionSize);

    /**
     * Print the declaration of a function.
     * \param proc Function to print
     * \param isDef True to print trailing opening bracket '{', false to print ';'
     */
    void addFunctionSignature(UserProc *proc, bool isDef);

    /*
     * Functions to add new code
     */

    // pretested loops (cond is optional because it is in the bb [somewhere])
    /// Adds: while (\p cond) {
    void addPretestedLoopHeader(const SharedExp &cond);

    /// Adds: }
    void addPretestedLoopEnd();

    // endless loops
    /// Adds: for(;;) {
    void addEndlessLoopHeader();

    /// Adds: }
    void addEndlessLoopEnd();

    // posttested loops
    /// Adds: do {
    void addPostTestedLoopHeader();

    /// Adds: } while (\a cond);
    void addPostTestedLoopEnd(const SharedConstExp &cond);

    // case conditionals "nways"
    /// Adds: switch(\a cond) {
    void addCaseCondHeader(const SharedConstExp &cond);

    /// Adds: case \a opt :
    void addCaseCondOption(const SharedConstExp &opt);

    /// Adds: break;
    void addCaseCondOptionEnd();

    /// Adds: default:
    void addCaseCondElse();

    /// Adds: }
    void addCaseCondEnd();

    // if conditions
    /// Adds: if(\a cond) {
    void addIfCondHeader(const SharedConstExp &cond);

    /// Adds: }
    void addIfCondEnd();

    // if else conditions
    /// Adds: if(\a cond) {
    void addIfElseCondHeader(const SharedConstExp &cond);

    /// Adds: } else {
    void addIfElseCondOption();

    /// Adds: }
    void addIfElseCondEnd();

    // goto, break, continue, etc
    void addGoto(const StmtASTNode *bb);

    /// Adds: continue;
    void addContinue();

    /// Adds: break;
    void addBreak();

    // labels
    /// Adds: L \a ord :
    void addLabel(const StmtASTNode *bb);

    // proc related
    /**
     * Print the start of a function, and also as a comment its address.
     */
    void addProcStart(UserProc *proc);

    /// Adds: }
    void addProcEnd();

    /**
     * Declare a local variable.
     * \param name given to the new local
     * \param type of this local variable
     * \param last true if an empty line should be added.
     */
    void addLocal(const QString &name, SharedType type, bool last = false);

    /**
     * Add the declaration for a global.
     * \param name given name for the global
     * \param type The type of the global
     * \param init The initial value of the global.
     */
    void addGlobal(const QString &name, SharedType type, const SharedExp &init = nullptr);

    /// Adds one line of comment to the code.
    void addLineComment(const QString &cmt);

private:
    /**
     * Append code for the given expression \a exp to stream \a str.
     *
     * \param str           The stream to output to.
     * \param exp           The expresson to output.
     * \param curPrec       The current operator precedence. Add parens around this expression if
     *                      necessary.
     * \param allowUnsigned If true, cast operands to unsigned if necessary.
     *
     * \todo This function is 800+ lines, and should possibly be split up.
     */
    void appendExp(OStream &str, const SharedConstExp &exp, OpPrec curPrec,
                   bool allowUnsigned = false);

    /// Print the type represented by \a typ to \a str.
    void appendType(OStream &str, SharedConstType typ);

    /**
     * Print the identified type to \a str.
     */
    void appendTypeIdent(OStream &str, SharedConstType typ, QString ident);

    /// Adds: (
    void openParen(OStream &str, OpPrec outer, OpPrec inner);

    /// Adds: )
    void closeParen(OStream &str, OpPrec outer, OpPrec inner);


    void generateCode(const StmtASTNode *bb, const StmtASTNode *latch,
                      std::list<const StmtASTNode *> &followSet,
                      std::list<const StmtASTNode *> &gotoSet, UserProc *proc);
    void generateCode_Loop(const StmtASTNode *bb, std::list<const StmtASTNode *> &gotoSet,
                           UserProc *proc, const StmtASTNode *latch,
                           std::list<const StmtASTNode *> &followSet);
    void generateCode_Branch(const StmtASTNode *bb, std::list<const StmtASTNode *> &gotoSet,
                             UserProc *proc, const StmtASTNode *latch,
                             std::list<const StmtASTNode *> &followSet);
    void generateCode_Seq(const StmtASTNode *bb, std::list<const StmtASTNode *> &gotoSet,
                          UserProc *proc, const StmtASTNode *latch,
                          std::list<const StmtASTNode *> &followSet);

    /// Emits a goto statement (at the correct indentation level) with the destination label for
    /// dest. Also places the label just before the destination code if it isn't already there. If
    /// the goto is to the return block, it would be nice to emit a 'return' instead (but would have
    /// to duplicate the other code in that return BB).    Also, 'continue' and 'break' statements
    /// are used instead if possible
    void emitGotoAndLabel(const StmtASTNode *bb, const StmtASTNode *dest);

    /// Generates code for each non-CTI (except procedure calls) statement within the block.
    void writeBB(const StmtASTNode *bb);

    /// \returns true if all predecessors of this BB have had their code generated.
    bool isAllParentsGenerated(const StmtASTNode *bb) const;
    bool isGenerated(const StmtASTNode *bb) const;

    void emitCodeForStmt(const SharedConstStmt &stmt);

    /**
     * Computes the optimal case ordering of switch statements.
     * The ordering is determined by the position in the list; by traversing the list front-to-back,
     * one emits the case statements in the correct order.
     * The value or the case label is determined by the value of the first part of the pair,
     * the jump destination for the case is determined by the second part of the pair.
     */
    std::list<std::pair<SharedExp, const StmtASTNode *>>
    computeOptimalCaseOrdering(const StmtASTNode *caseHead, const SwitchInfo *switchInfo);

private:
    void print(const Module *module);

    /// Output 4 * \p indLevel spaces to \p str
    void indent(OStream &str, int indLevel);

    /// Private helper functions, to reduce redundant code, and
    /// have a single place to put a breakpoint on.
    void appendLine(const QString &s);

private:
    int m_indent = 0;                       ///< Current indentation depth
    std::map<QString, SharedType> m_locals; ///< All locals in a Proc
    std::unordered_set<int> m_usedLabels;   ///< All used goto labels. (lowAddr of BB)
    std::unordered_set<const StmtASTNode *> m_generatedBBs;

    UserProc *m_proc = nullptr;
    ControlFlowAnalyzer m_analyzer;

    CodeWriter m_writer;
    QStringList m_lines; ///< The generated code.
};
