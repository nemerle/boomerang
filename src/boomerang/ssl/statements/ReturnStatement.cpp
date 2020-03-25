#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "ReturnStatement.h"

#include "boomerang/db/BasicBlock.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/ifc/ICodeGenerator.h"
#include "boomerang/ifc/IFrontEnd.h"
#include "boomerang/ssl/exp/RefExp.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/ImplicitAssign.h"
#include "boomerang/visitor/stmtexpvisitor/StmtExpVisitor.h"
#include "boomerang/visitor/stmtmodifier/StmtModifier.h"
#include "boomerang/visitor/stmtmodifier/StmtPartModifier.h"
#include "boomerang/visitor/stmtvisitor/StmtVisitor.h"

#include <QTextStreamManipulator>
#include <QtAlgorithms>


ReturnStatement::ReturnStatement()
    : Statement(StmtType::Ret)
    , m_retAddr(Address::INVALID)
{
}


ReturnStatement::~ReturnStatement()
{
}


ReturnStatement::iterator ReturnStatement::erase(ReturnStatement::iterator it)
{
    assert(it != end());
    return m_returns.erase(it);
}


SharedStmt ReturnStatement::clone() const
{
    std::shared_ptr<ReturnStatement> ret(new ReturnStatement());

    for (auto const &elem : m_modifieds) {
        ret->m_modifieds.append(elem->as<ImplicitAssign>()->clone());
    }

    for (auto const &elem : m_returns) {
        ret->m_returns.append(elem->as<ImplicitAssign>()->clone());
    }

    ret->m_retAddr = m_retAddr;
    ret->m_col.makeCloneOf(m_col);

    // Statement members
    ret->m_fragment = m_fragment;
    ret->m_proc     = m_proc;
    ret->m_number   = m_number;

    return ret;
}


void ReturnStatement::getDefinitions(LocationSet &ls, bool assumeABICompliance) const
{
    for (const SharedStmt &stmt : m_modifieds) {
        stmt->getDefinitions(ls, assumeABICompliance);
    }
}


bool ReturnStatement::definesLoc(SharedExp loc) const
{
    /// Does a ReturnStatement define anything?
    /// Not really, the locations are already defined earlier in the procedure.
    /// However, nothing comes after the return statement, so it doesn't hurt
    /// to pretend it does, and this is a place to store the return type(s) for example.
    /// FIXME: seems it would be cleaner to say that Return Statements don't define anything.

    for (const SharedStmt &stmt : m_modifieds) {
        if (stmt->definesLoc(loc)) {
            return true;
        }
    }

    return false;
}


bool ReturnStatement::search(const Exp &pattern, SharedExp &result) const
{
    result = nullptr;

    for (const SharedStmt &ret : m_returns) {
        if (ret->search(pattern, result)) {
            return true;
        }
    }

    return false;
}


bool ReturnStatement::searchAll(const Exp &pattern, std::list<SharedExp> &result) const
{
    bool found = false;

    for (SharedStmt ret : m_returns) {
        if (ret->searchAll(pattern, result)) {
            found = true;
        }
    }

    return found;
}


bool ReturnStatement::searchAndReplace(const Exp &pattern, SharedExp replace, bool cc)
{
    bool change = false;

    for (SharedStmt ret : m_returns) {
        change |= ret->searchAndReplace(pattern, replace, cc);
    }

    if (cc) {
        for (std::shared_ptr<Assign> def : m_col) {
            change |= def->searchAndReplace(pattern, replace);
        }
    }

    return change;
}


SharedConstType ReturnStatement::getTypeForExp(SharedConstExp e) const
{
    for (const SharedStmt &stmt : m_modifieds) {
        if (*stmt->as<Assignment>()->getLeft() == *e) {
            return stmt->as<Assignment>()->getType();
        }
    }

    return nullptr;
}


SharedType ReturnStatement::getTypeForExp(SharedExp e)
{
    for (const SharedStmt &stmt : m_modifieds) {
        if (*stmt->as<Assignment>()->getLeft() == *e) {
            return stmt->as<Assignment>()->getType();
        }
    }

    return nullptr;
}


void ReturnStatement::setTypeForExp(SharedExp e, SharedType ty)
{
    for (SharedStmt stmt : m_modifieds) {
        if (*stmt->as<Assignment>()->getLeft() == *e) {
            stmt->as<Assignment>()->setType(ty);
            break;
        }
    }

    for (SharedStmt stmt : m_returns) {
        if (*stmt->as<Assignment>()->getLeft() == *e) {
            stmt->as<Assignment>()->setType(ty);
            return;
        }
    }
}


void ReturnStatement::simplify()
{
    for (SharedStmt s : m_modifieds) {
        s->simplify();
    }

    for (SharedStmt s : m_returns) {
        s->simplify();
    }
}


bool ReturnStatement::accept(StmtVisitor *visitor) const
{
    return visitor->visit(this);
}


bool ReturnStatement::accept(StmtExpVisitor *v)
{
    bool visitChildren = true;

    if (!v->visit(shared_from_this()->as<ReturnStatement>(), visitChildren)) {
        return false;
    }
    else if (!visitChildren) {
        return true;
    }

    if (!v->isIgnoreCol()) {
        for (SharedStmt stmt : m_col) {
            if (!stmt->accept(v)) {
                return false;
            }
        }

        // EXPERIMENTAL: for now, count the modifieds as if they are a collector (so most, if not
        // all of the time, ignore them). This is so that we can detect better when a definition is
        // used only once, and therefore propagate anything to it
        for (SharedStmt stmt : m_modifieds) {
            if (!stmt->accept(v)) {
                return false;
            }
        }
    }

    for (SharedStmt stmt : m_returns) {
        if (!stmt->accept(v)) {
            return false;
        }
    }

    return true;
}


bool ReturnStatement::accept(StmtModifier *v)
{
    bool visitChildren = true;
    v->visit(shared_from_this()->as<ReturnStatement>(), visitChildren);

    if (!visitChildren) {
        return true;
    }

    if (!v->ignoreCollector()) {
        DefCollector::iterator dd;

        for (dd = m_col.begin(); dd != m_col.end(); ++dd) {
            if (!(*dd)->accept(v)) {
                return false;
            }
        }
    }

    for (SharedStmt stmt : m_modifieds) {
        if (!stmt->accept(v)) {
            return false;
        }
    }

    for (SharedStmt stmt : m_returns) {
        if (!stmt->accept(v)) {
            return false;
        }
    }

    return true;
}


bool ReturnStatement::accept(StmtPartModifier *v)
{
    bool visitChildren = true;
    v->visit(shared_from_this()->as<ReturnStatement>(), visitChildren);

    for (SharedStmt stmt : m_modifieds) {
        if (!stmt->accept(v)) {
            return false;
        }
    }

    for (SharedStmt stmt : m_returns) {
        if (!stmt->accept(v)) {
            return false;
        }
    }

    return true;
}


void ReturnStatement::print(OStream &os) const
{
    const int RETSTMT_COLS = 120;

    os << qSetFieldWidth(4) << m_number << qSetFieldWidth(0) << " ";
    os << "RET";

    bool first = true;
    int column = 19;

    for (const SharedStmt &stmt : m_returns) {
        QString tgt;
        OStream ost(&tgt);
        stmt->as<const Assignment>()->printCompact(ost);
        const int len = tgt.length();

        if (first) {
            first = false;
            os << " ";
        }
        else if (column + 4 + len > RETSTMT_COLS) { // 4 for command 3 spaces
            if (column != RETSTMT_COLS - 1) {
                os << ","; // Comma at end of line
            }

            os << "\n                ";
            column = 16;
        }
        else {
            os << ",   ";
            column += 4;
        }

        os << tgt;
        column += len;
    }

    os << "\n              ";
    os << "Modifieds: ";

    if (m_modifieds.empty()) {
        os << "<None>";
    }
    else {
        first  = true;
        column = 25;

        for (const SharedStmt &stmt : m_modifieds) {
            QString tgt2;
            OStream ost(&tgt2);
            std::shared_ptr<const Assignment> asgn = stmt->as<const Assignment>();
            const SharedType ty                    = asgn->getType();

            if (ty) {
                ost << "*" << ty << "* ";
            }

            ost << asgn->getLeft();
            unsigned len = tgt2.length();

            if (first) {
                first = false;
            }
            else if (column + 3 + len > RETSTMT_COLS) { // 3 for comma and 2 spaces
                if (column != RETSTMT_COLS - 1) {
                    os << ","; // Comma at end of line
                }

                os << "\n                ";
                column = 16;
            }
            else {
                os << ",  ";
                column += 3;
            }

            os << tgt2;
            column += len;
        }
    }

    // Collected reaching definitions
    os << "\n              ";
    os << "Reaching definitions: ";
    m_col.print(os);
}


void ReturnStatement::updateModifieds()
{
    assert(m_proc != nullptr);
    assert(m_fragment != nullptr);

    StatementList oldMods(m_modifieds); // Copy the old modifieds
    m_modifieds.clear();

    if ((m_fragment->getNumPredecessors() == 1) &&
        m_fragment->getPredecessor(0)->getLastStmt()->isCall()) {
        std::shared_ptr<CallStatement>
            call = m_fragment->getPredecessor(0)->getLastStmt()->as<CallStatement>();

        IFrontEnd *fe = m_proc->getProg()->getFrontEnd();
        if (call->getDestProc() && fe->isNoReturnCallDest(call->getDestProc()->getName())) {
            return;
        }
    }

    // For each location in the collector, make sure that there is an assignment in the old
    // modifieds, which will be filtered and sorted to become the new modifieds Ick... O(N*M)
    // (N existing modifeds, M collected locations)

    for (SharedStmt stmt : m_col) {
        bool found                   = false;
        std::shared_ptr<Assign> asgn = stmt->as<Assign>();
        SharedExp colLhs             = asgn->getLeft();

        if (!m_proc->canBeReturn(colLhs)) {
            continue; // Filtered out
        }

        for (SharedStmt s : oldMods) {
            SharedExp lhs = s->as<Assignment>()->getLeft();

            if (*lhs == *colLhs) {
                found = true;
                break;
            }
        }

        if (!found) {
            std::shared_ptr<ImplicitAssign> ias(
                new ImplicitAssign(asgn->getType()->clone(), asgn->getLeft()->clone()));
            ias->setProc(m_proc); // Comes from the Collector
            ias->setFragment(m_fragment);
            oldMods.append(ias);
        }
    }

    // Mostly the old modifications will be in the correct order,
    // and inserting will be fastest near the start of the
    // new list. So read the old modifications in reverse order
    for (auto rit = oldMods.rbegin(); rit != oldMods.rend(); ++rit) {
        // Make sure the LHS is still in the collector
        std::shared_ptr<Assignment> asgn = (*rit)->as<Assignment>();
        SharedExp lhs                    = asgn->getLeft();

        if (!m_col.hasDefOf(lhs)) {
            continue; // Not in collector: delete it (don't copy it)
        }

        // can happen e.g. when we can prove a function preserves an exp
        // when we could not do this before, e.g. after propagation
        if (!m_proc->canBeReturn(lhs)) {
            continue;
        }

        m_modifieds.append(asgn);
    }

    auto sig = m_proc->getSignature();

    m_modifieds.sort([&sig](const SharedConstStmt &mod1, const SharedConstStmt &mod2) {
        return sig->returnCompare(*mod1->as<const Assignment>(), *mod2->as<const Assignment>());
    });
}


void ReturnStatement::updateReturns()
{
    assert(m_proc != nullptr);

    auto sig     = m_proc->getSignature();
    const int sp = sig->getStackRegister();

    StatementList oldRets(m_returns); // Copy the old returns
    m_returns.clear();

    // For each location in the modifieds, make sure that there is an assignment in the old returns,
    // which will be filtered and sorted to become the new returns
    // Ick... O(N*M) (N existing returns, M modifieds locations)
    for (SharedStmt stmt : m_modifieds) {
        bool found    = false;
        SharedExp loc = stmt->as<Assignment>()->getLeft();

        if (!m_proc->canBeReturn(loc)) {
            continue; // Filtered out
        }

        // Special case for the stack pointer: it has to be a modified (otherwise, the changes will
        // bypass the calls), but it is not wanted as a return
        if (loc->isRegN(sp)) {
            continue;
        }

        for (SharedStmt oldRet : oldRets) {
            SharedExp lhs = oldRet->as<Assign>()->getLeft();

            if (*lhs == *loc) {
                found = true;
                break;
            }
        }

        if (!found) {
            // Find the definition that reaches the return statement's collector
            SharedExp rhs = m_col.findDefFor(loc);
            std::shared_ptr<Assign> asgn(new Assign(loc->clone(), rhs->clone()));
            asgn->setProc(m_proc);
            asgn->setFragment(m_fragment);
            oldRets.append(asgn);
        }
    }

    for (SharedStmt stmt : oldRets) {
        // Make sure the LHS is still in the modifieds
        assert(stmt->isAssign());
        std::shared_ptr<Assign> asgn = stmt->as<Assign>();
        SharedExp lhs                = asgn->getLeft();

        if (!m_modifieds.existsOnLeft(lhs)) {
            continue; // Not in modifieds: delete it (don't copy it)
        }

        if (!m_proc->canBeReturn(lhs)) {
            continue; // Filtered out: delete it
        }

        // Preserveds are NOT returns (nothing changes, so what are we returning?)
        // Check if it is a preserved location, e.g. r29 := r29{-}
        SharedExp rhs = asgn->getRight();

        if (rhs->isSubscript() && rhs->access<RefExp>()->isImplicitDef() &&
            (*rhs->getSubExp1() == *lhs)) {
            continue;
        }

        m_returns.append(asgn);
    }

    m_returns.sort([&sig](const SharedConstStmt &ret1, const SharedConstStmt &ret2) {
        return sig->returnCompare(*ret1->as<const Assignment>(), *ret2->as<const Assignment>());
    });
}


void ReturnStatement::addReturn(const std::shared_ptr<Assignment> &a)
{
    m_returns.append(a);
}


void ReturnStatement::removeFromModifiedsAndReturns(SharedExp loc)
{
    m_modifieds.removeFirstDefOf(loc);
    m_returns.removeFirstDefOf(loc);
}
