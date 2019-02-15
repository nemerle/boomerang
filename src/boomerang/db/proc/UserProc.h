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


#include "boomerang/db/DataFlow.h"
#include "boomerang/db/UseCollector.h"
#include "boomerang/db/proc/Proc.h"
#include "boomerang/db/proc/ProcCFG.h"
#include "boomerang/util/StatementList.h"


class Binary;
class UserProc;
class Assign;
class ReturnStatement;


enum class ProcStatus : uint8_t
{
    Undecoded,  ///< Has not even been decoded
    Decoded,    ///< Decoded, no attempt at decompiling
    Visited,    ///< Has been visited on the way down in decompile()
    InCycle,    ///< Is involved in cycles, has not completed early decompilation as yet
    Preserveds, ///< Has had preservation analysis done
    MiddleDone, ///< Has completed everything except the global analyses
    FinalDone,  ///< Has had final decompilation
    CodegenDone ///< Has had code generated
};


typedef std::set<UserProc *> ProcSet;
typedef std::list<UserProc *> ProcList;


/**
 * UserProc class.
 */
class BOOMERANG_API UserProc : public Function
{
    typedef std::map<SharedExp, SharedExp, lessExpStar> ExpExpMap;

public:
    /**
     * A map between machine dependent locations and their corresponding symbolic,
     * machine independent representations.
     * Example: m[r28{0} - 8] -> local5; this means that *after* transforming
     * out of SSA form, any locations not specifically mapped otherwise
     * (e.g. m[r28{0} - 8]{55} -> local6) will get this name.
     *
     * It is a *multi*map because one location can have several default names
     * differentiated by type.
     * E.g. r24 -> eax for int, r24 -> eax_1 for float
     */
    typedef std::multimap<SharedConstExp, SharedExp, lessExpStar> SymbolMap;

public:
    /**
     * \param address Address of entry point of function
     * \param name    Name of function
     * \param mod     Module that contains this function
     */
    UserProc(Address address, const QString &name, Module *mod);
    UserProc(const UserProc &) = delete;
    UserProc(UserProc &&)      = default;

    virtual ~UserProc() override;

    UserProc &operator=(const UserProc &) = delete;
    UserProc &operator=(UserProc &&) = default;

public:
    /// \copydoc Function::isNoReturn
    bool isNoReturn() const override;

    /// \copydoc Function::getProven
    SharedExp getProven(SharedExp left) override;

    /// \copydoc Function::getPremised
    SharedExp getPremised(SharedExp left) override;

    /// \copydoc Function::isPreserved
    bool isPreserved(SharedExp e) override;

public:
    /// \returns a pointer to the CFG object.
    ProcCFG *getCFG() { return m_cfg.get(); }
    const ProcCFG *getCFG() const { return m_cfg.get(); }

    /// Returns a pointer to the DataFlow object.
    DataFlow *getDataFlow() { return &m_df; }
    const DataFlow *getDataFlow() const { return &m_df; }

    const std::shared_ptr<ProcSet> &getRecursionGroup() { return m_recursionGroup; }
    void setRecursionGroup(const std::shared_ptr<ProcSet> &recursionGroup)
    {
        m_recursionGroup = recursionGroup;
    }

    ProcStatus getStatus() const { return m_status; }
    void setStatus(ProcStatus s);

    /// Returns whether or not this procedure can be decoded (i.e. has it already been decoded).
    bool isDecoded() const { return m_status >= ProcStatus::Decoded; }
    bool isDecompiled() const { return m_status >= ProcStatus::FinalDone; }

    /// Records that this procedure has been decoded.
    void setDecoded();

    bool isEarlyRecursive() const
    {
        return m_recursionGroup != nullptr && m_status <= ProcStatus::InCycle;
    }

    bool doesRecurseTo(UserProc *proc) const
    {
        return m_recursionGroup && m_recursionGroup->find(proc) != m_recursionGroup->end();
    }

    /**
     * Get the BB with the entry point address for this procedure.
     * \note (not always the first BB)
     * \returns   Pointer to the entry point BB, or nullptr if not found
     */
    BasicBlock *getEntryBB();

    /// Set the entry BB for this procedure (constructor has the entry address)
    void setEntryBB();

    /// Decompile this procedure, and all callees.
    void decompileRecursive();

public:
    // statement related

    /// Update statement numbers
    void numberStatements() const;

    /// \returns all statements in this UserProc
    void getStatements(StatementList &stmts) const;

    /// Remove (but not delete) \p stmt from this UserProc
    /// \returns true iff successfully removed
    bool removeStatement(Statement *stmt);

    Assign *insertAssignAfter(Statement *s, SharedExp left, SharedExp right);

    /// Insert statement \p stmt after statement \p afterThis.
    /// \note this procedure is designed for the front end, where enclosing BBs are not set up yet.
    /// So this is an inefficient linear search!
    bool insertStatementAfter(Statement *afterThis, Statement *stmt);

public:
    // parameter related

    StatementList &getParameters() { return m_parameters; }
    const StatementList &getParameters() const { return m_parameters; }

    /// Add the parameter to the signature
    void addParameterToSignature(SharedExp e, SharedType ty);

    /**
     * Insert into parameters list correctly sorted.
     * Update the parameters, in case the signature and hence ordering
     * and filtering has changed, or the locations in the collector have changed
     */
    void insertParameter(SharedExp e, SharedType ty);

    SharedConstType getParamType(const QString &name) const;
    SharedType getParamType(const QString &name);

    void setParamType(const QString &name, SharedType ty);
    void setParamType(int idx, SharedType ty);

    /// e is a parameter location, e.g. r8 or m[r28{0}+8]. Lookup a symbol for it
    /// Find the implicit definition for \a e and lookup a symbol
    QString lookupParam(SharedConstExp e) const;

    /**
     * Filter out locations not possible as parameters or arguments.
     * \returns true if \p e should be filtered out (i.e. removed)
     * \sa UserProc::filterReturns
     */
    bool filterParams(SharedExp e);

public:
    // return related

    /// \returns the address of the RTL that contains the one and only return statement
    Address getRetAddr();

    /// \param rtlAddr the address of the RTL containing \p retStmt
    void setRetStmt(ReturnStatement *retStmt, Address rtlAddr);

    ReturnStatement *getRetStmt() { return m_retStatement; }
    const ReturnStatement *getRetStmt() const { return m_retStatement; }

    void removeRetStmt() { m_retStatement = nullptr; }

    /**
     * Filter out locations not possible as return locations.
     * \returns true if \p e  should be filtered out (i.e. removed)
     * \sa UserProc::filterParams
     */
    bool filterReturns(SharedExp e);

public:
    // local variable related

    const std::map<QString, SharedType> &getLocals() const { return m_locals; }
    std::map<QString, SharedType> &getLocals() { return m_locals; }

    /**
     * Return the next available local variable; make it the given type.
     * \note was returning TypedExp*.
     * If \p name is non null, use that name
     */
    SharedExp createLocal(SharedType ty, const SharedExp &e, const QString &name = "");

    /// Add a new local supplying all needed information.
    void addLocal(SharedType ty, const QString &name, SharedExp e);

    /// Check if \p r is already mapped to a local, else add one
    void ensureExpIsMappedToLocal(const std::shared_ptr<RefExp> &ref);

    /// Return an expression that is equivalent to \p le in terms of local variables.
    /// Creates new locals as needed.
    SharedExp getSymbolExp(SharedExp le, SharedType ty, bool lastPass = false);

    /// Determine whether e is a local, either as a true opLocal
    /// (e.g. generated by fromSSA), or if it is in the symbol map
    ///  and the name is in the locals map.
    /// If it is a local, return its name, else the empty string.
    QString findLocal(const SharedExp &e, SharedType ty);

    /// return a local's type
    SharedConstType getLocalType(const QString &name) const;
    void setLocalType(const QString &name, SharedType ty);

    /// Checks wheether \p e could represent a stack local or stack param,
    /// i.e. whether \p e is of the form m[sp{-} +/- K]
    /// It does not check whether \p e actually is or is mapped to a local variable.
    /// \returns true if the pattern matches.
    bool isLocalOrParamPattern(SharedConstExp e) const;

public:
    // symbol related
    SymbolMap &getSymbolMap() { return m_symbolMap; }
    const SymbolMap &getSymbolMap() const { return m_symbolMap; }

    /// \returns the original expression that maps to the local variable with name \p name
    /// Example: If eax maps to the local variable foo, return eax
    /// (not Location::local("foo", proc))
    SharedConstExp expFromSymbol(const QString &name) const;

    void mapSymbolTo(const SharedConstExp &from, SharedExp to);

    /// \returns the name of a symbol (local variable or parameter)
    /// of a used expression \p e with type \p ty.
    QString lookupSym(const SharedConstExp &e, SharedConstType ty) const;

    /// Lookup a specific symbol for the given ref
    QString lookupSymFromRef(const std::shared_ptr<const RefExp> &ref) const;

    /// Lookup a specific symbol if any, else the general one if any
    QString lookupSymFromRefAny(const std::shared_ptr<const RefExp> &ref) const;

public:
    // call / recursion related

    /**
     * Mark calls involved in the recursion cycle as non childless
     * (each child has had middleDecompile called on it now).
     * \todo Not sure that this is needed...
     */
    void markAsNonChildless(const std::shared_ptr<ProcSet> &cs);

    /// Get the callees.
    std::list<Function *> &getCallees() { return m_calleeList; }

    /**
     * Add this callee to the set of callees for this proc
     * \param  callee A pointer to the callee function
     */
    void addCallee(Function *callee);

    /// \return true if this procedure does not define \p exp,
    /// or saves and restores the value of \p exp.
    bool preservesExp(const SharedExp &exp);

    /// Same as \ref preservesExp, but \p exp is preserved to \p exp + \p offset
    /// (e.g. x86 esp is preserved to esp+4)
    bool preservesExpWithOffset(const SharedExp &exp, int offset);

public:
    bool canRename(SharedConstExp e) const { return m_df.canRename(e); }

    UseCollector &getUseCollector() { return m_procUseCollector; }
    const UseCollector &getUseCollector() const { return m_procUseCollector; }

    /// promote the signature if possible
    void promoteSignature();

    QString findFirstSymbol(const SharedConstExp &e) const;

    /// Replace all occurrences of \p pattern in all statements by \p replacement
    /// \returns true if the pattern was found at least once
    /// (Therefore, replacing an expression with itself will return true)
    bool searchAndReplace(const Exp &pattern, SharedExp replacement);

    /// Add a location to the UseCollector; this means this location is used
    /// before defined, and hence is an *initial* parameter.
    /// \note final parameters don't use this information;
    /// it's only for handling recursion.
    void markAsInitialParam(const SharedExp &loc);

    bool allPhisHaveDefs() const;

    const ExpExpMap &getProvenTrue() const { return m_provenTrue; }

public:
    QString toString() const;

    /// print this proc, mainly for debugging
    void print(OStream &out) const;

    void debugPrintAll(const QString &stepName);

private:
    void printParams(OStream &out) const;

    /// Print just the symbol map
    void printSymbolMap(OStream &out) const;

    /// For debugging
    void printLocals(OStream &os) const;

private:
    /// True if a local exists with name \p name
    bool existsLocal(const QString &name) const;

    /// Return a string for a new local suitable for \p e
    QString newLocalName(const SharedExp &e);

    /// Get a name like eax or o2 from r24 or r8
    QString getRegName(SharedExp r);

    /// Find the type of the local or parameter \a e
    SharedType getTypeForLocation(const SharedExp &e);
    SharedConstType getTypeForLocation(const SharedConstExp &e) const;

    /// Prove any arbitary property of this procedure.
    /// If \p conditional is true, do not save the result,
    /// as it may be conditional on premises stored in other procedures
    /// \note this function was non-reentrant, but now reentrancy is frequently used
    bool proveEqual(const SharedExp &lhs, const SharedExp &rhs, bool conditional = false);

    /// helper function for proveEqual()
    bool prover(SharedExp query, std::set<PhiAssign *> &lastPhis,
                std::map<PhiAssign *, SharedExp> &cache, PhiAssign *lastPhi = nullptr);

    // FIXME: is this the same as lookupSym() now?
    /// Lookup the expression in the symbol map. Return nullptr or a C string with the symbol. Use
    /// the Type* ty to select from several names in the multimap; the name corresponding to the
    /// first compatible type is returned
    SharedExp getSymbolFor(const SharedConstExp &e, const SharedConstType &ty) const;

    /// Set a location as a new premise, i.e. assume e=e
    void setPremise(const SharedExp &e);

    void killPremise(const SharedExp &e);

    bool isNoReturnInternal(std::set<const Function *> &visited) const;

private:
    /// The status of this user procedure.
    /// Status: undecoded .. final decompiled
    ProcStatus m_status = ProcStatus::Undecoded;

    /// Number of the next local. Can't use locals.size() because some get deleted
    int m_nextLocal = 0;

    std::unique_ptr<ProcCFG> m_cfg; ///< The control flow graph.

    /// DataFlow object. Holds information relevant to transforming to and from SSA form.
    DataFlow m_df;

    /**
     * The list of parameters, ordered and filtered.
     * Note that a LocationList could be used, but then there would be nowhere
     * to store the types (for DFA based TA)
     *
     * The RHS is just ignored; the list is of ImplicitAssigns.
     *
     * \note DESIGN ISSUE: it would be nice for the parameters' implicit assignments
     * to be the sole definitions, i.e. not need other implicit assignments for these.
     * But the targets of RefExp's are not expected to change address, so they are
     * not suitable at present (since the addresses regularly get changed
     * as the parameters get recreated).
     */
    StatementList m_parameters;

    SymbolMap m_symbolMap;

    /// Set of callees (Procedures that this procedure calls).
    /// Used for call graph, among other things
    std::list<Function *> m_calleeList;

    /**
     * Somewhat DEPRECATED now. Eventually use the localTable.
     * This map records the names and types for local variables. It should be a subset of the
     * symbolMap, which also stores parameters. It is a convenient place to store the types of
     * locals after conversion from SSA form, since it is then difficult to access the definitions
     * of locations. This map could be combined with symbolMap below, but beware of parameters (in
     * symbols but not locals)
     */
    std::map<QString, SharedType> m_locals;

    /**
     * A collector for initial parameters (locations used before being defined).
     * Note that final parameters don't use this;
     * it's only of use during group decompilation analysis (sorting out recursion)
     */
    UseCollector m_procUseCollector;

    /**
     * All the expressions that have been proven true.
     * (Could perhaps do with a list of some that are proven false)
     * Proof the form r28 = r28 + 4 is stored as map from "r28" to "r28+4" (NOTE: no subscripts)
     * FIXME: shouldn't provenTrue be in UserProc, with logic associated with the signature doing
     * the equivalent thing for LibProcs?
     */
    ExpExpMap m_provenTrue;

    /**
     * Premises for recursion group analysis. This is a preservation
     * that is assumed true only for definitions by calls reached in the proof.
     * It also prevents infinite looping of this proof logic.
     */
    ExpExpMap m_recurPremises;

    std::shared_ptr<ProcSet> m_recursionGroup;

    /**
     * We ensure that there is only one return statement now.
     * See code in frontend/frontend.cpp handling case StmtType::Ret.
     * If no return statement, this will be nullptr.
     */
    ReturnStatement *m_retStatement = nullptr;
};
