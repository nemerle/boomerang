/*
 * Copyright (C) 2002-2006, Trent Waddington and Mike Van Emmerik
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 *
 */

/***************************************************************************/ /**
  * \file       statement.cpp
  * \brief   Implementation of the Statement and related classes.
  *               (Was dataflow.cpp a long time ago)
  ******************************************************************************/
#include "statement.h"

#include "exp.h"
#include "cfg.h"
#include "proc.h"
#include "prog.h"
#include "basicblock.h"
#include "boomerang.h"
#include "rtl.h" // For debugging code
#include "util.h"
#include "signature.h"
#include "visitor.h"
#include "dataflow.h"
#include "log.h"
#include "hllcode.h"

#include <cassert>
#include <cstring>
#include <sstream>
#include <cstddef>
#include <algorithm>

extern char debug_buffer[]; // For prints functions
extern QTextStream &alignStream(QTextStream &str,int align);

void Instruction::setProc(UserProc *p) {
    proc = p;
    LocationSet exps;
    addUsedLocs(exps);
    LocationSet defs;
    getDefinitions(defs);
    exps.makeUnion(defs);
    LocationSet::iterator ll;
    for (ll = exps.begin(); ll != exps.end(); ll++) {
        Location *l = dynamic_cast<Location *>(*ll);
        if (l)
            l->setProc(p);
    }
}

bool Instruction::mayAlias(Exp *e1, Exp *e2, int size) {
    if (*e1 == *e2)
        return true;
    // Pass the expressions both ways. Saves checking things like m[exp] vs m[exp+K] and m[exp+K] vs m[exp] explicitly
    // (only need to check one of these cases)
    bool b = (calcMayAlias(e1, e2, size) && calcMayAlias(e2, e1, size));
    if (b && VERBOSE) {
        LOG << "May alias: " << e1 << " and " << e2 << " size " << size << "\n";
    }
    return b;
}

// returns true if e1 may alias e2
bool Instruction::calcMayAlias(Exp *e1, Exp *e2, int size) {
    // currently only considers memory aliasing..
    if (!e1->isMemOf() || !e2->isMemOf()) {
        return false;
    }
    Exp *e1a = e1->getSubExp1();
    Exp *e2a = e2->getSubExp1();
    // constant memory accesses
    if (e1a->isIntConst() && e2a->isIntConst()) {
        ADDRESS a1 = ((Const *)e1a)->getAddr();
        ADDRESS a2 = ((Const *)e2a)->getAddr();
        ptrdiff_t diff = (a1 - a2).m_value;
        if (diff < 0)
            diff = -diff;
        if (diff * 8 >= size)
            return false;
    }
    // same left op constant memory accesses
    if (e1a->getArity() == 2 && e1a->getOper() == e2a->getOper() && e1a->getSubExp2()->isIntConst() &&
        e2a->getSubExp2()->isIntConst() && *e1a->getSubExp1() == *e2a->getSubExp1()) {
        int i1 = ((Const *)e1a->getSubExp2())->getInt();
        int i2 = ((Const *)e2a->getSubExp2())->getInt();
        int diff = i1 - i2;
        if (diff < 0)
            diff = -diff;
        if (diff * 8 >= size)
            return false;
    }
    // [left] vs [left +/- constant] memory accesses
    if ((e2a->getOper() == opPlus || e2a->getOper() == opMinus) && *e1a == *e2a->getSubExp1() &&
        e2a->getSubExp2()->isIntConst()) {
        int i1 = 0;
        int i2 = ((Const *)e2a->getSubExp2())->getInt();
        int diff = i1 - i2;
        if (diff < 0)
            diff = -diff;
        if (diff * 8 >= size)
            return false;
    }
    // Don't need [left +/- constant ] vs [left] because called twice with
    // args reversed
    return true;
}

bool JunctionStatement::isLoopJunction() const {
    for (size_t i = 0; i < Parent->getNumInEdges(); i++)
        if (Parent->isBackEdge(i))
            return true;
    return false;
}

bool Instruction::isFirstStatementInBB() {
    assert(Parent);
    assert(Parent->getRTLs());
    assert(Parent->getRTLs()->size());
    assert(Parent->getRTLs()->front());
    assert(Parent->getRTLs()->front()->size());
    return this == Parent->getRTLs()->front()->front();
}

bool Instruction::isLastStatementInBB() {
    assert(Parent);
    return this == Parent->getLastStmt();
}

Instruction *Instruction::getPreviousStatementInBB() {
    assert(Parent);
    std::list<RTL *> *rtls = Parent->getRTLs();
    assert(rtls);
    Instruction *previous = nullptr;
    for (auto rtl : *rtls) {

        for (Instruction *it : *rtl) {
            if (it == this)
                return previous;
            previous = it;
        }
    }
    return nullptr;
}

Instruction *Instruction::getNextStatementInBB() {
    assert(Parent);
    std::list<RTL *> *rtls = Parent->getRTLs();
    assert(rtls);
    bool wantNext = false;
    for (auto rtl : *rtls) {

        for (Instruction *it : *rtl) {
            if (wantNext)
                return it;
            if (it == this)
                wantNext = true;
        }
    }
    return nullptr;
}

/***************************************************************************/ /**
  * \brief Output operator for Instruction*
  *        Just makes it easier to use e.g. LOG_STREAM() << myStmtStar
  * \param os output stream to send to
  * \param s  ptr to Statement to print to the stream
  * \returns copy of os (for concatenation)
  ******************************************************************************/
QTextStream &operator<<(QTextStream &os, const Instruction *s) {
    if (s == nullptr) {
        os << "nullptr ";
        return os;
    }
    s->print(os);
    return os;
}

bool Instruction::isFlagAssgn() {
    if (Kind != STMT_ASSIGN)
        return false;
    OPER op = ((Assign *)this)->getRight()->getOper();
    return (op == opFlagCall);
}

char *Instruction::prints() {
    QString tgt;
    QTextStream ost(&tgt);
    print(ost);
    strncpy(debug_buffer, qPrintable(tgt), DEBUG_BUFSIZE - 1);
    debug_buffer[DEBUG_BUFSIZE - 1] = '\0';
    return debug_buffer;
}

// This version prints much better in gdb
void Instruction::dump() {
    QTextStream q_cerr(stderr);
    print(q_cerr);
    q_cerr << "\n";
}

/* This function is designed to find basic flag calls, plus in addition two variations seen with Pentium FP code.
        These variations involve ANDing and/or XORing with constants. So it should return true for these values of e:
        ADDFLAGS(...)
        SETFFLAGS(...) & 0x45
        (SETFFLAGS(...) & 0x45) ^ 0x40
        FIXME: this may not be needed any more...
*/
bool hasSetFlags(Exp *e) {
    if (e->isFlagCall())
        return true;
    OPER op = e->getOper();
    if (op != opBitAnd && op != opBitXor)
        return false;
    Exp *left = ((Binary *)e)->getSubExp1();
    Exp *right = ((Binary *)e)->getSubExp2();
    if (!right->isIntConst())
        return false;
    if (left->isFlagCall()) {
        LOG_STREAM() << "hasSetFlags returns true with " << e << "\n";
        return true;
    }
    op = left->getOper();
    if (op != opBitAnd && op != opBitXor)
        return false;
    right = ((Binary *)left)->getSubExp2();
    left = ((Binary *)left)->getSubExp1();
    if (!right->isIntConst())
        return false;
    bool ret = left->isFlagCall();
    if (ret)
        LOG_STREAM() << "hasSetFlags returns true with " << e << "\n";
    return ret;
}

// Return true if can propagate to Exp* e (must be a RefExp to return true)
// Note: does not consider whether e is able to be renamed (from a memory Primitive point of view), only if the
// definition can be propagated TO this stmt
// Note: static member function
bool Instruction::canPropagateToExp(Exp &e) {
    if (!e.isSubscript())
        return false;
    RefExp & re((RefExp &)e);
    if (re.isImplicitDef())
        // Can't propagate statement "-" or "0" (implicit assignments)
        return false;
    Instruction *def = re.getDef();
    //    if (def == this)
    // Don't propagate to self! Can happen with %pc's (?!)
    //        return false;
    if (def->isNullStatement())
        // Don't propagate a null statement! Can happen with %pc's (would have no effect, and would infinitely loop)
        return false;
    if (!def->isAssign())
        return false; // Only propagate ordinary assignments (so far)
    Assign *adef = (Assign *)def;

    if (adef->getType()->isArray()) {
        // Assigning to an array, don't propagate (Could be alias problems?)
        return false;
    }
    return true;
}

static int propagate_progress = 0;
/***************************************************************************/ /**
  * \brief Propagate to this statement
  * \param destCounts is a map that indicates how may times a statement's definition is used
  * \param convert set true if an indirect call is changed to direct (otherwise, no change)
  * \param force set to true to propagate even memofs (for switch analysis)
  * \param usedByDomPhi is a set of subscripted locations used in phi statements
  * \returns true if a change
  ******************************************************************************/
bool Instruction::propagateTo(bool &convert, std::map<Exp *, int, lessExpStar> *destCounts /* = nullptr */,
                              LocationSet *usedByDomPhi /* = nullptr */, bool force /* = false */) {
    if (++propagate_progress > 1000) {
        LOG_STREAM() << 'p';
        LOG_STREAM().flush();
        propagate_progress = 0;
    }
    bool change;
    int changes = 0;
    // int sp = proc->getSignature()->getStackRegister(proc->getProg());
    // Exp* regSp = Location::regOf(sp);
    int propMaxDepth = Boomerang::get()->propMaxDepth;
    do {
        LocationSet exps;
        // addUsedLocs(..,true) -> true to also add uses from collectors. For example, want to propagate into
        // the reaching definitions of calls. Third parameter defaults to false, to
        // find all locations, not just those inside m[...]
        addUsedLocs(exps, true);
        LocationSet::iterator ll;
        change = false; // True if changed this iteration of the do/while loop
        // Example: m[r24{10}] := r25{20} + m[r26{30}]
        // exps has r24{10}, r25{30}, m[r26{30}], r26{30}
        for (ll = exps.begin(); ll != exps.end(); ll++) {
            Exp *e = *ll;
            if (!canPropagateToExp(*e))
                continue;
            assert(dynamic_cast<Assignment *>(((RefExp *)e)->getDef())!=nullptr);
            Assignment *def = (Assignment *)((RefExp *)e)->getDef();
            Exp *rhs = def->getRight();
            // If force is true, ignore the fact that a memof should not be propagated (for switch analysis)
            if (rhs->containsBadMemof(proc) && !(force && rhs->isMemOf()))
                // Must never propagate unsubscripted memofs, or memofs that don't yet have symbols. You could be
                // propagating past a definition, thereby invalidating the IR
                continue;
            Exp *lhs = def->getLeft();

            if (EXPERIMENTAL) {
#if 0
                // This is the old "don't propagate x=f(x)" heuristic. Hopefully it will work better now that we always
                // propagate into memofs etc. However, it might need a "and we're inside the right kind of loop"
                // condition
                LocationSet used;
                def->addUsedLocs(used);
                RefExp left(def->getLeft(), (Statement*)-1);
                RefExp *right = dynamic_cast<RefExp*>(def->getRight());
                // Beware of x := x{something else} (because we do want to do copy propagation)
                if (used.exists(&left) && !(right && *right->getSubExp1() == *left.getSubExp1()))
                    // We have something like eax = eax + 1
                    continue;
#else
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
                    bool doNotPropagate = false;
                    for (rcit = rhsComps.begin(); rcit != rhsComps.end(); ++rcit) {
                        if (!(*rcit)->isSubscript())
                            continue; // Sometimes %pc sneaks in
                        Exp *rhsBase = ((RefExp *)*rcit)->getSubExp1();
                        // We don't know the statement number for the one definition in usedInDomPhi that might exist,
                        // so we use findNS()
                        Exp *OW = usedByDomPhi->findNS(rhsBase);
                        if (OW) {
                            Instruction *OWdef = ((RefExp *)OW)->getDef();
                            if (!OWdef->isAssign())
                                continue;
                            Exp *lhsOWdef = ((Assign *)OWdef)->getLeft();
                            LocationSet OWcomps;
                            def->addUsedLocs(OWcomps);
                            LocationSet::iterator cc;
                            bool isOverwrite = false;
                            for (cc = OWcomps.begin(); cc != OWcomps.end(); ++cc) {
                                if (**cc *= *lhsOWdef) {
                                    isOverwrite = true;
                                    break;
                                }
                            }
                            if (isOverwrite) {
                                // Now check for propagating a component past OWdef
                                if (def->getDomNumber() <= OWdef->getDomNumber() &&
                                    OWdef->getDomNumber() < DominanceNum)
                                    // The heuristic kicks in
                                    doNotPropagate = true;
                                break;
                            }
                            if (OW)
                                LOG_STREAM() << "Ow is " << OW << "\n";
                        }
                    }
                    if (doNotPropagate) {
                        if (VERBOSE)
                            LOG << "% propagation of " << def->getNumber() << " into " << Number
                                << " prevented by the "
                                   "propagate past overwriting statement in loop heuristic\n";
                        continue;
                    }
                }
            }
#endif

            // Check if the -l flag (propMaxDepth) prevents this propagation
            if (destCounts && !lhs->isFlags()) { // Always propagate to %flags
                std::map<Exp *, int, lessExpStar>::iterator ff = destCounts->find(e);
                if (ff != destCounts->end() && ff->second > 1 && rhs->getComplexityDepth(proc) >= propMaxDepth) {
                    if (!def->getRight()->containsFlags()) {
                        // This propagation is prevented by the -l limit
                        continue;
                    }
                }
            }
            change |= doPropagateTo(e, def, convert);
        }
    } while (change && ++changes < 10);
    // Simplify is very costly, especially for calls. I hope that doing one simplify at the end will not affect any
    // result...
    simplify();
    return changes > 0; // Note: change is only for the last time around the do/while loop
}

// Experimental: may want to propagate flags first, without tests about complexity or the propagation limiting
// heuristic
bool Instruction::propagateFlagsTo() {
    // FIXME: convert is uninitialized ?
    bool change = false;
    bool convert = false;
    int changes = 0;
    do {
        LocationSet exps;
        addUsedLocs(exps, true);
        LocationSet::iterator ll;
        for (ll = exps.begin(); ll != exps.end(); ll++) {
            Exp *e = *ll;
            if (!e->isSubscript())
                continue; // e.g. %pc
            Assignment *def = dynamic_cast<Assignment *>(((RefExp *)e)->getDef());
            if ( def == nullptr || nullptr == def->getRight() ) // process if it has definition with rhs
                continue;
            Exp *base = ((RefExp *)e)->getSubExp1();
            if (base->isFlags() || base->isMainFlag()) {
                change |= doPropagateTo(e, def, convert);
            }
        }
    } while (change && ++changes < 10);
    simplify();
    return change;
}

// Parameter convert is set true if an indirect call is converted to direct
// Return true if a change made
// Note: this procedure does not control what part of this statement is propagated to
// Propagate to e from definition statement def.
// Set convert to true if convert a call from indirect to direct.
bool Instruction::doPropagateTo(Exp * e, Assignment * def, bool &convert) {
    // Respect the -p N switch
    if (Boomerang::get()->numToPropagate >= 0) {
        if (Boomerang::get()->numToPropagate == 0)
            return false;
        Boomerang::get()->numToPropagate--;
    }

    if (VERBOSE)
        LOG << "propagating " << def << "\n"
            << "       into " << this << "\n";

    bool change = replaceRef(e, def, convert);

    if (VERBOSE) {
        LOG << "     result " << this << "\n\n";
    }
    return change;
}

//! replace a use of def->getLeft() by def->getRight() in this statement
//! replaces a use in this statement with an expression from an ordinary assignment
//! \returns true if change
//! \note Internal use only
bool Instruction::replaceRef(Exp * e, Assignment * def, bool &convert) {
    Exp *rhs = def->getRight();
    assert(rhs);

    Exp *base = ((RefExp *)e)->getSubExp1();
    // Could be propagating %flags into %CF
    Exp *lhs = def->getLeft();
    if (base->getOper() == opCF && lhs->isFlags()) {
        if (!rhs->isFlagCall())
            return false;
        QString str = ((Const *)((Binary *)rhs)->getSubExp1())->getStr();
        if (str.startsWith("SUBFLAGS")) {
            /* When the carry flag is used bare, and was defined in a subtract of the form lhs - rhs, then CF has
               the value (lhs <u rhs).  lhs and rhs are the first and second parameters of the flagcall.
               Note: the flagcall is a binary, with a Const (the name) and a list of expressions:
                     defRhs
                    /      \
              Const       opList
            "SUBFLAGS"    /    \
                         P1    opList
                       /     \
                     P2    opList
                  /     \
                P3     opNil
            */
            Exp *relExp = Binary::get(opLessUns, ((Binary *)rhs)->getSubExp2()->getSubExp1(),
                                      ((Binary *)rhs)->getSubExp2()->getSubExp2()->getSubExp1());
            searchAndReplace(RefExp(Terminal::get(opCF), def), relExp, true);
            return true;
        }
    }
    // need something similar for %ZF
    if (base->getOper() == opZF && lhs->isFlags()) {
        if (!rhs->isFlagCall())
            return false;
        QString str = ((Const *)((Binary *)rhs)->getSubExp1())->getStr();
        if (str.startsWith("SUBFLAGS")) {
            // for zf we're only interested in if the result part of the subflags is equal to zero
            Exp *relExp = Binary::get(
                opEquals, ((Binary *)rhs)->getSubExp2()->getSubExp2()->getSubExp2()->getSubExp1(), Const::get(0));
            searchAndReplace(RefExp(Terminal::get(opZF), def), relExp, true);
            return true;
        }
    }

    // do the replacement
    // bool convert = doReplaceRef(re, rhs);
    bool ret = searchAndReplace(*e, rhs, true); // Last parameter true to change collectors
    // assert(ret);

    if (ret && isCall()) {
        convert |= ((CallStatement *)this)->convertToDirect();
    }
    return ret;
}

bool Instruction::isNullStatement() {
    if (Kind != STMT_ASSIGN)
        return false;
    Exp *right = ((Assign *)this)->getRight();
    if (right->isSubscript()) {
        // Must refer to self to be null
        return this == ((RefExp *)right)->getDef();
    } else
        // Null if left == right
        return *((Assign *)this)->getLeft() == *right;
}

bool Instruction::isFpush() {
    if (Kind != STMT_ASSIGN)
        return false;
    return ((Assign *)this)->getRight()->getOper() == opFpush;
}
bool Instruction::isFpop() {
    if (Kind != STMT_ASSIGN)
        return false;
    return ((Assign *)this)->getRight()->getOper() == opFpop;
}

/*
 * This code was in hrtl.cpp
 * Classes derived from Statement
 */

/******************************************************************************
 * GotoStatement methods
 *****************************************************************************/

GotoStatement::GotoStatement() : pDest(nullptr), m_isComputed(false) { Kind = STMT_GOTO; }

/***************************************************************************/ /**
  *
  * \brief        Construct a jump to a fixed address
  * \param        uDest native address of destination
  *
  ******************************************************************************/
GotoStatement::GotoStatement(ADDRESS uDest) : m_isComputed(false) {
    Kind = STMT_GOTO;
    pDest = Const::get(uDest);
}

/***************************************************************************/ /**
  * \fn        GotoStatement::~GotoStatement
  * \brief        Destructor
  *
  ******************************************************************************/
GotoStatement::~GotoStatement() {
    if (pDest) {
        ; // delete pDest;
    }
}
//!< Return the fixed destination of this CTI.
/***************************************************************************/ /**
  * \brief Get the fixed destination of this CTI. Assumes destination
  *        simplication has already been done so that a fixed dest will
  *        be of the Exp form:
  *        opIntConst dest
  * \returns Fixed dest or NO_ADDRESS if there isn't one, For dynamic CTIs,
  *          returns NO_ADDRESS.
  ******************************************************************************/
ADDRESS GotoStatement::getFixedDest() const {
    if (pDest->getOper() != opIntConst)
        return NO_ADDRESS;
    return constDest()->getAddr();
}

/***************************************************************************/ /**
  * \brief        Set the destination of this jump to be a given expression.
  * \param        pd - the new target
  ******************************************************************************/
void GotoStatement::setDest(Exp * pd) { pDest = pd; }

/***************************************************************************/ /**
  * \brief        Set the destination of this jump to be a given fixed address.
  * \param   addr - the new fixed address
  ******************************************************************************/
void GotoStatement::setDest(ADDRESS addr) {
    // This fails in FrontSparcTest, do you really want it to Mike? -trent
    //    assert(addr >= prog.limitTextLow && addr < prog.limitTextHigh);
    // Delete the old destination if there is one
    if (pDest != nullptr) {
        ; // delete pDest;
    }
    pDest = Const::get(addr);
}

/***************************************************************************/ /**
  * \brief        Returns the destination of this CTI.
  * \returns Pointer to the Exp representing the dest of this jump
  ******************************************************************************/
Exp *GotoStatement::getDest() { return pDest; }
const Exp *GotoStatement::getDest() const { return pDest; }

/***************************************************************************/ /**
  * \brief        Adjust the destination of this CTI by a given amount. Causes
  *                    an error is this destination is not a fixed destination
  *                    (i.e. a constant offset).
  * \param   delta - the amount to add to the destination (can be
  *                    negative)
  ******************************************************************************/
void GotoStatement::adjustFixedDest(int delta) {
    // Ensure that the destination is fixed.
    if (pDest == nullptr || pDest->getOper() != opIntConst)
        LOG << "Can't adjust destination of non-static CTI\n";

    ADDRESS dest = constDest()->getAddr();
    constDest()->setAddr(dest + delta);
}

bool GotoStatement::search(const Exp &search, Exp *&result) {
    result = nullptr;
    if (pDest)
        return pDest->search(search, result);
    return false;
}

/***************************************************************************/ /**
  * \fn        GotoStatement::searchAndReplace
  * \brief        Replace all instances of search with replace.
  * \param search - a location to search for
  * \param replace - the expression with which to replace it
  * \param cc - ignored
  * \returns True if any change
  ******************************************************************************/
bool GotoStatement::searchAndReplace(const Exp &search, Exp *replace, bool /*cc*/) {
    bool change = false;
    if (pDest) {
        pDest = pDest->searchReplaceAll(search, replace, change);
    }
    return change;
}

/***************************************************************************/ /**
  * \fn        GotoStatement::searchAll
  * \brief        Find all instances of the search expression
  * \param search - a location to search for
  * \param result - a list which will have any matching exprs
  *                 appended to it
  * \returns true if there were any matches
  ******************************************************************************/
bool GotoStatement::searchAll(const Exp &search, std::list<Exp *> &result) {
    if (pDest)
        return pDest->searchAll(search, result);
    return false;
}

/***************************************************************************/ /**
  * \fn    GotoStatement::print
  * \brief Display a text reprentation of this RTL to the given stream
  * \note  Usually called from RTL::print, in which case the first 9
  *        chars of the print have already been output to os
  * \param os - stream to write to
  * \param html - produce html encoded representation
  *
  ******************************************************************************/
void GotoStatement::print(QTextStream & os, bool html) const {
    os << qSetFieldWidth(4) << Number << qSetFieldWidth(0) << " " ;
    if (html) {
        os << "</td><td>";
        os << "<a name=\"stmt" << Number << "\">";
    }
    os << "GOTO ";
    if (pDest == nullptr)
        os << "*no dest*";
    else if (pDest->getOper() != opIntConst)
        pDest->print(os);
    else
        os << "0x" << getFixedDest();
    if (html)
        os << "</a></td>";
}

/***************************************************************************/ /**
  * \fn      GotoStatement::setIsComputed
  * \brief      Sets the fact that this call is computed.
  * \note This should really be removed, once CaseStatement and
  *                    HLNwayCall are implemented properly
  *
  ******************************************************************************/
void GotoStatement::setIsComputed(bool b) { m_isComputed = b; }

/***************************************************************************/ /**
  * \fn      GotoStatement::isComputed
  * \brief      Returns whether or not this call is computed.
  * \note          This should really be removed, once CaseStatement and HLNwayCall
  *                    are implemented properly
  * \returns           this call is computed
  ******************************************************************************/
bool GotoStatement::isComputed() { return m_isComputed; }

/***************************************************************************/ /**
  * \fn        GotoStatement::clone
  * \brief        Deep copy clone
  * \returns             Pointer to a new Statement, a clone of this GotoStatement
  ******************************************************************************/
Instruction * GotoStatement::clone() const {
    GotoStatement *ret = new GotoStatement();
    ret->pDest = pDest->clone();
    ret->m_isComputed = m_isComputed;
    // Statement members
    ret->Parent = Parent;
    ret->proc = proc;
    ret->Number = Number;
    return ret;
}

// visit this Statement in the RTL
bool GotoStatement::accept(StmtVisitor * visitor) { return visitor->visit(this); }

void GotoStatement::generateCode(HLLCode * /*hll*/, BasicBlock * /*pbb*/, int /*indLevel*/) {
    // dont generate any code for jumps, they will be handled by the BB
}

void GotoStatement::simplify() {
    if (isComputed()) {
        pDest = pDest->simplifyArith();
        pDest = pDest->simplify();
    }
}

/**********************************
 * BranchStatement methods
 **********************************/

/***************************************************************************/ /**
  * \fn        BranchStatement::BranchStatement
  * \brief        Constructor.
  *
  ******************************************************************************/
BranchStatement::BranchStatement() : jtCond((BRANCH_TYPE)0), pCond(nullptr), bFloat(false), size(0) {
    Kind = STMT_BRANCH;
}

/***************************************************************************/ /**
  * \fn        BranchStatement::~BranchStatement
  * \brief        Destructor
  *
  ******************************************************************************/
BranchStatement::~BranchStatement() {
    if (pCond) {
        ; // delete pCond;
    }
}

/***************************************************************************/ /**
  * \fn    BranchStatement::setCondType
  * \brief Sets the BRANCH_TYPE of this jcond as well as the flag
  *        indicating whether or not the floating point condition codes
  *        are used.
  * \param cond - the BRANCH_TYPE
  * \param usesFloat - this condional jump checks the floating point condition codes
  ******************************************************************************/
void BranchStatement::setCondType(BRANCH_TYPE cond, bool usesFloat /*= false*/) {
    jtCond = cond;
    bFloat = usesFloat;

    // set pCond to a high level representation of this type
    Exp *p = nullptr;
    switch (cond) {
    case BRANCH_JE:
        p = Binary::get(opEquals, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JNE:
        p = Binary::get(opNotEqual, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JSL:
        p = Binary::get(opLess, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JSLE:
        p = Binary::get(opLessEq, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JSGE:
        p = Binary::get(opGtrEq, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JSG:
        p = Binary::get(opGtr, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JUL:
        p = Binary::get(opLessUns, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JULE:
        p = Binary::get(opLessEqUns, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JUGE:
        p = Binary::get(opGtrEqUns, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JUG:
        p = Binary::get(opGtrUns, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JMI:
        p = Binary::get(opLess, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JPOS:
        p = Binary::get(opGtr, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JOF:
        p = Binary::get(opLessUns, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JNOF:
        p = Binary::get(opGtrUns, Terminal::get(opFlags), Const::get(0));
        break;
    case BRANCH_JPAR:
        // Can't handle this properly here; leave an impossible expression involving %flags so propagation will
        // still happen, and we can recognise this later in condToRelational()
        // Update: these expressions seem to get ignored ???
        p = Binary::get(opEquals, Terminal::get(opFlags), Const::get(999));
        break;
    }
    // this is such a hack.. preferably we should actually recognise SUBFLAGS32(..,..,..) > 0 instead of just
    // SUBFLAGS32(..,..,..) but I'll leave this in here for the moment as it actually works.
    if (!Boomerang::get()->noDecompile)
        p = Terminal::get(usesFloat ? opFflags : opFlags);
    assert(p);
    setCondExpr(p);
}

/***************************************************************************/ /**
  * \fn        BranchStatement::makeSigned
  * \brief        Change this from an unsigned to a signed branch
  *
  ******************************************************************************/
void BranchStatement::makeSigned() {
    // Make this into a signed branch
    switch (jtCond) {
    case BRANCH_JUL:
        jtCond = BRANCH_JSL;
        break;
    case BRANCH_JULE:
        jtCond = BRANCH_JSLE;
        break;
    case BRANCH_JUGE:
        jtCond = BRANCH_JSGE;
        break;
    case BRANCH_JUG:
        jtCond = BRANCH_JSG;
        break;
    default:
        // Do nothing for other cases
        break;
    }
}

/***************************************************************************/ /**
  * \fn      BranchStatement::getCondExpr
  * \brief   Return the SemStr expression containing the HL condition.
  * \returns ptr to an expression
  ******************************************************************************/
Exp *BranchStatement::getCondExpr() { return pCond; }

/***************************************************************************/ /**
  * \fn          BranchStatement::setCondExpr
  * \brief       Set the SemStr expression containing the HL condition.
  * \param       pe - Pointer to Exp to set
  *
  ******************************************************************************/
void BranchStatement::setCondExpr(Exp * pe) {
    if (pCond) {
        ; // delete pCond;
    }
    pCond = pe;
}

BasicBlock *BranchStatement::getFallBB() {
    ADDRESS a = getFixedDest();
    if (a == NO_ADDRESS)
        return nullptr;
    if (Parent == nullptr)
        return nullptr;
    if (Parent->getNumOutEdges() != 2)
        return nullptr;
    if (Parent->getOutEdge(0)->getLowAddr() == a)
        return Parent->getOutEdge(1);
    return Parent->getOutEdge(0);
}

// not that if you set the taken BB or fixed dest first, you will not be able to set the fall BB
void BranchStatement::setFallBB(BasicBlock * bb) {
    ADDRESS a = getFixedDest();
    if (a == NO_ADDRESS)
        return;
    if (Parent == nullptr)
        return;
    if (Parent->getNumOutEdges() != 2)
        return;
    if (Parent->getOutEdge(0)->getLowAddr() == a) {
        Parent->getOutEdge(1)->deleteInEdge(Parent);
        Parent->setOutEdge(1, bb);
        bb->addInEdge(Parent);
    } else {
        Parent->getOutEdge(0)->deleteInEdge(Parent);
        Parent->setOutEdge(0, bb);
        bb->addInEdge(Parent);
    }
}

BasicBlock *BranchStatement::getTakenBB() {
    ADDRESS a = getFixedDest();
    if (a == NO_ADDRESS)
        return nullptr;
    if (Parent == nullptr)
        return nullptr;
    if (Parent->getNumOutEdges() != 2)
        return nullptr;
    if (Parent->getOutEdge(0)->getLowAddr() == a)
        return Parent->getOutEdge(0);
    return Parent->getOutEdge(1);
}

void BranchStatement::setTakenBB(BasicBlock * bb) {
    ADDRESS a = getFixedDest();
    if (a == NO_ADDRESS)
        return;
    if (Parent == nullptr)
        return;
    if (Parent->getNumOutEdges() != 2)
        return;
    if (Parent->getOutEdge(0)->getLowAddr() == a) {
        Parent->getOutEdge(0)->deleteInEdge(Parent);
        Parent->setOutEdge(0, bb);
        bb->addInEdge(Parent);
    } else {
        Parent->getOutEdge(1)->deleteInEdge(Parent);
        Parent->setOutEdge(1, bb);
        bb->addInEdge(Parent);
    }
}

bool BranchStatement::search(const Exp &search, Exp *&result) {
    if (pCond)
        return pCond->search(search, result);
    result = nullptr;
    return false;
}

/***************************************************************************/ /**
  * \fn    BranchStatement::searchAndReplace
  * \brief Replace all instances of search with replace.
  * \param search - a location to search for
  * \param replace - the expression with which to replace it
  * \param cc - ignored
  * \returns True if any change
  ******************************************************************************/
bool BranchStatement::searchAndReplace(const Exp &search, Exp *replace, bool cc) {
    GotoStatement::searchAndReplace(search, replace, cc);
    bool change = false;
    if (pCond)
        pCond = pCond->searchReplaceAll(search, replace, change);
    return change;
}

/***************************************************************************/ /**
  * \brief   Find all instances of the search expression
  * \param   search - a location to search for
  * \param   result - a list which will have any matching exprs
  *          appended to it
  * \returns true if there were any matches
  ******************************************************************************/
bool BranchStatement::searchAll(const Exp &search, std::list<Exp *> &result) {
    if (pCond)
        return pCond->searchAll(search, result);
    return false;
}

/***************************************************************************/ /**
  * \fn        BranchStatement::print
  * \brief        Write a text representation to the given stream
  * \param        os: stream
  * \param html - produce html encoded representation
  ******************************************************************************/
void BranchStatement::print(QTextStream & os, bool html) const {
    os << qSetFieldWidth(4) <<Number << qSetFieldWidth(0) <<" ";
    if (html) {
        os << "</td><td>";
        os << "<a name=\"stmt" << Number << "\">";
    }
    os << "BRANCH ";
    if (pDest == nullptr)
        os << "*no dest*";
    else if (!pDest->isIntConst())
        os << pDest;
    else {
        // Really we'd like to display the destination label here...
        os << "0x" << getFixedDest();
    }
    os << ", condition ";
    switch (jtCond) {
    case BRANCH_JE:
        os << "equals";
        break;
    case BRANCH_JNE:
        os << "not equals";
        break;
    case BRANCH_JSL:
        os << "signed less";
        break;
    case BRANCH_JSLE:
        os << "signed less or equals";
        break;
    case BRANCH_JSGE:
        os << "signed greater or equals";
        break;
    case BRANCH_JSG:
        os << "signed greater";
        break;
    case BRANCH_JUL:
        os << "unsigned less";
        break;
    case BRANCH_JULE:
        os << "unsigned less or equals";
        break;
    case BRANCH_JUGE:
        os << "unsigned greater or equals";
        break;
    case BRANCH_JUG:
        os << "unsigned greater";
        break;
    case BRANCH_JMI:
        os << "minus";
        break;
    case BRANCH_JPOS:
        os << "plus";
        break;
    case BRANCH_JOF:
        os << "overflow";
        break;
    case BRANCH_JNOF:
        os << "no overflow";
        break;
    case BRANCH_JPAR:
        os << "parity";
        break;
    }
    if (bFloat)
        os << " float";
    os << '\n';
    if (pCond) {
        if (html)
            os << "<br>";
        os << "High level: ";
        pCond->print(os, html);
    }
    if (html)
        os << "</a></td>";
}

/***************************************************************************/ /**
  * \fn        BranchStatement::clone
  * \brief        Deep copy clone
  * \returns             Pointer to a new Instruction, a clone of this BranchStatement
  ******************************************************************************/
Instruction * BranchStatement::clone() const {
    BranchStatement *ret = new BranchStatement();
    ret->pDest = pDest->clone();
    ret->m_isComputed = m_isComputed;
    ret->jtCond = jtCond;
    ret->pCond = pCond ? pCond->clone() : nullptr;
    ret->bFloat = bFloat;
    // Statement members
    ret->Parent = Parent;
    ret->proc = proc;
    ret->Number = Number;
    return ret;
}

// visit this stmt
bool BranchStatement::accept(StmtVisitor * visitor) { return visitor->visit(this); }

void BranchStatement::generateCode(HLLCode * /*hll*/, BasicBlock * /*pbb*/, int /*indLevel*/) {
    // dont generate any code for jconds, they will be handled by the bb
}

bool BranchStatement::usesExp(const Exp &e) {
    Exp *tmp;
    return pCond && pCond->search(e, tmp);
}

// Common to BranchStatement and BoolAssign
// Return true if this is now a floating point Branch
bool condToRelational(Exp * &pCond, BRANCH_TYPE jtCond) {
    pCond = pCond->simplifyArith()->simplify();

    QString tgt;
    QTextStream os(&tgt);
    pCond->print(os);

    OPER condOp = pCond->getOper();
    if (condOp == opFlagCall && ((Const *)pCond->getSubExp1())->getStr().startsWith("SUBFLAGS")) {
        OPER op = opWild;
        // Special for PPC unsigned compares; may be other cases in the future
        bool makeUns = ((Const *)pCond->getSubExp1())->getStr().startsWith("SUBFLAGSNL");
        switch (jtCond) {
        case BRANCH_JE:
            op = opEquals;
            break;
        case BRANCH_JNE:
            op = opNotEqual;
            break;
        case BRANCH_JSL:
            if (makeUns)
                op = opLessUns;
            else
                op = opLess;
            break;
        case BRANCH_JSLE:
            if (makeUns)
                op = opLessEqUns;
            else
                op = opLessEq;
            break;
        case BRANCH_JSGE:
            if (makeUns)
                op = opGtrEqUns;
            else
                op = opGtrEq;
            break;
        case BRANCH_JSG:
            if (makeUns)
                op = opGtrUns;
            else
                op = opGtr;
            break;
        case BRANCH_JUL:
            op = opLessUns;
            break;
        case BRANCH_JULE:
            op = opLessEqUns;
            break;
        case BRANCH_JUGE:
            op = opGtrEqUns;
            break;
        case BRANCH_JUG:
            op = opGtrUns;
            break;
        case BRANCH_JMI:
            /*     pCond
                    /      \
                Const       opList
 "SUBFLAGS"    /    \
             P1    opList
                   /     \
                 P2    opList
                        /     \
                      P3     opNil */
            pCond =
                Binary::get(opLess, // P3 < 0
                            pCond->getSubExp2()->getSubExp2()->getSubExp2()->getSubExp1()->clone(), Const::get(0));
            break;
        case BRANCH_JPOS:
            pCond =
                Binary::get(opGtrEq, // P3 >= 0
                            pCond->getSubExp2()->getSubExp2()->getSubExp2()->getSubExp1()->clone(), Const::get(0));
            break;
        case BRANCH_JOF:
        case BRANCH_JNOF:
        case BRANCH_JPAR:
            break;
        }
        if (op != opWild) {
            pCond = Binary::get(op,
                                pCond->getSubExp2()->getSubExp1()->clone(),                // P1
                                pCond->getSubExp2()->getSubExp2()->getSubExp1()->clone()); // P2
        }
    } else if (condOp == opFlagCall && ((Const *)pCond->getSubExp1())->getStr().startsWith("LOGICALFLAGS")) {
        // Exp *e = pCond;
        OPER op = opWild;
        switch (jtCond) {
        case BRANCH_JE:
            op = opEquals;
            break;
        case BRANCH_JNE:
            op = opNotEqual;
            break;
        case BRANCH_JMI:
            op = opLess;
            break;
        case BRANCH_JPOS:
            op = opGtrEq;
            break;
        // FIXME: This next set is quite shakey. Really, we should pull all the individual flag definitions out of
        // the flag definitions, and substitute these into the equivalent conditions for the branches (a big, ugly
        // job).
        case BRANCH_JSL:
            op = opLess;
            break;
        case BRANCH_JSLE:
            op = opLessEq;
            break;
        case BRANCH_JSGE:
            op = opGtrEq;
            break;
        case BRANCH_JSG:
            op = opGtr;
            break;
        // These next few seem to fluke working fine on architectures like X86, SPARC, and 68K which clear the
        // carry on all logical operations.
        case BRANCH_JUL:
            op = opLessUns;
            break; // NOTE: this is equivalent to never branching, since nothing
                   // can be unsigned less than zero
        case BRANCH_JULE:
            op = opLessEqUns;
            break;
        case BRANCH_JUGE:
            op = opGtrEqUns;
            break; // Similarly, this is equivalent to always branching
        case BRANCH_JUG:
            op = opGtrUns;
            break;
        case BRANCH_JPAR: {
            // This is pentium specific too; see below for more notes.
            /*
                                                    pCond
                                                    /    \
                                              Const        opList
                                    "LOGICALFLAGS8"        /    \
                                                    opBitAnd    opNil
                                                         /        \
                                               opFlagCall        opIntConst
                                                /        \            mask
                                            Const        opList
                                        "SETFFLAGS"      /    \
                                                        P1    opList
                                                      /    \
                                                    P2    opNil
                            */
            Exp *flagsParam = pCond->getSubExp2()->getSubExp1();
            Exp *test = flagsParam;
            if (test->isSubscript())
                test = test->getSubExp1();
            if (test->isTemp())
                return false; // Just not propagated yet
            int mask = 0;
            if (flagsParam->getOper() == opBitAnd) {
                Exp *setFlagsParam = flagsParam->getSubExp2();
                if (setFlagsParam->isIntConst())
                    mask = ((Const *)setFlagsParam)->getInt();
            }
            Exp *at_opFlagsCall_List = flagsParam->getSubExp1()->getSubExp2();
            // Sometimes the mask includes the 0x4 bit, but we expect that to be off all the time. So effectively
            // the branch is for any one of the (one or two) bits being on. For example, if the mask is 0x41, we
            // are branching of less (0x1) or equal (0x41).
            mask &= 0x41;
            OPER op;
            switch (mask) {
            case 0:
                LOG << "WARNING: unhandled pentium branch if parity with pCond = " << pCond << "\n";
                return false;
            case 1:
                op = opLess;
                break;
            case 0x40:
                op = opEquals;
                break;
            case 0x41:
                op = opLessEq;
                break;
            default:
                op = opWild; // Not possible, but avoid a compiler warning
                break;
            }
            pCond = Binary::get(op, at_opFlagsCall_List->getSubExp1()->clone(),
                                at_opFlagsCall_List->getSubExp2()->getSubExp1()->clone());
            return true; // This is a floating point comparison
        }
        default:
            break;
        }
        if (op != opWild) {
            pCond = Binary::get(op, pCond->getSubExp2()->getSubExp1()->clone(), Const::get(0));
        }
    } else if (condOp == opFlagCall && ((Const *)pCond->getSubExp1())->getStr().startsWith("SETFFLAGS")) {
        // Exp *e = pCond;
        OPER op = opWild;
        switch (jtCond) {
        case BRANCH_JE:
            op = opEquals;
            break;
        case BRANCH_JNE:
            op = opNotEqual;
            break;
        case BRANCH_JMI:
            op = opLess;
            break;
        case BRANCH_JPOS:
            op = opGtrEq;
            break;
        case BRANCH_JSL:
            op = opLess;
            break;
        case BRANCH_JSLE:
            op = opLessEq;
            break;
        case BRANCH_JSGE:
            op = opGtrEq;
            break;
        case BRANCH_JSG:
            op = opGtr;
            break;
        default:
            break;
        }
        if (op != opWild) {
            pCond = Binary::get(op, pCond->getSubExp2()->getSubExp1()->clone(),
                                pCond->getSubExp2()->getSubExp2()->getSubExp1()->clone());
        }
    }
    // ICK! This is all PENTIUM SPECIFIC... needs to go somewhere else.
    // Might be of the form (SETFFLAGS(...) & MASK) RELOP INTCONST where MASK could be a combination of 1, 4, and
    // 40,
    // and relop could be == or ~=.  There could also be an XOR 40h after the AND
    // From MSVC 6, we can also see MASK = 0x44, 0x41, 0x5 followed by jump if (even) parity (see above)
    // %fflags = 0..0.0 00 >
    // %fflags = 0..0.1 01 <
    // %fflags = 1..0.0 40 =
    // %fflags = 1..1.1 45 not comparable
    // Example: (SETTFLAGS(...) & 1) ~= 0
    // left = SETFFLAGS(...) & 1
    // left1 = SETFFLAGS(...) left2 = int 1, k = 0, mask = 1
    else if (condOp == opEquals || condOp == opNotEqual) {
        Exp *left = ((Binary *)pCond)->getSubExp1();
        Exp *right = ((Binary *)pCond)->getSubExp2();
        bool hasXor40 = false;
        if (left->getOper() == opBitXor && right->isIntConst()) {
            Exp *r2 = ((Binary *)left)->getSubExp2();
            if (r2->isIntConst()) {
                int k2 = ((Const *)r2)->getInt();
                if (k2 == 0x40) {
                    hasXor40 = true;
                    left = ((Binary *)left)->getSubExp1();
                }
            }
        }
        if (left->getOper() == opBitAnd && right->isIntConst()) {
            Exp *left1 = ((Binary *)left)->getSubExp1();
            Exp *left2 = ((Binary *)left)->getSubExp2();
            int k = ((Const *)right)->getInt();
            // Only interested in 40, 1
            k &= 0x41;
            if (left1->getOper() == opFlagCall && left2->isIntConst()) {
                int mask = ((Const *)left2)->getInt();
                // Only interested in 1, 40
                mask &= 0x41;
                OPER op = opWild;
                if (hasXor40) {
                    assert(k == 0);
                    op = condOp;
                } else {
                    switch (mask) {
                    case 1:
                        if ((condOp == opEquals && k == 0) || (condOp == opNotEqual && k == 1))
                            op = opGtrEq;
                        else
                            op = opLess;
                        break;
                    case 0x40:
                        if ((condOp == opEquals && k == 0) || (condOp == opNotEqual && k == 0x40))
                            op = opNotEqual;
                        else
                            op = opEquals;
                        break;
                    case 0x41:
                        switch (k) {
                        case 0:
                            if (condOp == opEquals)
                                op = opGtr;
                            else
                                op = opLessEq;
                            break;
                        case 1:
                            if (condOp == opEquals)
                                op = opLess;
                            else
                                op = opGtrEq;
                            break;
                        case 0x40:
                            if (condOp == opEquals)
                                op = opEquals;
                            else
                                op = opNotEqual;
                            break;
                        default:
                            LOG_STREAM() << "BranchStatement::simplify: k is " << QString::number(k,16) << "\n";
                            assert(0);
                        }
                        break;
                    default:
                        LOG_STREAM() << "BranchStatement::simplify: Mask is " << QString::number(mask,16) << "\n";
                        assert(0);
                    }
                }
                if (op != opWild) {
                    pCond = Binary::get(op, left1->getSubExp2()->getSubExp1(),
                                        left1->getSubExp2()->getSubExp2()->getSubExp1());
                    return true; // This is now a float comparison
                }
            }
        }
    }
    return false;
}

void BranchStatement::simplify() {
    if (pCond) {
        if (condToRelational(pCond, jtCond))
            bFloat = true;
    }
}

/**********************************
 * CaseStatement methods
 **********************************/
/***************************************************************************/ /**
  * \fn        CaseStatement::CaseStatement
  * \brief        Constructor.
  *
  ******************************************************************************/
CaseStatement::CaseStatement() : pSwitchInfo(nullptr) { Kind = STMT_CASE; }

/***************************************************************************/ /**
  * \fn    CaseStatement::~CaseStatement
  * \brief Destructor
  * \note  Don't delete the pSwitchVar; it's always a copy of something else (so don't delete twice)
  *
  ******************************************************************************/
CaseStatement::~CaseStatement() {
    if (pSwitchInfo){
        ; // delete pSwitchInfo;
    }
}

/***************************************************************************/ /**
  * \fn      CaseStatement::getSwitchInfo
  * \brief   Return a pointer to a struct with switch information in it
  * \returns SWITCH_INFO struct
  ******************************************************************************/
SWITCH_INFO *CaseStatement::getSwitchInfo() { return pSwitchInfo; }

/***************************************************************************/ /**
  * \fn    CaseStatement::setSwitchInfo
  * \brief Set a pointer to a SWITCH_INFO struct
  * \param psi Pointer to SWITCH_INFO struct
  *
  ******************************************************************************/
void CaseStatement::setSwitchInfo(SWITCH_INFO * psi) { pSwitchInfo = psi; }

/***************************************************************************/ /**
  * \fn    CaseStatement::searchAndReplace
  * \brief Replace all instances of search with replace.
  * \param search - a location to search for
  * \param replace - the expression with which to replace it
  * \param cc - ignored
  * \returns             True if any change
  ******************************************************************************/
bool CaseStatement::searchAndReplace(const Exp &search, Exp *replace, bool cc) {
    bool ch = GotoStatement::searchAndReplace(search, replace, cc);
    bool ch2 = false;
    if (pSwitchInfo && pSwitchInfo->pSwitchVar)
        pSwitchInfo->pSwitchVar = pSwitchInfo->pSwitchVar->searchReplaceAll(search, replace, ch2);
    return ch | ch2;
}

/***************************************************************************/ /**
  * \fn    CaseStatement::searchAll
  * \brief Find all instances of the search expression
  * \param search - a location to search for
  * \param result - a list which will have any matching exprs appended to it
  * \returns true if there were any matches
  ******************************************************************************/
bool CaseStatement::searchAll(const Exp &search, std::list<Exp *> &result) {
    return GotoStatement::searchAll(search, result) ||
           (pSwitchInfo && pSwitchInfo->pSwitchVar && pSwitchInfo->pSwitchVar->searchAll(search, result));
}

/***************************************************************************/ /**
  * \fn    CaseStatement::print
  * \brief Write a text representation to the given stream
  * \param os - target stream
  * \param html - produce html encoded representation
  ******************************************************************************/
void CaseStatement::print(QTextStream  & os, bool html) const {
    os << qSetFieldWidth(4) << Number << qSetFieldWidth(0) << " ";
    if (html) {
        os << "</td><td>";
        os << "<a name=\"stmt" << Number << "\">";
    }
    if (pSwitchInfo == nullptr) {
        os << "CASE [";
        if (pDest == nullptr)
            os << "*no dest*";
        else
            os << pDest;
        os << "]";
    } else
        os << "SWITCH(" << pSwitchInfo->pSwitchVar << ")\n";
    if (html)
        os << "</a></td>";
}

/***************************************************************************/ /**
  * \fn      CaseStatement::clone
  * \brief   Deep copy clone
  * \returns Pointer to a new Instruction that is a clone of this one
  ******************************************************************************/
Instruction * CaseStatement::clone() const {
    CaseStatement *ret = new CaseStatement();
    ret->pDest = pDest ? pDest->clone() : nullptr;
    ret->m_isComputed = m_isComputed;
    if(pSwitchInfo) {
        ret->pSwitchInfo = new SWITCH_INFO;
        *ret->pSwitchInfo = *pSwitchInfo;
        ret->pSwitchInfo->pSwitchVar = pSwitchInfo->pSwitchVar->clone();
    }
    // Statement members
    ret->Parent = Parent;
    ret->proc = proc;
    ret->Number = Number;
    return ret;
}

// visit this stmt
bool CaseStatement::accept(StmtVisitor * visitor) { return visitor->visit(this); }

void CaseStatement::generateCode(HLLCode * /*hll*/, BasicBlock * /*pbb*/, int /*indLevel*/) {
    // dont generate any code for switches, they will be handled by the bb
}

bool CaseStatement::usesExp(const Exp &e) {
    // Before a switch statement is recognised, pDest is non null
    if (pDest)
        return *pDest == e;
    // After a switch statement is recognised, pDest is null, and pSwitchInfo->pSwitchVar takes over
    if (pSwitchInfo->pSwitchVar)
        return *pSwitchInfo->pSwitchVar == e;
    return false;
}

void CaseStatement::simplify() {
    if (pDest)
        pDest = pDest->simplify();
    else if (pSwitchInfo && pSwitchInfo->pSwitchVar)
        pSwitchInfo->pSwitchVar = pSwitchInfo->pSwitchVar->simplify();
}

/**********************************
 *        CallStatement methods
 **********************************/

/***************************************************************************/ /**
  * \fn         CallStatement::CallStatement
  * \brief         Constructor for a call
  *
  ******************************************************************************/
CallStatement::CallStatement() : returnAfterCall(false), calleeReturn(nullptr) {
    Kind = STMT_CALL;
    procDest = nullptr;
    signature = nullptr;
}

/***************************************************************************/ /**
  * \fn      CallStatement::~CallStatement
  * \brief      Destructor
  ******************************************************************************/
CallStatement::~CallStatement() {}

// Temporarily needed for ad-hoc type analysis
int CallStatement::findDefine(Exp * e) {
    StatementList::iterator rr;
    int i = 0;
    for (rr = defines.begin(); rr != defines.end(); ++rr, ++i) {
        Exp *ret = ((Assignment *)*rr)->getLeft();
        if (*ret == *e)
            return i;
    }
    return -1;
}

Exp *CallStatement::getProven(Exp * e) {
    if (procDest)
        return procDest->getProven(e);
    return nullptr;
}

//! Localise only components of e, i.e. xxx if e is m[xxx]
void CallStatement::localiseComp(Exp * e) {
    if (e->isMemOf()) {
        ((Location *)e)->setSubExp1(localiseExp(((Location *)e)->getSubExp1()));
    }
}
//! Substitute the various components of expression e with the appropriate reaching definitions.
//! Used in e.g. fixCallBypass (via the CallBypasser). Locations defined in this call are replaced with their proven
//! values, which are in terms of the initial values at the start of the call (reaching definitions at the call)
Exp *CallStatement::localiseExp(Exp * e) {
    if (!defCol.isInitialised())
        return e; // Don't attempt to subscript if the data flow not started yet
    Localiser l(this);
    e = e->clone()->accept(&l);

    return e;
}

//! Find the definition for the given expression, using the embedded Collector object
//! Was called findArgument(), and used implicit arguments and signature parameters
//! \note must only operator on unsubscripted locations, otherwise it is invalid
Exp *CallStatement::findDefFor(Exp * e) { return defCol.findDefFor(e); }

SharedType CallStatement::getArgumentType(int i) {
    assert(i < (int)arguments.size());
    StatementList::iterator aa = arguments.begin();
    std::advance(aa, i);
    return ((Assign *)(*aa))->getType();
}
void CallStatement::setArgumentType(int i, SharedType ty) {
    assert(i < (int)arguments.size());
    StatementList::iterator aa = arguments.begin();
    std::advance(aa, i);
    ((Assign *)(*aa))->setType(ty);
}
/***************************************************************************/ /**
  * \fn      CallStatement::setArguments
  * \brief      Set the arguments of this call.
  * \param      args - the list of locations to set the arguments to (for testing)
  *
  ******************************************************************************/
void CallStatement::setArguments(StatementList & args) {
    arguments.clear();
    arguments.append(args);
    StatementList::iterator ll;
    for (ll = arguments.begin(); ll != arguments.end(); ++ll) {
        ((Assign *)*ll)->setProc(proc);
        ((Assign *)*ll)->setBB(Parent);
    }
}

/***************************************************************************/ /**
  * \fn      CallStatement::setSigArguments
  * \brief      Set the arguments of this call based on signature info
  * \note Should only be called for calls to library functions
  *
  ******************************************************************************/
void CallStatement::setSigArguments() {
    if (signature)
        return; // Already done
    if (procDest == nullptr)
        // FIXME: Need to check this
        return;
    // Clone here because each call to procDest could have a different signature, modified by ellipsisProcessing
    signature = procDest->getSignature()->clone();
    procDest->addCaller(this);

    if (!procDest->isLib())
        return; // Using dataflow analysis now
    int n = signature->getNumParams();
    int i;
    arguments.clear();
    for (i = 0; i < n; i++) {
        Exp *e = signature->getArgumentExp(i);
        assert(e);
        Location *l = dynamic_cast<Location *>(e);
        if (l) {
            l->setProc(proc); // Needed?
        }
        Assign *as = new Assign(signature->getParamType(i)->clone(), e->clone(), e->clone());
        as->setProc(proc);
        as->setBB(Parent);
        as->setNumber(Number); // So fromSSAform will work later. But note: this call is probably not numbered yet!
        // as->setParent(this);
        arguments.append(as);
    }

    // initialize returns
    // FIXME: anything needed here?
}

bool CallStatement::search(const Exp &search, Exp *&result) {
    bool found = GotoStatement::search(search, result);
    if (found)
        return true;
    StatementList::iterator ss;
    for (ss = defines.begin(); ss != defines.end(); ++ss) {
        if ((*ss)->search(search, result))
            return true;
    }
    for (ss = arguments.begin(); ss != arguments.end(); ++ss) {
        if ((*ss)->search(search, result))
            return true;
    }
    return false;
}

/***************************************************************************/ /**
  * \fn        CallStatement::searchAndReplace
  * \brief        Replace all instances of search with replace.
  * \param search - a location to search for
  * \param replace - the expression with which to replace it
  * \param cc - true to replace in collectors
  * \returns             True if any change
  ******************************************************************************/
bool CallStatement::searchAndReplace(const Exp &search, Exp *replace, bool cc) {
    bool change = GotoStatement::searchAndReplace(search, replace, cc);
    StatementList::iterator ss;
    // FIXME: MVE: Check if we ever want to change the LHS of arguments or defines...
    for (ss = defines.begin(); ss != defines.end(); ++ss)
        change |= (*ss)->searchAndReplace(search, replace, cc);
    for (ss = arguments.begin(); ss != arguments.end(); ++ss)
        change |= (*ss)->searchAndReplace(search, replace, cc);
    if (cc) {
        DefCollector::iterator dd;
        for (dd = defCol.begin(); dd != defCol.end(); ++dd)
            change |= (*dd)->searchAndReplace(search, replace, cc);
    }
    return change;
}

/***************************************************************************/ /**
  * \fn    CallStatement::searchAll
  * \brief Find all instances of the search expression
  * \param search - a location to search for
  * \param result - a list which will have any matching exprs appended to it
  * \returns true if there were any matches
  ******************************************************************************/
bool CallStatement::searchAll(const Exp &search, std::list<Exp *> &result) {
    bool found = GotoStatement::searchAll(search, result);
    StatementList::iterator ss;
    for (ss = defines.begin(); ss != defines.end(); ++ss) {
        if ((*ss)->searchAll(search, result))
            found = true;
    }
    for (ss = arguments.begin(); ss != arguments.end(); ++ss) {
        if ((*ss)->searchAll(search, result))
            found = true;
    }
    return found;
}

/***************************************************************************/ /**
  * \fn    CallStatement::print
  * \brief Write a text representation of this RTL to the given stream
  * \param os - stream to write to
  * \param html - produce html encoded representation
  *
  ******************************************************************************/
void CallStatement::print(QTextStream & os, bool html) const {
    os << qSetFieldWidth(4) << Number << qSetFieldWidth(0) <<" ";
    if (html) {
        os << "</td><td>";
        os << "<a name=\"stmt" << Number << "\">";
    }

    // Define(s), if any
    if (defines.size()) {
        if (defines.size() > 1)
            os << "{";
        StatementList::const_iterator rr;
        bool first = true;
        for (rr = defines.begin(); rr != defines.end(); ++rr) {
            assert((*rr)->isAssignment());
            Assignment *as = (Assignment *)*rr;
            if (first)
                first = false;
            else
                os << ", ";
            os << "*" << as->getType() << "* " << as->getLeft();
            if (as->isAssign())
                os << " := " << ((Assign *)as)->getRight();
        }
        if (defines.size() > 1)
            os << "}";
        os << " := ";
    } else if (isChildless()) {
        if (html)
            os << "&lt;all&gt; := ";
        else
            os << "<all> := ";
    }

    os << "CALL ";
    if (procDest)
        os << procDest->getName();
    else if (pDest == nullptr)
        os << "*no dest*";
    else {
        if (pDest->isIntConst())
            os << "0x" << QString::number(((Const *)pDest)->getInt(),16);
        else
            pDest->print(os, html); // Could still be an expression
    }

    // Print the actual arguments of the call
    if (isChildless()) {
        if (html)
            os << "(&lt;all&gt;)";
        else
            os << "(<all>)";
    } else {
        os << "(\n";
        for (const Instruction *aa : arguments) {
            os << "                ";
            ((const Assignment *)aa)->printCompact(os, html);
            os << "\n";
        }
        os << "              )";
    }

#if 1
    // Collected reaching definitions
    if (html)
        os << "<br>";
    else
        os << "\n              ";
    os << "Reaching definitions: ";
    defCol.print(os, html);
    if (html)
        os << "<br>";
    else
        os << "\n              ";
    os << "Live variables: ";
    useCol.print(os, html);
#endif

    if (html)
        os << "</a></td>";
}

/***************************************************************************/ /**
  * \fn    CallStatement::setReturnAfterCall
  * \brief Sets a bit that says that this call is effectively followed by a return. This happens e.g. on
  *        Sparc when there is a restore in the delay slot of the call
  * \param b: true if this is to be set; false to clear the bit
  *
  ******************************************************************************/
void CallStatement::setReturnAfterCall(bool b) { returnAfterCall = b; }

/***************************************************************************/ /**
  * \fn    CallStatement::isReturnAfterCall
  * \brief Tests a bit that says that this call is effectively followed by a return. This happens e.g. on
  *        Sparc when there is a restore in the delay slot of the call
  * \returns True if this call is effectively followed by a return
  ******************************************************************************/
bool CallStatement::isReturnAfterCall() { return returnAfterCall; }

/***************************************************************************/ /**
  * \fn        CallStatement::clone
  * \brief        Deep copy clone
  * \returns             Pointer to a new Statement, a clone of this CallStatement
  ******************************************************************************/
Instruction * CallStatement::clone() const {
    CallStatement *ret = new CallStatement();
    ret->pDest = pDest->clone();
    ret->m_isComputed = m_isComputed;
    StatementList::const_iterator ss;
    for (ss = arguments.begin(); ss != arguments.end(); ++ss)
        ret->arguments.append((*ss)->clone());
    for (ss = defines.begin(); ss != defines.end(); ++ss)
        ret->defines.append((*ss)->clone());
    // Statement members
    ret->Parent = Parent;
    ret->proc = proc;
    ret->Number = Number;
    return ret;
}

// visit this stmt
bool CallStatement::accept(StmtVisitor * visitor) { return visitor->visit(this); }

Function *CallStatement::getDestProc() { return procDest; }

void CallStatement::setDestProc(Function * dest) {
    assert(dest);
    // assert(procDest == nullptr);        // No: not convenient for unit testing
    procDest = dest;
}

void CallStatement::generateCode(HLLCode * hll, BasicBlock * pbb, int indLevel) {

    Function *p = getDestProc();

    if (p == nullptr && isComputed()) {
        hll->AddIndCallStatement(indLevel, pDest, arguments, calcResults());
        return;
    }
    StatementList *results = calcResults();

#if 0
LOG << "call: " << this;
LOG << " in proc " << proc->getName() << "\n";
for (StatementList::iterator it = results->begin(); it != results->end(); it++)
    LOG << "result: " << *it << "\n";
#endif
    assert(p);
    if (Boomerang::get()->noDecompile) {
        if (procDest->getSignature()->getNumReturns() > 0) {
            Assign *as = new Assign(IntegerType::get(STD_SIZE), new Unary(opRegOf, Const::get(24)),
                                    new Unary(opRegOf, Const::get(24)));
            as->setProc(proc);
            as->setBB(pbb);
            results->append(as);
        }

        // some hacks
        if (p->getName() == "printf" || p->getName() == "scanf") {
            for (int i = 1; i < 3; i++) {
                Exp *e = signature->getArgumentExp(i);
                assert(e);
                Location *l = dynamic_cast<Location *>(e);
                if (l) {
                    l->setProc(proc); // Needed?
                }
                Assign *as = new Assign(signature->getParamType(i), e->clone(), e->clone());
                as->setProc(proc);
                as->setBB(pbb);
                as->setNumber(Number); // So fromSSAform will work later
                arguments.append(as);
            }
        }
    }
    if (p->isLib() && !p->getSignature()->getPreferedName().isEmpty()) {
        hll->AddCallStatement(indLevel, p, p->getSignature()->getPreferedName(), arguments, results);
    } else
        hll->AddCallStatement(indLevel, p, qPrintable(p->getName()), arguments, results);
}

void CallStatement::simplify() {
    GotoStatement::simplify();
    StatementList::iterator ss;
    for (ss = arguments.begin(); ss != arguments.end(); ++ss)
        (*ss)->simplify();
    for (ss = defines.begin(); ss != defines.end(); ++ss)
        (*ss)->simplify();
}

bool GotoStatement::usesExp(const Exp &e) {
    Exp *where;
    return (pDest->search(e, where));
}

bool CallStatement::usesExp(const Exp &e) {
    if (GotoStatement::usesExp(e))
        return true;
    StatementList::iterator ss;
    for (ss = arguments.begin(); ss != arguments.end(); ++ss)
        if ((*ss)->usesExp(e))
            return true;
    for (ss = defines.begin(); ss != defines.end(); ++ss)
        if ((*ss)->usesExp(e))
            return true;
    return false;
}

void CallStatement::getDefinitions(LocationSet & defs) {
    StatementList::iterator dd;
    for (dd = defines.begin(); dd != defines.end(); ++dd)
        defs.insert(((Assignment *)*dd)->getLeft());
    // Childless calls are supposed to define everything. In practice they don't really define things like %pc, so
    // we
    // need some extra logic in getTypeFor()
    if (isChildless() && !Boomerang::get()->assumeABI)
        defs.insert(Terminal::get(opDefineAll));
}

// Attempt to convert this call, if indirect, to a direct call.
// NOTE: at present, we igore the possibility that some other statement will modify the global. This is a serious
// limitation!!
bool CallStatement::convertToDirect() {
    if (!m_isComputed)
        return false;
    bool convertIndirect = false;
    Exp *e = pDest;
    if (pDest->isSubscript()) {
        Instruction *def = ((RefExp *)e)->getDef();
        if (def && !def->isImplicit())
            return false; // If an already defined global, don't convert
        e = ((RefExp *)e)->getSubExp1();
    }
    if (e->getOper() == opArrayIndex && ((Binary *)e)->getSubExp2()->isIntConst() &&
        ((Const *)(((Binary *)e)->getSubExp2()))->getInt() == 0)
        e = ((Binary *)e)->getSubExp1();
    // Can actually have name{0}[0]{0} !!
    if (e->isSubscript())
        e = ((RefExp *)e)->getSubExp1();
    if (e->isIntConst()) {
        // ADDRESS u = (ADDRESS)((Const*)e)->getInt();
        // Just convert it to a direct call!
        // FIXME: to be completed
    } else if (e->isMemOf()) {
        // It might be a global that has not been processed yet
        Exp *sub = ((Unary *)e)->getSubExp1();
        if (sub->isIntConst()) {
            // m[K]: convert it to a global right here
            ADDRESS u = ADDRESS::g(((Const *)sub)->getInt());
            proc->getProg()->globalUsed(u);
            QString nam = proc->getProg()->newGlobalName(u);
            e = Location::global(nam, proc);
            pDest = new RefExp(e, nullptr);
        }
    }
    if (!e->isGlobal()) {
        return false;
    }
    QString nam = ((Const *)e->getSubExp1())->getStr();
    Prog *prog = proc->getProg();
    ADDRESS gloAddr = prog->getGlobalAddr(nam);
    ADDRESS dest = ADDRESS::g(prog->readNative4(gloAddr));
    // We'd better do some limit checking on the value. This does not guarantee that it's a valid proc pointer, but
    // it
    // may help
    if (dest < prog->getLimitTextLow() || dest > prog->getLimitTextHigh())
        return false; // Not a valid proc pointer
    Function *p = prog->findProc(nam);
    bool bNewProc = p == nullptr;
    if (bNewProc)
        p = prog->setNewProc(dest);
    LOG_VERBOSE(1) << (bNewProc ? "new" : "existing") << " procedure for call to global '" << nam << " is "
                   << p->getName() << "\n";
    // we need to:
    // 1) replace the current return set with the return set of the new procDest
    // 2) call fixCallBypass (now fixCallAndPhiRefs) on the enclosing procedure
    // 3) fix the arguments (this will only affect the implicit arguments, the regular arguments should
    //    be empty at this point)
    // 3a replace current arguments with those of the new proc
    // 3b copy the signature from the new proc
    // 4) change this to a non-indirect call
    procDest = p;
    Signature *sig = p->getSignature();
    // pDest is currently still global5{-}, but we may as well make it a constant now, since that's how it will be
    // treated now
    pDest = Const::get(dest);

    // 1
    // 2
    proc->fixCallAndPhiRefs();

    // 3
    // 3a Do the same with the regular arguments
    arguments.clear();
    for (unsigned i = 0; i < sig->getNumParams(); i++) {
        Exp *a = sig->getParamExp(i);
        Assign *as = new Assign(VoidType::get(), a->clone(), a->clone());
        as->setProc(proc);
        as->setBB(Parent);
        arguments.append(as);
    }
    // LOG_STREAM() << "Step 3a: arguments now: ";
    // StatementList::iterator xx; for (xx = arguments.begin(); xx != arguments.end(); ++xx) {
    //        ((Assignment*)*xx)->printCompact(LOG_STREAM()); LOG_STREAM() << ", ";
    // } LOG_STREAM() << "\n";
    // implicitArguments = newimpargs;
    // assert((int)implicitArguments.size() == sig->getNumImplicitParams());

    // 3b
    signature = p->getSignature()->clone();

    // 4
    m_isComputed = false;
    proc->undoComputedBB(this);
    proc->addCallee(procDest);
    procDest->printDetailsXML();
    convertIndirect = true;

    if (VERBOSE)
        LOG << "Result of convertToDirect: " << this << "\n";
    return convertIndirect;
}

bool CallStatement::isCallToMemOffset() const
{
    return (getKind() == STMT_CALL && getDest()->getOper() == opMemOf &&
        getDest()->getSubExp1()->getOper() == opIntConst );
}
Exp *CallStatement::getArgumentExp(int i) {
    assert(i < (int)arguments.size());
    StatementList::iterator aa = arguments.begin();
    std::advance(aa, i);
    return ((Assign *)*aa)->getRight();
}

void CallStatement::setArgumentExp(int i, Exp *e) {
    assert(i < (int)arguments.size());
    StatementList::iterator aa = arguments.begin();
    std::advance(aa, i);
    Exp *&a = ((Assign *)*aa)->getRightRef();
    a = e->clone();
}

int CallStatement::getNumArguments() { return arguments.size(); }

void CallStatement::setNumArguments(int n) {
    int oldSize = arguments.size();
    if (oldSize > n) {
        StatementList::iterator aa = arguments.begin();
        std::advance(aa, n);
        arguments.erase(aa, arguments.end());
    }
    // MVE: check if these need extra propagation
    for (int i = oldSize; i < n; i++) {
        Exp *a = procDest->getSignature()->getArgumentExp(i);
        SharedType ty = procDest->getSignature()->getParamType(i);
        if (ty == nullptr && oldSize)
            ty = procDest->getSignature()->getParamType(oldSize - 1);
        if (ty == nullptr)
            ty = VoidType::get();
        Assign *as = new Assign(ty, a->clone(), a->clone());
        as->setProc(proc);
        as->setBB(Parent);
        arguments.append(as);
    }
}

void CallStatement::removeArgument(int i) {
    StatementList::iterator aa = arguments.begin();
    std::advance(aa, i);
    arguments.erase(aa);
}

// Processes each argument of a CallStatement, and the RHS of an Assign. Ad-hoc type analysis only.
Exp *processConstant(Exp * e, SharedType  t, Prog * prog, UserProc * proc, ADDRESS stmt) {
    if (t == nullptr)
        return e;
    std::shared_ptr<NamedType> nt;
    if (t->isNamed()) {
        nt = std::static_pointer_cast<NamedType>(t);
        t = nt->resolvesTo();
    }
    if (t == nullptr)
        return e;
    // char* and a constant
    if (e->isIntConst()) {
        if (nt && (nt->getName() == "LPCWSTR")) {
            ADDRESS u = ((Const *)e)->getAddr();
            // TODO: wide char string processing
            LOG << "possible wide char string at " << u << "\n";
        }
        if (t->resolvesToPointer()) {
            auto pt = t->as<PointerType>();
            SharedType points_to = pt->getPointsTo();
            if (t->isCString()) {
                ADDRESS u = ((Const *)e)->getAddr();
                if (!u.isZero()) { // can't do anything with nullptr
                    const char *str = prog->getStringConstant(u, true);
                    if (str) {
                        e = Const::get(str);
                        // Check if we may have guessed this global incorrectly (usually as an array of char)
                        QString nam = prog->getGlobalName(u);
                        if (!nam.isEmpty())
                            prog->setGlobalType(nam, PointerType::get(CharType::get()));
                    } else {
                        proc->getProg()->globalUsed(u);
                        QString nam = proc->getProg()->getGlobalName(u);
                        if (!nam.isEmpty())
                            e = Location::global(nam, proc);
                    }
                }
            }
            if (points_to->resolvesToFunc()) {
                ADDRESS a = ((Const *)e)->getAddr();
                if (VERBOSE || 1)
                    LOG << "found function pointer with constant value "
                        << "of type " << pt->getCtype() << " in statement at addr " << stmt
                        << ".  Decoding address " << a << "\n";
                // the address can be zero, i.e., nullptr, if so, ignore it.
                if (!a.isZero()) {
                    if (!Boomerang::get()->noDecodeChildren)
                        prog->decodeEntryPoint(a);
                    Function *p = prog->findProc(a);
                    if (p) {
                        Signature *sig = points_to->asFunc()->getSignature()->clone();
                        if (sig->getName() == nullptr || sig->getName().isEmpty() || sig->getName() == "<ANON>" ||
                            prog->findProc(sig->getName()) != nullptr)
                            sig->setName(p->getName());
                        else
                            p->setName(sig->getName());
                        sig->setForced(true);
                        p->setSignature(sig);
                        e = Location::global(qPrintable(p->getName()), proc);
                    }
                }
            }
        } else if (t->resolvesToFloat()) {
            e = new Ternary(opItof, Const::get(32), Const::get((int)t->getSize()), e);
        }
    }

    return e;
}

SharedType Assignment::getTypeFor(Exp * /*e*/) {
    // assert(*lhs == *e);            // No: local vs base expression
    return type;
}

void Assignment::setTypeFor(Exp * /*e*/, SharedType ty) {
    // assert(*lhs == *e);
    SharedType oldType = type;
    type = ty;
    if (DEBUG_TA && oldType != ty)
        LOG << "    changed type of " << this << "  (type was " << oldType->getCtype() << ")\n";
}

// Scan the returns for e. If found, return the type associated with that return
SharedType CallStatement::getTypeFor(Exp * e) {
    // The defines "cache" what the destination proc is defining
    Assignment *as = defines.findOnLeft(e);
    if (as != nullptr)
        return as->getType();
    if (e->isPC())
        // Special case: just return void*
        return PointerType::get(VoidType::get());
    return VoidType::get();
}

void CallStatement::setTypeFor(Exp * e, SharedType ty) {
    Assignment *as = defines.findOnLeft(e);
    if (as != nullptr)
        return as->setType(ty);
    // See if it is in our reaching definitions
    Exp *ref = defCol.findDefFor(e);
    if (ref == nullptr || !ref->isSubscript())
        return;
    Instruction *def = ((RefExp *)ref)->getDef();
    if (def == nullptr)
        return;
    def->setTypeFor(e, ty);
}

bool CallStatement::objcSpecificProcessing(const QString &formatStr) {
    Function *proc = getDestProc();
    if (!proc)
        return false;

    QString name(proc->getName());
    if (name == "objc_msgSend") {
        if (!formatStr.isNull()) {
            int format = getNumArguments() - 1;
            int n = 1;
            int p =0;
            while ((p = formatStr.indexOf(':',p))!=-1) {
                p++; // Point past the :
                n++;
                addSigParam(PointerType::get(VoidType::get()), false);
            }
            setNumArguments(format + n);
            signature->killEllipsis(); // So we don't do this again
            return true;
        } else {
            bool change = false;
            LOG << this << "\n";
            for (int i = 0; i < getNumArguments(); i++) {
                Exp *e = getArgumentExp(i);
                SharedType ty = getArgumentType(i);
                LOG << "arg " << i << " e: " << e << " ty: " << ty << "\n";
                if (!(ty->isPointer() && (std::static_pointer_cast<PointerType>(ty)->getPointsTo()->isChar()) && e->isIntConst())) {
                    ADDRESS addr = ADDRESS::g(((Const *)e)->getInt());
                    LOG << "addr: " << addr << "\n";
                    if (proc->getProg()->isStringConstant(addr)) {
                        LOG << "making arg " << i << " of call c*\n";
                        setArgumentType(i, PointerType::get(CharType::get()));
                        change = true;
                    } else if (proc->getProg()->isCFStringConstant(addr)) {
                        ADDRESS addr2 = ADDRESS::g(proc->getProg()->readNative4(addr + 8));
                        LOG << "arg " << i << " of call is a cfstring\n";
                        setArgumentType(i, PointerType::get(CharType::get()));
                        // TODO: we'd really like to change this to CFSTR(addr)
                        setArgumentExp(i, Const::get(addr2));
                        change = true;
                    }
                }
            }
            return change;
        }
    }
    return false;
}

// This function has two jobs. One is to truncate the list of arguments based on the format string.
// The second is to add parameter types to the signature.
// If -Td is used, type analysis will be rerun with these changes.
bool CallStatement::ellipsisProcessing(Prog * prog) {
    Q_UNUSED(prog);
    // if (getDestProc() == nullptr || !getDestProc()->getSignature()->hasEllipsis())
    if (getDestProc() == nullptr || !signature->hasEllipsis())
        return objcSpecificProcessing(nullptr);
    // functions like printf almost always have too many args
    QString name(getDestProc()->getName());
    int format = -1;
    if ((name == "printf" || name == "scanf"))
        format = 0;
    else if (name == "sprintf" || name == "fprintf" || name == "sscanf")
        format = 1;
    else if (getNumArguments() && getArgumentExp(getNumArguments() - 1)->isStrConst())
        format = getNumArguments() - 1;
    else
        return false;
    if (VERBOSE)
        LOG << "ellipsis processing for " << name << "\n";
    QString formatStr=QString::null;
    Exp *formatExp = getArgumentExp(format);
    // We sometimes see a[m[blah{...}]]
    if (formatExp->isAddrOf()) {
        formatExp = ((Unary *)formatExp)->getSubExp1();
        if (formatExp->isSubscript())
            formatExp = ((RefExp *)formatExp)->getSubExp1();
        if (formatExp->isMemOf())
            formatExp = ((Unary *)formatExp)->getSubExp1();
    }
    if (formatExp->isSubscript()) {
        // Maybe it's defined to be a Const string
        Instruction *def = ((RefExp *)formatExp)->getDef();
        if (def == nullptr)
            return false; // Not all nullptr refs get converted to implicits
        if (def->isAssign()) {
            // This would be unusual; propagation would normally take care of this
            Exp *rhs = ((Assign *)def)->getRight();
            if (rhs == nullptr || !rhs->isStrConst())
                return false;
            formatStr = ((Const *)rhs)->getStr();
        } else if (def->isPhi()) {
            // More likely. Example: switch_gcc. Only need ONE candidate format string
            PhiAssign *pa = (PhiAssign *)def;
            for (auto &v : *pa) {
                def = v.second.def();
                if ((def == nullptr) || (!def->isAssign()))
                    continue;
                Exp *rhs = ((Assign *)def)->getRight();
                if (rhs == nullptr || !rhs->isStrConst())
                    continue;
                formatStr = ((Const *)rhs)->getStr();
                break;
            }
            if (formatStr.isNull())
                return false;
        } else
            return false;
    } else if (formatExp->isStrConst()) {
        formatStr = ((Const *)formatExp)->getStr();
    } else
        return false;
    if (objcSpecificProcessing(formatStr))
        return true;

    // actually have to parse it
    // Format string is: % [flags] [width] [.precision] [size] type
    int n = 1; // Count the format string itself (may also be "format" more arguments)
    char ch;
    // Set a flag if the name of the function is scanf/sscanf/fscanf
    bool isScanf = name.contains("scanf");
    int p_idx = 0;
    //TODO: use qregularexpression to match scanf arguments
    while ((p_idx = formatStr.indexOf('%',p_idx))!=-1) {
        p_idx++;                   // Point past the %
        bool veryLong = false; // %lld or %L
        do {
            ch = formatStr[p_idx++].toLatin1(); // Skip size and precisionA
            switch (ch) {
            case '*':
                // Example: printf("Val: %*.*f\n", width, precision, val);
                n++; // There is an extra parameter for the width or precision
                // This extra parameter is of type integer, never int* (so pass false as last argument)
                addSigParam(IntegerType::get(STD_SIZE), false);
                continue;
            case '-':
            case '+':
            case '#':
            case ' ':
                // Flag. Ignore
                continue;
            case '.':
                // Separates width and precision. Ignore.
                continue;
            case 'h':
            case 'l':
                // size of half or long. Argument is usually still one word. Ignore.
                // Exception: %llx
                // TODO: handle architectures where l implies two words
                // TODO: at least h has implications for scanf
                if (formatStr[p_idx] == 'l') {
                    // %llx
                    p_idx++; // Skip second l
                    veryLong = true;
                }
                continue;
            case 'L':
                // long. TODO: handle L for long doubles.
                // n++;        // At least chew up one more parameter so later types are correct
                veryLong = true;
                continue;
            default:
                if ('0' <= ch && ch <= '9')
                    continue; // width or precision
                break;        // Else must be format type, handled below
            }
            break;
        } while (1);
        if (ch != '%') // Don't count %%
            n++;
        switch (ch) {
        case 'd':
        case 'i': // Signed integer
            addSigParam(IntegerType::get(veryLong ? 64 : 32), isScanf);
            break;
        case 'u':
        case 'x':
        case 'X':
        case 'o': // Unsigned integer
            addSigParam(IntegerType::get(32, -1), isScanf);
            break;
        case 'f':
        case 'g':
        case 'G':
        case 'e':
        case 'E': // Various floating point formats
            // Note that for scanf, %f means float, and %lf means double, whereas for printf, both of these mean
            // double
            addSigParam(FloatType::get(veryLong ? 128 : (isScanf ? 32 : 64)), isScanf); // Note: may not be 64 bits
            // for some archs
            break;
        case 's': // String
            addSigParam(PointerType::get(ArrayType::get(CharType::get())), isScanf);
            break;
        case 'c': // Char
            addSigParam(CharType::get(), isScanf);
            break;
        case '%':
            break; // Ignore %% (emits 1 percent char)
        default:
            LOG << "Unhandled format character " << ch << " in format string for call " << this << "\n";
        }
    }
    setNumArguments(format + n);
    signature->killEllipsis(); // So we don't do this again
    return true;
}

// Make an assign suitable for use as an argument from a callee context expression
Assign *CallStatement::makeArgAssign(SharedType ty, Exp * e) {
    Exp *lhs = e->clone();
    localiseComp(lhs); // Localise the components of lhs (if needed)
    Exp *rhs = localiseExp(e->clone());
    Assign *as = new Assign(ty, lhs, rhs);
    as->setProc(proc);
    as->setBB(Parent);
    // It may need implicit converting (e.g. sp{-} -> sp{0})
    Cfg *cfg = proc->getCFG();
    if (cfg->implicitsDone()) {
        ImplicitConverter ic(cfg);
        StmtImplicitConverter sm(&ic, cfg);
        as->accept(&sm);
    }
    return as;
}

// Helper function for the above
void CallStatement::addSigParam(SharedType ty, bool isScanf) {
    if (isScanf)
        ty = PointerType::get(ty);
    signature->addParameter(ty);
    Exp *paramExp = signature->getParamExp(signature->getNumParams() - 1);
    if (VERBOSE)
        LOG << "  ellipsisProcessing: adding parameter " << paramExp << " of type " << ty->getCtype() << "\n";
    if (arguments.size() < (unsigned)signature->getNumParams()) {
        Assign *as = makeArgAssign(ty, paramExp);
        arguments.append(as);
    }
}

/**********************************
 * ReturnStatement methods
 **********************************/

ReturnStatement::ReturnStatement() : retAddr(NO_ADDRESS) { Kind = STMT_RET; }

ReturnStatement::~ReturnStatement() {}

/***************************************************************************/ /**
  * \brief        Deep copy clone
  * \returns             Pointer to a new Statement, a clone of this ReturnStatement
  ******************************************************************************/
Instruction * ReturnStatement::clone() const {
    ReturnStatement *ret = new ReturnStatement();
    for (auto const &elem : modifieds)
        ret->modifieds.append((ImplicitAssign *)(elem)->clone());
    for (auto const &elem : returns)
        ret->returns.append((Assignment *)(elem)->clone());
    ret->retAddr = retAddr;
    ret->col.makeCloneOf(col);
    // Statement members
    ret->Parent = Parent;
    ret->proc = proc;
    ret->Number = Number;
    return ret;
}

// visit this stmt
bool ReturnStatement::accept(StmtVisitor * visitor) { return visitor->visit(this); }

void ReturnStatement::generateCode(HLLCode * hll, BasicBlock * pbb, int indLevel) {
    Q_UNUSED(pbb);
    hll->AddReturnStatement(indLevel, &getReturns());
}

void ReturnStatement::simplify() {
    iterator it;
    for (it = modifieds.begin(); it != modifieds.end(); it++)
        (*it)->simplify();
    for (it = returns.begin(); it != returns.end(); it++)
        (*it)->simplify();
}

// Remove the return (if any) related to loc. Loc may or may not be subscripted
void ReturnStatement::removeReturn(Exp * loc) {
    if (loc->isSubscript())
        loc = ((Location *)loc)->getSubExp1();
    for (iterator rr = returns.begin(); rr != returns.end(); ++rr) {
        if (*((Assignment *)*rr)->getLeft() == *loc) {
            returns.erase(rr);
            return; // Assume only one definition
        }
    }
}

void ReturnStatement::addReturn(Assignment * a) { returns.append(a); }

bool ReturnStatement::search(const Exp &search, Exp *&result) {
    result = nullptr;
    ReturnStatement::iterator rr;
    for (rr = begin(); rr != end(); ++rr) {
        if ((*rr)->search(search, result))
            return true;
    }
    return false;
}

bool ReturnStatement::searchAndReplace(const Exp &search, Exp *replace, bool cc) {
    bool change = false;
    ReturnStatement::iterator rr;
    for (rr = begin(); rr != end(); ++rr)
        change |= (*rr)->searchAndReplace(search, replace, cc);
    if (cc) {
        DefCollector::iterator dd;
        for (dd = col.begin(); dd != col.end(); ++dd)
            change |= (*dd)->searchAndReplace(search, replace);
    }
    return change;
}

bool ReturnStatement::searchAll(const Exp &search, std::list<Exp *> &result) {
    bool found = false;
    ReturnStatement::iterator rr;
    for (rr = begin(); rr != end(); ++rr) {
        if ((*rr)->searchAll(search, result))
            found = true;
    }
    return found;
}

bool CallStatement::isDefinition() {
    LocationSet defs;
    getDefinitions(defs);
    return defs.size() != 0;
}

bool ReturnStatement::usesExp(const Exp &e) {
    Exp *where;
    ReturnStatement::iterator rr;
    for (rr = begin(); rr != end(); ++rr) {
        if ((*rr)->search(e, where))
            return true;
    }
    return false;
}

/**********************************************************************
 * BoolAssign methods
 * These are for statements that set a destination (usually to 1 or 0)
 * depending in a condition code (e.g. Pentium)
 **********************************************************************/

/***************************************************************************/ /**
  * \fn         BoolAssign::BoolAssign
  * \brief         Constructor.
  * \param         size - the size of the assignment
  ******************************************************************************/
BoolAssign::BoolAssign(int size)
    : Assignment(nullptr), jtCond((BRANCH_TYPE)0), pCond(nullptr), bFloat(false), Size(size) {
    Kind = STMT_BOOLASSIGN;
}

BoolAssign::~BoolAssign() {
    if (pCond) {
        ; // delete pCond;
    }
}

/***************************************************************************/ /**
  * \brief Sets the BRANCH_TYPE of this jcond as well as the flag
  * indicating whether or not the floating point condition codes
  * are used.
  * \param cond - the BRANCH_TYPE
  * \param usesFloat - this condional jump checks the floating point condition codes
  ******************************************************************************/
void BoolAssign::setCondType(BRANCH_TYPE cond, bool usesFloat /*= false*/) {
    jtCond = cond;
    bFloat = usesFloat;
    setCondExpr(Terminal::get(opFlags));
}

/***************************************************************************/ /**
  * \brief Change this from an unsigned to a signed branch
  * \note Not sure if this is ever going to be used
  ******************************************************************************/
void BoolAssign::makeSigned() {
    // Make this into a signed branch
    switch (jtCond) {
    case BRANCH_JUL:
        jtCond = BRANCH_JSL;
        break;
    case BRANCH_JULE:
        jtCond = BRANCH_JSLE;
        break;
    case BRANCH_JUGE:
        jtCond = BRANCH_JSGE;
        break;
    case BRANCH_JUG:
        jtCond = BRANCH_JSG;
        break;
    default:
        // Do nothing for other cases
        break;
    }
}

/***************************************************************************/ /**
  * \brief Return the Exp expression containing the HL condition.
  * \returns Exp instance
  ******************************************************************************/
Exp *BoolAssign::getCondExpr() { return pCond; }

/***************************************************************************/ /**
  * \brief Set the Exp expression containing the HL condition.
  * \param pss Pointer to semantic string to set
  *
  ******************************************************************************/
void BoolAssign::setCondExpr(Exp * pss) {
    if (pCond) {
        ; // delete pCond;
    }
    pCond = pss;
}

/***************************************************************************/ /**
  * \fn    BoolAssign::printCompact
  * \brief Write a text representation to the given stream
  * \param os: stream
  * \param html - produce html encoded representation
  ******************************************************************************/
void BoolAssign::printCompact(QTextStream & os /*= cout*/, bool html) const {
    os << "BOOL ";
    lhs->print(os);
    os << " := CC(";
    switch (jtCond) {
    case BRANCH_JE:
        os << "equals";
        break;
    case BRANCH_JNE:
        os << "not equals";
        break;
    case BRANCH_JSL:
        os << "signed less";
        break;
    case BRANCH_JSLE:
        os << "signed less or equals";
        break;
    case BRANCH_JSGE:
        os << "signed greater or equals";
        break;
    case BRANCH_JSG:
        os << "signed greater";
        break;
    case BRANCH_JUL:
        os << "unsigned less";
        break;
    case BRANCH_JULE:
        os << "unsigned less or equals";
        break;
    case BRANCH_JUGE:
        os << "unsigned greater or equals";
        break;
    case BRANCH_JUG:
        os << "unsigned greater";
        break;
    case BRANCH_JMI:
        os << "minus";
        break;
    case BRANCH_JPOS:
        os << "plus";
        break;
    case BRANCH_JOF:
        os << "overflow";
        break;
    case BRANCH_JNOF:
        os << "no overflow";
        break;
    case BRANCH_JPAR:
        os << "ev parity";
        break;
    }
    os << ")";
    if (bFloat)
        os << ", float";
    if (html)
        os << "<br>";
    os << '\n';
    if (pCond) {
        os << "High level: ";
        pCond->print(os, html);
        if (html)
            os << "<br>";
        os << "\n";
    }
}

/***************************************************************************/ /**
  * \fn        BoolAssign::clone
  * \brief        Deep copy clone
  * \returns             Pointer to a new Statement, a clone of this BoolAssign
  ******************************************************************************/
Instruction * BoolAssign::clone() const {
    BoolAssign *ret = new BoolAssign(Size);
    ret->jtCond = jtCond;
    if (pCond)
        ret->pCond = pCond->clone();
    else
        ret->pCond = nullptr;
    ret->bFloat = bFloat;
    ret->Size = Size;
    // Statement members
    ret->Parent = Parent;
    ret->proc = proc;
    ret->Number = Number;
    return ret;
}

//! visit this Statement
bool BoolAssign::accept(StmtVisitor * visitor) { return visitor->visit(this); }
//! code generation
void BoolAssign::generateCode(HLLCode * hll, BasicBlock * /*pbb*/, int indLevel) {
    assert(lhs);
    assert(pCond);
    // lhs := (pCond) ? 1 : 0
    Assign as(lhs->clone(), new Ternary(opTern, pCond->clone(), Const::get(1), Const::get(0)));
    hll->AddAssignmentStatement(indLevel, &as);
}
//! simplify all the uses/defs in this Statement
void BoolAssign::simplify() {
    if (pCond)
        condToRelational(pCond, jtCond);
}

void BoolAssign::getDefinitions(LocationSet & defs) { defs.insert(getLeft()); }

bool BoolAssign::usesExp(const Exp &e) {
    assert(lhs && pCond);
    Exp *where = nullptr;
    return (pCond->search(e, where) || (lhs->isMemOf() && lhs->getSubExp1()->search(e, where)));
}

bool BoolAssign::search(const Exp &search, Exp *&result) {
    assert(lhs);
    if (lhs->search(search, result))
        return true;
    assert(pCond);
    return pCond->search(search, result);
}

bool BoolAssign::searchAll(const Exp &search, std::list<Exp *> &result) {
    bool ch = false;
    assert(lhs);
    if (lhs->searchAll(search, result))
        ch = true;
    assert(pCond);
    return pCond->searchAll(search, result) || ch;
}

bool BoolAssign::searchAndReplace(const Exp &search, Exp *replace, bool cc) {
    Q_UNUSED(cc);
    bool chl, chr;
    assert(pCond);
    assert(lhs);
    pCond = pCond->searchReplaceAll(search, replace, chl);
    lhs = lhs->searchReplaceAll(search, replace, chr);
    return chl | chr;
}

// This is for setting up SETcc instructions; see include/decoder.h macro SETS
void BoolAssign::setLeftFromList(std::list<Instruction *> * stmts) {
    assert(stmts->size() == 1);
    Assign *first = (Assign *)stmts->front();
    assert(first->getKind() == STMT_ASSIGN);
    lhs = first->getLeft();
}

//    //    //    //
// Assign //
//    //    //    //

Assignment::Assignment(Exp * lhs) : TypingStatement(VoidType::get()), lhs(lhs) {
    if (lhs && lhs->isRegOf()) {
        int n = ((Const *)lhs->getSubExp1())->getInt();
        if (((Location *)lhs)->getProc()) {
            type = SizeType::get(((Location *)lhs)->getProc()->getProg()->getRegSize(n));
        }
    }
}
Assignment::Assignment(SharedType ty, Exp * lhs) : TypingStatement(ty), lhs(lhs) {}
Assignment::~Assignment() {}

Assign::Assign(Exp * lhs, Exp * r, Exp * guard) : Assignment(lhs), rhs(r), guard(guard) { Kind = STMT_ASSIGN; }

Assign::Assign(SharedType ty, Exp * lhs, Exp * r, Exp * guard) : Assignment(ty, lhs), rhs(r), guard(guard) {
    Kind = STMT_ASSIGN;
}
Assign::Assign(Assign & o) : Assignment(lhs->clone()) {
    Kind = STMT_ASSIGN;
    rhs = o.rhs->clone();
    type = nullptr;
    guard = nullptr;
    if (o.type)
        type = o.type->clone();
    if (o.guard)
        guard = o.guard->clone();
}

// Implicit Assignment
//! Constructor and subexpression
ImplicitAssign::ImplicitAssign(Exp * lhs) : Assignment(lhs) { Kind = STMT_IMPASSIGN; }
//! Constructor, type, and subexpression
ImplicitAssign::ImplicitAssign(SharedType ty, Exp * lhs) : Assignment(ty, lhs) { Kind = STMT_IMPASSIGN; }
ImplicitAssign::ImplicitAssign(ImplicitAssign & o) : Assignment(o.type ? o.type->clone() : nullptr, o.lhs->clone()) {
    Kind = STMT_IMPASSIGN;
}
// The first virtual function (here the destructor) can't be in statement.h file for gcc
ImplicitAssign::~ImplicitAssign() {}

Instruction * Assign::clone() const {
    Assign *a = new Assign(type == nullptr ? nullptr : type->clone(), lhs->clone(), rhs->clone(),
                           guard == nullptr ? nullptr : guard->clone());
    // Statement members
    a->Parent = Parent;
    a->proc = proc;
    a->Number = Number;
    return a;
}

Instruction * PhiAssign::clone() const {
    PhiAssign *pa = new PhiAssign(type, lhs);
    Definitions::const_iterator dd;
    for (dd = DefVec.begin(); dd != DefVec.end(); dd++) {
        PhiInfo pi;
        pi.def((Instruction *)dd->second.def()); // Don't clone the Statement pointer (never moves)
        pi.e = dd->second.e->clone();          // Do clone the expression pointer
        assert(pi.e);
        pa->DefVec.insert(std::make_pair(dd->first, pi));
    }
    return pa;
}

Instruction * ImplicitAssign::clone() const {
    ImplicitAssign *ia = new ImplicitAssign(type, lhs);
    return ia;
}

// visit this Statement
bool Assign::accept(StmtVisitor * visitor) { return visitor->visit(this); }
bool PhiAssign::accept(StmtVisitor * visitor) { return visitor->visit(this); }
bool ImplicitAssign::accept(StmtVisitor * visitor) { return visitor->visit(this); }

void Assign::simplify() {
    // simplify arithmetic of assignment
    OPER leftop = lhs->getOper();
    if (Boomerang::get()->noBranchSimplify) {
        if (leftop == opZF || leftop == opCF || leftop == opOF || leftop == opNF)
            return;
    }

    lhs = lhs->simplifyArith();
    rhs = rhs->simplifyArith();
    if (guard)
        guard = guard->simplifyArith();
    // simplify the resultant expression
    lhs = lhs->simplify();
    rhs = rhs->simplify();
    if (guard)
        guard = guard->simplify();

    // Perhaps the guard can go away
    if (guard && (guard->isTrue() || (guard->isIntConst() && ((Const *)guard)->getInt() == 1)))
        guard = nullptr; // No longer a guarded assignment

    if (lhs->getOper() == opMemOf) {
        lhs->setSubExp1(lhs->getSubExp1()->simplifyArith());
    }

// this hack finds address constants.. it should go away when Mike writes some decent type analysis.
#if 0
if (DFA_TYPE_ANALYSIS) return;
if (lhs->getOper() == opMemOf && lhs->getSubExp1()->getOper() == opSubscript) {
    RefExp *ref = (RefExp*)lhs->getSubExp1();
    Statement *phist = ref->getDef();
    PhiAssign *phi = nullptr;
    if (phist /* && phist->getRight() */)        // ?
        phi = dynamic_cast<PhiAssign*>(phist);
    for (int i = 0; phi && i < phi->getNumDefs(); i++) {
        if (phi->getStmtAt(i)) {
            Assign *def = dynamic_cast<Assign*>(phi->getStmtAt(i));
            // Look for rX{-} - K or K
            if (def && (def->rhs->isIntConst() ||
                        (def->rhs->getOper() == opMinus &&
                         def->rhs->getSubExp1()->isSubscript() &&
                         ((RefExp*)def->rhs->getSubExp1())->isImplicitDef() &&
                         def->rhs->getSubExp1()->getSubExp1()->isRegOf() &&
                         def->rhs->getSubExp2()->isIntConst()))) {
                Exp *ne = new Unary(opAddrOf, Location::memOf(def->rhs, proc));
                if (VERBOSE)
                    LOG << "replacing " << def->rhs << " with " << ne << " in " << def << "\n";
                def->rhs = ne;
            }
            if (def && def->rhs->getOper() == opAddrOf &&
                    def->rhs->getSubExp1()->getOper() == opSubscript &&
                    def->rhs->getSubExp1()->getSubExp1()->getOper() == opGlobal &&
                    // MVE: opPhi!!
                    rhs->getOper() != opPhi && rhs->getOper() != opItof &&
                    rhs->getOper() != opFltConst) {
                SharedType ty = proc->getProg()->getGlobalType(
                            ((Const*)def->rhs->getSubExp1()->
                             getSubExp1()->
                             getSubExp1())->getStr());
                if (ty && ty->isArray()) {
                    SharedType bty = ((ArrayType*)ty)->getBaseType();
                    if (bty->isFloat()) {
                        if (VERBOSE)
                            LOG << "replacing " << rhs << " with ";
                        rhs = new Ternary(opItof, Const::get(32), Const::get(bty->getSize()), rhs);
                        if (VERBOSE)
                            LOG << rhs << " (assign indicates float type)\n";
                    }
                }
            }
        }
    }
}
#endif
}

void Assign::simplifyAddr() {
    lhs = lhs->simplifyAddr();
    rhs = rhs->simplifyAddr();
}

void Assignment::simplifyAddr() { lhs = lhs->simplifyAddr(); }

void Assign::fixSuccessor() {
    lhs = lhs->fixSuccessor();
    rhs = rhs->fixSuccessor();
}

void Assignment::print(QTextStream &os, bool html) const {
    os << qSetFieldWidth(4) << Number << qSetFieldWidth(0) << " ";
    if (html) {
        os << "</td><td>";
        os << "<a name=\"stmt" << Number << "\">";
    }
    printCompact(os, html);
    if (html)
        os << "</a>";
// PassManager::printPassData(this,os,html);
//    if (!Ranges.empty()) {
//        os << "\n\t\t\tranges: ";
//        Ranges.print(os);
//    }
}
void Assign::printCompact(QTextStream & os, bool html) const {
    os << "*" << type << "* ";
    if (guard)
        os << guard << " => ";
    if (lhs)
        lhs->print(os, html);
    os << " := ";
    if (rhs)
        rhs->print(os, html);
}
void PhiAssign::printCompact(QTextStream & os, bool html) const {
    os << "*" << type << "* ";
    if (lhs)
        lhs->print(os, html);
    os << " := phi";
    // Print as lhs := phi{9 17} for the common case where the lhs is the same location as all the referenced
    // locations. When not, print as local4 := phi(r24{9} argc{17})
    bool simple = true;
    for (const auto &v : DefVec) {
        assert(v.second.e != nullptr);
        // If e is nullptr assume it is meant to match lhs
        if (!(*v.second.e == *lhs)) {
            // One of the phi parameters has a different base expression to lhs. Use non simple print.
            simple = false;
            break;
        }
    }
    if (simple) {
        os << "{" ;
        for (auto it = DefVec.begin(); it != DefVec.end(); /* no increment */) {
            if (it->second.def()) {
                if (html)
                    os << "<a href=\"#stmt" << it->second.def()->getNumber() << "\">";
                os << it->second.def()->getNumber();
                if (html)
                    os << "</a>";
            } else
                os << "-";
            if (++it != DefVec.end())
                os << " ";
        }
        os << "}";
    } else {
        os << "(";
        for (auto it = DefVec.begin(); it != DefVec.end(); /* no increment */) {
            Exp *e = it->second.e;
            if (e == nullptr)
                os << "nullptr{";
            else
                os << e << "{";
            if (it->second.def())
                os << it->second.def()->getNumber();
            else
                os << "-";
            os << "}";
            if (++it != DefVec.end())
                os << " ";
        }
        os << ")";
    }
}
void ImplicitAssign::printCompact(QTextStream & os, bool html) const {
    os << "*" << type << "* ";
    if (lhs)
        lhs->print(os, html);
    os << " := -";
}

// All the Assignment-derived classes have the same definitions: the lhs
void Assignment::getDefinitions(LocationSet & defs) {
    if (lhs->getOper() == opAt) // foo@[m:n] really only defines foo
        defs.insert(((Ternary *)lhs)->getSubExp1());
    else
        defs.insert(lhs);
    // Special case: flag calls define %CF (and others)
    if (lhs->isFlags()) {
        defs.insert(Terminal::get(opCF));
        defs.insert(Terminal::get(opZF));
    }
}

bool Assign::search(const Exp &search, Exp *&result) {
    if (lhs->search(search, result))
        return true;
    return rhs->search(search, result);
}
bool PhiAssign::search(const Exp &search, Exp *&result) {
    if (lhs->search(search, result))
        return true;

    for (auto &v : DefVec) {
        assert(v.second.e != nullptr);
        // Note: can't match foo{-} because of this
        RefExp re(v.second.e, v.second.def());
        if (re.search(search, result))
            return true;
    }
    return false;
}
bool ImplicitAssign::search(const Exp &search, Exp *&result) { return lhs->search(search, result); }

bool Assign::searchAll(const Exp &search, std::list<Exp *> &result) {
    bool res;
    std::list<Exp *> leftResult;
    std::list<Exp *>::iterator it;
    res = lhs->searchAll(search, leftResult);
    // Ugh: searchAll clears the list!
    res |= rhs->searchAll(search, result);
    for (it = leftResult.begin(); it != leftResult.end(); it++)
        result.push_back(*it);
    return res;
}
// FIXME: is this the right semantics for searching a phi statement, disregarding the RHS?
bool PhiAssign::searchAll(const Exp &search, std::list<Exp *> &result) { return lhs->searchAll(search, result); }
bool ImplicitAssign::searchAll(const Exp &search, std::list<Exp *> &result) {
    return lhs->searchAll(search, result);
}

bool Assign::searchAndReplace(const Exp &search, Exp *replace, bool /*cc*/) {
    bool chl, chr, chg = false;
    lhs = lhs->searchReplaceAll(search, replace, chl);
    rhs = rhs->searchReplaceAll(search, replace, chr);
    if (guard)
        guard = guard->searchReplaceAll(search, replace, chg);
    return chl | chr | chg;
}
bool PhiAssign::searchAndReplace(const Exp &search, Exp *replace, bool /*cc*/) {
    bool change;
    lhs = lhs->searchReplaceAll(search, replace, change);

    for (auto &v : DefVec) {
        assert(v.second.e != nullptr);
        bool ch;
        // Assume that the definitions will also be replaced
        v.second.e = v.second.e->searchReplaceAll(search, replace, ch);
        assert(v.second.e);
        change |= ch;
    }
    return change;
}
bool ImplicitAssign::searchAndReplace(const Exp &search, Exp *replace, bool cc) {
    Q_UNUSED(cc);
    bool change;
    lhs = lhs->searchReplaceAll(search, replace, change);
    return change;
}

void Assign::generateCode(HLLCode * hll, BasicBlock * pbb, int indLevel) {
    Q_UNUSED(pbb);
    hll->AddAssignmentStatement(indLevel, this);
}

int Assign::getMemDepth() {
    int d1 = lhs->getMemDepth();
    int d2 = rhs->getMemDepth();
    if (d1 >= d2)
        return d1;
    return d2;
}

// PhiExp and ImplicitExp:
bool Assignment::usesExp(const Exp &e) {
    Exp *where = nullptr;
    return (lhs->isMemOf() || lhs->isRegOf()) && ((Unary *)lhs)->getSubExp1()->search(e, where);
}

bool Assign::usesExp(const Exp &e) {
    Exp *where = nullptr;
    return (rhs->search(e, where) ||
            ((lhs->isMemOf() || lhs->isRegOf()) && ((Unary *)lhs)->getSubExp1()->search(e, where)));
}

#if 0
bool Assign::match(const QString &pattern, std::map<QString, Exp*> &bindings) {
if (strstr(pattern, ":=") == nullptr)
    return false;
QString left = pattern;
char *right = strstr(left, ":=");
*right++ = 0;
right++;
while (*right == ' ')
    right++;
char *endleft = left + strlen(left) - 1;
while (*endleft == ' ') {
    *endleft = 0;
    endleft--;
}

return lhs->match(left, bindings) && rhs->match(right, bindings);
}

void addPhiReferences(InstructionSet &stmts, Statement *def);

void addSimpleCopyReferences(InstructionSet &stmts, Statement *def) {
if (!(*((Assign*)def)->getLeft() == *((Assign*)def)->getRight()->getSubExp1()))
    return;
Statement *copy = ((RefExp*)((Assign*)def)->getRight())->getDef();
if (!stmts.exists(copy)) {
    stmts.insert(copy);
    if (copy->isPhi())
        addPhiReferences(stmts, copy);
    else if (copy->isAssign() && ((Assign*)copy)->getRight()->isSubscript())
        addSimpleCopyReferences(stmts, copy);
}
}

void addPhiReferences(InstructionSet &stmts, Statement *def) {
PhiAssign *p = (PhiAssign*)def;
for (PhiAssign::iterator it = p->begin(); it != p->end(); it++) {
    if ((*it).def->isPhi() && !stmts.exists((*it).def)) {
        stmts.insert((*it).def);
        addPhiReferences(stmts, (*it).def);
    } else if ((*it).def->isAssign() && ((Assign*)(*it).def)->getRight()->isSubscript()) {
        stmts.insert((*it).def);
        addSimpleCopyReferences(stmts, (*it).def);
    } else
        stmts.insert((*it).def);
}
}
#endif

void Assignment::genConstraints(LocationSet & cons) {
    // Almost every assignment has at least a size from decoding
    // MVE: do/will PhiAssign's have a valid type? Why not?
    if (type)
        cons.insert(Binary::get(opEquals, new Unary(opTypeOf, new RefExp(lhs, this)), new TypeVal(type)));
}

// generate constraints
void Assign::genConstraints(LocationSet & cons) {
    Assignment::genConstraints(cons); // Gen constraint for the LHS
    Exp *con = rhs->genConstraints(new Unary(opTypeOf, new RefExp(lhs->clone(), this)));
    if (con)
        cons.insert(con);
}

void PhiAssign::genConstraints(LocationSet & cons) {
    // Generate a constraints st that all the phi's have to be the same type as
    // result
    Exp *result = new Unary(opTypeOf, new RefExp(lhs, this));

    for (auto &v : DefVec) {
        assert(v.second.e != nullptr);
        Exp *conjunct = Binary::get(opEquals, result, new Unary(opTypeOf, new RefExp(v.second.e, v.second.def())));
        cons.insert(conjunct);
    }
}

PhiInfo &PhiAssign::getAt(BasicBlock * idx) { return DefVec[idx]; }

void CallStatement::genConstraints(LocationSet & cons) {
    Function *dest = getDestProc();
    if (dest == nullptr)
        return;
    Signature *destSig = dest->getSignature();
    // Generate a constraint for the type of each actual argument to be equal to the type of each formal parameter
    // (hopefully, these are already calculated correctly; if not, we need repeat till no change)
    StatementList::iterator aa;
    int p = 0;
    for (aa = arguments.begin(); aa != arguments.end(); ++aa, ++p) {
        Exp *arg = ((Assign *)*aa)->getRight();
        // Handle a[m[x]]
        if (arg->isAddrOf()) {
            Exp *sub = arg->getSubExp1();
            if (sub->isSubscript())
                sub = ((RefExp *)sub)->getSubExp1();
            if (sub->isMemOf())
                arg = ((Location *)sub)->getSubExp1();
        }
        if (arg->isRegOf() || arg->isMemOf() || arg->isSubscript() || arg->isLocal() || arg->isGlobal()) {
            Exp *con = Binary::get(opEquals, new Unary(opTypeOf, arg->clone()),
                                   new TypeVal(destSig->getParamType(p)->clone()));
            cons.insert(con);
        }
    }

    if (dest->isLib()) {
        // A library procedure... check for two special cases
        QString name = dest->getName();
        // Note: might have to chase back via a phi statement to get a sample
        // string
        QString str;
        Exp *arg0 = ((Assign *)*arguments.begin())->getRight();
        if ((name == "printf" || name == "scanf") && !(str = arg0->getAnyStrConst()).isNull()) {
            // actually have to parse it
            int n = 1; // Number of %s plus 1 = number of args
            int percent_idx=0;
            while ((percent_idx = str.indexOf('%',percent_idx))!=-1) {
                percent_idx++;
                SharedType t = nullptr;
                int longness = 0;
                bool sign = true;
                bool cont;
                do {
                    cont = false;
                    switch (str[percent_idx].toLatin1()) {
                    case 'u':
                        sign = false;
                        cont = true;
                        break;
                    case 'x':
                        sign = false;
                    // Fall through
                    case 'i':
                    case 'd': {
                        int size = 32;
                        // Note: the following only works for 32 bit code or where sizeof(long) == sizeof(int)
                        if (longness == 2)
                            size = 64;
                        t = IntegerType::get(size, sign);
                        break;
                    }
                    case 'f':
                    case 'g':
                        t = FloatType::get(64);
                        break;
                    case 's':
                        t = PointerType::get(CharType::get());
                        break;
                    case 'l':
                        longness++;
                        cont = true;
                        break;
                    case '.':
                        cont = true;
                        break;
                    case '*':
                        assert(0); // Star format not handled yet
                    default:
                        if (str[percent_idx] >= '0' && str[percent_idx] <= '9')
                            cont = true;
                        break;
                    }
                    percent_idx++;
                } while (cont);
                if (t) {
                    // scanf takes addresses of these
                    if (name == "scanf")
                        t = PointerType::get(t);
                    // Generate a constraint for the parameter
                    TypeVal *tv = new TypeVal(t);
                    StatementList::iterator aa = arguments.begin();
                    std::advance(aa, n);
                    Exp *argn = ((Assign *)*aa)->getRight();
                    Exp *con = argn->genConstraints(tv);
                    cons.insert(con);
                }
                n++;
            }
        }
    }
}

void BranchStatement::genConstraints(LocationSet & cons) {
    if (pCond == nullptr) {
        if (VERBOSE)
            LOG << "Warning: BranchStatment " << Number << " has no condition expression!\n";
        return;
    }
    SharedType opsType;
    if (bFloat)
        opsType = FloatType::get(0);
    else
        opsType = IntegerType::get(0);
    if (jtCond == BRANCH_JUGE || jtCond == BRANCH_JULE || jtCond == BRANCH_JUG || jtCond == BRANCH_JUL) {
        assert(!bFloat);
        opsType->asInteger()->bumpSigned(-1);
    } else if (jtCond == BRANCH_JSGE || jtCond == BRANCH_JSLE || jtCond == BRANCH_JSG || jtCond == BRANCH_JSL) {
        assert(!bFloat);
        opsType->asInteger()->bumpSigned(+1);
    }

    // Constraints leading from the condition
    assert(pCond->getArity() == 2);
    Exp *a = ((Binary *)pCond)->getSubExp1();
    Exp *b = ((Binary *)pCond)->getSubExp2();
    // Generate constraints for a and b separately (if any).  Often only need a size, since we get basic type and
    // signedness from the branch condition (e.g. jump if unsigned less)
    Exp *Ta;
    Exp *Tb;
    if (a->isSizeCast()) {
        opsType->setSize(((Const *)((Binary *)a)->getSubExp1())->getInt());
        Ta = new Unary(opTypeOf, ((Binary *)a)->getSubExp2());
    } else
        Ta = new Unary(opTypeOf, a);
    if (b->isSizeCast()) {
        opsType->setSize(((Const *)((Binary *)b)->getSubExp1())->getInt());
        Tb = new Unary(opTypeOf, ((Binary *)b)->getSubExp2());
    } else
        Tb = new Unary(opTypeOf, b);
    // Constrain that Ta == opsType and Tb == opsType
    Exp *con = Binary::get(opEquals, Ta, new TypeVal(opsType));
    cons.insert(con);
    con = Binary::get(opEquals, Tb, new TypeVal(opsType));
    cons.insert(con);
}
//! Set or clear the constant subscripts (using a visitor)
int Instruction::setConscripts(int n) {
    StmtConscriptSetter scs(n, false);
    accept(&scs);
    return scs.getLast();
}

void Instruction::clearConscripts() {
    StmtConscriptSetter scs(0, true);
    accept(&scs);
}

// Cast the constant num to be of type ty. Return true if a change made
bool Instruction::castConst(int num, SharedType ty) {
    ExpConstCaster ecc(num, ty);
    StmtModifier scc(&ecc);
    accept(&scc);
    return ecc.isChanged();
}
//! Strip all size casts
void Instruction::stripSizes() {
    SizeStripper ss;
    StmtModifier sm(&ss);
    accept(&sm);
}

// Visiting from class StmtExpVisitor
// Visit all the various expressions in a statement
bool Assign::accept(StmtExpVisitor * v) {
    bool override;
    bool ret = v->visit(this, override);
    if (override)
        // The visitor has overridden this functionality.  This is needed for example in UsedLocFinder, where the
        // lhs of
        // an assignment is not used (but if it's m[blah], then blah is used)
        return ret;
    if (ret && lhs)
        ret = lhs->accept(v->ev);
    if (ret && rhs)
        ret = rhs->accept(v->ev);
    return ret;
}

bool PhiAssign::accept(StmtExpVisitor * visitor) {
    bool override;
    bool ret = visitor->visit(this, override);
    if (override)
        return ret;
    if (ret && lhs)
        ret = lhs->accept(visitor->ev);

    for (auto &v : DefVec) {
        assert(v.second.e != nullptr);
        //RefExp *re = new RefExp(v.second.e, v.second.def());
        RefExp re(v.second.e, v.second.def());
        ret = re.accept(visitor->ev);
        if (ret == false)
            return false;
    }
    return true;
}

bool ImplicitAssign::accept(StmtExpVisitor * v) {
    bool override;
    bool ret = v->visit(this, override);
    if (override)
        return ret;
    if (ret && lhs)
        ret = lhs->accept(v->ev);
    return ret;
}

bool GotoStatement::accept(StmtExpVisitor * v) {
    bool override;
    bool ret = v->visit(this, override);
    if (override)
        return ret;
    if (ret && pDest)
        ret = pDest->accept(v->ev);
    return ret;
}

bool BranchStatement::accept(StmtExpVisitor * v) {
    bool override;
    bool ret = v->visit(this, override);
    if (override)
        return ret;
    // Destination will always be a const for X86, so the below will never be used in practice
    if (ret && pDest)
        ret = pDest->accept(v->ev);
    if (ret && pCond)
        ret = pCond->accept(v->ev);
    return ret;
}

bool CaseStatement::accept(StmtExpVisitor * v) {
    bool override;
    bool ret = v->visit(this, override);
    if (override)
        return ret;
    if (ret && pDest)
        ret = pDest->accept(v->ev);
    if (ret && pSwitchInfo && pSwitchInfo->pSwitchVar)
        ret = pSwitchInfo->pSwitchVar->accept(v->ev);
    return ret;
}

bool CallStatement::accept(StmtExpVisitor * v) {
    bool override;
    bool ret = v->visit(this, override);
    if (override)
        return ret;
    if (ret && pDest)
        ret = pDest->accept(v->ev);
    StatementList::iterator it;
    for (it = arguments.begin(); ret && it != arguments.end(); it++)
        ret = (*it)->accept(v);
// FIXME: why aren't defines counted?
#if 0 // Do we want to accept visits to the defines? Not sure now...
std::vector<ReturnInfo>::iterator rr;
for (rr = defines.begin(); ret && rr != defines.end(); rr++)
    if (rr->e)            // Can be nullptr now to line up with other returns
        ret = rr->e->accept(v->ev);
#endif
    // FIXME: surely collectors should be counted?
    return ret;
}

bool ReturnStatement::accept(StmtExpVisitor * v) {
    bool override;
    ReturnStatement::iterator rr;
    if (!v->visit(this, override))
        return false;
    if (override)
        return true;
    if (!v->isIgnoreCol()) {
        DefCollector::iterator dd;
        for (dd = col.begin(); dd != col.end(); ++dd)
            if (!(*dd)->accept(v))
                return false;
        // EXPERIMENTAL: for now, count the modifieds as if they are a collector (so most, if not all of the time,
        // ignore them). This is so that we can detect better when a definition is used only once, and therefore
        // propagate anything to it
        for (rr = modifieds.begin(); rr != modifieds.end(); ++rr)
            if (!(*rr)->accept(v))
                return false;
    }
    for (rr = returns.begin(); rr != returns.end(); ++rr)
        if (!(*rr)->accept(v))
            return false;
    return true;
}

bool BoolAssign::accept(StmtExpVisitor * v) {
    bool override;
    bool ret = v->visit(this, override);
    if (override)
        return ret;
    if (ret && pCond)
        ret = pCond->accept(v->ev);
    return ret;
}

// Visiting from class StmtModifier
// Modify all the various expressions in a statement
bool Assign::accept(StmtModifier * v) {
    bool recur;
    v->visit(this, recur);
    v->mod->clearMod();
    if (recur)
        lhs = lhs->accept(v->mod);
    if (recur)
        rhs = rhs->accept(v->mod);
    if (VERBOSE && v->mod->isMod())
        LOG << "Assignment changed: now " << this << "\n";
    return true;
}
bool PhiAssign::accept(StmtModifier * v) {
    bool recur;
    v->visit(this, recur);
    v->mod->clearMod();
    if (recur)
        lhs = lhs->accept(v->mod);
    if (VERBOSE && v->mod->isMod())
        LOG << "PhiAssign changed: now " << this << "\n";
    return true;
}

bool ImplicitAssign::accept(StmtModifier * v) {
    bool recur;
    v->visit(this, recur);
    v->mod->clearMod();
    if (recur)
        lhs = lhs->accept(v->mod);
    if (VERBOSE && v->mod->isMod())
        LOG << "ImplicitAssign changed: now " << this << "\n";
    return true;
}

bool BoolAssign::accept(StmtModifier * v) {
    bool recur;
    v->visit(this, recur);
    if (pCond && recur)
        pCond = pCond->accept(v->mod);
    if (recur && lhs->isMemOf()) {
        ((Location *)lhs)->setSubExp1(((Location *)lhs)->getSubExp1()->accept(v->mod));
    }
    return true;
}

bool GotoStatement::accept(StmtModifier * v) {
    bool recur;
    v->visit(this, recur);
    if (pDest && recur)
        pDest = pDest->accept(v->mod);
    return true;
}

bool BranchStatement::accept(StmtModifier * v) {
    bool recur;
    v->visit(this, recur);
    if (pDest && recur)
        pDest = pDest->accept(v->mod);
    if (pCond && recur)
        pCond = pCond->accept(v->mod);
    return true;
}

bool CaseStatement::accept(StmtModifier * v) {
    bool recur;
    v->visit(this, recur);
    if (pDest && recur)
        pDest = pDest->accept(v->mod);
    if (pSwitchInfo && pSwitchInfo->pSwitchVar && recur)
        pSwitchInfo->pSwitchVar = pSwitchInfo->pSwitchVar->accept(v->mod);
    return true;
}

bool CallStatement::accept(StmtModifier * v) {
    bool recur;
    v->visit(this, recur);
    if (!recur)
        return true;
    if (pDest)
        pDest = pDest->accept(v->mod);
    StatementList::iterator it;
    for (it = arguments.begin(); recur && it != arguments.end(); it++)
        (*it)->accept(v);
    // For example: needed for CallBypasser so that a collected definition that happens to be another call gets
    // adjusted
    // I'm thinking no at present... let the bypass and propagate while possible logic take care of it, and leave
    // the
    // collectors as the rename logic set it
    // Well, sort it out with ignoreCollector()
    if (!v->ignoreCollector()) {
        DefCollector::iterator cc;
        for (cc = defCol.begin(); cc != defCol.end(); cc++)
            (*cc)->accept(v);
    }
    StatementList::iterator dd;
    for (dd = defines.begin(); recur && dd != defines.end(); ++dd)
        (*dd)->accept(v);
    return true;
}

bool ReturnStatement::accept(StmtModifier * v) {
    bool recur;
    v->visit(this, recur);
    if (!recur)
        return true;
    if (!v->ignoreCollector()) {
        DefCollector::iterator dd;
        for (dd = col.begin(); dd != col.end(); ++dd)
            if (!(*dd)->accept(v))
                return false;
    }
    ReturnStatement::iterator rr;
    for (rr = modifieds.begin(); rr != modifieds.end(); ++rr)
        if (!(*rr)->accept(v))
            return false;
    for (rr = returns.begin(); rr != returns.end(); ++rr)
        if (!(*rr)->accept(v))
            return false;
    return true;
}

// Visiting from class StmtPartModifier
// Modify all the various expressions in a statement, except for the top level of the LHS of assignments
bool Assign::accept(StmtPartModifier * v) {
    bool recur;
    v->visit(this, recur);
    v->mod->clearMod();
    if (recur && lhs->isMemOf()) {
        ((Location *)lhs)->setSubExp1(((Location *)lhs)->getSubExp1()->accept(v->mod));
    }
    if (recur)
        rhs = rhs->accept(v->mod);
    if (VERBOSE && v->mod->isMod())
        LOG << "Assignment changed: now " << this << "\n";
    return true;
}
bool PhiAssign::accept(StmtPartModifier * v) {
    bool recur;
    v->visit(this, recur);
    v->mod->clearMod();
    if (recur && lhs->isMemOf()) {
        ((Location *)lhs)->setSubExp1(((Location *)lhs)->getSubExp1()->accept(v->mod));
    }
    if (VERBOSE && v->mod->isMod())
        LOG << "PhiAssign changed: now " << this << "\n";
    return true;
}

bool ImplicitAssign::accept(StmtPartModifier * v) {
    bool recur;
    v->visit(this, recur);
    v->mod->clearMod();
    if (recur && lhs->isMemOf()) {
        ((Location *)lhs)->setSubExp1(((Location *)lhs)->getSubExp1()->accept(v->mod));
    }
    if (VERBOSE && v->mod->isMod())
        LOG << "ImplicitAssign changed: now " << this << "\n";
    return true;
}

bool GotoStatement::accept(StmtPartModifier * v) {
    bool recur;
    v->visit(this, recur);
    if (pDest && recur)
        pDest = pDest->accept(v->mod);
    return true;
}

bool BranchStatement::accept(StmtPartModifier * v) {
    bool recur;
    v->visit(this, recur);
    if (pDest && recur)
        pDest = pDest->accept(v->mod);
    if (pCond && recur)
        pCond = pCond->accept(v->mod);
    return true;
}

bool CaseStatement::accept(StmtPartModifier * v) {
    bool recur;
    v->visit(this, recur);
    if (pDest && recur)
        pDest = pDest->accept(v->mod);
    if (pSwitchInfo && pSwitchInfo->pSwitchVar && recur)
        pSwitchInfo->pSwitchVar = pSwitchInfo->pSwitchVar->accept(v->mod);
    return true;
}

bool CallStatement::accept(StmtPartModifier * v) {
    bool recur;
    v->visit(this, recur);
    if (pDest && recur)
        pDest = pDest->accept(v->mod);
    StatementList::iterator it;
    for (it = arguments.begin(); recur && it != arguments.end(); it++)
        (*it)->accept(v);
    // For example: needed for CallBypasser so that a collected definition that happens to be another call gets
    // adjusted
    // But now I'm thinking no, the bypass and propagate while possible logic should take care of it.
    // Then again, what about the use collectors in calls? Best to do it.
    if (!v->ignoreCollector()) {
        DefCollector::iterator dd;
        for (dd = defCol.begin(); dd != defCol.end(); dd++)
            (*dd)->accept(v);
        UseCollector::iterator uu;
        for (uu = useCol.begin(); uu != useCol.end(); ++uu)
            // I believe that these should never change at the top level, e.g. m[esp{30} + 4] -> m[esp{-} - 20]
            (*uu)->accept(v->mod);
    }
    StatementList::iterator dd;
    for (dd = defines.begin(); recur && dd != defines.end(); dd++)
        (*dd)->accept(v);
    return true;
}

bool ReturnStatement::accept(StmtPartModifier * v) {
    bool recur;
    v->visit(this, recur);
    ReturnStatement::iterator rr;
    for (rr = modifieds.begin(); rr != modifieds.end(); ++rr)
        if (!(*rr)->accept(v))
            return false;
    for (rr = returns.begin(); rr != returns.end(); ++rr)
        if (!(*rr)->accept(v))
            return false;
    return true;
}

bool BoolAssign::accept(StmtPartModifier * v) {
    bool recur;
    v->visit(this, recur);
    if (pCond && recur)
        pCond = pCond->accept(v->mod);
    if (lhs && recur)
        lhs = lhs->accept(v->mod);
    return true;
}

//! Fix references to the returns of call statements
//! Bypass calls for references in this statement
void Instruction::bypass() {
    CallBypasser cb(this);
    StmtPartModifier sm(&cb); // Use the Part modifier so we don't change the top level of LHS of assigns etc
    accept(&sm);
    if (cb.isTopChanged())
        simplify(); // E.g. m[esp{20}] := blah -> m[esp{-}-20+4] := blah
}

//! Find the locations used by expressions in this Statement.
//! Use the StmtExpVisitor and UsedLocsFinder visitor classes
//! Adds (inserts) all locations (registers or memory etc) used by this statement
//! Set \a cc to true to count the uses in collectors
//! \param used set of used locations
//! \param cc count collectors
//! \param memOnly - only add memory references.
void Instruction::addUsedLocs(LocationSet & used, bool cc /* = false */, bool memOnly /*= false */) {
    UsedLocsFinder ulf(used, memOnly);
    UsedLocsVisitor ulv(&ulf, cc);
    accept(&ulv);
}
//! Special version of Statement::addUsedLocs for finding used locations.
//! \return true if defineAll was found
bool Instruction::addUsedLocals(LocationSet & used) {
    UsedLocalFinder ulf(used, proc);
    UsedLocsVisitor ulv(&ulf, false);
    accept(&ulv);
    return ulf.wasAllFound();
}

//! For all expressions in this Statement, replace any e with e{def}
void Instruction::subscriptVar(Exp * e, Instruction * def /*, Cfg* cfg */) {
    ExpSubscripter es(e, def /*, cfg*/);
    StmtSubscripter ss(&es);
    accept(&ss);
}

//! Find all constants in this statement
void Instruction::findConstants(std::list<Const *> & lc) {
    ConstFinder cf(lc);
    StmtConstFinder scf(&cf);
    accept(&scf);
}

// Convert this PhiAssignment to an ordinary Assignment.  Hopefully, this is the only place that Statements change
// from
// one class to another.  All throughout the code, we assume that the addresses of Statement objects do not change,
// so we need this slight hack to overwrite one object with another
void PhiAssign::convertToAssign(Exp * rhs) {
    // I believe we always want to propagate to these ex-phi's; check!:
    rhs = rhs->propagateAll();
    // Thanks to tamlin for this cleaner way of implementing this hack
    assert(sizeof(Assign) <= sizeof(PhiAssign));
    int n = Number; // These items disappear with the destructor below
    BasicBlock *bb = Parent;
    UserProc *p = proc;
    Exp *lhs_ = lhs;
    Exp *rhs_ = rhs;
    SharedType type_ = type;
    this->~PhiAssign();                               // Explicitly destroy this, but keep the memory allocated.
    Assign *a = new (this) Assign(type_, lhs_, rhs_); // construct in-place. Note that 'a' == 'this'
    a->setNumber(n);
    a->setProc(p);
    a->setBB(bb);
}

void PhiAssign::simplify() {
    lhs = lhs->simplify();

    if (DefVec.empty())
        return;
    bool allSame = true;
    Definitions::iterator uu = DefVec.begin();
    Instruction *first = DefVec.begin()->second.def();
    ++uu;
    for (; uu != DefVec.end(); uu++) {
        if (uu->second.def() != first) {
            allSame = false;
            break;
        }
    }

    if (allSame) {
        LOG_VERBOSE(1) << "all the same in " << this << "\n";
        convertToAssign(new RefExp(lhs, first));
        return;
    }

    bool onlyOneNotThis = true;
    Instruction *notthis = (Instruction *)-1;
    for (auto &v : DefVec) {
        if (v.second.def() == nullptr || v.second.def()->isImplicit() || !v.second.def()->isPhi() ||
            v.second.def() != this) {
            if (notthis != (Instruction *)-1) {
                onlyOneNotThis = false;
                break;
            } else
                notthis = v.second.def();
        }
    }

    if (onlyOneNotThis && notthis != (Instruction *)-1) {
        if (VERBOSE)
            LOG << "all but one not this in " << this << "\n";
        convertToAssign(new RefExp(lhs, notthis));
        return;
    }
}

void PhiAssign::putAt(BasicBlock * i, Instruction * def, Exp * e) {
    assert(e); // should be something surely
    // assert(defVec.end()==defVec.find(i));
    DefVec[i].def(def);
    DefVec[i].e = e;
}

bool Assignment::definesLoc(Exp * loc) {
    if (lhs->getOper() == opAt) // For foo@[x:y], match of foo==loc OR whole thing == loc
        if (*((Ternary *)lhs)->getSubExp1() == *loc)
            return true;
    return *lhs == *loc;
}

bool CallStatement::definesLoc(Exp * loc) {
    StatementList::iterator dd;
    for (dd = defines.begin(); dd != defines.end(); ++dd) {
        Exp *lhs = ((Assign *)*dd)->getLeft();
        if (*lhs == *loc)
            return true;
    }
    return false;
}

// Does a ReturnStatement define anything? Not really, the locations are already defined earlier in the procedure.
// However, nothing comes after the return statement, so it doesn't hurt to pretend it does, and this is a place to
// store the return type(s) for example.
// FIXME: seems it would be cleaner to say that Return Statements don't define anything.
bool ReturnStatement::definesLoc(Exp * loc) {
    for (auto &elem : modifieds) {
        if ((elem)->definesLoc(loc))
            return true;
    }
    return false;
}

// FIXME: see above
void ReturnStatement::getDefinitions(LocationSet & ls) {
    for (auto &elem : modifieds)
        (elem)->getDefinitions(ls);
}

SharedType ReturnStatement::getTypeFor(Exp * e) {
    for (auto &elem : modifieds) {
        if (*((Assignment *)elem)->getLeft() == *e)
            return ((Assignment *)elem)->getType();
    }
    return nullptr;
}

void ReturnStatement::setTypeFor(Exp * e, SharedType ty) {
    for (auto &elem : modifieds) {
        if (*((Assignment *)elem)->getLeft() == *e) {
            ((Assignment *)elem)->setType(ty);
            break;
        }
    }
    for (auto &elem : returns) {
        if (*((Assignment *)elem)->getLeft() == *e) {
            ((Assignment *)elem)->setType(ty);
            return;
        }
    }
}

#define RETSTMT_COLS 120
void ReturnStatement::print(QTextStream & os, bool html) const {
    os << qSetFieldWidth(4) << Number << qSetFieldWidth(0) <<" ";
    if (html) {
        os << "</td><td>";
        os << "<a name=\"stmt" << Number << "\">";
    }
    os << "RET";
    bool first = true;
    unsigned column = 19;
    for (auto const &elem : returns) {
        QString tgt;
        QTextStream ost(&tgt);
        ((const Assignment *)elem)->printCompact(ost, html);
        unsigned len = tgt.length();
        if (first) {
            first = false;
            os << " ";
        } else if (column + 4 + len > RETSTMT_COLS) { // 4 for command 3 spaces
            if (column != RETSTMT_COLS - 1)
                os << ","; // Comma at end of line
            os << "\n                ";
            column = 16;
        } else {
            os << ",   ";
            column += 4;
        }
        os << tgt;
        column += len;
    }
    if (html)
        os << "</a><br>";
    else
        os << "\n              ";
    os << "Modifieds: ";
    first = true;
    column = 25;
    for (auto const &elem : modifieds) {
        QString tgt2;
        QTextStream ost(&tgt2);
        const Assignment *as = (const Assignment *)elem;
        const SharedType ty = as->getType();
        if (ty)
            ost << "*" << ty << "* ";
        ost << as->getLeft();
        unsigned len = tgt2.length();
        if (first)
            first = false;
        else if (column + 3 + len > RETSTMT_COLS) { // 3 for comma and 2 spaces
            if (column != RETSTMT_COLS - 1)
                os << ","; // Comma at end of line
            os << "\n                ";
            column = 16;
        } else {
            os << ",  ";
            column += 3;
        }
        os << tgt2;
        column += len;
    }
#if 1
    // Collected reaching definitions
    if (html)
        os << "<br>";
    else
        os << "\n              ";
    os << "Reaching definitions: ";
    col.print(os, html);
#endif
}

// A helper class for comparing Assignment*'s sensibly
bool lessAssignment::operator()(const Assignment *x, const Assignment *y) const {
    Assignment *xx = const_cast<Assignment *>(x);
    Assignment *yy = const_cast<Assignment *>(y);
    return (*xx->getLeft() < *yy->getLeft()); // Compare the LHS expressions
}

// Repeat the above for Assign's; sometimes the compiler doesn't (yet) understand that Assign's are Assignment's
bool lessAssign::operator()(const Assign *x, const Assign *y) const {
    Assign *xx = const_cast<Assign *>(x);
    Assign *yy = const_cast<Assign *>(y);
    return (*xx->getLeft() < *yy->getLeft()); // Compare the LHS expressions
}

// Update the modifieds, in case the signature and hence ordering and filtering has changed, or the locations in the
// collector have changed. Does NOT remove preserveds (deferred until updating returns).
void ReturnStatement::updateModifieds() {
    Signature *sig = proc->getSignature();
    StatementList oldMods(modifieds); // Copy the old modifieds
    modifieds.clear();

    if (Parent->getNumInEdges() == 1 && Parent->getInEdges()[0]->getLastStmt()->isCall()) {
        CallStatement *call = (CallStatement *)Parent->getInEdges()[0]->getLastStmt();
        if (call->getDestProc() && FrontEnd::noReturnCallDest(call->getDestProc()->getName()))
            return;
    }
    // For each location in the collector, make sure that there is an assignment in the old modifieds, which will
    // be filtered and sorted to become the new modifieds
    // Ick... O(N*M) (N existing modifeds, M collected locations)
    DefCollector::iterator ll;
    StatementList::iterator it;
    for (ll = col.begin(); ll != col.end(); ++ll) {
        bool found = false;
        Assign *as = (Assign *)*ll;
        Exp *colLhs = as->getLeft();
        if (proc->filterReturns(colLhs))
            continue; // Filtered out
        for (it = oldMods.begin(); it != oldMods.end(); it++) {
            Exp *lhs = ((Assignment *)*it)->getLeft();
            if (*lhs == *colLhs) {
                found = true;
                break;
            }
        }
        if (!found) {
            ImplicitAssign *ias = new ImplicitAssign(as->getType()->clone(), as->getLeft()->clone());
            ias->setProc(proc); // Comes from the Collector
            ias->setBB(Parent);
            oldMods.append(ias);
        }
    }

    // Mostly the old modifications will be in the correct order, and inserting will be fastest near the start of
    // the
    // new list. So read the old modifications in reverse order
    for (it = oldMods.end(); it != oldMods.begin();) {
        --it; // Becuase we are using a forwards iterator backwards
        // Make sure the LHS is still in the collector
        Assignment *as = (Assignment *)*it;
        Exp *lhs = as->getLeft();
        if (!col.existsOnLeft(lhs))
            continue; // Not in collector: delete it (don't copy it)
        if (proc->filterReturns(lhs))
            continue; // Filtered out: delete it

        // Insert as, in order, into the existing set of modifications
        StatementList::iterator nn;
        bool inserted = false;
        for (nn = modifieds.begin(); nn != modifieds.end(); ++nn) {
            if (sig->returnCompare(*as, *(Assignment *)*nn)) { // If the new assignment is less than the current one
                nn = modifieds.insert(nn, as);             // then insert before this position
                inserted = true;
                break;
            }
        }
        if (!inserted)
            modifieds.insert(modifieds.end(), as); // In case larger than all existing elements
    }
}

// Update the returns, in case the signature and hence ordering and filtering has changed, or the locations in the
// modifieds list
void ReturnStatement::updateReturns() {
    Signature *sig = proc->getSignature();
    int sp = sig->getStackRegister();
    StatementList oldRets(returns); // Copy the old returns
    returns.clear();
    // For each location in the modifieds, make sure that there is an assignment in the old returns, which will
    // be filtered and sorted to become the new returns
    // Ick... O(N*M) (N existing returns, M modifieds locations)
    StatementList::iterator dd, it;
    for (dd = modifieds.begin(); dd != modifieds.end(); ++dd) {
        bool found = false;
        Exp *loc = ((Assignment *)*dd)->getLeft();
        if (proc->filterReturns(loc))
            continue; // Filtered out
        // Special case for the stack pointer: it has to be a modified (otherwise, the changes will bypass the
        // calls),
        // but it is not wanted as a return
        if (loc->isRegN(sp))
            continue;
        for (it = oldRets.begin(); it != oldRets.end(); it++) {
            Exp *lhs = ((Assign *)*it)->getLeft();
            if (*lhs == *loc) {
                found = true;
                break;
            }
        }
        if (!found) {
            Exp *rhs = col.findDefFor(loc); // Find the definition that reaches the return statement's collector
            Assign *as = new Assign(loc->clone(), rhs->clone());
            as->setProc(proc);
            as->setBB(Parent);
            oldRets.append(as);
        }
    }

    // Mostly the old returns will be in the correct order, and inserting will be fastest near the start of the
    // new list. So read the old returns in reverse order
    for (it = oldRets.end(); it != oldRets.begin();) {
        --it; // Becuase we are using a forwards iterator backwards
        // Make sure the LHS is still in the modifieds
        Assign *as = (Assign *)*it;
        Exp *lhs = as->getLeft();
        if (!modifieds.existsOnLeft(lhs))
            continue; // Not in modifieds: delete it (don't copy it)
        if (proc->filterReturns(lhs))
            continue; // Filtered out: delete it
#if 1
        // Preserveds are NOT returns (nothing changes, so what are we returning?)
        // Check if it is a preserved location, e.g. r29 := r29{-}
        Exp *rhs = as->getRight();
        if (rhs->isSubscript() && ((RefExp *)rhs)->isImplicitDef() && *((RefExp *)rhs)->getSubExp1() == *lhs)
            continue; // Filter out the preserveds
#endif

        // Insert as, in order, into the existing set of returns
        StatementList::iterator nn;
        bool inserted = false;
        for (nn = returns.begin(); nn != returns.end(); ++nn) {
            if (sig->returnCompare(*as, *(Assign *)*nn)) { // If the new assignment is less than the current one
                nn = returns.insert(nn, as);               // then insert before this position
                inserted = true;
                break;
            }
        }
        if (!inserted)
            returns.insert(returns.end(), as); // In case larger than all existing elements
    }
}

// Set the defines to the set of locations modified by the callee, or if no callee, to all variables live at this
// call
void CallStatement::updateDefines() {
    Signature *sig;
    if (procDest)
        // The signature knows how to order the returns
        sig = procDest->getSignature();
    else
        // Else just use the enclosing proc's signature
        sig = proc->getSignature();

    if (procDest && procDest->isLib()) {
        sig->setLibraryDefines(&defines); // Set the locations defined
        return;
    } else if (Boomerang::get()->assumeABI) {
        // Risky: just assume the ABI caller save registers are defined
        Signature::setABIdefines(proc->getProg(), &defines);
        return;
    }

    // Move the defines to a temporary list
    StatementList oldDefines(defines); // Copy the old defines
    StatementList::iterator it;
    defines.clear();

    if (procDest && calleeReturn) {
        StatementList &modifieds = ((UserProc *)procDest)->getModifieds();
        for (Instruction *mm : modifieds) {
            Assignment *as = (Assignment *)mm;
            Exp *loc = as->getLeft();
            if (proc->filterReturns(loc))
                continue;
            SharedType ty = as->getType();
            if (!oldDefines.existsOnLeft(loc))
                oldDefines.append(new ImplicitAssign(ty, loc));
        }
    } else {
        // Ensure that everything in the UseCollector has an entry in oldDefines
        LocationSet::iterator ll;
        for (ll = useCol.begin(); ll != useCol.end(); ++ll) {
            Exp *loc = *ll;
            if (proc->filterReturns(loc))
                continue; // Filtered out
            if (!oldDefines.existsOnLeft(loc)) {
                ImplicitAssign *as = new ImplicitAssign(loc->clone());
                as->setProc(proc);
                as->setBB(Parent);
                oldDefines.append(as);
            }
        }
    }

    for (it = oldDefines.end(); it != oldDefines.begin();) {
        --it; // Becuase we are using a forwards iterator backwards
        // Make sure the LHS is still in the return or collector
        Assignment *as = (Assignment *)*it;
        Exp *lhs = as->getLeft();
        if (calleeReturn) {
            if (!calleeReturn->definesLoc(lhs))
                continue; // Not in callee returns
        } else {
            if (!useCol.exists(lhs))
                continue; // Not in collector: delete it (don't copy it)
        }
        if (proc->filterReturns(lhs))
            continue; // Filtered out: delete it

        // Insert as, in order, into the existing set of definitions
        StatementList::iterator nn;
        bool inserted = false;
        for (nn = defines.begin(); nn != defines.end(); ++nn) {
            if (sig->returnCompare(*as, *(Assignment *)*nn)) { // If the new assignment is less than the current one
                nn = defines.insert(nn, as);               // then insert before this position
                inserted = true;
                break;
            }
        }
        if (!inserted)
            defines.push_back(as); // In case larger than all existing elements
    }
}

// A helper class for updateArguments. It just dishes out a new argument from one of the three sources: the
// signature,
// the callee parameters, or the defCollector in the call
class ArgSourceProvider {
    enum Src { SRC_LIB, SRC_CALLEE, SRC_COL };
    Src src;
    CallStatement *call;
    int i, n; // For SRC_LIB
    Signature *callSig;
    StatementList::iterator pp; // For SRC_CALLEE
    StatementList *calleeParams;
    DefCollector::iterator cc; // For SRC_COL
    DefCollector *defCol;

  public:
    ArgSourceProvider(CallStatement *call);
    Exp *nextArgLoc();     // Get the next location (not subscripted)
    SharedType curType(Exp *e); // Get the current location's type
    bool exists(Exp *loc); // True if the given location (not subscripted) exists as a source
    Exp *localise(Exp *e); // Localise to this call if necessary
};

ArgSourceProvider::ArgSourceProvider(CallStatement * _call) : call(_call) {
    Function *procDest = call->getDestProc();
    if (procDest && procDest->isLib()) {
        src = SRC_LIB;
        callSig = call->getSignature();
        n = callSig->getNumParams();
        i = 0;
    } else if (call->getCalleeReturn() != nullptr) {
        src = SRC_CALLEE;
        calleeParams = &((UserProc *)procDest)->getParameters();
        pp = calleeParams->begin();
    } else {
        Signature *destSig = nullptr;
        if (procDest)
            destSig = procDest->getSignature();
        if (destSig && destSig->isForced()) {
            src = SRC_LIB;
            callSig = destSig;
            n = callSig->getNumParams();
            i = 0;
        } else {
            src = SRC_COL;
            defCol = call->getDefCollector();
            cc = defCol->begin();
        }
    }
}

Exp *ArgSourceProvider::nextArgLoc() {
    Exp *s;
    bool allZero;
    switch (src) {
    case SRC_LIB:
        if (i == n)
            return nullptr;
        s = callSig->getParamExp(i++)->clone();
        s->removeSubscripts(allZero); // e.g. m[sp{-} + 4] -> m[sp + 4]
        call->localiseComp(s);
        return s;
    case SRC_CALLEE:
        if (pp == calleeParams->end())
            return nullptr;
        s = ((Assignment *)*pp++)->getLeft()->clone();
        s->removeSubscripts(allZero);
        call->localiseComp(s); // Localise the components. Has the effect of translating into
        // the contect of this caller
        return s;
    case SRC_COL:
        if (cc == defCol->end())
            return nullptr;
        // Give the location, i.e. the left hand side of the assignment
        return ((Assign *)*cc++)->getLeft();
    }
    return nullptr; // Suppress warning
}

Exp *ArgSourceProvider::localise(Exp * e) {
    if (src == SRC_COL) {
        // Provide the RHS of the current assignment
        Exp *ret = ((Assign *)*--cc)->getRight();
        ++cc;
        return ret;
    }
    // Else just use the call to localise
    return call->localiseExp(e);
}

SharedType ArgSourceProvider::curType(Exp * e) {
    Q_UNUSED(e);
    switch (src) {
    case SRC_LIB:
        return callSig->getParamType(i - 1);
    case SRC_CALLEE: {
        SharedType ty = ((Assignment *)*--pp)->getType();
        pp++;
        return ty;
    }
    case SRC_COL: {
        // Mostly, there won't be a type here, I would think...
        SharedType ty = (*--cc)->getType();
        ++cc;
        return ty;
    }
    }
    return nullptr; // Suppress warning
}

bool ArgSourceProvider::exists(Exp * e) {
    bool allZero;
    switch (src) {
    case SRC_LIB:
        if (callSig->hasEllipsis())
            // FIXME: for now, just don't check
            return true;
        for (i = 0; i < n; i++) {
            Exp *sigParam = callSig->getParamExp(i)->clone();
            sigParam->removeSubscripts(allZero);
            call->localiseComp(sigParam);
            if (*sigParam == *e)
                return true;
        }
        return false;
    case SRC_CALLEE:
        for (pp = calleeParams->begin(); pp != calleeParams->end(); ++pp) {
            Exp *par = ((Assignment *)*pp)->getLeft()->clone();
            par->removeSubscripts(allZero);
            call->localiseComp(par);
            if (*par == *e)
                return true;
        }
        return false;
    case SRC_COL:
        return defCol->existsOnLeft(e);
    }
    return false; // Suppress warning
}

void CallStatement::updateArguments() {
    /* If this is a library call, source = signature
                else if there is a callee return, source = callee parameters
                else
                  if a forced callee signature, source = signature
                  else source is def collector in this call.
                oldArguments = arguments
                clear arguments
                for each arg lhs in source
                        if exists in oldArguments, leave alone
                        else if not filtered append assignment lhs=lhs to oldarguments
                for each argument as in oldArguments in reverse order
                        lhs = as->getLeft
                        if (lhs does not exist in source) continue
                        if filterParams(lhs) continue
                        insert as into arguments, considering sig->argumentCompare
        */
    // Note that if propagations are limited, arguments and collected reaching definitions can be in terms of phi
    // statements that have since been translated to assignments. So propagate through them now
    // FIXME: reconsider! There are problems (e.g. with test/pentium/fromSSA2, test/pentium/fbranch) if you
    // propagate
    // to the expressions in the arguments (e.g. m[esp{phi1}-20]) but don't propagate into ordinary statements that
    // define the actual argument. For example, you might have m[esp{-}-56] in the call, but the actual definition
    // of
    // the printf argument is still m[esp{phi1} -20] = "%d".
    if (EXPERIMENTAL) {
        bool convert;
        proc->propagateStatements(convert, 88);
    }
    StatementList oldArguments(arguments);
    arguments.clear();
    if (EXPERIMENTAL) {
        // I don't really know why this is needed, but I was seeing r28 := ((((((r28{-}-4)-4)-4)-8)-4)-4)-4:
        DefCollector::iterator dd;
        for (dd = defCol.begin(); dd != defCol.end(); ++dd)
            (*dd)->simplify();
    }

    Signature *sig = proc->getSignature();
    // Ensure everything in the callee's signature (if this is a library call), or the callee parameters (if
    // available),
    // or the def collector if not,  exists in oldArguments
    ArgSourceProvider asp(this);
    Exp *loc;
    while ((loc = asp.nextArgLoc()) != nullptr) {
        if (proc->filterParams(loc))
            continue;
        if (!oldArguments.existsOnLeft(loc)) {
            // Check if the location is renamable. If not, localising won't work, since it relies on definitions
            // collected in the call, and you just get m[...]{-} even if there are definitions.
            Exp *rhs;
            if (proc->canRename(loc))
                rhs = asp.localise(loc->clone());
            else
                rhs = loc->clone();
            SharedType ty = asp.curType(loc);
            Assign *as = new Assign(ty, loc->clone(), rhs);
            as->setNumber(Number); // Give the assign the same statement number as the call (for now)
            // as->setParent(this);
            as->setProc(proc);
            as->setBB(Parent);
            oldArguments.append(as);
        }
    }

    StatementList::iterator it;
    for (it = oldArguments.end(); it != oldArguments.begin();) {
        --it; // Becuase we are using a forwards iterator backwards
        // Make sure the LHS is still in the callee signature / callee parameters / use collector
        Assign *as = (Assign *)*it;
        Exp *lhs = as->getLeft();
        if (!asp.exists(lhs))
            continue;
        if (proc->filterParams(lhs))
            continue; // Filtered out: delete it

        // Insert as, in order, into the existing set of definitions
        StatementList::iterator nn;
        bool inserted = false;
        for (nn = arguments.begin(); nn != arguments.end(); ++nn) {
            if (sig->argumentCompare(*as, *(Assign *)*nn)) { // If the new assignment is less than the current one
                nn = arguments.insert(nn, as);               // then insert before this position
                inserted = true;
                break;
            }
        }
        if (!inserted)
            arguments.insert(arguments.end(), as); // In case larger than all existing elements
    }
}

// Calculate results(this) = defines(this) intersect live(this)
// Note: could use a LocationList for this, but then there is nowhere to store the types (for DFA based TA)
// So the RHS is just ignored
StatementList *CallStatement::calcResults() {
    StatementList *ret = new StatementList;
    if (procDest) {
        Signature *sig = procDest->getSignature();
        if (procDest && procDest->isLib() && false) {
            size_t n = sig->getNumReturns();
            for (size_t i = 0; i < n; i++) { // Ignore first (stack pointer) return
                Exp *sigReturn = sig->getReturnExp(i);
                if (sigReturn->isRegN(sig->getStackRegister(proc->getProg())))
                    continue; // ignore stack reg
#if SYMS_IN_BACK_END
                // But we have translated out of SSA form, so some registers have had to have been replaced with
                // locals
                // So wrap the return register in a ref to this and check the locals
                RefExp wrappedRet(sigReturn, this);
                QString locName = proc->findLocalFromRef(wrappedRet); // E.g. r24{16}
                if (!locName.isEmpty()) {
                    sigReturn = Location::local(locName, proc); // Replace e.g. r24 with local19
                }
#endif
                if (useCol.exists(sigReturn)) {
                    ImplicitAssign *as = new ImplicitAssign(getTypeFor(sigReturn), sigReturn);
                    ret->append(as);
                }
            }
        } else {
            Exp *rsp = Location::regOf(proc->getSignature()->getStackRegister(proc->getProg()));
            for (Instruction *dd : defines) {
                Exp *lhs = ((Assignment *)dd)->getLeft();
                // The stack pointer is allowed as a define, so remove it here as a special case non result
                if (*lhs == *rsp)
                    continue;
                if (useCol.exists(lhs))
                    ret->append(dd);
            }
            delete rsp;
        }
    } else {
        // For a call with no destination at this late stage, use everything live at the call except for the stack
        // pointer register. Needs to be sorted
        UseCollector::iterator rr;  // Iterates through reaching definitions
        StatementList::iterator nn; // Iterates through new results
        Signature *sig = proc->getSignature();
        int sp = sig->getStackRegister();
        for (rr = useCol.begin(); rr != useCol.end(); ++rr) {
            Exp *loc = *rr;
            if (proc->filterReturns(loc))
                continue; // Ignore filtered locations
            if (loc->isRegN(sp))
                continue;                                 // Ignore the stack pointer
            ImplicitAssign *as = new ImplicitAssign(loc); // Create an implicit assignment
            bool inserted = false;
            for (nn = ret->begin(); nn != ret->end(); ++nn) {
                // If the new assignment is less than the current one,
                if (sig->returnCompare(*as, *(Assignment *)*nn)) {
                    nn = ret->insert(nn, as); // then insert before this position
                    inserted = true;
                    break;
                }
            }
            if (!inserted)
                ret->insert(ret->end(), as); // In case larger than all existing elements
        }
    }
    return ret;
}

#if 0
void TypingStatement::setType(Type* ty) {
type = ty;
}
#endif

void CallStatement::removeDefine(Exp * e) {
    StatementList::iterator ss;
    for (ss = defines.begin(); ss != defines.end(); ++ss) {
        Assignment *as = ((Assignment *)*ss);
        if (*as->getLeft() == *e) {
            defines.erase(ss);
            return;
        }
    }
    LOG << "WARNING: could not remove define " << e << " from call " << this << "\n";
}

bool CallStatement::isChildless() const {
    if (procDest == nullptr)
        return true;
    if (procDest->isLib())
        return false;
    // Early in the decompile process, recursive calls are treated as childless, so they use and define all
    if (((UserProc *)procDest)->isEarlyRecursive())
        return true;
    return calleeReturn == nullptr;
}

Exp *CallStatement::bypassRef(RefExp * r, bool &ch) {
    Exp *base = r->getSubExp1();
    Exp *proven;
    ch = false;
    if (procDest && procDest->isLib()) {
        Signature *sig = procDest->getSignature();
        proven = sig->getProven(base);
        if (proven == nullptr) { // Not (known to be) preserved
            if (sig->findReturn(base) != -1)
                return r; // Definately defined, it's the return
            // Otherwise, not all that sure. Assume that library calls pass things like local variables
        }
    } else {
        // Was using the defines to decide if something is preserved, but consider sp+4 for stack based machines
        // Have to use the proven information for the callee (if any)
        if (procDest == nullptr)
            return r; // Childless callees transmit nothing
        // if (procDest->isLocal(base))                    // ICK! Need to prove locals and parameters through
        // calls...
        // FIXME: temporary HACK! Ignores alias issues.
        if (!procDest->isLib() && ((UserProc *)procDest)->isLocalOrParamPattern(base)) {
            Exp *ret = localiseExp(base->clone()); // Assume that it is proved as preserved
            ch = true;
            if (VERBOSE)
                LOG << base << " allowed to bypass call statement " << Number << " ignoring aliasing; result "
                    << ret << "\n";
            return ret;
        }

        proven = procDest->getProven(base); // e.g. r28+4
    }
    if (proven == nullptr)
        return r;                // Can't bypass, since nothing proven
    Exp *to = localiseExp(base); // e.g. r28{17}
    assert(to);
    proven = proven->clone();                         // Don't modify the expressions in destProc->proven!
    proven = proven->searchReplaceAll(*base, to, ch); // e.g. r28{17} + 4
    if (ch)
        LOG_VERBOSE(1) << "bypassRef() replacing " << r << " with " << proven << "\n";
    return proven;
}

void ReturnStatement::removeModified(Exp * loc) {
    modifieds.removeDefOf(loc);
    returns.removeDefOf(loc);
}

void CallStatement::addDefine(ImplicitAssign * as) { defines.append(as); }

TypingStatement::TypingStatement(SharedType ty) : type(ty) {}

// NOTE: ImpRefStatement not yet used
void ImpRefStatement::print(QTextStream & os, bool html) const {
    os << "     *"; // No statement number
    if (html) {
        os << "</td><td>";
        os << "<a name=\"stmt" << Number << "\">";
    }
    os << type << "* IMP REF " << addressExp;
    if (html)
        os << "</a></td>";
}

void ImpRefStatement::meetWith(SharedType ty, bool &ch) { type = type->meetWith(ty, ch); }

Instruction * ImpRefStatement::clone() const { return new ImpRefStatement(type->clone(), addressExp->clone()); }
bool ImpRefStatement::accept(StmtVisitor * visitor) { return visitor->visit(this); }
bool ImpRefStatement::accept(StmtExpVisitor * v) {
    bool override;
    bool ret = v->visit(this, override);
    if (override)
        return ret;
    if (ret)
        ret = addressExp->accept(v->ev);
    return ret;
}
bool ImpRefStatement::accept(StmtModifier * v) {
    bool recur;
    v->visit(this, recur);
    v->mod->clearMod();
    if (recur)
        addressExp = addressExp->accept(v->mod);
    if (VERBOSE && v->mod->isMod())
        LOG << "ImplicitRef changed: now " << this << "\n";
    return true;
}
bool ImpRefStatement::accept(StmtPartModifier * v) {
    bool recur;
    v->visit(this, recur);
    v->mod->clearMod();
    if (recur)
        addressExp = addressExp->accept(v->mod);
    if (VERBOSE && v->mod->isMod())
        LOG << "ImplicitRef changed: now " << this << "\n";
    return true;
}
bool ImpRefStatement::search(const Exp &search, Exp *&result) {
    result = nullptr;
    return addressExp->search(search, result);
}
bool ImpRefStatement::searchAll(const Exp &search, std::list<Exp *, std::allocator<Exp *>> &result) {
    return addressExp->searchAll(search, result);
}
bool ImpRefStatement::searchAndReplace(const Exp &search, Exp *replace, bool /*cc*/) {
    bool change;
    addressExp = addressExp->searchReplaceAll(search, replace, change);
    return change;
}
void ImpRefStatement::simplify() { addressExp = addressExp->simplify(); }

void CallStatement::eliminateDuplicateArgs() {
    StatementList::iterator it;
    LocationSet ls;
    for (it = arguments.begin(); it != arguments.end();) {
        Exp *lhs = ((Assignment *)*it)->getLeft();
        if (ls.exists(lhs)) {
            // This is a duplicate
            it = arguments.erase(it);
            continue;
        }
        ls.insert(lhs);
        ++it;
    }
}

void PhiAssign::enumerateParams(std::list<Exp *> & le) {
    for (auto &v : DefVec) {
        assert(v.second.e != nullptr);
        RefExp *r = new RefExp(v.second.e, v.second.def());
        le.push_back(r);
    }
}

// For debugging
void dumpDestCounts(std::map<Exp *, int, lessExpStar> * destCounts) {
    std::map<Exp *, int, lessExpStar>::iterator it;
    for (it = destCounts->begin(); it != destCounts->end(); ++it) {
        alignStream(LOG_STREAM(),4) << it->second << " " << it->first << "\n";
    }
}

bool JunctionStatement::accept(StmtVisitor * visitor) { return visitor->visit(this); }

bool JunctionStatement::accept(StmtExpVisitor * /*visitor*/) { return true; }

bool JunctionStatement::accept(StmtModifier * /*visitor*/) { return true; }

bool JunctionStatement::accept(StmtPartModifier * /*visitor*/) { return true; }

void JunctionStatement::print(QTextStream & os, bool html) const {
    os << qSetFieldWidth(4) << Number << qSetFieldWidth(0) <<" ";
    if (html) {
        os << "</td><td>";
        os << "<a name=\"stmt" << Number << "\">";
    }
    os << "JUNCTION ";
    for (size_t i = 0; i < Parent->getNumInEdges(); i++) {
        os << Parent->getInEdges()[i]->getHiAddr();
        if (Parent->isBackEdge(i))
            os << "*";
        os << " ";
    }
    if (isLoopJunction())
        os << "LOOP";
// TODO: PassManager::get("RangeAnalysis",this)->printData(os);
//    os << "\n\t\t\tranges: ";
//    Ranges.print(os);
//    if (html)
//        os << "</a></td>";
}

// Map registers and temporaries to locals
void Instruction::mapRegistersToLocals() {
    ExpRegMapper erm(proc);
    StmtRegMapper srm(&erm);
    accept(&srm);
}

void Instruction::insertCasts() {
    // First we postvisit expressions using a StmtModifier and an ExpCastInserter
    ExpCastInserter eci(proc);
    StmtModifier sm(&eci, true); // True to ignore collectors
    accept(&sm);
    // Now handle the LHS of assigns that happen to be m[...], using a StmtCastInserter
    StmtCastInserter sci;
    accept(&sci);
}

void Instruction::replaceSubscriptsWithLocals() {
    ExpSsaXformer esx(proc);
    StmtSsaXformer ssx(&esx, proc);
    accept(&ssx);
}

void Instruction::dfaMapLocals() {
    DfaLocalMapper dlm(proc);
    StmtModifier sm(&dlm, true); // True to ignore def collector in return statement
    accept(&sm);
    if (VERBOSE && dlm.change)
        LOG << "statement mapped with new local(s): " << Number << "\n";
}

void CallStatement::setNumber(int num) {
    Number = num;
    // Also number any existing arguments. Important for library procedures, since these have arguments set by the
    // front
    // end based in their signature
    StatementList::iterator aa;
    for (aa = arguments.begin(); aa != arguments.end(); ++aa)
        (*aa)->setNumber(num);
}
