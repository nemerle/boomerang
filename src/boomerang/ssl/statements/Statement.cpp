#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "Statement.h"

#include "boomerang/core/Project.h"
#include "boomerang/core/Settings.h"
#include "boomerang/db/BasicBlock.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/proc/ProcCFG.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/ifc/ICodeGenerator.h"
#include "boomerang/ssl/RTL.h"
#include "boomerang/ssl/exp/Binary.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/exp/RefExp.h"
#include "boomerang/ssl/exp/Terminal.h"
#include "boomerang/ssl/statements/BoolAssign.h"
#include "boomerang/ssl/statements/BranchStatement.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/CaseStatement.h"
#include "boomerang/ssl/statements/ImpRefStatement.h"
#include "boomerang/ssl/statements/ImplicitAssign.h"
#include "boomerang/ssl/statements/PhiAssign.h"
#include "boomerang/util/Util.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/visitor/expmodifier/CallBypasser.h"
#include "boomerang/visitor/expmodifier/DFALocalMapper.h"
#include "boomerang/visitor/expmodifier/ExpCastInserter.h"
#include "boomerang/visitor/expmodifier/ExpSSAXformer.h"
#include "boomerang/visitor/expmodifier/ExpSubscripter.h"
#include "boomerang/visitor/expmodifier/SizeStripper.h"
#include "boomerang/visitor/expvisitor/ConstFinder.h"
#include "boomerang/visitor/expvisitor/ExpRegMapper.h"
#include "boomerang/visitor/expvisitor/UsedLocalFinder.h"
#include "boomerang/visitor/expvisitor/UsedLocsFinder.h"
#include "boomerang/visitor/stmtexpvisitor/StmtConstFinder.h"
#include "boomerang/visitor/stmtexpvisitor/StmtRegMapper.h"
#include "boomerang/visitor/stmtexpvisitor/UsedLocsVisitor.h"
#include "boomerang/visitor/stmtmodifier/StmtModifier.h"
#include "boomerang/visitor/stmtmodifier/StmtPartModifier.h"
#include "boomerang/visitor/stmtmodifier/StmtSSAXFormer.h"
#include "boomerang/visitor/stmtmodifier/StmtSubscripter.h"
#include "boomerang/visitor/stmtvisitor/StmtCastInserter.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <sstream>


Statement::Statement()
    : m_bb(nullptr)
    , m_proc(nullptr)
    , m_number(0)
{
}


void Statement::setProc(UserProc *proc)
{
    m_proc = proc;

    const bool assumeABICompliance = (proc && proc->getProg())
                                         ? proc->getProg()->getProject()->getSettings()->assumeABI
                                         : false;
    LocationSet exps, defs;
    addUsedLocs(exps);
    getDefinitions(defs, assumeABICompliance);
    exps.makeUnion(defs);

    for (SharedExp exp : exps) {
        if (exp->isLocation()) {
            exp->access<Location>()->setProc(proc);
        }
    }
}


bool Statement::calcMayAlias(SharedExp e1, SharedExp e2, int size) const
{
    // currently only considers memory aliasing..
    if (!e1->isMemOf() || !e2->isMemOf()) {
        return false;
    }

    SharedExp e1a = e1->getSubExp1();
    SharedExp e2a = e2->getSubExp1();

    // constant memory accesses
    if (e1a->isIntConst() && e2a->isIntConst()) {
        Address a1     = e1a->access<Const>()->getAddr();
        Address a2     = e2a->access<Const>()->getAddr();
        ptrdiff_t diff = (a1 - a2).value();

        if (diff < 0) {
            diff = -diff;
        }

        if (diff * 8 >= size) {
            return false;
        }
    }

    // same left op constant memory accesses
    if ((e1a->getArity() == 2) && (e1a->getOper() == e2a->getOper()) &&
        e1a->getSubExp2()->isIntConst() && e2a->getSubExp2()->isIntConst() &&
        (*e1a->getSubExp1() == *e2a->getSubExp1())) {
        int i1   = e1a->access<Const, 2>()->getInt();
        int i2   = e2a->access<Const, 2>()->getInt();
        int diff = i1 - i2;

        if (diff < 0) {
            diff = -diff;
        }

        if (diff * 8 >= size) {
            return false;
        }
    }

    // [left] vs [left +/- constant] memory accesses
    if (((e2a->getOper() == opPlus) || (e2a->getOper() == opMinus)) &&
        (*e1a == *e2a->getSubExp1()) && e2a->getSubExp2()->isIntConst()) {
        int i1   = 0;
        int i2   = e2a->access<Const, 2>()->getInt();
        int diff = i1 - i2;

        if (diff < 0) {
            diff = -diff;
        }

        if (diff * 8 >= size) {
            return false;
        }
    }

    // Don't need [left +/- constant ] vs [left] because called twice with
    // args reversed
    return true;
}


OStream &operator<<(OStream &os, const Statement *s)
{
    if (s == nullptr) {
        os << "nullptr ";
        return os;
    }

    s->print(os);
    return os;
}


bool Statement::isFlagAssign() const
{
    if (m_kind != StmtType::Assign) {
        return false;
    }

    const OPER op = static_cast<const Assign *>(this)->getRight()->getOper();
    return op == opFlagCall;
}


QString Statement::prints() const
{
    QString tgt;
    OStream ost(&tgt);
    print(ost);
    return tgt;
}


bool Statement::canPropagateToExp(const Exp &exp)
{
    if (!exp.isSubscript()) {
        return false;
    }

    const RefExp &ref = static_cast<const RefExp &>(exp);

    if (ref.isImplicitDef()) {
        // Can't propagate statement "-" or "0" (implicit assignments)
        return false;
    }

    const Statement *def = ref.getDef();

    //    if (def == this)
    // Don't propagate to self! Can happen with %pc's (?!)
    //        return false;
    if (def->isNullStatement()) {
        // Don't propagate a null statement! Can happen with %pc's (would have no effect, and would
        // infinitely loop)
        return false;
    }

    if (!def->isAssign()) {
        return false; // Only propagate ordinary assignments (so far)
    }

    const Assign *adef = static_cast<const Assign *>(def);

    if (adef->getType()->isArray()) {
        // Assigning to an array, don't propagate (Could be alias problems?)
        return false;
    }

    return true;
}


bool Statement::propagateTo(bool &convert, Settings *settings,
                            std::map<SharedExp, int, lessExpStar> *destCounts,
                            LocationSet *usedByDomPhi, bool force)
{
    bool change            = false;
    int changes            = 0;
    const int propMaxDepth = settings->propMaxDepth;

    do {
        LocationSet exps;
        // addUsedLocs(..,true) -> true to also add uses from collectors. For example, want to
        // propagate into the reaching definitions of calls. Third parameter defaults to false, to
        // find all locations, not just those inside m[...]
        addUsedLocs(exps, true);
        change = false; // True if changed this iteration of the do/while loop

        // Example: m[r24{10}] := r25{20} + m[r26{30}]
        // exps has r24{10}, r25{30}, m[r26{30}], r26{30}
        for (SharedExp e : exps) {
            if (!canPropagateToExp(*e)) {
                continue;
            }

            assert(dynamic_cast<Assignment *>(e->access<RefExp>()->getDef()) != nullptr);
            Assignment *def = static_cast<Assignment *>(e->access<RefExp>()->getDef());
            SharedExp rhs   = def->getRight();

            // If force is true, ignore the fact that a memof should not be propagated (for switch
            // analysis)
            if (rhs->containsBadMemof() && !(force && rhs->isMemOf())) {
                // Must never propagate unsubscripted memofs, or memofs that don't yet have symbols.
                // You could be propagating past a definition, thereby invalidating the IR
                continue;
            }

            SharedExp lhs = def->getLeft();

            if (settings->experimental) {
                // This is Mike's experimental propagation limiting heuristic. At present, it is:
                // for each component of def->rhs
                //   test if the base expression is in the set usedByDomPhi
                //     if so, check if this statement OW overwrites a parameter (like ebx = ebx-1)
                //     if so, check for propagating past this overwriting statement, i.e.
                //        domNum(def) <= domNum(OW) && dimNum(OW) < domNum(def)
                //        if so, don't propagate (heuristic takes effect)
                if (usedByDomPhi) {
                    LocationSet rhsComps;
                    rhs->addUsedLocs(rhsComps);
                    LocationSet::iterator rcit;

                    for (rcit = rhsComps.begin(); rcit != rhsComps.end(); ++rcit) {
                        if (!(*rcit)->isSubscript()) {
                            continue; // Sometimes %pc sneaks in
                        }

                        SharedExp rhsBase = (*rcit)->getSubExp1();
                        // We don't know the statement number for the one definition in usedInDomPhi
                        // that might exist, so we use findNS()
                        SharedExp OW = usedByDomPhi->findNS(rhsBase);

                        if (OW) {
                            Statement *OWdef = OW->access<RefExp>()->getDef();

                            if (!OWdef->isAssign()) {
                                continue;
                            }

                            SharedExp lhsOWdef = static_cast<Assign *>(OWdef)->getLeft();
                            LocationSet OWcomps;
                            def->addUsedLocs(OWcomps);

                            bool isOverwrite = false;

                            for (const SharedExp &loc : OWcomps) {
                                if (*loc *= *lhsOWdef) {
                                    isOverwrite = true;
                                    break;
                                }
                            }

                            if (isOverwrite) {
                                break;
                            }

                            if (OW != nullptr) {
                                LOG_MSG("OW is %1", OW);
                            }
                        }
                    }
                }
            }


            // Check if the -l flag (propMaxDepth) prevents this propagation,
            // but always propagate to %flags
            if (!destCounts || lhs->isFlags() || def->getRight()->containsFlags()) {
                change |= doPropagateTo(e, def, convert, settings);
            }
            else {
                std::map<SharedExp, int, lessExpStar>::iterator ff = destCounts->find(e);

                if (ff == destCounts->end()) {
                    change |= doPropagateTo(e, def, convert, settings);
                }
                else if (ff->second <= 1) {
                    change |= doPropagateTo(e, def, convert, settings);
                }
                else if (rhs->getComplexityDepth(m_proc) < propMaxDepth) {
                    change |= doPropagateTo(e, def, convert, settings);
                }
            }
        }
    } while (change && ++changes < 10);

    // Simplify is very costly, especially for calls.
    // I hope that doing one simplify at the end will not affect any
    // result...
    simplify();

    // Note: change is only for the last time around the do/while loop
    return changes > 0;
}


bool Statement::propagateFlagsTo(Settings *settings)
{
    bool change  = false;
    bool convert = false;
    int changes  = 0;

    do {
        LocationSet exps;
        addUsedLocs(exps, true);

        for (SharedExp e : exps) {
            if (!e->isSubscript()) {
                continue; // e.g. %pc
            }

            Assignment *def = dynamic_cast<Assignment *>(e->access<RefExp>()->getDef());

            if ((def == nullptr) ||
                (nullptr == def->getRight())) { // process if it has definition with rhs
                continue;
            }

            SharedExp base = e->access<Exp, 1>(); // Either RefExp or Location ?

            if (base->isFlags() || base->isMainFlag()) {
                change |= doPropagateTo(e, def, convert, settings);
            }
        }
    } while (change && ++changes < 10);

    simplify();
    return change;
}


void Statement::setTypeFor(SharedExp, SharedType)
{
    assert(false);
}


bool Statement::doPropagateTo(const SharedExp &e, Assignment *def, bool &convert,
                              Settings *settings)
{
    // Respect the -p N switch
    if (settings->numToPropagate >= 0) {
        if (settings->numToPropagate == 0) {
            return false;
        }

        settings->numToPropagate--;
    }

    LOG_VERBOSE2("Propagating %1 into %2", def, this);

    bool change = replaceRef(e, def, convert);

    LOG_VERBOSE2("    result %1", this);

    return change;
}


bool Statement::replaceRef(SharedExp e, Assignment *def, bool &convert)
{
    SharedExp rhs = def->getRight();

    assert(rhs);

    SharedExp base = e->getSubExp1();
    // Could be propagating %flags into %CF
    SharedExp lhs = def->getLeft();

    if ((base->getOper() == opCF) && lhs->isFlags()) {
        if (!rhs->isFlagCall()) {
            return false;
        }

        QString str = rhs->access<Const, 1>()->getStr();

        // FIXME: check SUBFLAGSFL handling, and implement it if needed
        if (str.startsWith("SUBFLAGS") && (str != "SUBFLAGSFL")) {
            /* When the carry flag is used bare, and was defined in a subtract of the form lhs -
             * rhs, then CF has the value (lhs <u rhs).  lhs and rhs are the first and second
             * parameters of the flagcall. Note: the flagcall is a binary, with a Const (the name)
             * and a list of expressions: defRhs
             *      /      \
             * Const       opList
             * "SUBFLAGS"    /    \
             *           P1    opList
             *         /     \
             *       P2    opList
             *    /     \
             *  P3     opNil
             */
            SharedExp relExp = Binary::get(opLessUns, rhs->getSubExp2()->getSubExp1(),
                                           rhs->getSubExp2()->getSubExp2()->getSubExp1());
            searchAndReplace(*RefExp::get(Terminal::get(opCF), def), relExp, true);
            return true;
        }
    }

    // need something similar for %ZF
    if ((base->getOper() == opZF) && lhs->isFlags()) {
        if (!rhs->isFlagCall()) {
            return false;
        }

        QString str = rhs->access<Const, 1>()->getStr();

        if (str.startsWith("SUBFLAGS") && (str != "SUBFLAGSFL")) {
            // for zf we're only interested in if the result part of the subflags is equal to zero
            SharedExp relExp = Binary::get(
                opEquals, rhs->getSubExp2()->getSubExp2()->getSubExp2()->getSubExp1(),
                Const::get(0));
            searchAndReplace(*RefExp::get(Terminal::get(opZF), def), relExp, true);
            return true;
        }

        if (str == "SUBFLAGSFL") {
            // for float zf we'll replace the ZF with (P1==P2)
            SharedExp relExp = Binary::get(opEquals, rhs->getSubExp2()->getSubExp1(),
                                           rhs->getSubExp2()->getSubExp2()->getSubExp1());
            searchAndReplace(*RefExp::get(Terminal::get(opZF), def), relExp, true);
            return true;
        }
    }

    // do the replacement
    // bool convert = doReplaceRef(re, rhs);
    bool ret = searchAndReplace(*e, rhs, true); // Last parameter true to change collectors
    // assert(ret);

    if (ret && isCall()) {
        convert |= static_cast<CallStatement *>(this)->convertToDirect();
    }

    return ret;
}


bool Statement::isNullStatement() const
{
    if (m_kind != StmtType::Assign) {
        return false;
    }

    SharedExp right = static_cast<const Assign *>(this)->getRight();

    if (right->isSubscript()) {
        // Must refer to self to be null
        return this == right->access<RefExp>()->getDef();
    }
    else {
        // Null if left == right
        return *static_cast<const Assign *>(this)->getLeft() == *right;
    }
}


bool Statement::isFpush() const
{
    if (m_kind != StmtType::Assign) {
        return false;
    }

    return static_cast<const Assign *>(this)->getRight()->getOper() == opFpush;
}


bool Statement::isFpop() const
{
    if (m_kind != StmtType::Assign) {
        return false;
    }

    return static_cast<const Assign *>(this)->getRight()->getOper() == opFpop;
}


void Statement::stripSizes()
{
    SizeStripper ss;
    StmtModifier sm(&ss);

    accept(&sm);
}


void Statement::bypass()
{
    // Use the Part modifier so we don't change the top level of LHS of assigns etc
    CallBypasser cb(this);
    StmtPartModifier sm(&cb);

    accept(&sm);

    if (cb.isTopChanged()) {
        simplify(); // E.g. m[esp{20}] := blah -> m[esp{-}-20+4] := blah
    }
}


void Statement::addUsedLocs(LocationSet &used, bool cc /* = false */, bool memOnly /*= false */)
{
    UsedLocsFinder ulf(used, memOnly);
    UsedLocsVisitor ulv(&ulf, cc);

    accept(&ulv);
}


bool Statement::addUsedLocals(LocationSet &used)
{
    UsedLocalFinder ulf(used, m_proc);
    UsedLocsVisitor ulv(&ulf, false);

    accept(&ulv);
    return ulf.wasAllFound();
}


void Statement::subscriptVar(SharedExp e, Statement *def /*, ProcCFG* cfg */)
{
    ExpSubscripter es(e, def /*, cfg*/);
    StmtSubscripter ss(&es);

    accept(&ss);
}


void Statement::findConstants(std::list<std::shared_ptr<Const>> &lc)
{
    ConstFinder cf(lc);
    StmtConstFinder scf(&cf);

    accept(&scf);
}


void Statement::mapRegistersToLocals()
{
    ExpRegMapper erm(m_proc);
    StmtRegMapper srm(&erm);

    accept(&srm);
}


void Statement::insertCasts()
{
    // First we postvisit expressions using a StmtModifier and an ExpCastInserter
    ExpCastInserter eci;
    StmtModifier sm(&eci, true); // True to ignore collectors

    accept(&sm);
    // Now handle the LHS of assigns that happen to be m[...], using a StmtCastInserter
    StmtCastInserter sci;
    accept(&sci);
}


void Statement::replaceSubscriptsWithLocals()
{
    ExpSSAXformer esx(m_proc);
    StmtSSAXformer ssx(&esx, m_proc);

    accept(&ssx);
}


void Statement::dfaMapLocals()
{
    DfaLocalMapper dlm(m_proc);
    StmtModifier sm(&dlm, true); // True to ignore def collector in return statement

    accept(&sm);

    if (dlm.change) {
        LOG_VERBOSE2("Statement '%1' mapped with new local(s)", this);
    }
}


SharedType Statement::meetWithFor(const SharedType &ty, const SharedExp &e, bool &changed)
{
    bool thisCh        = false;
    SharedType typeFor = getTypeFor(e);

    assert(typeFor);
    SharedType newType = typeFor->meetWith(ty, thisCh);

    if (thisCh) {
        changed = true;
        setTypeFor(e, newType->clone());
    }

    return newType;
}
