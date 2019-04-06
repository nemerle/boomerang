#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "BasicBlock.h"

#include "boomerang/db/proc/ProcCFG.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/ssl/RTL.h"
#include "boomerang/ssl/statements/Assign.h"
#include "boomerang/ssl/statements/BranchStatement.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/CaseStatement.h"
#include "boomerang/ssl/statements/ImplicitAssign.h"
#include "boomerang/ssl/statements/PhiAssign.h"
#include "boomerang/util/Util.h"
#include "boomerang/util/log/Log.h"


BasicBlock::BasicBlock(Address lowAddr, Function *function)
    : m_function(function)
    , m_lowAddr(lowAddr)
    , m_bbType(BBType::Invalid)
{
}


BasicBlock::BasicBlock(BBType bbType, std::unique_ptr<RTLList> bbRTLs, Function *function)
    : m_function(function)
    , m_bbType(bbType)
{
    // Set the RTLs. This also updates the low and the high address of the BB.
    setRTLs(std::move(bbRTLs));
}


BasicBlock::BasicBlock(const BasicBlock &bb)
    : m_function(bb.m_function)
    , m_lowAddr(bb.m_lowAddr)
    , m_highAddr(bb.m_highAddr)
    , m_bbType(bb.m_bbType)
    // m_labelNeeded is initialized to false, not copied
    , m_predecessors(bb.m_predecessors)
    , m_successors(bb.m_successors)
{
    if (bb.m_listOfRTLs) {
        // make a deep copy of the RTL list
        std::unique_ptr<RTLList> newList(new RTLList());
        newList->resize(bb.m_listOfRTLs->size());

        RTLList::const_iterator srcIt = bb.m_listOfRTLs->begin();
        RTLList::const_iterator endIt = bb.m_listOfRTLs->end();
        RTLList::iterator destIt      = newList->begin();

        while (srcIt != endIt) {
            *destIt++ = std::make_unique<RTL>(**srcIt++);
        }
        setRTLs(std::move(newList));
    }
}


BasicBlock::~BasicBlock()
{
}


BasicBlock &BasicBlock::operator=(const BasicBlock &bb)
{
    m_function = bb.m_function;
    m_lowAddr  = bb.m_lowAddr;
    m_highAddr = bb.m_highAddr;
    m_bbType   = bb.m_bbType;
    // m_labelNeeded is initialized to false, not copied
    m_predecessors = bb.m_predecessors;
    m_successors   = bb.m_successors;

    if (bb.m_listOfRTLs) {
        // make a deep copy of the RTL list
        std::unique_ptr<RTLList> newList(new RTLList());
        newList->resize(bb.m_listOfRTLs->size());

        RTLList::const_iterator srcIt = bb.m_listOfRTLs->begin();
        RTLList::const_iterator endIt = bb.m_listOfRTLs->end();
        RTLList::iterator destIt      = newList->begin();

        while (srcIt != endIt) {
            *destIt++ = std::make_unique<RTL>(**srcIt++);
        }
        setRTLs(std::move(newList));
    }

    return *this;
}


void BasicBlock::setRTLs(std::unique_ptr<RTLList> rtls)
{
    m_listOfRTLs = std::move(rtls);
    updateBBAddresses();

    if (!m_listOfRTLs) {
        return;
    }

    bool firstRTL = true;

    for (auto &rtl : *m_listOfRTLs) {
        for (Statement *stmt : *rtl) {
            assert(stmt != nullptr);
            stmt->setBB(this);
        }

        if (!firstRTL) {
            assert(rtl->getAddress() != Address::ZERO);
        }

        firstRTL = false;
    }
}


QString BasicBlock::toString() const
{
    QString tgt;
    OStream ost(&tgt);
    print(ost);
    return tgt;
}


void BasicBlock::print(OStream &os) const
{
    switch (getType()) {
    case BBType::Oneway: os << "Oneway BB"; break;
    case BBType::Twoway: os << "Twoway BB"; break;
    case BBType::Nway: os << "Nway BB"; break;
    case BBType::Call: os << "Call BB"; break;
    case BBType::Ret: os << "Ret BB"; break;
    case BBType::Fall: os << "Fall BB"; break;
    case BBType::CompJump: os << "Computed jump BB"; break;
    case BBType::CompCall: os << "Computed call BB"; break;
    case BBType::Invalid: os << "Invalid BB"; break;
    }

    os << ":\n";
    os << "  in edges: ";

    for (BasicBlock *bb : m_predecessors) {
        os << bb->getHiAddr() << "(" << bb->getLowAddr() << ") ";
    }

    os << "\n";
    os << "  out edges: ";

    for (BasicBlock *bb : m_successors) {
        os << bb->getLowAddr() << " ";
    }

    os << "\n";

    if (m_listOfRTLs) { // Can be null if e.g. INVALID
        for (auto &rtl : *m_listOfRTLs) {
            rtl->print(os);
        }
    }
}


Address BasicBlock::getLowAddr() const
{
    return m_lowAddr;
}


Address BasicBlock::getHiAddr() const
{
    return m_highAddr;
}


RTLList *BasicBlock::getRTLs()
{
    return m_listOfRTLs.get();
}


const RTLList *BasicBlock::getRTLs() const
{
    return m_listOfRTLs.get();
}

RTL *BasicBlock::getLastRTL()
{
    return m_listOfRTLs ? m_listOfRTLs->back().get() : nullptr;
}


const RTL *BasicBlock::getLastRTL() const
{
    return m_listOfRTLs ? m_listOfRTLs->back().get() : nullptr;
}


const std::vector<BasicBlock *> &BasicBlock::getPredecessors() const
{
    return m_predecessors;
}


const std::vector<BasicBlock *> &BasicBlock::getSuccessors() const
{
    return m_successors;
}


void BasicBlock::setPredecessor(int i, BasicBlock *predecessor)
{
    assert(Util::inRange(i, 0, getNumPredecessors()));
    m_predecessors[i] = predecessor;
}


void BasicBlock::setSuccessor(int i, BasicBlock *successor)
{
    assert(Util::inRange(i, 0, getNumSuccessors()));
    m_successors[i] = successor;
}


BasicBlock *BasicBlock::getPredecessor(int i)
{
    return Util::inRange(i, 0, getNumPredecessors()) ? m_predecessors[i] : nullptr;
}


const BasicBlock *BasicBlock::getPredecessor(int i) const
{
    return Util::inRange(i, 0, getNumPredecessors()) ? m_predecessors[i] : nullptr;
}


BasicBlock *BasicBlock::getSuccessor(int i)
{
    return Util::inRange(i, 0, getNumSuccessors()) ? m_successors[i] : nullptr;
}


const BasicBlock *BasicBlock::getSuccessor(int i) const
{
    return Util::inRange(i, 0, getNumSuccessors()) ? m_successors[i] : nullptr;
}


void BasicBlock::addPredecessor(BasicBlock *predecessor)
{
    m_predecessors.push_back(predecessor);
}


void BasicBlock::addSuccessor(BasicBlock *successor)
{
    m_successors.push_back(successor);
}


void BasicBlock::removePredecessor(BasicBlock *pred)
{
    // Only remove a single predecessor (prevents issues with double edges)
    for (auto it = m_predecessors.begin(); it != m_predecessors.end(); ++it) {
        if (*it == pred) {
            m_predecessors.erase(it);
            return;
        }
    }
}


void BasicBlock::removeSuccessor(BasicBlock *succ)
{
    // Only remove a single successor (prevents issues with double edges)
    for (auto it = m_successors.begin(); it != m_successors.end(); ++it) {
        if (*it == succ) {
            m_successors.erase(it);
            return;
        }
    }
}


Function *BasicBlock::getCallDestProc() const
{
    if (!isType(BBType::Call) || !m_listOfRTLs || m_listOfRTLs->empty()) {
        return nullptr;
    }

    RTL *lastRTL = m_listOfRTLs->back().get();

    // search backwards for a CallStatement
    for (auto it = lastRTL->rbegin(); it != lastRTL->rend(); ++it) {
        if ((*it)->getKind() == StmtType::Call) {
            return static_cast<CallStatement *>(*it)->getDestProc();
        }
    }

    return nullptr;
}


Statement *BasicBlock::getFirstStmt(RTLIterator &rit, StatementList::iterator &sit)
{
    if ((m_listOfRTLs == nullptr) || m_listOfRTLs->empty()) {
        return nullptr;
    }

    rit = m_listOfRTLs->begin();

    while (rit != m_listOfRTLs->end()) {
        auto &rtl = *rit;
        sit       = rtl->begin();

        if (sit != rtl->end()) {
            return *sit;
        }

        ++rit;
    }

    return nullptr;
}


Statement *BasicBlock::getNextStmt(RTLIterator &rit, StatementList::iterator &sit)
{
    if (++sit != (*rit)->end()) {
        return *sit; // End of current RTL not reached, so return next
    }

    // Else, find next non-empty RTL & return its first statement
    do {
        if (++rit == m_listOfRTLs->end()) {
            return nullptr; // End of all RTLs reached, return null Statement
        }
    } while ((*rit)->empty()); // Ignore all RTLs with no statements

    sit = (*rit)->begin(); // Point to 1st statement at start of next RTL
    return *sit;           // Return first statement
}


Statement *BasicBlock::getPrevStmt(RTLRIterator &rit, StatementList::reverse_iterator &sit)
{
    if (++sit != (*rit)->rend()) {
        return *sit; // Beginning of current RTL not reached, so return next
    }

    // Else, find prev non-empty RTL & return its last statement
    do {
        if (++rit == m_listOfRTLs->rend()) {
            return nullptr; // End of all RTLs reached, return null Statement
        }
    } while ((*rit)->empty()); // Ignore all RTLs with no statements

    sit = (*rit)->rbegin(); // Point to last statement at end of prev RTL
    return *sit;            // Return last statement
}


Statement *BasicBlock::getLastStmt(RTLRIterator &rit, StatementList::reverse_iterator &sit)
{
    if (m_listOfRTLs == nullptr) {
        return nullptr;
    }

    rit = m_listOfRTLs->rbegin();

    while (rit != m_listOfRTLs->rend()) {
        auto &rtl = *rit;
        sit       = rtl->rbegin();

        if (sit != rtl->rend()) {
            return *sit;
        }

        ++rit;
    }

    return nullptr;
}


Statement *BasicBlock::getFirstStmt()
{
    if (m_listOfRTLs == nullptr) {
        return nullptr;
    }

    for (auto &rtl : *m_listOfRTLs) {
        if (!rtl->empty()) {
            return rtl->front();
        }
    }

    return nullptr;
}

const Statement *BasicBlock::getFirstStmt() const
{
    if (m_listOfRTLs == nullptr) {
        return nullptr;
    }

    for (auto &rtl : *m_listOfRTLs) {
        if (!rtl->empty()) {
            return rtl->front();
        }
    }

    return nullptr;
}


Statement *BasicBlock::getLastStmt()
{
    if (m_listOfRTLs == nullptr) {
        return nullptr;
    }

    RTLRIterator revIt = m_listOfRTLs->rbegin();

    while (revIt != m_listOfRTLs->rend()) {
        auto &rtl = *revIt++;

        if (!rtl->empty()) {
            return rtl->back();
        }
    }

    return nullptr;
}


const Statement *BasicBlock::getLastStmt() const
{
    if (m_listOfRTLs == nullptr) {
        return nullptr;
    }

    RTLRIterator revIt = m_listOfRTLs->rbegin();

    while (revIt != m_listOfRTLs->rend()) {
        auto &rtl = *revIt++;

        if (!rtl->empty()) {
            return rtl->back();
        }
    }

    return nullptr;
}


void BasicBlock::appendStatementsTo(StatementList &stmts) const
{
    const RTLList *rtls = getRTLs();

    if (!rtls) {
        return;
    }

    for (const auto &rtl : *rtls) {
        for (Statement *st : *rtl) {
            assert(st->getBB() == this);
            stmts.append(st);
        }
    }
}


SharedExp BasicBlock::getCond() const
{
    // the condition will be in the last rtl
    if (!m_listOfRTLs || m_listOfRTLs->empty()) {
        return nullptr;
    }

    RTL *last = m_listOfRTLs->back().get();

    // it should contain a BranchStatement
    BranchStatement *branch = dynamic_cast<BranchStatement *>(last->getHlStmt());

    if (branch) {
        assert(branch->getKind() == StmtType::Branch);
        return branch->getCondExpr();
    }

    return nullptr;
}


SharedExp BasicBlock::getDest() const
{
    // The destianation will be in the last rtl
    if (!m_listOfRTLs || m_listOfRTLs->empty()) {
        return nullptr;
    }

    const RTL *lastRTL = getLastRTL();

    // It should contain a GotoStatement or derived class
    Statement *lastStmt = lastRTL->getHlStmt();
    CaseStatement *cs   = dynamic_cast<CaseStatement *>(lastStmt);

    if (cs) {
        // Get the expression from the switch info
        SwitchInfo *si = cs->getSwitchInfo();

        if (si) {
            return si->switchExp;
        }
    }
    else {
        GotoStatement *gs = dynamic_cast<GotoStatement *>(lastStmt);

        if (gs) {
            return gs->getDest();
        }
    }

    LOG_ERROR("Last statement of BB at address %1 is not a goto!", this->getLowAddr());
    return nullptr;
}


void BasicBlock::setCond(const SharedExp &e)
{
    // the condition will be in the last rtl
    assert(m_listOfRTLs);
    assert(!m_listOfRTLs->empty());

    RTL *last = m_listOfRTLs->back().get();
    assert(!last->empty());

    // it should contain a BranchStatement
    for (auto it = last->rbegin(); it != last->rend(); ++it) {
        if ((*it)->getKind() == StmtType::Branch) {
            assert(dynamic_cast<BranchStatement *>(*it) != nullptr);
            static_cast<BranchStatement *>(*it)->setCondExpr(e);
            return;
        }
    }
}


void BasicBlock::simplify()
{
    if (m_listOfRTLs) {
        for (auto &rtl : *m_listOfRTLs) {
            rtl->simplify();
        }
    }

    if (isType(BBType::Twoway)) {
        assert(getNumSuccessors() > 1);

        if ((m_listOfRTLs == nullptr) || m_listOfRTLs->empty()) {
            setType(BBType::Fall);
        }
        else {
            RTL *last = m_listOfRTLs->back().get();

            if (last->size() == 0) {
                setType(BBType::Fall);
            }
            else if (last->back()->isGoto()) {
                setType(BBType::Oneway);
            }
            else if (!last->back()->isBranch()) {
                setType(BBType::Fall);
            }
            else if (getNumSuccessors() == 2 && getSuccessor(BTHEN) == getSuccessor(BELSE)) {
                setType(BBType::Oneway);
            }
        }

        if (isType(BBType::Fall)) {
            BasicBlock *redundant = getSuccessor(BTHEN);
            this->removeSuccessor(redundant);
            redundant->removePredecessor(this);
        }
        else if (isType(BBType::Oneway)) {
            BasicBlock *redundant = getSuccessor(BELSE);
            this->removeSuccessor(redundant);
            redundant->removePredecessor(this);
        }

        assert(static_cast<UserProc *>(m_function)->getCFG()->isWellFormed());
    }
}


bool BasicBlock::isPredecessorOf(const BasicBlock *bb) const
{
    return std::find(m_successors.begin(), m_successors.end(), bb) != m_successors.end();
}


bool BasicBlock::isSuccessorOf(const BasicBlock *bb) const
{
    return std::find(m_predecessors.begin(), m_predecessors.end(), bb) != m_predecessors.end();
}


ImplicitAssign *BasicBlock::addImplicitAssign(const SharedExp &lhs)
{
    assert(m_listOfRTLs);

    if (m_listOfRTLs->empty() || m_listOfRTLs->front()->getAddress() != Address::ZERO) {
        m_listOfRTLs->push_front(std::unique_ptr<RTL>(new RTL(Address::ZERO)));
    }

    // do not allow BB with 2 zero address RTLs
    assert(m_listOfRTLs->size() < 2 ||
           (*std::next(m_listOfRTLs->begin()))->getAddress() != Address::ZERO);

    for (Statement *s : *m_listOfRTLs->front()) {
        if (s->isPhi() && *static_cast<PhiAssign *>(s)->getLeft() == *lhs) {
            // phis kill implicits; don't add an implict assign
            // if we already have a phi assigning to the LHS
            return nullptr;
        }
        else if (s->isImplicit() && *static_cast<ImplicitAssign *>(s)->getLeft() == *lhs) {
            // already present
            return static_cast<ImplicitAssign *>(s);
        }
    }

    // no phi or implicit assigning to the LHS already
    ImplicitAssign *newImplicit = new ImplicitAssign(lhs);
    newImplicit->setBB(this);
    newImplicit->setProc(static_cast<UserProc *>(m_function));

    m_listOfRTLs->front()->append(newImplicit);
    return newImplicit;
}


PhiAssign *BasicBlock::addPhi(const SharedExp &usedExp)
{
    assert(m_listOfRTLs);

    if (m_listOfRTLs->empty() || m_listOfRTLs->front()->getAddress() != Address::ZERO) {
        m_listOfRTLs->push_front(std::unique_ptr<RTL>(new RTL(Address::ZERO)));
    }

    // do not allow BB with 2 zero address RTLs
    assert(m_listOfRTLs->size() < 2 ||
           (*std::next(m_listOfRTLs->begin()))->getAddress() != Address::ZERO);

    for (auto existingIt = m_listOfRTLs->front()->begin();
         existingIt != m_listOfRTLs->front()->end();) {
        Statement *s = *existingIt;
        if (s->isPhi() && *static_cast<PhiAssign *>(s)->getLeft() == *usedExp) {
            // already present
            return static_cast<PhiAssign *>(s);
        }
        else if (s->isAssignment() && *static_cast<Assignment *>(s)->getLeft() == *usedExp) {
            // the LHS is already assigned to properly, don't create a second assignment
            return nullptr;
        }

        ++existingIt;
    }

    PhiAssign *phi = new PhiAssign(usedExp);
    phi->setBB(this);
    phi->setProc(static_cast<UserProc *>(m_function));

    m_listOfRTLs->front()->append(phi);
    return phi;
}


void BasicBlock::updateBBAddresses()
{
    if ((m_listOfRTLs == nullptr) || m_listOfRTLs->empty()) {
        m_highAddr = Address::INVALID;
        return;
    }

    Address a = m_listOfRTLs->front()->getAddress();

    if (a.isZero() && (m_listOfRTLs->size() > 1)) {
        RTLList::iterator it = m_listOfRTLs->begin();
        Address add2         = (*++it)->getAddress();

        // This is a bit of a hack for 286 programs, whose main actually starts at offset 0. A
        // better solution would be to change orphan BBs' addresses to Address::INVALID, but I
        // suspect that this will cause many problems. MVE
        if (add2 < Address(0x10)) {
            // Assume that 0 is the real address
            m_lowAddr = Address::ZERO;
        }
        else {
            m_lowAddr = add2;
        }
    }
    else {
        m_lowAddr = a;
    }

    assert(m_listOfRTLs != nullptr);
    m_highAddr = m_listOfRTLs->back()->getAddress();
}


bool BasicBlock::hasStatement(const Statement *stmt) const
{
    if (!stmt || !m_listOfRTLs) {
        return false;
    }

    for (const auto &rtl : *m_listOfRTLs) {
        for (const Statement *s : *rtl) {
            if (s == stmt) {
                return true;
            }
        }
    }

    return false;
}


void BasicBlock::removeRTL(RTL *rtl)
{
    if (!m_listOfRTLs) {
        return;
    }

    RTLList::iterator it = std::find_if(
        m_listOfRTLs->begin(), m_listOfRTLs->end(),
        [rtl](const std::unique_ptr<RTL> &rtl2) { return rtl == rtl2.get(); });

    if (it != m_listOfRTLs->end()) {
        m_listOfRTLs->erase(it);
        updateBBAddresses();
    }
}


bool BasicBlock::isEmpty() const
{
    if (getRTLs() == nullptr) {
        return true;
    }

    for (const auto &rtl : *getRTLs()) {
        if (!rtl->empty()) {
            return false;
        }
    }

    return true;
}


bool BasicBlock::isEmptyJump() const
{
    if (m_listOfRTLs == nullptr || m_listOfRTLs->empty()) {
        return false;
    }
    else if (m_listOfRTLs->back()->size() != 1) {
        return false;
    }
    else if (!m_listOfRTLs->back()->back()->isGoto()) {
        return false;
    }
    else {
        for (auto it = m_listOfRTLs->begin(); it != std::prev(m_listOfRTLs->end()); ++it) {
            if (!(*it)->empty()) {
                return false;
            }
        }
    }

    return true;
}
