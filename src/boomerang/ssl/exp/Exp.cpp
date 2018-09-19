#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "Exp.h"

#include "boomerang/db/Prog.h"
#include "boomerang/db/proc/Proc.h"
#include "boomerang/db/proc/ProcCFG.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/ssl/RTL.h"
#include "boomerang/ssl/Register.h"
#include "boomerang/ssl/exp/Binary.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/exp/RefExp.h"
#include "boomerang/ssl/exp/Terminal.h"
#include "boomerang/ssl/exp/Ternary.h"
#include "boomerang/ssl/exp/TypedExp.h"
#include "boomerang/ssl/exp/Unary.h"
#include "boomerang/ssl/type/ArrayType.h"
#include "boomerang/ssl/type/PointerType.h"
#include "boomerang/util/ExpPrinter.h"
#include "boomerang/util/LocationSet.h"
#include "boomerang/util/Types.h"
#include "boomerang/util/Util.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/visitor/expmodifier/CallBypasser.h"
#include "boomerang/visitor/expmodifier/ExpAddressSimplifier.h"
#include "boomerang/visitor/expmodifier/ExpArithSimplifier.h"
#include "boomerang/visitor/expmodifier/ExpPropagator.h"
#include "boomerang/visitor/expmodifier/ExpSSAXformer.h"
#include "boomerang/visitor/expmodifier/ExpSimplifier.h"
#include "boomerang/visitor/expmodifier/ExpSubscripter.h"
#include "boomerang/visitor/expmodifier/SizeStripper.h"
#include "boomerang/visitor/expvisitor/BadMemofFinder.h"
#include "boomerang/visitor/expvisitor/ComplexityFinder.h"
#include "boomerang/visitor/expvisitor/FlagsFinder.h"
#include "boomerang/visitor/expvisitor/MemDepthFinder.h"
#include "boomerang/visitor/expvisitor/UsedLocsFinder.h"

#include <QRegularExpression>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>


// This to satisfy the compiler (never gets called!)
SharedExp _dummy;
SharedExp &Exp::refSubExp1()
{
    assert(false);
    return _dummy;
}


SharedExp &Exp::refSubExp2()
{
    assert(false);
    return _dummy;
}


SharedExp &Exp::refSubExp3()
{
    assert(false);
    return _dummy;
}


bool Exp::isRegOfConst() const
{
    return isRegOf() && getSubExp1()->isIntConst();
}


bool Exp::isRegN(int N) const
{
    return isRegOfConst() && access<Const, 1>()->getInt() == N;
}


bool Exp::isAfpTerm()
{
    auto cur = shared_from_this();

    if (m_oper == opTypedExp) {
        cur = getSubExp1();
    }

    if (cur->getOper() == opAddrOf) {
        SharedExp p = cur->getSubExp1();

        if (p && (p->getOper() == opMemOf)) {
            cur = p->getSubExp1();
        }
    }

    OPER curOp = cur->getOper();

    if (curOp == opAFP) {
        return true;
    }

    if ((curOp != opPlus) && (curOp != opMinus)) {
        return false;
    }

    // cur must be a Binary* now
    OPER subOp1 = cur->getSubExp1()->getOper();
    OPER subOp2 = cur->getSubExp2()->getOper();
    return ((subOp1 == opAFP) && (subOp2 == opIntConst));
}


SharedExp Exp::getGuard()
{
    if (m_oper == opGuard) {
        return getSubExp1();
    }

    return nullptr;
}


void Exp::doSearch(const Exp &pattern, SharedExp &toSearch, std::list<SharedExp *> &matches,
                   bool once)
{
    bool compare;

    compare = (pattern == *toSearch);

    if (compare) {
        matches.push_back(&toSearch); // Success

        if (once) {
            return; // No more to do
        }
    }

    // Either want to find all occurrences, or did not match at this level
    // Recurse into children, unless a matching opSubscript
    if (!compare || (toSearch->m_oper != opSubscript)) {
        toSearch->doSearchChildren(pattern, matches, once);
    }
}


void Exp::doSearchChildren(const Exp &pattern, std::list<SharedExp *> &matches, bool once)
{
    Q_UNUSED(pattern);
    Q_UNUSED(matches);
    Q_UNUSED(once);
    // Const and Terminal do not override this
}


SharedExp Exp::searchReplace(const Exp &pattern, const SharedExp &replace, bool &change)
{
    return searchReplaceAll(pattern, replace, change, true);
}


SharedExp Exp::searchReplaceAll(const Exp &pattern, const SharedExp &replace, bool &change,
                                bool once /* = false */)
{
    // TODO: consider working on base object, and only in case when we find the search, use clone
    // call to return the new object ?
    if (*this == pattern) {
        change = true;
        return replace->clone();
    }

    std::list<SharedExp *> matches;
    SharedExp top = shared_from_this(); // top may change; that's why we have to return it
    doSearch(pattern, top, matches, false);

    for (SharedExp *pexp : matches) {
        *pexp = replace->clone(); // Do the replacement

        if (once) {
            change = true;
            return top;
        }
    }

    change = !matches.empty();
    return top;
}


bool Exp::search(const Exp &pattern, SharedExp &result)
{
    std::list<SharedExp *> li;
    result = nullptr; // In case it fails; don't leave it unassigned
    // The search requires a reference to a pointer to this object.
    // This isn't needed for searches, only for replacements, but we want to re-use the same search
    // routine
    SharedExp top = shared_from_this();
    doSearch(pattern, top, li, false);

    if (!li.empty()) {
        result = *li.front();
        return true;
    }

    return false;
}


bool Exp::searchAll(const Exp &pattern, std::list<SharedExp> &result)
{
    std::list<SharedExp *> matches;

    // The search requires a reference to a pointer to this object.
    // This isn't needed for searches, only for replacements,
    // but we want to re-use the same search routine
    SharedExp toSearch = shared_from_this();
    doSearch(pattern, toSearch, matches, false);

    for (auto it : matches) {
        result.push_back(*it);
    }

    return !matches.empty();
}


void Exp::partitionTerms(std::list<SharedExp> &positives, std::list<SharedExp> &negatives,
                         std::vector<int> &integers, bool negate)
{
    SharedExp p1, p2;

    switch (m_oper) {
    case opPlus:
        p1 = getSubExp1();
        p2 = getSubExp2();
        p1->partitionTerms(positives, negatives, integers, negate);
        p2->partitionTerms(positives, negatives, integers, negate);
        break;

    case opMinus:
        p1 = getSubExp1();
        p2 = getSubExp2();
        p1->partitionTerms(positives, negatives, integers, negate);
        p2->partitionTerms(positives, negatives, integers, !negate);
        break;

    case opTypedExp:
        p1 = getSubExp1();
        p1->partitionTerms(positives, negatives, integers, negate);
        break;

    case opIntConst: {
        int k = access<Const>()->getInt();

        if (negate) {
            integers.push_back(-k);
        }
        else {
            integers.push_back(k);
        }

        break;
    }

    default:

        // These can be any other expression tree
        if (negate) {
            negatives.push_back(shared_from_this());
        }
        else {
            positives.push_back(shared_from_this());
        }
    }
}


SharedExp Exp::accumulate(std::list<SharedExp> &exprs)
{
    if (exprs.empty()) {
        return Const::get(0);
    }

    if (exprs.size() == 1) {
        return exprs.front()->clone();
    }

    std::list<SharedExp> cloned_list;

    for (const SharedExp &v : exprs) {
        cloned_list.push_back(v->clone());
    }

    SharedExp last_val = cloned_list.back();
    cloned_list.pop_back();
    auto res = Binary::get(opPlus, cloned_list.back(), last_val);
    cloned_list.pop_back();

    while (!cloned_list.empty()) {
        res = Binary::get(opPlus, cloned_list.back(), res);
        cloned_list.pop_back();
    }

    return res;
}


SharedExp Exp::simplify()
{
#if DEBUG_SIMP
    SharedExp save = clone();
#endif
    bool changed  = false; // True if simplified at this or lower level
    SharedExp res = shared_from_this();

    do {
        ExpSimplifier es;
        res     = res->acceptModifier(&es);
        changed = es.isModified();
    } while (changed); // If modified at this (or a lower) level, redo

    // The below is still important. E.g. want to canonicalise sums, so we know that a + K + b is
    // the same as a + b + K No! This slows everything down, and it's slow enough as it is. Call
    // only where needed:
    //    res = res->simplifyArith();

#if DEBUG_SIMP
    if (!(*res == *save)) {
        LOG_MSG("Simplified %1 to %2", save, res);
    }
#endif
    return res;
}


SharedExp Exp::simplifyAddr()
{
    ExpAddressSimplifier eas;
    return this->acceptModifier(&eas);
}


SharedExp Exp::simplifyArith()
{
    ExpArithSimplifier eas;
    return this->acceptModifier(&eas);
}


const char *Exp::getOperName() const
{
    return operToString(m_oper);
}


QString Exp::toString() const
{
    QString res;
    OStream os(&res);
    os << shared_from_this();
    return res;
}


void Exp::print(OStream &os) const
{
    ExpPrinter().print(os, shared_from_this());
}


OStream &operator<<(OStream &os, const SharedConstExp &exp)
{
    ExpPrinter().print(os, exp);
    return os;
}


SharedType Exp::ascendType()
{
    assert(false);
    return nullptr;
}


void Exp::descendType(SharedType, bool &, Statement *)
{
    assert(false);
}


SharedExp Exp::fixSuccessor()
{
    SharedExp result;
    UniqExp search_expression(new Unary(opSuccessor, Location::regOf(Terminal::get(opWild))));

    // Assume only one successor function in any 1 expression
    if (search(*search_expression, result)) {
        // Result has the matching expression, i.e. succ(r[K])
        SharedExp sub1 = result->getSubExp1();
        assert(sub1->getOper() == opRegOf);
        SharedExp sub2 = sub1->getSubExp1();
        assert(sub2->getOper() == opIntConst);
        // result     sub1    sub2
        // succ(      r[   Const K    ])
        // Note: we need to clone the r[K] part, since it will be deleted as
        // part of the searchReplace below
        auto replace = sub1->clone();
        auto c       = replace->access<Const, 1>();
        c->setInt(c->getInt() + 1); // Do the increment
        bool change   = false;
        SharedExp res = searchReplace(*result, replace, change);
        return res;
    }

    return shared_from_this();
}


bool Exp::isTemp() const
{
    if (m_oper == opTemp) {
        return true;
    }

    if (m_oper != opRegOf) {
        return false;
    }

    // Some old code has r[tmpb] instead of just tmpb
    SharedConstExp sub = getSubExp1();
    return sub->m_oper == opTemp;
}


SharedExp Exp::removeSubscripts(bool &allZero)
{
    auto e = shared_from_this();
    LocationSet locs;

    e->addUsedLocs(locs);
    allZero = true;

    for (const SharedExp &loc : locs) {
        if (loc->getOper() == opSubscript) {
            auto r1              = std::static_pointer_cast<RefExp>(loc);
            const Statement *def = r1->getDef();

            if (!((def == nullptr) || (def->getNumber() == 0))) {
                allZero = false;
            }

            bool change;
            e = e->searchReplaceAll(*loc, r1->getSubExp1() /*->clone()*/,
                                    change); // TODO: what happens when clone is restored here ?
        }
    }

    return e;
}


SharedExp Exp::fromSSAleft(UserProc *proc, Statement *def)
{
    auto r = RefExp::get(shared_from_this(), def); // "Wrap" in a ref

    ExpSSAXformer xformer(proc);
    SharedExp result = r->acceptModifier(&xformer);
    return result;
}


SharedExp Exp::stripSizes()
{
    SizeStripper ss;
    return this->acceptModifier(&ss);
}


void Exp::addUsedLocs(LocationSet &used, bool memOnly)
{
    UsedLocsFinder ulf(used, memOnly);

    acceptVisitor(&ulf);
}


SharedExp Exp::expSubscriptVar(const SharedExp &e, Statement *def)
{
    ExpSubscripter es(e, def);

    return acceptModifier(&es);
}


SharedExp Exp::expSubscriptValNull(const SharedExp &e)
{
    return expSubscriptVar(e, nullptr);
}


SharedExp Exp::expSubscriptAllNull()
{
    return expSubscriptVar(Terminal::get(opWild), nullptr);
}


SharedExp Exp::bypass()
{
    CallBypasser cb(nullptr);

    return acceptModifier(&cb);
}


int Exp::getComplexityDepth(UserProc *proc)
{
    ComplexityFinder cf(proc);

    acceptVisitor(&cf);
    return cf.getDepth();
}


int Exp::getMemDepth()
{
    MemDepthFinder mdf;

    acceptVisitor(&mdf);
    return mdf.getDepth();
}


SharedExp Exp::propagateAll()
{
    ExpPropagator ep;

    return acceptModifier(&ep);
}


SharedExp Exp::propagateAllRpt(bool &changed)
{
    changed       = false;
    SharedExp ret = shared_from_this();

    while (true) {
        ExpPropagator ep;
        ret = ret->acceptModifier(&ep);

        if (ep.isChanged()) {
            changed = true;
        }
        else {
            break;
        }
    }

    return ret;
}


bool Exp::containsFlags()
{
    FlagsFinder ff;

    acceptVisitor(&ff);
    return ff.isFound();
}


bool Exp::containsBadMemof()
{
    BadMemofFinder bmf;

    acceptVisitor(&bmf);
    return bmf.isFound();
}


SharedExp Exp::acceptModifier(ExpModifier *mod)
{
    bool visitChildren = true;
    SharedExp ret      = acceptPreModifier(mod, visitChildren);

    if (visitChildren) {
        this->acceptChildModifier(mod);
    }

    return ret->acceptPostModifier(mod);
}
