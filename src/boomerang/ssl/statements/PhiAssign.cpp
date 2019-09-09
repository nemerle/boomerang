#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "PhiAssign.h"

#include "boomerang/db/BasicBlock.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/ssl/exp/Binary.h"
#include "boomerang/ssl/exp/RefExp.h"
#include "boomerang/ssl/statements/Assign.h"
#include "boomerang/ssl/type/Type.h"
#include "boomerang/util/LocationSet.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/visitor/expmodifier/ExpModifier.h"
#include "boomerang/visitor/expvisitor/ExpVisitor.h"
#include "boomerang/visitor/stmtexpvisitor/StmtExpVisitor.h"
#include "boomerang/visitor/stmtmodifier/StmtModifier.h"
#include "boomerang/visitor/stmtmodifier/StmtPartModifier.h"
#include "boomerang/visitor/stmtvisitor/StmtVisitor.h"


bool BasicBlock::BBComparator::operator()(const BasicBlock *bb1, const BasicBlock *bb2) const
{
    // special case: in test code, we have statements that do not belong to BBs.
    // Thus, bb is nullptr
    if (bb1 && bb2) {
        return bb1->getLowAddr() < bb2->getLowAddr();
    }
    else {
        // compare pointers
        return bb1 < bb2;
    }
}


Statement *PhiAssign::clone() const
{
    PhiAssign *pa = new PhiAssign(m_type, m_lhs);

    for (const auto &[bb, ref] : m_defs) {
        assert(ref->getSubExp1());

        // Clone the expression pointer, but not the statement pointer (never moves)
        pa->m_defs.insert({ bb, RefExp::get(ref->getSubExp1()->clone(), ref->getDef()) });
    }

    return pa;
}


bool PhiAssign::accept(StmtVisitor *visitor) const
{
    return visitor->visit(this);
}


void PhiAssign::printCompact(OStream &os) const
{
    os << "*" << m_type << "* ";

    if (m_lhs) {
        m_lhs->print(os);
    }

    os << " := phi";
    for (const auto &[bb, ref] : m_defs) {
        Q_UNUSED(bb);
        Q_UNUSED(ref);

        assert(ref->getSubExp1() != nullptr);
        assert(*ref->getSubExp1() == *m_lhs);
    }
    os << "{";

    for (auto it = m_defs.begin(); it != m_defs.end(); /* no increment */) {
        if (it->second->getDef()) {
            os << it->second->getDef()->getNumber();
        }
        else {
            os << "-";
        }

        if (++it != m_defs.end()) {
            os << " ";
        }
    }

    os << "}";
}


bool PhiAssign::search(const Exp &pattern, SharedExp &result) const
{
    if (m_lhs->search(pattern, result)) {
        return true;
    }

    for (const std::shared_ptr<RefExp> &ref : *this) {
        assert(ref->getSubExp1() != nullptr);
        // Note: can't match foo{-} because of this
        if (ref->search(pattern, result)) {
            return true;
        }
    }

    return false;
}


bool PhiAssign::searchAll(const Exp &pattern, std::list<SharedExp> &result) const
{
    // FIXME: is this the right semantics for searching a phi statement,
    // disregarding the RHS?
    return m_lhs->searchAll(pattern, result);
}


bool PhiAssign::searchAndReplace(const Exp &pattern, SharedExp replace, bool /*cc*/)
{
    bool change = false;

    m_lhs = m_lhs->searchReplaceAll(pattern, replace, change);

    for (const std::shared_ptr<RefExp> &refExp : *this) {
        assert(refExp->getSubExp1() != nullptr);
        bool ch;

        // Assume that the definitions will also be replaced
        refExp->setSubExp1(refExp->getSubExp1()->searchReplaceAll(pattern, replace, ch));
        assert(refExp->getSubExp1());
        change |= ch;
    }

    return change;
}


bool PhiAssign::accept(StmtExpVisitor *visitor)
{
    bool visitChildren = true;
    if (!visitor->visit(this, visitChildren)) {
        return false;
    }
    else if (!visitChildren) {
        return true;
    }
    else if (m_lhs && !m_lhs->acceptVisitor(visitor->ev)) {
        return false;
    }

    for (const std::shared_ptr<RefExp> &ref : *this) {
        assert(ref->getSubExp1() != nullptr);
        if (!ref->acceptVisitor(visitor->ev)) {
            return false;
        }
    }

    return true;
}


bool PhiAssign::accept(StmtModifier *v)
{
    bool visitChildren;
    v->visit(this, visitChildren);

    if (v->m_mod) {
        v->m_mod->clearModified();

        if (visitChildren) {
            m_lhs = m_lhs->acceptModifier(v->m_mod);
        }

        if (v->m_mod->isModified()) {
            LOG_VERBOSE("PhiAssign changed: now %1", this);
        }
    }

    return true;
}


bool PhiAssign::accept(StmtPartModifier *v)
{
    bool visitChildren;
    v->visit(this, visitChildren);
    v->mod->clearModified();

    if (visitChildren && m_lhs->isMemOf()) {
        m_lhs->setSubExp1(m_lhs->getSubExp1()->acceptModifier(v->mod));
    }

    if (v->mod->isModified()) {
        LOG_VERBOSE("PhiAssign changed: now %1", this);
    }

    return true;
}


void PhiAssign::simplify()
{
    m_lhs = m_lhs->simplify();

    if (m_defs.empty()) {
        return;
    }

    bool allSame        = true;
    Statement *firstDef = (*begin())->getDef();
    UserProc *proc      = this->getProc();

    for (auto &refExp : *this) {
        if (refExp->getDef() != firstDef) {
            allSame = false;
            break;
        }
    }

    if (allSame) {
        LOG_VERBOSE("all the same in %1", this);
        proc->replacePhiByAssign(this, RefExp::get(m_lhs, firstDef));
        return;
    }

    bool onlyOneNotThis = true;
    Statement *notthis  = STMT_WILD;

    for (const std::shared_ptr<RefExp> &ref : *this) {
        Statement *def = ref->getDef();
        if (def == this) {
            continue; // ok
        }
        else if (notthis == STMT_WILD) {
            notthis = def;
        }
        else {
            onlyOneNotThis = false;
            break;
        }
    }

    if (onlyOneNotThis && (notthis != STMT_WILD)) {
        LOG_VERBOSE("All but one not this in %1", this);

        proc->replacePhiByAssign(this, RefExp::get(m_lhs, notthis));
        return;
    }
}


void PhiAssign::putAt(BasicBlock *bb, Statement *def, SharedExp e)
{
    assert(e); // should be something surely

    // Can't use operator[] here since PhiInfo is not default-constructible
    PhiDefs::iterator it = m_defs.find(bb);
    if (it == m_defs.end()) {
        m_defs.insert({ bb, RefExp::get(e, def) });
    }
    else {
        it->second->setDef(def);
        it->second->setSubExp1(e);
    }
}


const Statement *PhiAssign::getStmtAt(BasicBlock *idx) const
{
    PhiDefs::const_iterator it = m_defs.find(idx);
    return (it != m_defs.end()) ? it->second->getDef() : nullptr;
}


Statement *PhiAssign::getStmtAt(BasicBlock *idx)
{
    PhiDefs::iterator it = m_defs.find(idx);
    return (it != m_defs.end()) ? it->second->getDef() : nullptr;
}


void PhiAssign::removeAllReferences(const std::shared_ptr<RefExp> &refExp)
{
    for (PhiDefs::iterator pi = m_defs.begin(); pi != m_defs.end();) {
        std::shared_ptr<RefExp> &p = pi->second;
        assert(p->getSubExp1());

        if (*p == *refExp) {       // Will we ever see this?
            pi = m_defs.erase(pi); // Erase this phi parameter
            continue;
        }

        // Chase the definition
        Statement *def = p->getDef();
        if (def && def->isAssign()) {
            SharedExp rhs = static_cast<Assign *>(def)->getRight();

            if (*rhs == *refExp) {     // Check if RHS is a single reference to this
                pi = m_defs.erase(pi); // Yes, erase this phi parameter
                continue;
            }
        }

        ++pi; // keep it
    }
}
