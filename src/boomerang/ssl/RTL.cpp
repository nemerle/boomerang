#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "RTL.h"

#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Operator.h"
#include "boomerang/ssl/statements/Assign.h"
#include "boomerang/ssl/statements/BranchStatement.h"
#include "boomerang/util/Types.h"
#include "boomerang/util/Util.h"
#include "boomerang/util/log/Log.h"

#include <QTextStreamManipulator>
#include <QtAlgorithms>

#include <cassert>
#include <cstdio>
#include <cstring>


RTL::RTL(Address instrAddr, const std::list<Statement *> *listStmt /*= nullptr*/)
    : m_nativeAddr(instrAddr)
{
    if (listStmt) {
        m_stmts = *listStmt;
    }
}


RTL::RTL(Address instrAddr, const std::initializer_list<Statement *> &statements)
    : m_stmts(statements)
    , m_nativeAddr(instrAddr)
{
}


RTL::RTL(const RTL &other)
    : m_nativeAddr(other.m_nativeAddr)
{
    append(other.m_stmts);
}


RTL::~RTL()
{
    qDeleteAll(m_stmts);
}


RTL &RTL::operator=(const RTL &other)
{
    if (this == &other) {
        return *this;
    }

    m_nativeAddr = other.m_nativeAddr;

    // Do a deep copy always
    qDeleteAll(*this);
    clear();

    other.deepCopyList(m_stmts);
    return *this;
}


void RTL::deepCopyList(std::list<Statement *> &dest) const
{
    for (const Statement *it : *this) {
        dest.push_back(it->clone());
    }
}


void RTL::append(Statement *s)
{
    assert(s != nullptr);

    if (!empty() && back()->isFlagAssign()) {
        insert(std::prev(end()), s);
        return;
    }

    m_stmts.push_back(s);
}


void RTL::append(const std::list<Statement *> &stmts)
{
    for (Statement *stmt : stmts) {
        m_stmts.push_back(stmt->clone());
    }
}


void RTL::print(OStream &os) const
{
    // print out the instruction address of this RTL
    os << m_nativeAddr;

    // Print the statements
    // First line has 8 extra chars as above
    bool firstStmt = true;

    for (Statement *stmt : *this) {
        if (firstStmt) {
            os << " ";
        }
        else {
            os << qSetFieldWidth(11) << " " << qSetFieldWidth(0);
        }

        if (stmt) {
            stmt->print(os);
        }

        os << "\n";
        firstStmt = false;
    }

    if (empty()) {
        os << '\n'; // New line for NOP
    }
}


QString RTL::toString() const
{
    QString tgt;
    OStream ost(&tgt);
    print(ost);
    return tgt;
}


void RTL::simplify()
{
    for (iterator it = begin(); it != end();) {
        (*it)->simplify();
        Statement *s = *it;

        if (s->isBranch()) {
            SharedExp cond = static_cast<BranchStatement *>(s)->getCondExpr();

            if (cond && cond->isIntConst()) {
                if (cond->access<Const>()->getInt() == 0) {
                    LOG_VERBOSE("Removing branch with false condition at %1 %2", getAddress(), *it);
                    it = this->erase(it);
                    continue;
                }

                LOG_VERBOSE("Replacing branch with true condition with goto at %1 %2", getAddress(),
                            *it);
                BasicBlock *bb = (*it)->getBB();
                *it = new GotoStatement(static_cast<BranchStatement *>(s)->getFixedDest());
                (*it)->setBB(bb);
            }
        }
        else if (s->isAssign()) {
            SharedExp guard = static_cast<Assign *>(s)->getGuard();

            if (guard && guard->isFalse()) {
                // This assignment statement can be deleted
                LOG_VERBOSE("Removing assignment with false guard at %1 %2", getAddress(), *it);
                it = erase(it);
                continue;
            }
            else if (guard && guard->isTrue()) {
                // The guard can be deleted
                static_cast<Assign *>(s)->setGuard(nullptr);
            }
        }

        ++it;
    }
}


bool RTL::isCall() const
{
    if (empty()) {
        return false;
    }

    Statement *last = this->back();
    return last->getKind() == StmtType::Call;
}


Statement *RTL::getHlStmt() const
{
    for (auto rit = rbegin(); rit != rend(); ++rit) {
        if ((*rit)->getKind() != StmtType::Assign) {
            return *rit;
        }
    }

    return nullptr;
}


void RTL::insert(RTL::iterator where, const RTL::value_type &val)
{
    m_stmts.insert(where, val);
}
