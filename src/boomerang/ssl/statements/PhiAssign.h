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


#include "boomerang/db/BasicBlock.h"
#include "boomerang/ssl/exp/RefExp.h"
#include "boomerang/ssl/statements/Assignment.h"
#include "boomerang/util/MapIterators.h"


/**
 * PhiAssign is a subclass of Assignment, having a left hand side, and a StatementVec with the
 * references. For more information about phi functions, see
 * https://en.wikipedia.org/wiki/Static_single_assignment_form.
 *
 * \code
 * m[1000] := phi{3 7 10}    // m[1000] is defined at statements 3, 7, and 10
 * m[r28{3}+4] := phi{2 8}   // the memof is defined at 2 and 8, and the r28 is defined at 3.
 * \endcode
 * The integers are really pointers to statements, printed as the statement number for compactness
 *
 * \note Although the left hand side is nearly always redundant, it is essential in at least one
 * circumstance: when finding locations used by some statement, and the reference is to a
 * CallStatement returning multiple locations.
 */
class BOOMERANG_API PhiAssign : public Assignment
{
public:
    typedef std::map<BasicBlock *, std::shared_ptr<RefExp>, BasicBlock::BBComparator> PhiDefs;
    typedef MapValueIterator<PhiDefs> iterator;
    typedef MapValueConstIterator<PhiDefs> const_iterator;
    typedef MapValueReverseIterator<PhiDefs> reverse_iterator;
    typedef MapValueConstReverseIterator<PhiDefs> const_reverse_iterator;

public:
    PhiAssign(SharedExp _lhs)
        : Assignment(_lhs)
        , m_defs()
    {
        m_kind = StmtType::PhiAssign;
    }

    PhiAssign(SharedType ty, SharedExp _lhs)
        : Assignment(ty, _lhs)
        , m_defs()
    {
        m_kind = StmtType::PhiAssign;
    }

    PhiAssign(const PhiAssign &other) = delete;
    PhiAssign(PhiAssign &&other)      = delete;

    virtual ~PhiAssign() override {}

    PhiAssign &operator=(const PhiAssign &other) = delete;
    PhiAssign &operator=(PhiAssign &&other) = delete;

public:
    iterator begin() { return m_defs.begin(); }
    iterator end() { return m_defs.end(); }
    const_iterator begin() const { return m_defs.begin(); }
    const_iterator end() const { return m_defs.end(); }

    reverse_iterator rbegin() { return m_defs.rbegin(); }
    reverse_iterator rend() { return m_defs.rend(); }
    const_reverse_iterator rbegin() const { return m_defs.rbegin(); }
    const_reverse_iterator rend() const { return m_defs.rend(); }

public:
    /// \copydoc Statement::clone
    virtual Statement *clone() const override;

    /// \copydoc Statement::getRight
    virtual SharedExp getRight() const override { return nullptr; }

    /// \copydoc Statement::accept
    virtual bool accept(StmtVisitor *visitor) const override;

    /// \copydoc Statement::accept
    virtual bool accept(StmtExpVisitor *visitor) override;

    /// \copydoc Statement::accept
    virtual bool accept(StmtModifier *modifier) override;

    /// \copydoc Statement::accept
    virtual bool accept(StmtPartModifier *modifier) override;

    /// \copydoc Assignment::printCompact
    virtual void printCompact(OStream &os) const override;

    /// \copydoc Statement::search
    virtual bool search(const Exp &search, SharedExp &result) const override;

    /// \copydoc Statement::searchAll
    virtual bool searchAll(const Exp &search, std::list<SharedExp> &result) const override;

    /// \copydoc Statement::searchAndReplace
    virtual bool searchAndReplace(const Exp &search, SharedExp replace, bool cc = false) override;

    /// \copydoc Statement::simplify
    virtual void simplify() override;

    //
    //    Phi specific functions
    //

    /// Get statement at index \p idx
    Statement *getStmtAt(BasicBlock *bb);
    const Statement *getStmtAt(BasicBlock *bb) const;

    /// Update the statement at index \p idx
    void putAt(BasicBlock *idx, Statement *d, SharedExp e);

    size_t getNumDefs() const { return m_defs.size(); }
    PhiDefs &getDefs() { return m_defs; }
    const PhiDefs &getDefs() const { return m_defs; }

    void removeAllReferences(const std::shared_ptr<RefExp> &ref);

private:
    PhiDefs m_defs; ///< A vector of information about definitions
};
