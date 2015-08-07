/*
 * Copyright (C) 1997-2001, The University of Queensland
 * Copyright (C) 2000-2001, Sun Microsystems, Inc
 * Copyright (C) 2002, Trent Waddington
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 *
 */

/***************************************************************************/ /**
  * \file  basicblock.cpp
  * \brief Implementation of the BasicBlock class.
  ******************************************************************************/

/***************************************************************************/ /**
  * Dependencies.
  ******************************************************************************/
#include "basicblock.h"

#include "config.h"
#ifdef HAVE_LIBGC
#include "gc.h"
#else
#define NO_GARBAGE_COLLECTOR
#endif
#include "types.h"
#include "statement.h"
#include "exp.h"
#include "cfg.h"
#include "register.h"
#include "rtl.h"
#include "hllcode.h"
#include "proc.h"
#include "prog.h"
#include "util.h"
#include "boomerang.h"
#include "type.h"
#include "log.h"
#include "visitor.h"

#include <QtCore/QDebug>
#include <cassert>
#include <algorithm>
#include <cstring>
#include <inttypes.h>
using namespace std;
/**********************************
 * BasicBlock methods
 **********************************/

BasicBlock::BasicBlock(Function *parent)
    : TargetOutEdges(0),
      // From Doug's code
      InEdgesVisited(0), NumForwardInEdges(-1), Traversed(UNTRAVERSED), HllLabel(false),
      IndentLevel(0), ImmPDom(nullptr), LoopHead(nullptr), CaseHead(nullptr), CondFollow(nullptr), LoopFollow(nullptr),
      LatchNode(nullptr), StructuringType(Seq), UnstructuredType(Structured),
      // Others
      overlappedRegProcessingDone(false) {
    Parent = parent;
}

BasicBlock::~BasicBlock() {
    if (ListOfRTLs) {
        // Delete the RTLs
        for (RTL *it : *ListOfRTLs) {
            delete it;
        }
        // and delete the list
        delete ListOfRTLs;
        ListOfRTLs = nullptr;
    }
}

/***************************************************************************/ /**
  *
  * \brief Copy constructor.
  * \param bb - the BB to copy from
  ******************************************************************************/
BasicBlock::BasicBlock(const BasicBlock &bb)
    : NodeType(bb.NodeType),
      LabelNum(bb.LabelNum), // m_labelneeded is initialized to false, not copied
      Incomplete(bb.Incomplete), JumpReqd(bb.JumpReqd), InEdges(bb.InEdges), OutEdges(bb.OutEdges),
      TargetOutEdges(bb.TargetOutEdges),
      // From Doug's code
      Ord(bb.Ord), RevOrd(bb.RevOrd), InEdgesVisited(bb.InEdgesVisited), NumForwardInEdges(bb.NumForwardInEdges),
      Traversed(bb.Traversed), HllLabel(bb.HllLabel), IndentLevel(bb.IndentLevel),
      LoopCondType(bb.LoopCondType), StructType(bb.StructType), ImmPDom(bb.ImmPDom),
      LoopHead(bb.LoopHead), CaseHead(bb.CaseHead), CondFollow(bb.CondFollow), LoopFollow(bb.LoopFollow),
      LatchNode(bb.LatchNode), StructuringType(bb.StructuringType), UnstructuredType(bb.UnstructuredType) {
    setRTLs(bb.ListOfRTLs);
    Parent = bb.Parent;
}

/***************************************************************************/ /**
  *
  * \brief        Private constructor.
  * \param parent - Function this BasicBlock belongs to.
  * \param pRtls - rtl statements that will be contained in this BasicBlock
  * \param bbType - type of BasicBlock
  * \param iNumOutEdges - expected number of out edges from this BasicBlock
  ******************************************************************************/
BasicBlock::BasicBlock(Function *parent, std::list<RTL *> *pRtls, BBTYPE bbType, uint32_t iNumOutEdges)
    : NodeType(bbType), Incomplete(false),
      // From Doug's code
      InEdgesVisited(0), NumForwardInEdges(-1), Traversed(UNTRAVERSED), HllLabel(false),
      IndentLevel(0), ImmPDom(nullptr), LoopHead(nullptr), CaseHead(nullptr), CondFollow(nullptr), LoopFollow(nullptr),
      LatchNode(nullptr), StructuringType(Seq), UnstructuredType(Structured),
      // Others
      overlappedRegProcessingDone(false) {
    if(bbType==BBTYPE::TWOWAY)
        assert(iNumOutEdges>=2);
    OutEdges.reserve(iNumOutEdges); // Reserve the space; values added with AddOutEdge()
    Parent = parent;
    TargetOutEdges = iNumOutEdges;
    // Set the RTLs
    setRTLs(pRtls);
}

bool BasicBlock::isCaseOption() {
    if (CaseHead)
        for (unsigned int i = 0; i < CaseHead->getNumOutEdges() - 1; i++)
            if (CaseHead->getOutEdge(i) == this)
                return true;
    return false;
}

/***************************************************************************/ /**
  *
  * \brief        Returns nonzero if this BB has been traversed
  * \returns     True if traversed
  ******************************************************************************/
bool BasicBlock::isTraversed() { return TraversedMarker; }

/***************************************************************************/ /**
  *
  * \brief        Sets the traversed flag
  * \param        bTraversed true to set this BB to traversed
  ******************************************************************************/
void BasicBlock::setTraversed(bool bTraversed) { TraversedMarker = bTraversed; }

/***************************************************************************/ /**
  *
  * \brief        Sets the RTLs for a basic block. This is the only place that
  * the RTLs for a block must be set as we need to add the back link for a call
  * instruction to its enclosing BB.
  * \param rtls - a list of RTLs
  *
  ******************************************************************************/
void BasicBlock::setRTLs(std::list<RTL *> *rtls) {
    // should we delete old ones here? breaks some things - trent
    ListOfRTLs = rtls;

    // Used to set the link between the last instruction (a call) and this BB if this is a call BB
}

/***************************************************************************/ /**
  *
  * \brief        Return the type of the basic block.
  * \returns            the type of the basic block
  *
  ******************************************************************************/
BBTYPE BasicBlock::getType() { return NodeType; }

/***************************************************************************/ /**
  *
  * \brief Update the type and number of out edges. Used for example where a COMPJUMP type is updated to an
  * NWAY when a switch idiom is discovered.
  * \param bbType - the new type
  * \param iNumOutEdges - new number of inedges
  *
  ******************************************************************************/
void BasicBlock::updateType(BBTYPE bbType, uint32_t iNumOutEdges) {
    NodeType = bbType;
    TargetOutEdges = iNumOutEdges;
    // m_OutEdges.resize(iNumOutEdges);
}

/***************************************************************************/ /**
  *
  * \brief Sets the "jump required" bit. This means that this BB is an orphan
  * (not generated from input code), and that the "fall through" out edge
  * (m_OutEdges[1]) needs to be implemented as a jump. The back end
  * needs to take heed of this bit
  *
  ******************************************************************************/
void BasicBlock::setJumpReqd() { JumpReqd = true; }

/***************************************************************************/ /**
  *
  * \brief        Returns the "jump required" bit. See above for details
  * \returns            True if a jump is required
  *
  ******************************************************************************/
bool BasicBlock::isJumpReqd() { return JumpReqd; }

char debug_buffer[DEBUG_BUFSIZE];
/***************************************************************************/ /**
  *
  * \brief       Print to a static string (for debugging)
  * \returns     Address of the static buffer
  *
  ******************************************************************************/
const char *BasicBlock::prints() {
    QString tgt;
    QTextStream ost(&tgt);
    print(ost);
    strncpy(debug_buffer, qPrintable(tgt), DEBUG_BUFSIZE - 1);
    debug_buffer[DEBUG_BUFSIZE - 1] = '\0';
    return debug_buffer;
}

void BasicBlock::dump() {
    QTextStream ost(stderr);
    print(ost);
}

/***************************************************************************/ /**
  *
  * \brief Display the whole BB to the given stream
  *  Used for "-R" option, and handy for debugging
  * \param os - stream to output to
  * \param html - print in html mode
  *
  ******************************************************************************/
void BasicBlock::print(QTextStream &os, bool html) {
    if (html)
        os << "<br>";
    if (LabelNum)
        os << "L" << LabelNum << ": ";
    switch (NodeType) {
    case BBTYPE::ONEWAY:
        os << "Oneway BB";
        break;
    case BBTYPE::TWOWAY:
        os << "Twoway BB";
        break;
    case BBTYPE::NWAY:
        os << "Nway BB";
        break;
    case BBTYPE::CALL:
        os << "Call BB";
        break;
    case BBTYPE::RET:
        os << "Ret BB";
        break;
    case BBTYPE::FALL:
        os << "Fall BB";
        break;
    case BBTYPE::COMPJUMP:
        os << "Computed jump BB";
        break;
    case BBTYPE::COMPCALL:
        os << "Computed call BB";
        break;
    case BBTYPE::INVALID:
        os << "Invalid BB";
        break;
    }
    os << ":\n";
    os << "in edges: ";
    for (BasicBlock *bb : InEdges)
        os << bb->getHiAddr() << "(" << bb->getLowAddr() << ") ";
    os << "\n";
    os << "out edges: ";
    for (BasicBlock *bb : OutEdges)
        os << bb->getLowAddr() << " ";
    os << "\n";
    if (ListOfRTLs) { // Can be zero if e.g. INVALID
        if (html)
            os << "<table>\n";
        for(RTL * r : *ListOfRTLs) {
            r->print(os, html);
        }
        if (html)
            os << "</table>\n";
    }
    if (JumpReqd) {
        if (html)
            os << "<br>";
        os << "Synthetic out edge(s) to ";
        //assert(TargetOutEdges == OutEdges.size());
        for (BasicBlock *outEdge : OutEdges) {
            if (outEdge && outEdge->LabelNum)
                os << "L" << outEdge->LabelNum << " ";
        }
        os << '\n';
    }
}

void BasicBlock::printToLog() {
    QString tgt;
    QTextStream ost(&tgt);
    print(ost);
    LOG << tgt;
}

bool BasicBlock::isBackEdge(size_t inEdge) const {
    const BasicBlock *in = InEdges[inEdge];
    return this == in || (DFTfirst < in->DFTfirst && DFTlast > in->DFTlast);
}

// Another attempt at printing BBs that gdb doesn't like to print
// void printBB(BasicBlock * bb) {
//    bb->print(LOG_STREAM());
//}

/***************************************************************************/ /**
  *
  * \brief        Get the lowest real address associated with this BB.
  *
  *  Note that although this is usually the address of the first RTL, it is not
  * always so. For example, if the BB contains just a delayed branch,and the delay
  * instruction for the branch does not affect the branch, so the delay instruction
  * is copied in front of the branch instruction. Its address will be
  * UpdateAddress()'ed to 0, since it is "not really there", so the low address
  * for this BB will be the address of the branch.
  * \returns            the lowest real address associated with this BB
  ******************************************************************************/
ADDRESS BasicBlock::getLowAddr() const {
    if (ListOfRTLs == nullptr || ListOfRTLs->empty() )
        return ADDRESS::g(0L);
    ADDRESS a = ListOfRTLs->front()->getAddress();

    if (a.isZero() && (ListOfRTLs->size() > 1)) {
        std::list<RTL *>::iterator it = ListOfRTLs->begin();
        ADDRESS add2 = (*++it)->getAddress();
        // This is a bit of a hack for 286 programs, whose main actually starts at offset 0. A better solution would be
        // to change orphan BBs' addresses to NO_ADDRESS, but I suspect that this will cause many problems. MVE
        if (add2 < ADDRESS::g(0x10))
            // Assume that 0 is the real address
            return ADDRESS::g(0L);
        return add2;
    }
    return a;
}

/***************************************************************************/ /**
  *
  * \brief        Get the highest address associated with this BB. This is
  *                    always the address associated with the last RTL.
  * \returns     the address
  ******************************************************************************/
ADDRESS BasicBlock::getHiAddr() {
    assert(ListOfRTLs != nullptr);
    return ListOfRTLs->back()->getAddress();
}

/***************************************************************************/ /**
  *
  * \brief        Get pointer to the list of RTL*.
  * \returns     the pointer
  ******************************************************************************/
std::list<RTL *> *BasicBlock::getRTLs() { return ListOfRTLs; }
const std::list<RTL *> *BasicBlock::getRTLs() const { return ListOfRTLs; }

RTL *BasicBlock::getRTLWithStatement(Instruction *stmt) {
    if (ListOfRTLs == nullptr)
        return nullptr;
    for (RTL *rtl : *ListOfRTLs) {
        for (Instruction *it1 : *rtl)
            if (it1 == stmt)
                return rtl;
    }
    return nullptr;
}

/***************************************************************************/ /**
  *
  * \brief Get a constant reference to the vector of in edges.
  * \returns a constant reference to the vector of in edges
  ******************************************************************************/
std::vector<BasicBlock *> &BasicBlock::getInEdges() { return InEdges; }

/***************************************************************************/ /**
  *
  * \brief        Get a constant reference to the vector of out edges.
  * \returns            a constant reference to the vector of out edges
  ******************************************************************************/
const std::vector<BasicBlock *> &BasicBlock::getOutEdges() { return OutEdges; }

/***************************************************************************/ /**
  *
  * \brief Change the given in-edge (0 is first) to the given value
  * Needed for example when duplicating BBs
  * \param i - index (0 based) of in-edge to change
  * \param pNewInEdge - pointer to BasicBlock that will be a new parent
  ******************************************************************************/
void BasicBlock::setInEdge(size_t i, BasicBlock *pNewInEdge) { InEdges[i] = pNewInEdge; }

/***************************************************************************/ /**
  *
  * \brief        Change the given out-edge (0 is first) to the given value
  * Needed for example when duplicating BBs
  * \note Cannot add an additional out-edge with this function; use addOutEdge for this rare case
  * \param i - index (0 based) of out-edge to change
  * \param pNewOutEdge - pointer to BB that will be the new successor
  ******************************************************************************/
void BasicBlock::setOutEdge(size_t i, BasicBlock *pNewOutEdge) {
    if (OutEdges.empty()) {
        assert(i == 0);
        OutEdges.push_back(pNewOutEdge); // TODO: why is it allowed to set new edge in empty m_OutEdges array ?
    } else {
        assert(i < OutEdges.size());
        OutEdges[i] = pNewOutEdge;
    }
}

/***************************************************************************/ /**
  *
  * \brief        Returns the i-th out edge of this BB; counting starts at 0
  * \param i - index (0 based) of the desired out edge
  * \returns            the i-th out edge; 0 if there is no such out edge
  ******************************************************************************/
BasicBlock *BasicBlock::getOutEdge(size_t i) {
    if (i < OutEdges.size())
        return OutEdges[i];
    else
        return nullptr;
}
/*
 *    given an address, returns
 */

/***************************************************************************/ /**
  *
  * \brief        given an address this method returns the corresponding
  *               out edge
  * \param        a the address
  * \returns      the outedge which corresponds to \a a or 0 if there was no such outedge
  ******************************************************************************/
BasicBlock *BasicBlock::getCorrectOutEdge(ADDRESS a) {
    for (BasicBlock *it : OutEdges) {
        if (it->getLowAddr() == a)
            return it;
    }
    return nullptr;
}

/***************************************************************************/ /**
  *
  * \brief Add the given in-edge
  * Needed for example when duplicating BBs
  * \param pNewInEdge -  pointer to BB that will be a new parent
  *
  ******************************************************************************/
void BasicBlock::addInEdge(BasicBlock *pNewInEdge) {
    InEdges.push_back(pNewInEdge);
    // m_iNumInEdges++;
}

/***************************************************************************/ /**
  *
  * \brief Delete the in-edge from the given BB
  *  Needed for example when duplicating BBs
  * \param   it iterator to BB that will no longer be a parent
  * \note    Side effects: The iterator argument is incremented.
  * It should be used like this:
  * \code{.cpp}
  *     if (pred) deleteInEdge(it) else it++;
  * \endcode
  ******************************************************************************/
void BasicBlock::deleteInEdge(std::vector<BasicBlock *>::iterator &it) {
    it = InEdges.erase(it);
    // m_iNumInEdges--;
}

void BasicBlock::deleteInEdge(BasicBlock *edge) {
    for (auto it = InEdges.begin(); it != InEdges.end(); it++) {
        if (*it == edge) {
            deleteInEdge(it);
            break;
        }
    }
}

void BasicBlock::deleteEdge(BasicBlock *edge) {
    edge->deleteInEdge(this);
    for (auto it = OutEdges.begin(); it != OutEdges.end(); it++) {
        if (*it == edge) {
            OutEdges.erase(it);
            break;
        }
    }
}

/***************************************************************************/ /**
  *
  * \brief Traverse this node and recurse on its children in a depth first manner.
  * Records the times at which this node was first visited and last visited
  * \param first - the number of nodes that have been visited
  * \param last - the number of nodes that have been visited for the last time during this traversal
  * \returns the number of nodes (including this one) that were traversed from this node
  ******************************************************************************/
unsigned BasicBlock::DFTOrder(int &first, int &last) {
    first++;
    DFTfirst = first;

    unsigned numTraversed = 1;
    TraversedMarker = true;

    for (BasicBlock *child : OutEdges) {
        if (child->TraversedMarker == false)
            numTraversed = numTraversed + child->DFTOrder(first, last);
    }

    last++;
    DFTlast = last;

    return numTraversed;
}

/***************************************************************************/ /**
  *
  * \brief Traverse this node and recurse on its parents in a reverse depth first manner.
  * Records the times at which this node was first visited and last visited
  * \param first - the number of nodes that have been visited
  * \param last - the number of nodes that have been visited for the last time during this traversal
  * \returns        the number of nodes (including this one) that were traversed from this node
  ******************************************************************************/
unsigned BasicBlock::RevDFTOrder(int &first, int &last) {
    first++;
    DFTrevfirst = first;

    unsigned numTraversed = 1;
    TraversedMarker = true;

    for (BasicBlock *parent : InEdges) {
        if (parent->TraversedMarker == false)
            numTraversed = numTraversed + parent->RevDFTOrder(first, last);
    }

    last++;
    DFTrevlast = last;

    return numTraversed;
}

/***************************************************************************/ /**
  *
  * \brief Static comparison function that returns true if the first BB has an
  * address less than the second BB.
  * \param bb1 - first BB
  * \param bb2 - last BB
  * \returns bb1.address < bb2.address
  ******************************************************************************/
bool BasicBlock::lessAddress(BasicBlock *bb1, BasicBlock *bb2) { return bb1->getLowAddr() < bb2->getLowAddr(); }

/***************************************************************************/ /**
  *
  * \brief Static comparison function that returns true if the first BB has DFT
  * first order less than the second BB.
  * \param bb1 - first BB
  * \param bb2 - last BB
  * \returns bb1.first_DFS < bb2.first_DFS
  ******************************************************************************/
bool BasicBlock::lessFirstDFT(BasicBlock *bb1, BasicBlock *bb2) { return bb1->DFTfirst < bb2->DFTfirst; }

/***************************************************************************/ /**
  *
  * \brief Static comparison function that returns true if the first BB has DFT
  * first order less than the second BB.
  * \param bb1 - first BB
  * \param bb2 - last BB
  * \returns bb1.last_DFS < bb2.last_DFS
  ******************************************************************************/
bool BasicBlock::lessLastDFT(BasicBlock *bb1, BasicBlock *bb2) { return bb1->DFTlast < bb2->DFTlast; }

/***************************************************************************/ /**
  *
  * \brief Get the destination of the call, if this is a CALL BB with
  *  a fixed dest. Otherwise, return -1
  * \returns     Native destination of the call, or -1
  ******************************************************************************/
ADDRESS BasicBlock::getCallDest() {
    if (NodeType != BBTYPE::CALL)
        return NO_ADDRESS;
    if (ListOfRTLs->empty())
        return NO_ADDRESS;
    RTL *lastRtl = ListOfRTLs->back();
    for (auto rit = lastRtl->rbegin(); rit != lastRtl->rend(); rit++) {
        if ((*rit)->getKind() == STMT_CALL)
            return ((CallStatement *)(*rit))->getFixedDest();
    }
    return NO_ADDRESS;
}

Function *BasicBlock::getCallDestProc() {
    if (NodeType != BBTYPE::CALL)
        return nullptr;
    if (ListOfRTLs->size() == 0)
        return nullptr;
    RTL *lastRtl = ListOfRTLs->back();
    for (auto it = lastRtl->rbegin(); it != lastRtl->rend(); it++) {
        if ((*it)->getKind() == STMT_CALL)
            return ((CallStatement *)(*it))->getDestProc();
    }
    return nullptr;
}

//
// Get First/Next Statement in a BB
//
Instruction *BasicBlock::getFirstStmt(rtlit &rit, StatementList::iterator &sit) {
    if (ListOfRTLs == nullptr || ListOfRTLs->empty())
        return nullptr;
    rit = ListOfRTLs->begin();
    while (rit != ListOfRTLs->end()) {
        RTL *rtl = *rit;
        sit = rtl->begin();
        if (sit != rtl->end())
            return *sit;
        rit++;
    }
    return nullptr;
}

Instruction *BasicBlock::getNextStmt(rtlit &rit, StatementList::iterator &sit) {
    if (++sit != (*rit)->end())
        return *sit; // End of current RTL not reached, so return next
                     // Else, find next non-empty RTL & return its first statement
    do {
        if (++rit == ListOfRTLs->end())
            return nullptr;    // End of all RTLs reached, return null Statement
    } while ((*rit)->empty()); // Ignore all RTLs with no statements
    sit = (*rit)->begin();     // Point to 1st statement at start of next RTL
    return *sit;               // Return first statement
}

Instruction *BasicBlock::getPrevStmt(rtlrit &rit, StatementList::reverse_iterator &sit) {
    if (++sit != (*rit)->rend())
        return *sit; // Beginning of current RTL not reached, so return next
                     // Else, find prev non-empty RTL & return its last statement
    do {
        if (++rit == ListOfRTLs->rend())
            return nullptr;    // End of all RTLs reached, return null Statement
    } while ((*rit)->empty()); // Ignore all RTLs with no statements
    sit = (*rit)->rbegin();    // Point to last statement at end of prev RTL
    return *sit;               // Return last statement
}

Instruction *BasicBlock::getLastStmt(rtlrit &rit, StatementList::reverse_iterator &sit) {
    if (ListOfRTLs == nullptr)
        return nullptr;
    rit = ListOfRTLs->rbegin();
    while (rit != ListOfRTLs->rend()) {
        RTL *rtl = *rit;
        sit = rtl->rbegin();
        if (sit != rtl->rend())
            return *sit;
        rit++;
    }
    return nullptr;
}

Instruction *BasicBlock::getFirstStmt() {
    if (ListOfRTLs == nullptr)
        return nullptr;
    for (RTL *rtl : *ListOfRTLs) {
        if (!rtl->empty())
            return rtl->front();
    }
    return nullptr;
}
Instruction *BasicBlock::getLastStmt() {
    if (ListOfRTLs == nullptr)
        return nullptr;
    rtlrit rit = ListOfRTLs->rbegin();
    while (rit != ListOfRTLs->rend()) {
        RTL *rtl = *rit;
        if (!rtl->empty())
            return rtl->back();
        rit++;
    }
    return nullptr;
}

void BasicBlock::getStatements(StatementList &stmts) const {
    const std::list<RTL *> *rtls = getRTLs();
    if (!rtls)
        return;
    for (const RTL *rtl : *rtls) {
        for (Instruction *st : *rtl) {
            if (st->getBB() == nullptr) // TODO: why statement would have nullptr BB here ?
                st->setBB(const_cast<BasicBlock *>(this));
            stmts.append(st);
        }
    }
}

/*
 * Structuring and code generation.
 *
 * This code is whole heartly based on AST by Doug Simon. Portions may be copyright to him and are available under a BSD
 * style license.
 *
 * Adapted for Boomerang by Trent Waddington, 20 June 2002.
 *
 */

/*! Get the condition */
Exp *BasicBlock::getCond() {
    // the condition will be in the last rtl
    assert(ListOfRTLs);
    RTL *last = ListOfRTLs->back();
    // it should contain a BranchStatement
    BranchStatement *bs = (BranchStatement *)last->getHlStmt();
    if (bs && bs->getKind() == STMT_BRANCH)
        return bs->getCondExpr();
    LOG_VERBOSE(1) << "throwing LastStatementNotABranchError\n";
    throw LastStatementNotABranchError(last->getHlStmt());
}

/*! Get the destiantion, if any */
Exp *BasicBlock::getDest() noexcept(false) {
    // The destianation will be in the last rtl
    assert(ListOfRTLs);
    RTL *lastRtl = ListOfRTLs->back();
    // It should contain a GotoStatement or derived class
    Instruction *lastStmt = lastRtl->getHlStmt();
    CaseStatement *cs = dynamic_cast<CaseStatement *>(lastStmt);
    if (cs) {
        // Get the expression from the switch info
        SWITCH_INFO *si = cs->getSwitchInfo();
        if (si)
            return si->pSwitchVar;
    } else {
        GotoStatement *gs = dynamic_cast<GotoStatement *>(lastStmt);
        if (gs)
            return gs->getDest();
    }
    LOG_VERBOSE(1) << "throwing LastStatementNotAGotoError\n";
    throw LastStatementNotAGotoError(lastStmt);
}
/*! set the condition */
void BasicBlock::setCond(Exp *e) noexcept(false) {
    // the condition will be in the last rtl
    assert(ListOfRTLs);
    RTL *last = ListOfRTLs->back();
    // it should contain a BranchStatement
    assert(not last->empty());
    for (auto it = last->rbegin(); it != last->rend(); it++) {
        if ((*it)->getKind() == STMT_BRANCH) {
            ((BranchStatement *)(*it))->setCondExpr(e);
            return;
        }
    }
    throw LastStatementNotABranchError(nullptr);
}

/*! Check for branch if equal relation */
bool BasicBlock::isJmpZ(BasicBlock *dest) {
    // The condition will be in the last rtl
    assert(ListOfRTLs);
    RTL *last = ListOfRTLs->back();
    // it should contain a BranchStatement
    assert(not last->empty());
    for (auto it = last->rbegin(); it != last->rend(); it++) {
        if ((*it)->getKind() == STMT_BRANCH) {
            BRANCH_TYPE jt = ((BranchStatement *)(*it))->getCond();
            if ((jt != BRANCH_JE) && (jt != BRANCH_JNE))
                return false;
            BasicBlock *trueEdge = OutEdges[0];
            if (jt == BRANCH_JE)
                return dest == trueEdge;
            else {
                BasicBlock *falseEdge = OutEdges[1];
                return dest == falseEdge;
            }
        }
    }
    assert(0);
    return false;
}

/*! Get the loop body */
BasicBlock *BasicBlock::getLoopBody() {
    assert(StructType == PRETESTLOOP || StructType == POSTTESTLOOP || StructType == ENDLESSLOOP);
    assert(OutEdges.size() == 2);
    if (OutEdges[0] != LoopFollow)
        return OutEdges[0];
    return OutEdges[1];
}
//! establish if this bb is an ancestor of another BB
bool BasicBlock::isAncestorOf(BasicBlock *other) {
    return ((LoopStamps[0] < other->LoopStamps[0] && LoopStamps[1] > other->LoopStamps[1]) ||
            (RevLoopStamps[0] < other->RevLoopStamps[0] && RevLoopStamps[1] > other->RevLoopStamps[1]));
    /*    return (m_DFTfirst < other->m_DFTfirst && m_DFTlast > other->m_DFTlast) ||
        (m_DFTrevlast < other->m_DFTrevlast &&
         m_DFTrevfirst > other->m_DFTrevfirst);*/
}
/*! Simplify all the expressions in this BB
 */
void BasicBlock::simplify() {
    if (ListOfRTLs)
        for (auto &elem : *ListOfRTLs)
            (elem)->simplify();
    if (NodeType == BBTYPE::TWOWAY) {
        assert(OutEdges.size()>1);
        if (ListOfRTLs == nullptr || ListOfRTLs->empty()) {
            NodeType = BBTYPE::FALL;
        } else {
            RTL *last = ListOfRTLs->back();
            if (last->size() == 0) {
                NodeType = BBTYPE::FALL;
            } else if (last->back()->isGoto()) {
                NodeType = BBTYPE::ONEWAY;
            } else if (!last->back()->isBranch()) {
                NodeType = BBTYPE::FALL;
            }
        }
        if (NodeType == BBTYPE::FALL) {
            // set out edges to be the second one
            if (VERBOSE) {
                LOG << "turning TWOWAY into FALL: " << OutEdges[0]->getLowAddr() << " " << OutEdges[1]->getLowAddr()
                    << "\n";
            }
            BasicBlock *redundant = OutEdges[0];
            OutEdges[0] = OutEdges[1];
            OutEdges.resize(1);
            TargetOutEdges = 1;
            LOG_VERBOSE(1) << "redundant edge to " << redundant->getLowAddr() << " inedges: ";
            std::vector<BasicBlock *> rinedges = redundant->InEdges;
            redundant->InEdges.clear();
            for (BasicBlock *redundant_edge : rinedges) {
                LOG_VERBOSE(1) << redundant_edge->getLowAddr() << " ";
                if (redundant_edge != this)
                    redundant->InEdges.push_back(redundant_edge);
                else {
                    LOG_VERBOSE(1) << "(ignored) ";
                }
            }
            LOG_VERBOSE(1) << "\n";
            // redundant->m_iNumInEdges = redundant->m_InEdges.size();
            LOG_VERBOSE(1) << "   after: " << OutEdges[0]->getLowAddr() << "\n";
        }
        if (NodeType == BBTYPE::ONEWAY) {
            // set out edges to be the first one
            LOG_VERBOSE(1) << "turning TWOWAY into ONEWAY: " << OutEdges[0]->getLowAddr() << " "
                           << OutEdges[1]->getLowAddr() << "\n";
            BasicBlock *redundant = OutEdges[1];
            OutEdges.resize(1);
            TargetOutEdges = 1;
            LOG_VERBOSE(1) << "redundant edge to " << redundant->getLowAddr() << " inedges: ";
            std::vector<BasicBlock *> rinedges = redundant->InEdges;
            redundant->InEdges.clear();
            for (BasicBlock *redundant_edge : rinedges) {
                if (VERBOSE)
                    LOG << redundant_edge->getLowAddr() << " ";
                if (redundant_edge != this)
                    redundant->InEdges.push_back(redundant_edge);
                else {
                    LOG_VERBOSE(1) << "(ignored) ";
                }
            }
            LOG_VERBOSE(1) << "\n";
            // redundant->m_iNumInEdges = redundant->m_InEdges.size();
            LOG_VERBOSE(1) << "   after: " << OutEdges[0]->getLowAddr() << "\n";
        }
    }
}
//! establish if this bb has a back edge to the given destination
bool BasicBlock::hasBackEdgeTo(BasicBlock *dest) {
    //    assert(HasEdgeTo(dest) || dest == this);
    return dest == this || dest->isAncestorOf(this);
}

// Return true if every parent (i.e. forward in edge source) of this node has
// had its code generated
bool BasicBlock::allParentsGenerated() {
    for (BasicBlock *in : InEdges)
        if (!in->hasBackEdgeTo(this) && in->Traversed != DFS_CODEGEN)
            return false;
    return true;
}

// Emits a goto statement (at the correct indentation level) with the destination label for dest. Also places the label
// just before the destination code if it isn't already there.    If the goto is to the return block, it would be nice
// to
// emit a 'return' instead (but would have to duplicate the other code in that return BB).    Also, 'continue' and
// 'break'
// statements are used instead if possible
void BasicBlock::emitGotoAndLabel(HLLCode *hll, int indLevel, BasicBlock *dest) {
    if (LoopHead && (LoopHead == dest || LoopHead->LoopFollow == dest)) {
        if (LoopHead == dest)
            hll->AddContinue(indLevel);
        else
            hll->AddBreak(indLevel);
    } else {
        hll->AddGoto(indLevel, dest->Ord);
        dest->HllLabel = true;
    }
}

// Generates code for each non CTI (except procedure calls) statement within the block.
void BasicBlock::WriteBB(HLLCode *hll, int indLevel) {
    if (DEBUG_GEN)
        LOG << "Generating code for BB at " << getLowAddr() << "\n";

    // Allocate space for a label to be generated for this node and add this to the generated code. The actual label can
    // then be generated now or back patched later
    hll->AddLabel(indLevel, Ord);

    if (ListOfRTLs) {
        for (RTL *rtl : *ListOfRTLs) {
            if (DEBUG_GEN)
                LOG << rtl->getAddress() << "\t";
            for (Instruction *st : *rtl) {
                st->generateCode(hll, this, indLevel);
            }
        }
        if (DEBUG_GEN)
            LOG << "\n";
    }

    // save the indentation level that this node was written at
    IndentLevel = indLevel;
}

void BasicBlock::generateCode_Loop(HLLCode *hll, std::list<BasicBlock *> &gotoSet, int indLevel, UserProc *proc,
                                   BasicBlock *latch, std::list<BasicBlock *> &followSet) {
    // add the follow of the loop (if it exists) to the follow set

    if (LoopFollow)
        followSet.push_back(LoopFollow);

    if (LoopHeaderType == PreTested) {
        assert(LatchNode->OutEdges.size() == 1);

        // write the body of the block (excluding the predicate)
        WriteBB(hll, indLevel);

        // write the 'while' predicate
        Exp *cond = getCond();
        if (OutEdges[BTHEN] == LoopFollow) {
            cond = Unary::get(opNot, cond);
            cond = cond->simplify();
        }
        hll->AddPretestedLoopHeader(indLevel, cond);

        // write the code for the body of the loop
        BasicBlock *loopBody = (OutEdges[BELSE] == LoopFollow) ? OutEdges[BTHEN] : OutEdges[BELSE];
        loopBody->generateCode(hll, indLevel + 1, LatchNode, followSet, gotoSet, proc);

        // if code has not been generated for the latch node, generate it now
        if (LatchNode->Traversed != DFS_CODEGEN) {
            LatchNode->Traversed = DFS_CODEGEN;
            LatchNode->WriteBB(hll, indLevel + 1);
        }

        // rewrite the body of the block (excluding the predicate) at the next nesting level after making sure
        // another label won't be generated
        HllLabel = false;
        WriteBB(hll, indLevel + 1);

        // write the loop tail
        hll->AddPretestedLoopEnd(indLevel);
    } else {
        // write the loop header
        if (LoopHeaderType == Endless)
            hll->AddEndlessLoopHeader(indLevel);
        else
            hll->AddPosttestedLoopHeader(indLevel);

        // if this is also a conditional header, then generate code for the conditional. Otherwise generate code
        // for the loop body.
        if (StructuringType == LoopCond) {
            // set the necessary flags so that generateCode can successfully be called again on this node
            StructuringType = Cond;
            Traversed = UNTRAVERSED;
            generateCode(hll, indLevel + 1, LatchNode, followSet, gotoSet, proc);
        } else {
            WriteBB(hll, indLevel + 1);

            // write the code for the body of the loop
            OutEdges[0]->generateCode(hll, indLevel + 1, LatchNode, followSet, gotoSet, proc);
        }

        if (LoopHeaderType == PostTested) {
            // if code has not been generated for the latch node, generate it now
            if (LatchNode->Traversed != DFS_CODEGEN) {
                LatchNode->Traversed = DFS_CODEGEN;
                LatchNode->WriteBB(hll, indLevel + 1);
            }

            // hll->AddPosttestedLoopEnd(indLevel, getCond());
            // MVE: the above seems to fail when there is a call in the middle of the loop (so loop is 2 BBs)
            // Just a wild stab:
            hll->AddPosttestedLoopEnd(indLevel, LatchNode->getCond());
        } else {
            assert(LoopHeaderType == Endless);

            // if code has not been generated for the latch node, generate it now
            if (LatchNode->Traversed != DFS_CODEGEN) {
                LatchNode->Traversed = DFS_CODEGEN;
                LatchNode->WriteBB(hll, indLevel + 1);
            }

            // write the closing bracket for an endless loop
            hll->AddEndlessLoopEnd(indLevel);
        }
    }

    // write the code for the follow of the loop (if it exists)
    if (LoopFollow) {
        // remove the follow from the follow set
        followSet.resize(followSet.size() - 1);

        if (LoopFollow->Traversed != DFS_CODEGEN)
            LoopFollow->generateCode(hll, indLevel, latch, followSet, gotoSet, proc);
        else
            emitGotoAndLabel(hll, indLevel, LoopFollow);
    }
}

void BasicBlock::generateCode(HLLCode *hll, int indLevel, BasicBlock *latch, std::list<BasicBlock *> &followSet,
                              std::list<BasicBlock *> &gotoSet, UserProc *proc) {
    // If this is the follow for the most nested enclosing conditional, then don't generate anything. Otherwise if it is
    // in the follow set generate a goto to the follow
    BasicBlock *enclFollow = followSet.size() == 0 ? nullptr : followSet.back();

    if (isIn(gotoSet, this) && !isLatchNode() &&
        ((latch && latch->LoopHead && this == latch->LoopHead->LoopFollow) || !allParentsGenerated())) {
        emitGotoAndLabel(hll, indLevel, this);
        return;
    } else if (isIn(followSet, this)) {
        if (this != enclFollow) {
            emitGotoAndLabel(hll, indLevel, this);
            return;
        } else
            return;
    }

    // Has this node already been generated?
    if (Traversed == DFS_CODEGEN) {
        // this should only occur for a loop over a single block
        // FIXME: is this true? Perl_list (0x8068028) in the SPEC 2000 perlbmk seems to have a case with sType = Cond,
        // lType == PreTested, and latchNod == 0
        // assert(sType == Loop && lType == PostTested && latchNode == this);
        return;
    } else
        Traversed = DFS_CODEGEN;

    // if this is a latchNode and the current indentation level is the same as the first node in the loop, then this
    // write out its body and return otherwise generate a goto
    if (isLatchNode()) {
        if (latch && latch->LoopHead &&
            indLevel == latch->LoopHead->IndentLevel + (latch->LoopHead->LoopHeaderType == PreTested ? 1 : 0)) {
            WriteBB(hll, indLevel);
            return;
        } else {
            // unset its traversed flag
            Traversed = UNTRAVERSED;

            emitGotoAndLabel(hll, indLevel, this);
            return;
        }
    }

    BasicBlock *child = nullptr;
    switch (StructuringType) {
    case Loop:
    case LoopCond:
        generateCode_Loop(hll, gotoSet, indLevel, proc, latch, followSet);
        break;

    case Cond: {
        // reset this back to LoopCond if it was originally of this type
        if (LatchNode)
            StructuringType = LoopCond;

        // for 2 way conditional headers that are effectively jumps into
        // or out of a loop or case body, we will need a new follow node
        BasicBlock *tmpCondFollow = nullptr;

        // keep track of how many nodes were added to the goto set so that
        // the correct number are removed
        int gotoTotal = 0;

        // add the follow to the follow set if this is a case header
        if (ConditionHeaderType == Case)
            followSet.push_back(CondFollow);
        else if (ConditionHeaderType != Case && CondFollow) {
            // For a structured two conditional header, its follow is
            // added to the follow set
            // myLoopHead = (sType == LoopCond ? this : loopHead);

            if (UnstructuredType == Structured)
                followSet.push_back(CondFollow);

            // Otherwise, for a jump into/outof a loop body, the follow is added to the goto set.
            // The temporary follow is set for any unstructured conditional header branch that is within the
            // same loop and case.
            else {
                if (UnstructuredType == JumpInOutLoop) {
                    // define the loop header to be compared against
                    BasicBlock *myLoopHead = (StructuringType == LoopCond ? this : LoopHead);
                    gotoSet.push_back(CondFollow);
                    gotoTotal++;

                    // also add the current latch node, and the loop header of the follow if they exist
                    if (latch) {
                        gotoSet.push_back(latch);
                        gotoTotal++;
                    }

                    if (CondFollow->LoopHead && CondFollow->LoopHead != myLoopHead) {
                        gotoSet.push_back(CondFollow->LoopHead);
                        gotoTotal++;
                    }
                }

                if (ConditionHeaderType == IfThen)
                    tmpCondFollow = OutEdges[BELSE];
                else
                    tmpCondFollow = OutEdges[BTHEN];

                // for a jump into a case, the temp follow is added to the follow set
                if (UnstructuredType == JumpIntoCase)
                    followSet.push_back(tmpCondFollow);
            }
        }

        // write the body of the block (excluding the predicate)
        WriteBB(hll, indLevel);

        // write the conditional header
        SWITCH_INFO *psi = nullptr; // Init to nullptr to suppress a warning
        if (ConditionHeaderType == Case) {
            // The CaseStatement will be in the last RTL this BB
            RTL *last = ListOfRTLs->back();
            CaseStatement *cs = (CaseStatement *)last->getHlStmt();
            psi = cs->getSwitchInfo();
            // Write the switch header (i.e. "switch(var) {")
            hll->AddCaseCondHeader(indLevel, psi->pSwitchVar);
        } else {
            Exp *cond = getCond();
            if (!cond)
                cond = Const::get(ADDRESS::g(0xfeedface)); // hack, but better than a crash
            if (ConditionHeaderType == IfElse) {
                cond = Unary::get(opNot, cond->clone());
                cond = cond->simplify();
            }
            if (ConditionHeaderType == IfThenElse)
                hll->AddIfElseCondHeader(indLevel, cond);
            else
                hll->AddIfCondHeader(indLevel, cond);
        }

        // write code for the body of the conditional
        if (ConditionHeaderType != Case) {
            BasicBlock *succ = (ConditionHeaderType == IfElse ? OutEdges[BELSE] : OutEdges[BTHEN]);

            // emit a goto statement if the first clause has already been
            // generated or it is the follow of this node's enclosing loop
            if (succ->Traversed == DFS_CODEGEN || (LoopHead && succ == LoopHead->LoopFollow))
                emitGotoAndLabel(hll, indLevel + 1, succ);
            else
                succ->generateCode(hll, indLevel + 1, latch, followSet, gotoSet, proc);

            // generate the else clause if necessary
            if (ConditionHeaderType == IfThenElse) {
                // generate the 'else' keyword and matching brackets
                hll->AddIfElseCondOption(indLevel);

                succ = OutEdges[BELSE];

                // emit a goto statement if the second clause has already
                // been generated
                if (succ->Traversed == DFS_CODEGEN)
                    emitGotoAndLabel(hll, indLevel + 1, succ);
                else
                    succ->generateCode(hll, indLevel + 1, latch, followSet, gotoSet, proc);

                // generate the closing bracket
                hll->AddIfElseCondEnd(indLevel);
            } else {
                // generate the closing bracket
                hll->AddIfCondEnd(indLevel);
            }
        } else { // case header
            // TODO: linearly emitting each branch of the switch does not result in optimal fall-through.
            // generate code for each out branch
            for (unsigned int i = 0; i < OutEdges.size(); i++) {
                // emit a case label
                // FIXME: Not valid for all switch types
                Const caseVal(0);
                if (psi->chForm == 'F')                              // "Fortran" style?
                    caseVal.setInt(((int *)psi->uTable.m_value)[i]); // Yes, use the table value itself
                // Note that uTable has the address of an int array
                else
                    caseVal.setInt((int)(psi->iLower + i));
                hll->AddCaseCondOption(indLevel, caseVal);

                // generate code for the current out-edge
                BasicBlock *succ = OutEdges[i];
                // assert(succ->caseHead == this || succ == condFollow || HasBackEdgeTo(succ));
                if (succ->Traversed == DFS_CODEGEN)
                    emitGotoAndLabel(hll, indLevel + 1, succ);
                else {
                    succ->generateCode(hll, indLevel + 1, latch, followSet, gotoSet, proc);
                }
            }
            // generate the closing bracket
            hll->AddCaseCondEnd(indLevel);
        }

        // do all the follow stuff if this conditional had one
        if (CondFollow) {
            // remove the original follow from the follow set if it was
            // added by this header
            if (UnstructuredType == Structured || UnstructuredType == JumpIntoCase) {
                assert(gotoTotal == 0);
                followSet.resize(followSet.size() - 1);
            } else // remove all the nodes added to the goto set
                for (int i = 0; i < gotoTotal; i++)
                    gotoSet.resize(gotoSet.size() - 1);

            // do the code generation (or goto emitting) for the new conditional follow if it exists, otherwise do
            // it for the original follow
            if (!tmpCondFollow)
                tmpCondFollow = CondFollow;

            if (tmpCondFollow->Traversed == DFS_CODEGEN)
                emitGotoAndLabel(hll, indLevel, tmpCondFollow);
            else
                tmpCondFollow->generateCode(hll, indLevel, latch, followSet, gotoSet, proc);
        }
        break;
    }
    case Seq:
        // generate code for the body of this block
        WriteBB(hll, indLevel);

        // return if this is the 'return' block (i.e. has no out edges) after emmitting a 'return' statement
        if (getType() == BBTYPE::RET) {
            // This should be emited now, like a normal statement
            // hll->AddReturnStatement(indLevel, getReturnVal());
            return;
        }

        // return if this doesn't have any out edges (emit a warning)
        if (OutEdges.empty()) {
            QTextStream q_cerr(stderr);
            q_cerr << "WARNING: no out edge for this BB in " << proc->getName() << ":\n";
            this->print(q_cerr);
            q_cerr << '\n';
            if (NodeType == BBTYPE::COMPJUMP) {
                QString dat;
                QTextStream ost(&dat);
                assert(ListOfRTLs->size());
                RTL *lastRTL = ListOfRTLs->back();
                assert(!lastRTL->empty());
                GotoStatement *gs = (GotoStatement *)lastRTL->back();
                ost << "goto " << gs->getDest();
                hll->AddLineComment(dat);
            }
            return;
        }

        child = OutEdges[0];
        if (OutEdges.size() != 1) {
            BasicBlock *other = OutEdges[1];
            LOG << "found seq with more than one outedge!\n";
            auto const_dest = static_cast<Const *>(getDest());
            if (const_dest->isIntConst() && const_dest->getAddr() == child->getLowAddr()) {
                other = child;
                child = OutEdges[1];
                LOG << "taken branch is first out edge\n";
            }

            try {
                hll->AddIfCondHeader(indLevel, getCond());
                if (other->Traversed == DFS_CODEGEN)
                    emitGotoAndLabel(hll, indLevel + 1, other);
                else
                    other->generateCode(hll, indLevel + 1, latch, followSet, gotoSet, proc);
                hll->AddIfCondEnd(indLevel);
            } catch (LastStatementNotABranchError &) {
                LOG << "last statement is not a cond, don't know what to do with this.\n";
            }
        }

        // generate code for its successor if it hasn't already been visited and is in the same loop/case and is not
        // the latch for the current most enclosing loop.     The only exception for generating it when it is not in
        // the same loop is when it is only reached from this node
        if (child->Traversed == DFS_CODEGEN ||
            ((child->LoopHead != LoopHead) && (!child->allParentsGenerated() || isIn(followSet, child))) ||
            (latch && latch->LoopHead && latch->LoopHead->LoopFollow == child) ||
            !(CaseHead == child->CaseHead || (CaseHead && child == CaseHead->CondFollow)))
            emitGotoAndLabel(hll, indLevel, child);
        else {
            if (CaseHead && child == CaseHead->CondFollow) {
                // generate the 'break' statement
                hll->AddCaseCondOptionEnd(indLevel);
            } else if (CaseHead == nullptr || CaseHead != child->CaseHead || !child->isCaseOption())
                child->generateCode(hll, indLevel, latch, followSet, gotoSet, proc);
        }
        break;
    default:
        LOG_STREAM() << "unhandled sType " << (int)StructuringType << "\n";
    }
}
/**
    Get the destination proc
    \note this must be a call BB!
*/
Function *BasicBlock::getDestProc() {
    // The last Statement of the last RTL should be a CallStatement
    CallStatement *call = (CallStatement *)(ListOfRTLs->back()->getHlStmt());
    assert(call->getKind() == STMT_CALL);
    Function *proc = call->getDestProc();
    if (proc == nullptr) {
        LOG_STREAM() << "Indirect calls not handled yet\n";
        assert(0);
    }
    return proc;
}

void BasicBlock::setLoopStamps(int &time, std::vector<BasicBlock *> &order) {
    // timestamp the current node with the current time and set its traversed
    // flag
    Traversed = DFS_LNUM;
    LoopStamps[0] = time;

    // recurse on unvisited children and set inedges for all children
    for (BasicBlock *out : OutEdges) {
        // set the in edge from this child to its parent (the current node)
        // (not done here, might be a problem)
        // outEdges[i]->inEdges.Add(this);

        // recurse on this child if it hasn't already been visited
        if (out->Traversed != DFS_LNUM)
            out->setLoopStamps(++time, order);
    }

    // set the the second loopStamp value
    LoopStamps[1] = ++time;

    // add this node to the ordering structure as well as recording its position within the ordering
    Ord = (int)order.size();
    order.push_back(this);
}

void BasicBlock::setRevLoopStamps(int &time) {
    // timestamp the current node with the current time and set its traversed flag
    Traversed = DFS_RNUM;
    RevLoopStamps[0] = time;

    // recurse on the unvisited children in reverse order
    for (int i = (int)OutEdges.size() - 1; i >= 0; i--) {
        // recurse on this child if it hasn't already been visited
        if (OutEdges[i]->Traversed != DFS_RNUM)
            OutEdges[i]->setRevLoopStamps(++time);
    }

    // set the the second loopStamp value
    RevLoopStamps[1] = ++time;
}

void BasicBlock::setRevOrder(std::vector<BasicBlock *> &order) {
    // Set this node as having been traversed during the post domimator DFS ordering traversal
    Traversed = DFS_PDOM;

    // recurse on unvisited children
    for (BasicBlock *in : InEdges)
        if (in->Traversed != DFS_PDOM)
            in->setRevOrder(order);

    // add this node to the ordering structure and record the post dom. order of this node as its index within this
    // ordering structure
    RevOrd = (int)order.size();
    order.push_back(this);
}

void BasicBlock::setCaseHead(BasicBlock *head, BasicBlock *follow) {
    assert(!CaseHead);

    Traversed = DFS_CASE;

    // don't tag this node if it is the case header under investigation
    if (this != head)
        CaseHead = head;

    // if this is a nested case header, then it's member nodes will already have been tagged so skip straight to its
    // follow
    if (getType() == BBTYPE::NWAY && this != head) {
        if (CondFollow && CondFollow->Traversed != DFS_CASE && CondFollow != follow)
            CondFollow->setCaseHead(head, follow);
    } else
        // traverse each child of this node that:
        //   i) isn't on a back-edge,
        //  ii) hasn't already been traversed in a case tagging traversal and,
        // iii) isn't the follow node.
        for (BasicBlock *out : OutEdges)
            if (!hasBackEdgeTo(out) && out->Traversed != DFS_CASE && out != follow)
                out->setCaseHead(head, follow);
}

void BasicBlock::setStructType(structType s) {
    // if this is a conditional header, determine exactly which type of conditional header it is (i.e. switch, if-then,
    // if-then-else etc.)
    if (s == Cond) {
        if (getType() == BBTYPE::NWAY)
            ConditionHeaderType = Case;
        else if (OutEdges[BELSE] == CondFollow)
            ConditionHeaderType = IfThen;
        else if (OutEdges[BTHEN] == CondFollow)
            ConditionHeaderType = IfElse;
        else
            ConditionHeaderType = IfThenElse;
    }

    StructuringType = s;
}

void BasicBlock::setUnstructType(unstructType us) {
    assert((StructuringType == Cond || StructuringType == LoopCond) && ConditionHeaderType != Case);
    UnstructuredType = us;
}

unstructType BasicBlock::getUnstructType() {
    assert((StructuringType == Cond || StructuringType == LoopCond) && ConditionHeaderType != Case);
    return UnstructuredType;
}

void BasicBlock::setLoopType(LoopType l) {
    assert(StructuringType == Loop || StructuringType == LoopCond);
    LoopHeaderType = l;

    // set the structured class (back to) just Loop if the loop type is PreTested OR it's PostTested and is a single
    // block loop
    if (LoopHeaderType == PreTested || (LoopHeaderType == PostTested && this == LatchNode))
        StructuringType = Loop;
}

LoopType BasicBlock::getLoopType() {
    assert(StructuringType == Loop || StructuringType == LoopCond);
    return LoopHeaderType;
}

void BasicBlock::setCondType(CondType c) {
    assert(StructuringType == Cond || StructuringType == LoopCond);
    ConditionHeaderType = c;
}

CondType BasicBlock::getCondType() {
    assert(StructuringType == Cond || StructuringType == LoopCond);
    return ConditionHeaderType;
}

bool BasicBlock::inLoop(BasicBlock *header, BasicBlock *latch) {
    assert(header->LatchNode == latch);
    assert(header == latch ||
           ((header->LoopStamps[0] > latch->LoopStamps[0] && latch->LoopStamps[1] > header->LoopStamps[1]) ||
            (header->LoopStamps[0] < latch->LoopStamps[0] && latch->LoopStamps[1] < header->LoopStamps[1])));
    // this node is in the loop if it is the latch node OR
    // this node is within the header and the latch is within this when using the forward loop stamps OR
    // this node is within the header and the latch is within this when using the reverse loop stamps
    return this == latch || (header->LoopStamps[0] < LoopStamps[0] && LoopStamps[1] < header->LoopStamps[1] &&
                             LoopStamps[0] < latch->LoopStamps[0] && latch->LoopStamps[1] < LoopStamps[1]) ||
           (header->RevLoopStamps[0] < RevLoopStamps[0] && RevLoopStamps[1] < header->RevLoopStamps[1] &&
            RevLoopStamps[0] < latch->RevLoopStamps[0] && latch->RevLoopStamps[1] < RevLoopStamps[1]);
}

// Return the first statement number as a string.
// Used in dotty file generation
/**
 * Get the statement number for the first BB as a character array.
 * If not possible (e.g. because the BB has no statements), return
 * a unique string (e.g. bb8048c10)
 */
char *BasicBlock::getStmtNumber() {
    static char ret[12];
    Instruction *first = getFirstStmt();
    if (first)
        sprintf(ret, "%d", first->getNumber());
    else
        sprintf(ret, "bb%" PRIxPTR, ADDRESS::value_type(this));
    return ret;
}

//! Prepend an assignment (usually a PhiAssign or ImplicitAssign)
//! \a proc is the enclosing Proc
void BasicBlock::prependStmt(Instruction *s, UserProc *proc) {
    assert(Parent==proc);
    // Check the first RTL (if any)
    assert(ListOfRTLs);
    s->setBB(this);
    s->setProc(proc);
    if (!ListOfRTLs)
        ListOfRTLs = new std::list<RTL*>;
    if (!ListOfRTLs->empty()) {
        RTL *rtl = ListOfRTLs->front();
        if (rtl->getAddress().isZero()) {
            // Append to this RTL
            rtl->appendStmt(s);
            return;
        }
    }
    // Otherwise, prepend a new RTL
    std::list<Instruction *> listStmt = {s};
    RTL *rtl = new RTL(ADDRESS::g(0L), &listStmt);
    ListOfRTLs->push_front(rtl);
}

////////////////////////////////////////////////////

// Check for overlap of liveness between the currently live locations (liveLocs) and the set of locations in ls
// Also check for type conflicts if DFA_TYPE_ANALYSIS
// This is a helper function that is not directly declated in the BasicBlock class
void checkForOverlap(LocationSet &liveLocs, LocationSet &ls, ConnectionGraph &ig, UserProc * /*proc*/) {
    // For each location to be considered
    for (Exp *u : ls) {
        if (!u->isSubscript())
            continue; // Only interested in subscripted vars
        RefExp *r = (RefExp *)u;
        // Interference if we can find a live variable which differs only in the reference
        Exp *dr;
        if (liveLocs.findDifferentRef(r, dr)) {
            // We have an interference between r and dr. Record it
            ig.connect(r, dr);
            if (VERBOSE || DEBUG_LIVENESS)
                LOG << "interference of " << dr << " with " << r << "\n";
        }
        // Add the uses one at a time. Note: don't use makeUnion, because then we don't discover interferences
        // from the same statement, e.g.  blah := r24{2} + r24{3}
        liveLocs.insert(u);
    }
}

bool BasicBlock::calcLiveness(ConnectionGraph &ig, UserProc *myProc) {
    // Start with the liveness at the bottom of the BB
    LocationSet liveLocs, phiLocs;
    getLiveOut(liveLocs, phiLocs);
    // Do the livensses that result from phi statements at successors first.
    // FIXME: document why this is necessary
    checkForOverlap(liveLocs, phiLocs, ig, myProc);
    // For each RTL in this BB
    std::list<RTL *>::reverse_iterator rit;
    if (ListOfRTLs) // this can be nullptr
        for (rit = ListOfRTLs->rbegin(); rit != ListOfRTLs->rend(); ++rit) {
            std::list<Instruction *>::reverse_iterator sit;
            // For each statement this RTL
            for (sit = (*rit)->rbegin(); sit != (*rit)->rend(); ++sit) {
                Instruction *s = *sit;
                LocationSet defs;
                s->getDefinitions(defs);
                // The definitions don't have refs yet
                defs.addSubscript(s /* , myProc->getCFG() */);
#if 0 // I used to think it necessary to consider definitions as a special case. However, I now believe that
                // this was either an error of implementation (e.g. it didn't seem to correctly consider the livenesses
                // causesd by phis) or something to do with renaming but not propagating certain memory locations.
                // The idea is now to clearly divide locations into those that can be renamed and propagated, and those
                // which are not renamed or propagated. (Check this.)

                // Also consider it an interference if we define a location that is the same base variable. This can happen
                // when there is a definition that is unused but for whatever reason not eliminated
                // This check is done at the "bottom" of the statement, i.e. before we add s's uses and remove s's
                // definitions to liveLocs
                // Note that phi assignments don't count
                if (!s->isPhi())
                    checkForOverlap(liveLocs, defs, ig, myProc, false);
#endif
                // Definitions kill uses. Now we are moving to the "top" of statement s
                liveLocs.makeDiff(defs);
                // Phi functions are a special case. The operands of phi functions are uses, but they don't interfere
                // with each other (since they come via different BBs). However, we don't want to put these uses into
                // liveLocs, because then the livenesses will flow to all predecessors. Only the appropriate livenesses
                // from the appropriate phi parameter should flow to the predecessor. This is done in getLiveOut()
                if (s->isPhi())
                    continue;
                // Check for livenesses that overlap
                LocationSet uses;
                s->addUsedLocs(uses);
                checkForOverlap(liveLocs, uses, ig, myProc);
                if (DEBUG_LIVENESS)
                    LOG << " ## liveness: at top of " << s << ", liveLocs is " << liveLocs.prints() << "\n";
            }
        }
    // liveIn is what we calculated last time
    if (!(liveLocs == LiveIn)) {
        LiveIn = liveLocs;
        return true; // A change
    }
    // No change
    return false;
}

// Locations that are live at the end of this BB are the union of the locations that are live at the start of its
// successors
// liveout gets all the livenesses, and phiLocs gets a subset of these, which are due to phi statements at the top of
// successors
void BasicBlock::getLiveOut(LocationSet &liveout, LocationSet &phiLocs) {
    liveout.clear();
    for (BasicBlock *currBB : OutEdges) {
        // First add the non-phi liveness
        liveout.makeUnion(currBB->LiveIn); // add successor liveIn to this liveout set.
        // The first RTL will have the phi functions, if any
        if (currBB->ListOfRTLs == nullptr || currBB->ListOfRTLs->size() == 0)
            continue;
        RTL *phiRtl = currBB->ListOfRTLs->front();
        for (Instruction *st : *phiRtl) {
            // Only interested in phi assignments. Note that it is possible that some phi assignments have been
            // converted to ordinary assignments. So the below is a continue, not a break.
            if (!st->isPhi())
                continue;
            PhiAssign *pa = (PhiAssign *)st;
            // Get the jth operand to the phi function; it has a use from BB *this
            // assert(j>=0);
            Instruction *def = pa->getStmtAt(this);
            if (!def) {
                std::deque<BasicBlock *> to_visit(InEdges.begin(), InEdges.end());
                std::set<BasicBlock *> tried{this};
                // TODO: this looks like a hack ?  but sometimes PhiAssign has value which is defined in parent of
                // 'this'
                //  BB1 1  - defines r20
                //  BB2 33 - transfers control to BB3
                //  BB3 40 - r10 = phi { 1 }
                while (!to_visit.empty()) {
                    BasicBlock *pbb = to_visit.back();
                    if (tried.find(pbb) != tried.end()) {
                        to_visit.pop_back();
                        continue;
                    }
                    def = pa->getStmtAt(pbb);
                    if (def)
                        break;
                    tried.insert(pbb);
                    to_visit.pop_back();
                    for (BasicBlock *bb : pbb->InEdges) {
                        if (tried.find(bb) != tried.end()) // already tried
                            continue;
                        to_visit.push_back(bb);
                    }
                }
            }
            Exp *r = RefExp::get(pa->getLeft()->clone(), def);
            liveout.insert(r);
            phiLocs.insert(r);
            if (DEBUG_LIVENESS)
                LOG << " ## Liveness: adding " << r << " due to ref to phi " << st << " in BB at " << getLowAddr()
                    << "\n";
        }
    }
}

// Basically the "whichPred" function as per Briggs, Cooper, et al (and presumably "Cryton, Ferante, Rosen, Wegman, and
// Zadek").  Return -1 if not found
/*
 * Get the index of my in-edges is BB pred
 */
int BasicBlock::whichPred(BasicBlock *pred) {
    int n = InEdges.size();
    for (int i = 0; i < n; i++) {
        if (InEdges[i] == pred)
            return i;
    }
    assert(0);
    return -1;
}

//    //    //    //    //    //    //    //    //    //    //    //    //
//                                                //
//         Indirect jump and call analyses        //
//                                                //
//    //    //    //    //    //    //    //    //    //    //    //    //

// Switch High Level patterns

// With array processing, we get a new form, call it form 'a' (don't confuse with form 'A'):
// Pattern: <base>{}[<index>]{} where <index> could be <var> - <Kmin>
// TODO: use initializer lists
static Exp *forma =
    RefExp::get(Binary::get(opArrayIndex, RefExp::get(Terminal::get(opWild), (Instruction *)-1), Terminal::get(opWild)),
                (Instruction *)-1);

// Pattern: m[<expr> * 4 + T ]
static Exp *formA = Location::memOf(
    Binary::get(opPlus, Binary::get(opMult, Terminal::get(opWild), Const::get(4)), Terminal::get(opWildIntConst)));

// With array processing, we get a new form, call it form 'o' (don't confuse with form 'O'):
// Pattern: <base>{}[<index>]{} where <index> could be <var> - <Kmin>
// NOT COMPLETED YET!
static Exp *formo =
    RefExp::get(Binary::get(opArrayIndex, RefExp::get(Terminal::get(opWild), (Instruction *)-1), Terminal::get(opWild)),
                (Instruction *)-1);

// Pattern: m[<expr> * 4 + T ] + T
static Exp *formO =
    Binary::get(opPlus, Location::memOf(Binary::get(opPlus, Binary::get(opMult, Terminal::get(opWild), Const::get(4)),
                                                    Terminal::get(opWildIntConst))),
                Terminal::get(opWildIntConst));

// Pattern: %pc + m[%pc     + (<expr> * 4) + k]
// where k is a small constant, typically 28 or 20
static Exp *formR = Binary::get(
    opPlus, Terminal::get(opPC),
    Location::memOf(Binary::get(
        opPlus, Terminal::get(opPC),
        Binary::get(opPlus, Binary::get(opMult, Terminal::get(opWild), Const::get(4)), Const::get(opWildIntConst)))));

// Pattern: %pc + m[%pc + ((<expr> * 4) - k)] - k
// where k is a smallish constant, e.g. 288 (/usr/bin/vi 2.6, 0c4233c).
static Exp *formr = Binary::get(
    opPlus, Terminal::get(opPC),
    Location::memOf(Binary::get(opPlus, Terminal::get(opPC),
                                Binary::get(opMinus, Binary::get(opMult, Terminal::get(opWild), Const::get(4)),
                                            Terminal::get(opWildIntConst)))));

static Exp *hlForms[] = {forma, formA, formo, formO, formR, formr};
static char chForms[] = {'a', 'A', 'o', 'O', 'R', 'r'};

void init_basicblock() {
#ifndef NO_GARBAGE_COLLECTOR
    Exp **gc_pointers = (Exp **)GC_MALLOC_UNCOLLECTABLE(6 * sizeof(Exp *));
    gc_pointers[0] = forma;
    gc_pointers[1] = formA;
    gc_pointers[2] = formo;
    gc_pointers[3] = formO;
    gc_pointers[4] = formR;
    gc_pointers[5] = formr;
#endif
}

// Vcall high level patterns
// Pattern 0: global<wild>[0]
static Exp *vfc_funcptr =
    Binary::get(opArrayIndex, Location::get(opGlobal, Terminal::get(opWildStrConst), nullptr), Const::get(0));

// Pattern 1: m[ m[ <expr> + K1 ] + K2 ]
// K1 is vtable offset, K2 is virtual function offset (could come from m[A2], if A2 is in read-only memory
static Exp *vfc_both = Location::memOf(
    Binary::get(opPlus, Location::memOf(Binary::get(opPlus, Terminal::get(opWild), Terminal::get(opWildIntConst))),
                Terminal::get(opWildIntConst)));

// Pattern 2: m[ m[ <expr> ] + K2]
static Exp *vfc_vto =
    Location::memOf(Binary::get(opPlus, Location::memOf(Terminal::get(opWild)), Terminal::get(opWildIntConst)));

// Pattern 3: m[ m[ <expr> + K1] ]
static Exp *vfc_vfo =
    Location::memOf(Location::memOf(Binary::get(opPlus, Terminal::get(opWild), Terminal::get(opWildIntConst))));

// Pattern 4: m[ m[ <expr> ] ]
static Exp *vfc_none = Location::memOf(Location::memOf(Terminal::get(opWild)));

static Exp *hlVfc[] = {vfc_funcptr, vfc_both, vfc_vto, vfc_vfo, vfc_none};

void findSwParams(char form, Exp *e, Exp *&expr, ADDRESS &T) {
    switch (form) {
    case 'a': {
        // Pattern: <base>{}[<index>]{}
        e = e->getSubExp1();
        Exp *base = e->getSubExp1();
        if (base->isSubscript())
            base = base->getSubExp1();
        auto con = static_cast<Const *>(base->getSubExp1());
        QString gloName = con->getStr();
        UserProc *p = static_cast<Location *>(base)->getProc();
        Prog *prog = p->getProg();
        T = prog->getGlobalAddr(gloName);
        expr = e->getSubExp2();
        break;
    }
    case 'A': {
        // Pattern: m[<expr> * 4 + T ]
        if (e->isSubscript())
            e = e->getSubExp1();
        // b will be (<expr> * 4) + T
        Exp *b = e->getSubExp1();
        T = static_cast<Const *>(b->getSubExp2())->getAddr();
        b = b->getSubExp1(); // b is now <expr> * 4
        expr = b->getSubExp1();
        break;
    }
    case 'O': { // Form O
        // Pattern: m[<expr> * 4 + T ] + T
        T = static_cast<Const *>(e->getSubExp2())->getAddr();
        // l = m[<expr> * 4 + T ]:
        Exp *l = e->getSubExp1();
        if (l->isSubscript())
            l = l->getSubExp1();
        // b = <expr> * 4 + T:
        Exp *b = l->getSubExp1();
        // b = <expr> * 4:
        b = b->getSubExp1();
        // expr = <expr>:
        expr = b->getSubExp1();
        break;
    }
    case 'R': {
        // Pattern: %pc + m[%pc     + (<expr> * 4) + k]
        T = ADDRESS::g(0L); // ?
        // l = m[%pc  + (<expr> * 4) + k]:
        Exp *l = e->getSubExp2();
        if (l->isSubscript())
            l = l->getSubExp1();
        // b = %pc    + (<expr> * 4) + k:
        Exp *b = l->getSubExp1();
        // b = (<expr> * 4) + k:
        b = b->getSubExp2();
        // b = <expr> * 4:
        b = b->getSubExp1();
        // expr = <expr>:
        expr = b->getSubExp1();
        break;
    }
    case 'r': {
        // Pattern: %pc + m[%pc + ((<expr> * 4) - k)] - k
        T = ADDRESS::g(0L); // ?
        // b = %pc + m[%pc + ((<expr> * 4) - k)]:
        Exp *b = e->getSubExp1();
        // l = m[%pc + ((<expr> * 4) - k)]:
        Exp *l = b->getSubExp2();
        if (l->isSubscript())
            l = l->getSubExp1();
        // b = %pc + ((<expr> * 4) - k)
        b = l->getSubExp1();
        // b = ((<expr> * 4) - k):
        b = b->getSubExp2();
        // b = <expr> * 4:
        b = b->getSubExp1();
        // expr = <expr>
        expr = b->getSubExp1();
        break;
    }
    default:
        expr = nullptr;
        T = NO_ADDRESS;
    }
    // normalize address to native
    T=T.native();
}
/**
 Find the number of cases for this switch statement. Assumes that there is a compare and branch around the indirect
 branch. Note: fails test/sparc/switchAnd_cc because of the and instruction, and the compare that is outside is not
 the compare for the upper bound. Note that you CAN have an and and still a test for an upper bound. So this needs
 tightening.
 TMN: It also needs to check for and handle the double indirect case; where there is one array (of e.g. ubyte)
 that is indexed by the actual switch value, then the value from that array is used as an index into the array of
 code pointers.
*/
int BasicBlock::findNumCases() {
    // should actually search from the statement to i
    for (BasicBlock *in : InEdges) { // For each in-edge
        if (in->NodeType != BBTYPE::TWOWAY)  // look for a two-way BB
            continue;                  // Ignore all others
        assert(in->ListOfRTLs && in->ListOfRTLs->size());
        RTL *lastRtl = in->ListOfRTLs->back();
        assert(not lastRtl->empty());
        BranchStatement *lastStmt = (BranchStatement *)lastRtl->back();
        Exp *pCond = lastStmt->getCondExpr();
        if (pCond->getArity() != 2)
            continue;
        Exp *rhs = pCond->getSubExp2();
        if (!rhs->isIntConst())
            continue;
        int k = static_cast<Const *>(rhs)->getInt();
        OPER op = pCond->getOper();
        if (op == opGtr || op == opGtrUns)
            return k + 1;
        if (op == opGtrEq || op == opGtrEqUns)
            return k;
        if (op == opLess || op == opLessUns)
            return k;
        if (op == opLessEq || op == opLessEqUns)
            return k + 1;
    }
    LOG << "Could not find number of cases for n-way at address " << getLowAddr() << "\n";
    return 3; // Bald faced guess if all else fails
}

// Find all the possible constant values that the location defined by s could be assigned with
static void findConstantValues(const Instruction *s, std::list<int> &dests) {
    if (s == nullptr)
        return;
    if (s->isPhi()) {
        // For each definition, recurse
        for (const auto &it : *((PhiAssign *)s))
            findConstantValues(it.second.def(), dests);
    } else if (s->isAssign()) {
        Exp *rhs = ((Assign *)s)->getRight();
        if (rhs->isIntConst())
            dests.push_back(((Const *)rhs)->getInt());
    }
}
// Find indirect jumps and calls
//! Find any BBs of type COMPJUMP or COMPCALL. If found, analyse, and if possible decode extra code and return true
bool BasicBlock::decodeIndirectJmp(UserProc *proc) {
#define CHECK_REAL_PHI_LOOPS 0
#if CHECK_REAL_PHI_LOOPS
    rtlit rit;
    StatementList::iterator sit;
    Statement *s = getFirstStmt(rit, sit);
    for (s = getFirstStmt(rit, sit); s; s = getNextStmt(rit, sit)) {
        if (!s->isPhi())
            continue;
        Statement *originalPhi = s;
        InstructionSet workSet, seenSet;
        workSet.insert(s);
        seenSet.insert(s);
        do {
            PhiAssign *pi = (PhiAssign *)*workSet.begin();
            workSet.remove(pi);
            PhiAssign::Definitions::iterator it;
            for (it = pi->begin(); it != pi->end(); it++) {
                if (it->def == nullptr)
                    continue;
                if (!it->def->isPhi())
                    continue;
                if (seenSet.exists(it->def)) {
                    LOG_STREAM() << "Real phi loop involving statements " << originalPhi->getNumber() << " and "
                              << pi->getNumber() << "\n";
                    break;
                } else {
                    workSet.insert(it->def);
                    seenSet.insert(it->def);
                }
            }
        } while (workSet.size());
    }
#endif

    if (NodeType == BBTYPE::COMPJUMP) {
        assert(ListOfRTLs->size());
        RTL *lastRtl = ListOfRTLs->back();
        if (DEBUG_SWITCH)
            LOG << "decodeIndirectJmp: " << lastRtl->prints();
        assert(not lastRtl->empty());
        CaseStatement *lastStmt = (CaseStatement *)lastRtl->back();
        // Note: some programs might not have the case expression propagated to, because of the -l switch (?)
        // We used to use ordinary propagation here to get the memory expression, but now it refuses to propagate memofs
        // because of the alias safety issue. Eventually, we should use an alias-safe incremental propagation, but for
        // now we'll assume no alias problems and force the propagation
        bool convert; // FIXME: uninitialized value passed to propagateTo
        lastStmt->propagateTo(convert, nullptr, nullptr, true /* force */);
        Exp *e = lastStmt->getDest();
        int n = sizeof(hlForms) / sizeof(Exp *);
        char form = 0;
        for (int i = 0; i < n; i++) {
            if (*e *= *hlForms[i]) { // *= compare ignores subscripts
                form = chForms[i];
                if (DEBUG_SWITCH)
                    LOG << "indirect jump matches form " << form << "\n";
                break;
            }
        }
        if (form) {
            SWITCH_INFO *swi = new SWITCH_INFO;
            swi->chForm = form;
            ADDRESS T;
            Exp *expr;
            findSwParams(form, e, expr, T);
            if (expr) {
                swi->uTable = T;
                swi->iNumTable = findNumCases();
#if 1 // TMN: Added actual control of the array members, to possibly truncate what findNumCases()
                // thinks is the number of cases, when finding the first array element not pointing to code.
                if (form == 'A') {
                    Prog *prog = proc->getProg();
                    for (int iPtr = 0; iPtr < swi->iNumTable; ++iPtr) {
                        ADDRESS uSwitch = ADDRESS::g(prog->readNative4(swi->uTable + iPtr * 4));
                        if (uSwitch >= prog->getLimitTextHigh() || uSwitch < prog->getLimitTextLow()) {
                            if (DEBUG_SWITCH)
                                LOG << "Truncating type A indirect jump array to " << iPtr
                                    << " entries "
                                       "due to finding an array entry pointing outside valid code " << uSwitch
                                    << " isn't in " << prog->getLimitTextLow() << " .. " << prog->getLimitTextHigh()
                                    << "\n";
                            // Found an array that isn't a pointer-to-code. Assume array has ended.
                            swi->iNumTable = iPtr;
                            break;
                        }
                    }
                }
                if (swi->iNumTable <= 0) {
                    LOG << "Switch analysis failure at: " << this->getLowAddr();
                    return false;
                }
#endif
                // TODO: missing form = 'R' iOffset is not being set
                swi->iUpper = swi->iNumTable - 1;
                swi->iLower = 0;
                if (expr->getOper() == opMinus && expr->getSubExp2()->isIntConst()) {
                    swi->iLower = static_cast<Const *>(expr->getSubExp2())->getInt();
                    swi->iUpper += swi->iLower;
                    expr = expr->getSubExp1();
                }
                swi->pSwitchVar = expr;
                lastStmt->setDest((Exp *)nullptr);
                lastStmt->setSwitchInfo(swi);
                return swi->iNumTable != 0;
            }
        } else {
            // Did not match a switch pattern. Perhaps it is a Fortran style goto with constants at the leaves of the
            // phi tree. Basically, a location with a reference, e.g. m[r28{-} - 16]{87}
            if (e->isSubscript()) {
                Exp *sub = e->getSubExp1();
                if (sub->isLocation()) {
                    // Yes, we have <location>{ref}. Follow the tree and store the constant values that <location>
                    // could be assigned to in dests
                    std::list<int> dests;
                    findConstantValues(((RefExp *)e)->getDef(), dests);
                    // The switch info wants an array of native addresses
                    size_t num_dests = dests.size();
                    if (num_dests) {
                        int *destArray = new int[num_dests];
                        std::copy(dests.begin(), dests.end(), destArray);
                        SWITCH_INFO *swi = new SWITCH_INFO;
                        swi->chForm = 'F'; // The "Fortran" form
                        swi->pSwitchVar = e;
                        swi->uTable = ADDRESS::host_ptr(destArray); // WARN: Abuse the uTable member as a pointer
                        swi->iNumTable = num_dests;
                        swi->iLower = 1;         // Not used, except to compute
                        swi->iUpper = num_dests; // the number of options
                        lastStmt->setDest((Exp *)nullptr);
                        lastStmt->setSwitchInfo(swi);
                        return true;
                    }
                }
            }
        }
        return false;
    } else if (NodeType == BBTYPE::COMPCALL) {
        assert(ListOfRTLs->size());
        RTL *lastRtl = ListOfRTLs->back();
        if (DEBUG_SWITCH)
            LOG << "decodeIndirectJmp: COMPCALL:\n" << lastRtl->prints() << "\n";
        assert(not lastRtl->empty());
        CallStatement *lastStmt = (CallStatement *)lastRtl->back();
        Exp *e = lastStmt->getDest();
        // Indirect calls may sometimes not be propagated to, because of limited propagation (-l switch).
        // Propagate to e, but only keep the changes if the expression matches (don't want excessive propagation to
        // a genuine function pointer expression, even though it's hard to imagine).
        e = e->propagateAll();
        // We also want to replace any m[K]{-} with the actual constant from the (presumably) read-only data section
        ConstGlobalConverter cgc(proc->getProg());
        e = e->accept(&cgc);
        // Simplify the result, e.g. for m[m[(r24{16} + m[0x8048d74]{-}) + 12]{-}]{-} get
        // m[m[(r24{16} + 20) + 12]{-}]{-}, want m[m[r24{16} + 32]{-}]{-}. Note also that making the
        // ConstGlobalConverter a simplifying expression modifier won't work in this case, since the simplifying
        // converter will only simplify the direct parent of the changed expression (which is r24{16} + 20).
        e = e->simplify();
        if (DEBUG_SWITCH)
            LOG << "decodeIndirect: propagated and const global converted call expression is " << e << "\n";

        int n = sizeof(hlVfc) / sizeof(Exp *);
        bool recognised = false;
        int i;
        for (i = 0; i < n; i++) {
            if (*e *= *hlVfc[i]) { // *= compare ignores subscripts
                recognised = true;
                if (DEBUG_SWITCH)
                    LOG << "indirect call matches form " << i << "\n";
                break;
            }
        }
        if (!recognised)
            return false;
        lastStmt->setDest(e); // Keep the changes to the indirect call expression
        int K1, K2;
        Exp *vtExp, *t1;
        Prog *prog = proc->getProg();
        switch (i) {
        case 0: {
            // This is basically an indirection on a global function pointer.  If it is initialised, we have a
            // decodable entry point.  Note: it could also be a library function (e.g. Windows)
            // Pattern 0: global<name>{0}[0]{0}
            K2 = 0;
            if (e->isSubscript())
                e = e->getSubExp1();
            e = e->getSubExp1(); // e is global<name>{0}[0]
            t1 = e->getSubExp2();
            auto t1_const = static_cast<Const *>(t1);
            if (e->isArrayIndex() && (t1->isIntConst()) && t1_const->getInt() == 0)
                e = e->getSubExp1(); // e is global<name>{0}
            if (e->isSubscript())
                e = e->getSubExp1();                            // e is global<name>
            Const *con = static_cast<Const *>(e->getSubExp1()); // e is <name>
            Global *glo = prog->getGlobal(con->getStr());
            assert(glo);
            // Set the type to pointer to function, if not already
            SharedType ty = glo->getType();
            if (!ty->isPointer() && !std::static_pointer_cast<PointerType>(ty)->getPointsTo()->isFunc())
                glo->setType(PointerType::get(FuncType::get()));
            ADDRESS addr = glo->getAddress();
            // FIXME: not sure how to find K1 from here. I think we need to find the earliest(?) entry in the data
            // map that overlaps with addr
            // For now, let K1 = 0:
            K1 = 0;
            vtExp = Const::get(addr);
            break;
        }
        case 1: {
            // Example pattern: e = m[m[r27{25} + 8]{-} + 8]{-}
            if (e->isSubscript())
                e = e->getSubExp1();
            e = e->getSubExp1();        // e = m[r27{25} + 8]{-} + 8
            Exp *rhs = e->getSubExp2(); // rhs = 8
            K2 = ((Const *)rhs)->getInt();
            Exp *lhs = e->getSubExp1(); // lhs = m[r27{25} + 8]{-}
            if (lhs->isSubscript())
                lhs = lhs->getSubExp1(); // lhs = m[r27{25} + 8]
            vtExp = lhs;
            lhs = lhs->getSubExp1(); // lhs =   r27{25} + 8
            Exp *CK1 = lhs->getSubExp2();
            K1 = ((Const *)CK1)->getInt();
            break;
        }
        case 2: {
            // Example pattern: e = m[m[r27{25}]{-} + 8]{-}
            if (e->isSubscript())
                e = e->getSubExp1();
            e = e->getSubExp1();        // e = m[r27{25}]{-} + 8
            Exp *rhs = e->getSubExp2(); // rhs = 8
            K2 = ((Const *)rhs)->getInt();
            Exp *lhs = e->getSubExp1(); // lhs = m[r27{25}]{-}
            if (lhs->isSubscript())
                lhs = lhs->getSubExp1(); // lhs = m[r27{25}]
            vtExp = lhs;
            K1 = 0;
            break;
        }
        case 3: {
            // Example pattern: e = m[m[r27{25} + 8]{-}]{-}
            if (e->isSubscript())
                e = e->getSubExp1();
            e = e->getSubExp1(); // e = m[r27{25} + 8]{-}
            K2 = 0;
            if (e->isSubscript())
                e = e->getSubExp1(); // e = m[r27{25} + 8]
            vtExp = e;
            Exp *lhs = e->getSubExp1(); // lhs =   r27{25} + 8
            // Exp* object = ((Binary*)lhs)->getSubExp1();
            Exp *CK1 = lhs->getSubExp2();
            K1 = ((Const *)CK1)->getInt();
            break;
        }
        case 4: {
            // Example pattern: e = m[m[r27{25}]{-}]{-}
            if (e->isSubscript())
                e = e->getSubExp1();
            e = e->getSubExp1(); // e = m[r27{25}]{-}
            K2 = 0;
            if (e->isSubscript())
                e = e->getSubExp1(); // e = m[r27{25}]
            vtExp = e;
            K1 = 0;
            // Exp* object = ((Unary*)e)->getSubExp1();
            break;
        }
        default:
            K1 = K2 = -1; // Suppress warnings
            vtExp = (Exp *)-1;
        }
        if (DEBUG_SWITCH)
            LOG << "form " << i << ": from statement " << lastStmt->getNumber() << " get e = " << lastStmt->getDest()
                << ", K1 = " << K1 << ", K2 = " << K2 << ", vtExp = " << vtExp << "\n";
        // The vt expression might not be a constant yet, because of expressions not fully propagated, or because of
        // m[K] in the expression (fixed with the ConstGlobalConverter).  If so, look it up in the defCollector in the
        // call
        vtExp = lastStmt->findDefFor(vtExp);
        if (vtExp && DEBUG_SWITCH)
            LOG << "VT expression boils down to this: " << vtExp << "\n";

        // Danger. For now, only do if -ic given
        bool decodeThru = Boomerang::get()->decodeThruIndCall;
        if (decodeThru && vtExp && vtExp->isIntConst()) {
            ADDRESS addr = ((Const *)vtExp)->getAddr();
            ADDRESS pfunc = ADDRESS::g(prog->readNative4(addr));
            if (prog->findProc(pfunc) == nullptr) {
                // A new, undecoded procedure
                if (Boomerang::get()->noDecodeChildren)
                    return false;
                prog->decodeEntryPoint(pfunc);
                // Since this was not decoded, this is a significant change, and we want to redecode the current
                // function now that the callee has been decoded
                return true;
            }
        }
    }
    return false;
}

/***************************************************************************/ /**
  *
  * \brief    Called when a switch has been identified. Visits the destinations of the switch, adds out edges to the
  *                BB, etc
  * \note    Used to be called as soon as a switch statement is discovered, but this causes decoded but unanalysed
  *          BBs (statements not numbered, locations not SSA renamed etc) to appear in the CFG. This caused problems
  *          when there were nested switch statements. Now only called when re-decoding a switch statement
  * \param   proc - Pointer to the UserProc object for this code
  *
  ******************************************************************************/
void BasicBlock::processSwitch(UserProc *proc) {

    RTL *last(ListOfRTLs->back());
    CaseStatement *lastStmt((CaseStatement *)last->getHlStmt());
    SWITCH_INFO *si(lastStmt->getSwitchInfo());
    Boomerang::get()->debugSwitch = true;
    if (Boomerang::get()->debugSwitch) {
        LOG << "processing switch statement type " << si->chForm << " with table at 0x" << si->uTable << ", ";
        if (si->iNumTable)
            LOG << si->iNumTable << " entries, ";
        LOG << "lo= " << si->iLower << ", hi= " << si->iUpper << "\n";
    }
    ADDRESS uSwitch;
    int iNumOut, iNum;
    iNumOut = si->iUpper - si->iLower + 1;
    iNum = iNumOut;
    // Emit an NWAY BB instead of the COMPJUMP. Also update the number of out edges.
    updateType(BBTYPE::NWAY, iNumOut);

    Prog *prog(proc->getProg());
    Cfg *cfg(proc->getCFG());
    // Where there are repeated switch cases, we have repeated out-edges from the BB. Example:
    // switch (x) {
    //   case 3: case 5:
    //        do something;
    //        break;
    //     case 4: case 10:
    //        do something else
    // ... }
    // The switch statement is emitted assuming one out-edge for each switch value, which is assumed to be iLower+i
    // for the ith zero-based case. It may be that the code for case 5 above will be a goto to the code for case 3,
    // but a smarter back end could group them
    std::list<ADDRESS> dests;
    for (int i = 0; i < iNum; i++) {
        // Get the destination address from the switch table.
        if (si->chForm == 'H') {
            int iValue = prog->readNative4(si->uTable + i * 2);
            if (iValue == -1)
                continue;
            uSwitch = ADDRESS::g(prog->readNative4(si->uTable + i * 8 + 4));
        } else if (si->chForm == 'F')
            uSwitch = ADDRESS::g(((int *)si->uTable.m_value)[i]);
        else
            uSwitch = ADDRESS::g(prog->readNative4(si->uTable + i * 4));
        if ((si->chForm == 'O') || (si->chForm == 'R') || (si->chForm == 'r')) {
            // Offset: add table address to make a real pointer to code.  For type R, the table is relative to the
            // branch, so take iOffset. For others, iOffset is 0, so no harm
            if (si->chForm != 'R')
                assert(si->iOffset == 0);
            uSwitch += si->uTable - si->iOffset;
        }
        if (uSwitch < prog->getLimitTextHigh()) {
            // tq.visit(cfg, uSwitch, this);
            cfg->addOutEdge(this, uSwitch, true);
            // Remember to decode the newly discovered switch code arms, if necessary
            // Don't do it right now, in case there are recursive switch statements (e.g. app7win.exe from
            // hackthissite.org)
            dests.push_back(uSwitch);
        } else {
            LOG << "switch table entry branches to past end of text section " << uSwitch << "\n";
#if 1 // TMN: If we reached an array entry pointing outside the program text, we can be quite confident the array
            // has ended. Don't try to pull any more data from it.
            LOG << "Assuming the end of the pointer-array has been reached at index " << i << "\n";
            // TODO: Elevate this logic to the code calculating iNumTable, but still leave this code as a safeguard.
            // Q: Should iNumOut and m_iNumOutEdges really be adjusted (iNum - i) ?
            //            assert(iNumOut        >= (iNum - i));
            assert(int(OutEdges.size()) >= (iNum - i));
            size_t remove_from_this = OutEdges.size() - (iNum - i);
            // remove last (iNum - i) out edges
            OutEdges.erase(OutEdges.begin() + remove_from_this, OutEdges.end());
            //            iNumOut        -= (iNum - i);
            TargetOutEdges -= (iNum - i);
            break;
#else
            iNumOut--;
            m_iNumOutEdges--; // FIXME: where is this set?
#endif
        }
    }
    // Decode the newly discovered switch code arms, if any, and if not already decoded
    int count = 0;
    for (ADDRESS addr : dests) {
        char tmp[1024];
        count++;
        sprintf(tmp, "before decoding fragment %i of %zi (%" PRIxPTR ")", count, dests.size(), addr.m_value);
        Boomerang::get()->alertDecompileDebugPoint(proc, tmp);
        prog->decodeFragment(proc, addr);
    }
}

/*!
 * Change the BB enclosing stmt to be CALL, not COMPCALL
 */
bool BasicBlock::undoComputedBB(Instruction *stmt) {
    if (!ListOfRTLs)
        return false;
    RTL *last = ListOfRTLs->back();
    for (auto rr = last->rbegin(); rr != last->rend(); rr++) {
        if (*rr == stmt) {
            NodeType = BBTYPE::CALL;
            LOG << "undoComputedBB for statement " << stmt << "\n";
            return true;
        }
    }
    return false;
}

/***************************************************************************/ /**
  * \brief        Searches for all instances of "search" and adds them to "result"
  * in reverse nesting order. The search is optionally type sensitive.
  * \note out of date doc, unless type senistivity is a part of \a search_for ?
  * \param search_for - a location to search for
  * \param results - a list which will have any matching exprs
  *                 appended to it
  * \returns true if there were any matches
  ******************************************************************************/
bool BasicBlock::searchAll(const Exp &search_for, std::list<Exp *> &results) {
    bool ch = false;
    for (RTL *rtl_it : *ListOfRTLs) {
        for (Instruction *e : *rtl_it) {
            Exp *res; // searchAll can be used here too, would it change anything ?
            if (e->search(search_for, res)) {
                ch = true;
                results.push_back(res);
            }
        }
    }
    return ch;
}
/***************************************************************************/ /**
  * \brief Replace all instances of search with replace. Can be type sensitive if
  * reqd
  * \param search - ptr to an expression to search for
  * \param replace the expression with which to replace it
  * \returns true if replacement took place
  ******************************************************************************/
bool BasicBlock::searchAndReplace(const Exp &search, Exp *replace) {
    bool ch = false;

    for (RTL *rtl_it : *ListOfRTLs) {
        for (auto &elem : *rtl_it)
            ch |= (elem)->searchAndReplace(search, replace);
    }
    return ch;
}
