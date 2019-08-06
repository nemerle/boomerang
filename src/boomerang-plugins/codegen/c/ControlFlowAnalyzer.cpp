#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "ControlFlowAnalyzer.h"

#include "boomerang/db/BasicBlock.h"
#include "boomerang/db/proc/ProcCFG.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/ssl/RTL.h"
#include "boomerang/ssl/statements/BranchStatement.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/CaseStatement.h"
#include "boomerang/ssl/statements/ReturnStatement.h"
#include "boomerang/util/log/Log.h"

#include <QFile>


ControlFlowAnalyzer::ControlFlowAnalyzer()
{
}


void ControlFlowAnalyzer::structureCFG(ProcCFG *cfg)
{
    m_cfg = cfg;
    m_nodes.clear();
    m_postOrdering.clear();
    m_revPostOrdering.clear();
    m_info.clear();

    rebuildASTForest();

    if (m_cfg->findRetNode() == nullptr) {
        return;
    }

    setTimeStamps();
    updateImmedPDom();

    structConds();
    structLoops();
    checkConds();

    unTraverse();
}


void ControlFlowAnalyzer::setTimeStamps()
{
    // set the parenthesis for the nodes as well as setting the post-order ordering between the
    // nodes
    int time = 1;
    m_postOrdering.clear();

    updateLoopStamps(findEntryNode(), time);

    // set the reverse parenthesis for the nodes
    time = 1;
    updateRevLoopStamps(findEntryNode(), time);

    StmtASTNode *retNode = findExitNode();
    assert(retNode);
    m_revPostOrdering.clear();
    updateRevOrder(retNode);
}


void ControlFlowAnalyzer::updateImmedPDom()
{
    // traverse the nodes in order (i.e from the bottom up)
    for (int i = m_revPostOrdering.size() - 1; i >= 0; i--) {
        const StmtASTNode *node = m_revPostOrdering[i];

        for (StmtASTNode *succ : node->getSuccessors()) {
            if (getRevOrd(succ) > getRevOrd(node)) {
                setImmPDom(node, findCommonPDom(getImmPDom(node), succ));
            }
        }
    }

    // make a second pass but consider the original CFG ordering this time
    for (const StmtASTNode *node : m_postOrdering) {
        if (node->getNumSuccessors() <= 1) {
            continue;
        }

        for (auto &succ : node->getSuccessors()) {
            StmtASTNode *succNode = succ;
            setImmPDom(node, findCommonPDom(getImmPDom(node), succNode));
        }
    }

    // one final pass to fix up nodes involved in a loop
    for (const StmtASTNode *node : m_postOrdering) {
        if (node->getNumSuccessors() > 1) {
            for (auto &succ : node->getSuccessors()) {
                StmtASTNode *succNode = succ;

                if (isBackEdge(node, succNode) && (node->getNumSuccessors() > 1) &&
                    getImmPDom(succNode) &&
                    (getPostOrdering(getImmPDom(succ)) < getPostOrdering(getImmPDom(node)))) {
                    setImmPDom(node, findCommonPDom(getImmPDom(succNode), getImmPDom(node)));
                }
                else {
                    setImmPDom(node, findCommonPDom(getImmPDom(node), succNode));
                }
            }
        }
    }
}


const StmtASTNode *ControlFlowAnalyzer::findCommonPDom(const StmtASTNode *currImmPDom,
                                                       const StmtASTNode *succImmPDom)
{
    if (!currImmPDom) {
        return succImmPDom;
    }

    if (!succImmPDom) {
        return currImmPDom;
    }

    if (getRevOrd(currImmPDom) == getRevOrd(succImmPDom)) {
        return currImmPDom; // ordering hasn't been done
    }

    const StmtASTNode *oldCurImmPDom = currImmPDom;

    int giveup = 0;
#define GIVEUP 10000

    while (giveup < GIVEUP && currImmPDom && succImmPDom && (currImmPDom != succImmPDom)) {
        if (getRevOrd(currImmPDom) > getRevOrd(succImmPDom)) {
            succImmPDom = getImmPDom(succImmPDom);
        }
        else {
            currImmPDom = getImmPDom(currImmPDom);
        }

        giveup++;
    }

    if (giveup >= GIVEUP) {
        return oldCurImmPDom; // no change
    }

    return currImmPDom;
}


void ControlFlowAnalyzer::structConds()
{
    // Process the nodes in order
    for (const StmtASTNode *currNode : m_postOrdering) {
        if (currNode->getNumSuccessors() <= 1) {
            // not an if/case condition
            continue;
        }

        // if the current conditional header is a two way node and has a back edge, then it
        // won't have a follow
        if (hasBackEdge(currNode) && (currNode->getStatement()->isBranch())) {
            setStructType(currNode, StructType::Cond);
            continue;
        }

        // set the follow of a node to be its immediate post dominator
        setCondFollow(currNode, getImmPDom(currNode));

        // set the structured type of this node
        setStructType(currNode, StructType::Cond);

        // if this is an nway header, then we have to tag each of the nodes within the body of
        // the nway subgraph
        if (getCondType(currNode) == CondType::Case) {
            setCaseHead(currNode, currNode, getCondFollow(currNode));
        }
    }
}


void ControlFlowAnalyzer::determineLoopType(const StmtASTNode *header, bool *&loopNodes)
{
    assert(getLatchNode(header));

    // if the latch node is a two way node then this must be a post tested loop
    if (getLatchNode(header)->getStatement()->isBranch()) {
        setLoopType(header, LoopType::PostTested);

        // if the head of the loop is a two way node and the loop spans more than one block  then it
        // must also be a conditional header
        if (header->getStatement()->isBranch() && (header != getLatchNode(header))) {
            setStructType(header, StructType::LoopCond);
        }
    }
    // otherwise it is either a pretested or endless loop
    else if (header->getStatement()->isBranch()) {
        // if the header is a two way node then it must have a conditional follow (since it can't
        // have any backedges leading from it). If this follow is within the loop then this must be
        // an endless loop
        if (getCondFollow(header) && loopNodes[getPostOrdering(getCondFollow(header))]) {
            setLoopType(header, LoopType::Endless);

            // retain the fact that this is also a conditional header
            setStructType(header, StructType::LoopCond);
        }
        else {
            setLoopType(header, LoopType::PreTested);
        }
    }
    else {
        // both the header and latch node are one way nodes so this must be an endless loop
        setLoopType(header, LoopType::Endless);
    }
}


void ControlFlowAnalyzer::findLoopFollow(const StmtASTNode *header, bool *&loopNodes)
{
    assert(getStructType(header) == StructType::Loop ||
           getStructType(header) == StructType::LoopCond);
    const LoopType loopType  = getLoopType(header);
    const StmtASTNode *latch = getLatchNode(header);

    if (loopType == LoopType::PreTested) {
        // if the 'while' loop's true child is within the loop, then its false child is the loop
        // follow
        if (loopNodes[getPostOrdering(header->getSuccessor(BTHEN))]) {
            setLoopFollow(header, header->getSuccessor(BELSE));
        }
        else {
            setLoopFollow(header, header->getSuccessor(BTHEN));
        }
    }
    else if (loopType == LoopType::PostTested) {
        // the follow of a post tested ('repeat') loop is the node on the end of the non-back edge
        // from the latch node
        if (latch->getSuccessor(BELSE) == header) {
            setLoopFollow(header, latch->getSuccessor(BTHEN));
        }
        else {
            setLoopFollow(header, latch->getSuccessor(BELSE));
        }
    }
    else {
        // endless loop
        const StmtASTNode *follow = nullptr;

        // traverse the ordering array between the header and latch nodes.
        // StmtASTNode * latch = header->getLatchNode(); initialized at function start
        for (int i = getPostOrdering(header) - 1; i > getPostOrdering(latch); i--) {
            const StmtASTNode *&desc = m_postOrdering[i];
            // the follow for an endless loop will have the following
            // properties:
            //   i) it will have a parent that is a conditional header inside the loop whose follow
            //      is outside the loop
            //  ii) it will be outside the loop according to its loop stamp pair
            // iii) have the highest ordering of all suitable follows (i.e. highest in the graph)

            if ((getStructType(desc) == StructType::Cond) && getCondFollow(desc) &&
                (getLoopHead(desc) == header)) {
                if (loopNodes[getPostOrdering(getCondFollow(desc))]) {
                    // if the conditional's follow is in the same loop AND is lower in the loop,
                    // jump to this follow
                    if (getPostOrdering(desc) > getPostOrdering(getCondFollow(desc))) {
                        i = getPostOrdering(getCondFollow(desc));
                    }
                    else {
                        // otherwise there is a backward jump somewhere to a node earlier in this
                        // loop. We don't need to any nodes below this one as they will all have a
                        // conditional within the loop.
                        break;
                    }
                }
                else {
                    // otherwise find the child (if any) of the conditional header that isn't inside
                    // the same loop
                    const StmtASTNode *succ = desc->getSuccessor(BTHEN);

                    if (loopNodes[getPostOrdering(succ)]) {
                        if (!loopNodes[getPostOrdering(desc->getSuccessor(BELSE))]) {
                            succ = desc->getSuccessor(BELSE);
                        }
                        else {
                            succ = nullptr;
                        }
                    }

                    // if a potential follow was found, compare its ordering with the currently
                    // found follow
                    if (succ && (!follow || (getPostOrdering(succ) > getPostOrdering(follow)))) {
                        follow = succ;
                    }
                }
            }
        }

        // if a follow was found, assign it to be the follow of the loop under
        // investigation
        if (follow) {
            setLoopFollow(header, follow);
        }
    }
}


void ControlFlowAnalyzer::tagNodesInLoop(const StmtASTNode *header, bool *&loopNodes)
{
    // Traverse the ordering structure from the header to the latch node tagging the nodes
    // determined to be within the loop. These are nodes that satisfy the following:
    //    i)   header.loopStamps encloses curNode.loopStamps and curNode.loopStamps encloses
    //         latch.loopStamps
    //    OR
    //   ii)   latch.revLoopStamps encloses curNode.revLoopStamps and curNode.revLoopStamps encloses
    //         header.revLoopStamps
    //    OR
    //  iii) curNode is the latch node

    const StmtASTNode *latch = getLatchNode(header);
    assert(latch);

    for (int i = getPostOrdering(header) - 1; i >= getPostOrdering(latch); i--) {
        if (isNodeInLoop(m_postOrdering[i], header, latch)) {
            // update the membership map to reflect that this node is within the loop
            loopNodes[i] = true;

            setLoopHead(m_postOrdering[i], header);
        }
    }
}


void ControlFlowAnalyzer::structLoops()
{
    for (int i = m_postOrdering.size() - 1; i >= 0; i--) {
        const StmtASTNode *currNode = m_postOrdering[i]; // the current node under investigation
        const StmtASTNode *latch    = nullptr;           // the latching node of the loop

        // If the current node has at least one back edge into it, it is a loop header. If there are
        // numerous back edges into the header, determine which one comes form the proper latching
        // node. The proper latching node is defined to have the following properties:
        //     i) has a back edge to the current node
        //    ii) has the same case head as the current node
        //   iii) has the same loop head as the current node
        //    iv) is not an nway node
        //     v) is not the latch node of an enclosing loop
        //    vi) has a lower ordering than all other suitable candiates
        // If no nodes meet the above criteria, then the current node is not a loop header

        for (const StmtASTNode *pred : currNode->getPredecessors()) {
            if ((getCaseHead(pred) == getCaseHead(currNode)) &&                      // ii)
                (getLoopHead(pred) == getLoopHead(currNode)) &&                      // iii)
                (!latch || (getPostOrdering(latch) > getPostOrdering(pred))) &&      // vi)
                !(getLoopHead(pred) && (getLatchNode(getLoopHead(pred)) == pred)) && // v)
                isBackEdge(pred, currNode)) {                                        // i)
                latch = pred;
            }
        }

        // if a latching node was found for the current node then it is a loop header.
        if (!latch) {
            continue;
        }

        // define the map that maps each node to whether or not it is within the current loop
        bool *loopNodes = new bool[m_postOrdering.size()];

        for (unsigned int j = 0; j < m_postOrdering.size(); j++) {
            loopNodes[j] = false;
        }

        setLatchNode(currNode, latch);

        // the latching node may already have been structured as a conditional header. If it is
        // not also the loop header (i.e. the loop is over more than one block) then reset it to
        // be a sequential node otherwise it will be correctly set as a loop header only later
        if ((latch != currNode) && (getStructType(latch) == StructType::Cond)) {
            setStructType(latch, StructType::Seq);
        }

        // set the structured type of this node
        setStructType(currNode, StructType::Loop);

        // tag the members of this loop
        tagNodesInLoop(currNode, loopNodes);

        // calculate the type of this loop
        determineLoopType(currNode, loopNodes);

        // calculate the follow node of this loop
        findLoopFollow(currNode, loopNodes);

        delete[] loopNodes;
    }
}


void ControlFlowAnalyzer::checkConds()
{
    for (const StmtASTNode *currNode : m_postOrdering) {
        // consider only conditional headers that have a follow and aren't case headers
        if (((getStructType(currNode) == StructType::Cond) ||
             (getStructType(currNode) == StructType::LoopCond)) &&
            getCondFollow(currNode) && (getCondType(currNode) != CondType::Case)) {
            // define convenient aliases for the relevant loop and case heads and the out edges
            const StmtASTNode *myLoopHead = (getStructType(currNode) == StructType::LoopCond)
                                                ? currNode
                                                : getLoopHead(currNode);
            const StmtASTNode *follLoopHead = getLoopHead(getCondFollow(currNode));
            const StmtASTNode *thenNode     = currNode->getSuccessor(BTHEN);
            const StmtASTNode *elseNode     = currNode->getSuccessor(BELSE);

            // analyse whether this is a jump into/outof a loop
            if (myLoopHead != follLoopHead) {
                // we want to find the branch that the latch node is on for a jump out of a loop
                if (myLoopHead) {
                    // this is a jump out of a loop (break or return)
                    if (getLoopHead(thenNode) != nullptr) {
                        // the "else" branch jumps out of the loop. (e.g. "if (!foo) break;")
                        setUnstructType(currNode, UnstructType::JumpInOutLoop);
                        setCondType(currNode, CondType::IfElse);
                    }
                    else {
                        assert(getLoopHead(elseNode) != nullptr);
                        // the "then" branch jumps out of the loop
                        setUnstructType(currNode, UnstructType::JumpInOutLoop);
                        setCondType(currNode, CondType::IfThen);
                    }
                }

                if ((getUnstructType(currNode) == UnstructType::Structured) && follLoopHead) {
                    // find the branch that the loop head is on for a jump into a loop body. If a
                    // branch has already been found, then it will match this one anyway

                    // does the else branch goto the loop head?
                    if (isBackEdge(thenNode, follLoopHead)) {
                        setUnstructType(currNode, UnstructType::JumpInOutLoop);
                        setCondType(currNode, CondType::IfElse);
                    }
                    // does the else branch goto the loop head?
                    else if (isBackEdge(elseNode, follLoopHead)) {
                        setUnstructType(currNode, UnstructType::JumpInOutLoop);
                        setCondType(currNode, CondType::IfThen);
                    }
                }
            }

            // this is a jump into a case body if either of its children don't have the same same
            // case header as itself
            if ((getUnstructType(currNode) == UnstructType::Structured) &&
                ((getCaseHead(currNode) != getCaseHead(thenNode)) ||
                 (getCaseHead(currNode) != getCaseHead(elseNode)))) {
                const StmtASTNode *myCaseHead   = getCaseHead(currNode);
                const StmtASTNode *thenCaseHead = getCaseHead(thenNode);
                const StmtASTNode *elseCaseHead = getCaseHead(elseNode);

                if ((thenCaseHead == myCaseHead) &&
                    (!myCaseHead || (elseCaseHead != getCondFollow(myCaseHead)))) {
                    setUnstructType(currNode, UnstructType::JumpIntoCase);
                    setCondType(currNode, CondType::IfElse);
                }
                else if ((elseCaseHead == myCaseHead) &&
                         (!myCaseHead || (thenCaseHead != getCondFollow(myCaseHead)))) {
                    setUnstructType(currNode, UnstructType::JumpIntoCase);
                    setCondType(currNode, CondType::IfThen);
                }
            }
        }

        // for 2 way conditional headers that don't have a follow (i.e. are the source of a back
        // edge) and haven't been structured as latching nodes, set their follow to be the non-back
        // edge child.
        if ((getStructType(currNode) == StructType::Cond) && !getCondFollow(currNode) &&
            (getCondType(currNode) != CondType::Case) &&
            (getUnstructType(currNode) == UnstructType::Structured)) {
            // latching nodes will already have been reset to Seq structured type
            if (hasBackEdge(currNode)) {
                if (isBackEdge(currNode, currNode->getSuccessor(BTHEN))) {
                    setCondType(currNode, CondType::IfThen);
                    setCondFollow(currNode, currNode->getSuccessor(BELSE));
                }
                else {
                    setCondType(currNode, CondType::IfElse);
                    setCondFollow(currNode, currNode->getSuccessor(BTHEN));
                }
            }
        }
    }
}


bool ControlFlowAnalyzer::isBackEdge(const StmtASTNode *source, const StmtASTNode *dest) const
{
    return dest == source || isAncestorOf(dest, source);
}


bool ControlFlowAnalyzer::isCaseOption(const StmtASTNode *node) const
{
    if (!getCaseHead(node)) {
        return false;
    }

    for (int i = 0; i < getCaseHead(node)->getNumSuccessors() - 1; i++) {
        if (getCaseHead(node)->getSuccessor(i) == node) {
            return true;
        }
    }

    return false;
}


bool ControlFlowAnalyzer::isAncestorOf(const StmtASTNode *node, const StmtASTNode *other) const
{
    return (m_info[node].m_preOrderID < m_info[other].m_preOrderID &&
            m_info[node].m_postOrderID > m_info[other].m_postOrderID) ||
           (m_info[node].m_revPreOrderID < m_info[other].m_revPreOrderID &&
            m_info[node].m_revPostOrderID > m_info[other].m_revPostOrderID);
}


void ControlFlowAnalyzer::updateLoopStamps(const StmtASTNode *node, int &time)
{
    // timestamp the current node with the current time
    // and set its traversed flag
    setTravType(node, TravType::DFS_LNum);
    m_info[node].m_preOrderID = time;

    // recurse on unvisited children and set inedges for all children
    for (const StmtASTNode *succ : node->getSuccessors()) {
        // set the in edge from this child to its parent (the current node)
        // (not done here, might be a problem)
        // outEdges[i]->inEdges.Add(this);

        // recurse on this child if it hasn't already been visited
        if (getTravType(succ) != TravType::DFS_LNum) {
            updateLoopStamps(succ, ++time);
        }
    }

    // set the the second loopStamp value
    m_info[node].m_postOrderID = ++time;

    // add this node to the ordering structure as well as recording its position within the ordering
    m_info[node].m_postOrderIndex = static_cast<int>(m_postOrdering.size());
    m_postOrdering.push_back(node);
}


void ControlFlowAnalyzer::updateRevLoopStamps(const StmtASTNode *node, int &time)
{
    // timestamp the current node with the current time and set its traversed flag
    setTravType(node, TravType::DFS_RNum);
    m_info[node].m_revPreOrderID = time;

    // recurse on the unvisited children in reverse order
    for (int i = node->getNumSuccessors() - 1; i >= 0; i--) {
        // recurse on this child if it hasn't already been visited
        if (getTravType(node->getSuccessor(i)) != TravType::DFS_RNum) {
            updateRevLoopStamps(node->getSuccessor(i), ++time);
        }
    }

    m_info[node].m_revPostOrderID = ++time;
}


void ControlFlowAnalyzer::updateRevOrder(const StmtASTNode *node)
{
    // Set this node as having been traversed during the post domimator DFS ordering traversal
    setTravType(node, TravType::DFS_PDom);

    // recurse on unvisited children
    for (const StmtASTNode *pred : node->getPredecessors()) {
        if (getTravType(pred) != TravType::DFS_PDom) {
            updateRevOrder(pred);
        }
    }

    // add this node to the ordering structure and record the post dom. order of this node as its
    // index within this ordering structure
    m_info[node].m_revPostOrderIndex = static_cast<int>(m_revPostOrdering.size());
    m_revPostOrdering.push_back(node);
}


void ControlFlowAnalyzer::setCaseHead(const StmtASTNode *node, const StmtASTNode *head,
                                      const StmtASTNode *follow)
{
    assert(!getCaseHead(node));

    setTravType(node, TravType::DFS_Case);

    // don't tag this node if it is the case header under investigation
    if (node != head) {
        m_info[node].m_caseHead = head;
    }

    // if this is a nested case header, then it's member nodes
    // will already have been tagged so skip straight to its follow
    if (node->getStatement()->isCase() && (node != head)) {
        if (getCondFollow(node) && (getTravType(getCondFollow(node)) != TravType::DFS_Case) &&
            (getCondFollow(node) != follow)) {
            setCaseHead(node, head, follow);
        }
    }
    else {
        // traverse each child of this node that:
        //   i) isn't on a back-edge,
        //  ii) hasn't already been traversed in a case tagging traversal and,
        // iii) isn't the follow node.
        for (StmtASTNode *succ : node->getSuccessors()) {
            if (!isBackEdge(node, succ) && (getTravType(succ) != TravType::DFS_Case) &&
                (succ != follow)) {
                setCaseHead(succ, head, follow);
            }
        }
    }
}


void ControlFlowAnalyzer::setStructType(const StmtASTNode *node, StructType structType)
{
    // if this is a conditional header, determine exactly which type of conditional header it is
    // (i.e. switch, if-then, if-then-else etc.)
    if (structType == StructType::Cond) {
        if (node->getStatement()->isCase()) {
            m_info[node].m_conditionHeaderType = CondType::Case;
        }
        else if (getCondFollow(node) == node->getSuccessor(BELSE)) {
            m_info[node].m_conditionHeaderType = CondType::IfThen;
        }
        else if (getCondFollow(node) == node->getSuccessor(BTHEN)) {
            m_info[node].m_conditionHeaderType = CondType::IfElse;
        }
        else {
            m_info[node].m_conditionHeaderType = CondType::IfThenElse;
        }
    }

    m_info[node].m_structuringType = structType;
}


void ControlFlowAnalyzer::setUnstructType(const StmtASTNode *node, UnstructType unstructType)
{
    assert((m_info[node].m_structuringType == StructType::Cond ||
            m_info[node].m_structuringType == StructType::LoopCond) &&
           m_info[node].m_conditionHeaderType != CondType::Case);
    m_info[node].m_unstructuredType = unstructType;
}


UnstructType ControlFlowAnalyzer::getUnstructType(const StmtASTNode *node) const
{
    assert((m_info[node].m_structuringType == StructType::Cond ||
            m_info[node].m_structuringType == StructType::LoopCond));
    // fails when cenerating code for switches; not sure if actually needed TODO
    // assert(m_conditionHeaderType != CondType::Case);

    return m_info[node].m_unstructuredType;
}


void ControlFlowAnalyzer::setLoopType(const StmtASTNode *node, LoopType l)
{
    assert(getStructType(node) == StructType::Loop || getStructType(node) == StructType::LoopCond);
    m_info[node].m_loopHeaderType = l;

    // set the structured class (back to) just Loop if the loop type is PreTested OR it's PostTested
    // and is a single block loop
    if ((m_info[node].m_loopHeaderType == LoopType::PreTested) ||
        ((m_info[node].m_loopHeaderType == LoopType::PostTested) && (node == getLatchNode(node)))) {
        setStructType(node, StructType::Loop);
    }
}


LoopType ControlFlowAnalyzer::getLoopType(const StmtASTNode *node) const
{
    assert(getStructType(node) == StructType::Loop || getStructType(node) == StructType::LoopCond);
    return m_info[node].m_loopHeaderType;
}


void ControlFlowAnalyzer::setCondType(const StmtASTNode *node, CondType condType)
{
    assert(getStructType(node) == StructType::Cond || getStructType(node) == StructType::LoopCond);
    m_info[node].m_conditionHeaderType = condType;
}


CondType ControlFlowAnalyzer::getCondType(const StmtASTNode *node) const
{
    assert(getStructType(node) == StructType::Cond || getStructType(node) == StructType::LoopCond);
    return m_info[node].m_conditionHeaderType;
}


bool ControlFlowAnalyzer::isNodeInLoop(const StmtASTNode *node, const StmtASTNode *header,
                                       const StmtASTNode *latch) const
{
    assert(getLatchNode(header) == latch);
    assert(header == latch || ((m_info[header].m_preOrderID > m_info[latch].m_preOrderID &&
                                m_info[latch].m_postOrderID > m_info[header].m_postOrderID) ||
                               (m_info[header].m_preOrderID < m_info[latch].m_preOrderID &&
                                m_info[latch].m_postOrderID < m_info[header].m_postOrderID)));

    // this node is in the loop if it is the latch node OR
    // this node is within the header and the latch is within this when using the forward loop
    // stamps OR this node is within the header and the latch is within this when using the reverse
    // loop stamps
    return node == latch ||
           (m_info[header].m_preOrderID < m_info[node].m_preOrderID &&
            m_info[node].m_postOrderID < m_info[header].m_postOrderID &&
            m_info[node].m_preOrderID < m_info[latch].m_preOrderID &&
            m_info[latch].m_postOrderID < m_info[node].m_postOrderID) ||
           (m_info[header].m_revPreOrderID < m_info[node].m_revPreOrderID &&
            m_info[node].m_revPostOrderID < m_info[header].m_revPostOrderID &&
            m_info[node].m_revPreOrderID < m_info[latch].m_revPreOrderID &&
            m_info[latch].m_revPostOrderID < m_info[node].m_revPostOrderID);
}


bool ControlFlowAnalyzer::hasBackEdge(const StmtASTNode *node) const
{
    return std::any_of(node->getSuccessors().begin(), node->getSuccessors().end(),
                       [this, node](const StmtASTNode *succ) { return isBackEdge(node, succ); });
}


void ControlFlowAnalyzer::unTraverse()
{
    for (auto &elem : m_info) {
        elem.second.m_travType = TravType::Untraversed;
    }
}


StmtASTNode *ControlFlowAnalyzer::findEntryNode() const
{
    const BasicBlock *bb = m_cfg->getEntryBB();
    while (bb) {
        const auto it = m_nodes.find(bb->getFirstStmt());
        if (it != m_nodes.end()) {
            return it->second;
        }
        bb = bb->getSuccessor(0);
    }

    return nullptr; // not found
}


StmtASTNode *ControlFlowAnalyzer::findExitNode() const
{
    const BasicBlock *exitBB = m_cfg->findRetNode();
    if (exitBB) {
        const auto it = m_nodes.find(exitBB->getLastStmt());
        if (it != m_nodes.end()) {
            return it->second;
        }
    }

    return nullptr; // not found
}


void ControlFlowAnalyzer::rebuildASTForest()
{
    // Wire up successors within a BB
    for (const BasicBlock *bb : *m_cfg) {
        SharedConstStmt prev = nullptr;
        for (const auto &rtl : *bb->getRTLs()) {
            for (SharedConstStmt stmt : *rtl) {
                m_nodes[stmt] = new StmtASTNode(stmt);
                if (prev != nullptr) {
                    m_nodes[stmt]->addPredecessor(m_nodes[prev]);
                    m_nodes[prev]->addSuccessor(m_nodes[stmt]);
                }
                prev = stmt;
            }
        }
    }

    // wire up successors between BBs
    for (const BasicBlock *bb : *m_cfg) {
        SharedConstStmt lastStmt = bb->getLastStmt();

        // it is important to process the nodes in order to preserve ordering for branches
        for (const BasicBlock *succ : bb->getSuccessors()) {
            SharedConstStmt firstStmt = findSuccessorStmt(lastStmt, succ);
            if (!lastStmt || !firstStmt) {
                continue;
            }

            m_nodes[lastStmt]->addSuccessor(m_nodes[firstStmt]);
            m_nodes[firstStmt]->addPredecessor(m_nodes[lastStmt]);
        }
    }


    // debug
    dumpStmtCFGToFile();
}


void ControlFlowAnalyzer::dumpStmtCFGToFile() const
{
    QFile dest("StmtCFG.dot");
    dest.open(QFile::WriteOnly);
    OStream ost(&dest);

    ost << "digraph StmtCFG {\n\n";

    for (auto &[stmt, node] : m_nodes) {
        QString label;

        if (stmt->isCall()) {
            const Function *proc = node->getStatement<CallStatement>()->getDestProc();
            label                = QString("CALL ") + (proc ? proc->getName() : "/* no dest */");
            label += "(";
            for (auto &arg : node->getStatement<CallStatement>()->getArguments()) {
                label += arg->as<const Assign>()->getRight()->toString() + ",";
            }

            if (label.endsWith(",")) {
                label.chop(1);
            }
            label += ")";
        }
        else if (stmt->isCase()) {
            label = QString("CASE ");
            if (node->getStatement<CaseStatement>()->getSwitchInfo()) {
                label += node->getStatement<CaseStatement>()
                             ->getSwitchInfo()
                             ->switchExp->toString();
            }
        }
        else if (stmt->isBranch()) {
            label = "BRANCH if " + stmt->as<const BranchStatement>()->getCondExpr()->toString();
        }
        else if (stmt->isReturn()) {
            label = QString("RET ");
            for (auto &ret : node->getStatement<ReturnStatement>()->getReturns()) {
                label += ret->as<const Assign>()->getRight()->toString() + ",";
            }

            if (label.endsWith(",")) {
                label.chop(1);
            }
        }
        else {
            label = stmt->toString();
        }

        label = label.replace("\n", " ");
        label = label.replace("\"", "'");

        ost << "stmt" << stmt->getNumber() << "[label=\"";
        ost << label << "\"];\n";
    }

    ost << "\n";

    for (auto &[stmt, node] : m_nodes) {
        if (stmt->isBranch() && node->getNumSuccessors() == 2) {
            ost << "stmt" << stmt->getNumber() << " -> stmt"
                << node->getSuccessor(BTHEN)->getStatement()->getNumber() << "[color=green];\n";
            ost << "stmt" << stmt->getNumber() << " -> stmt"
                << node->getSuccessor(BELSE)->getStatement()->getNumber() << "[color=red];\n";
        }
        else if (stmt->isCase()) {
            for (int i = 0; i < node->getNumSuccessors(); ++i) {
                StmtASTNode *succ = node->getSuccessor(i);
                ost << "stmt" << stmt->getNumber() << " -> stmt"
                    << succ->getStatement()->getNumber() << "[label=\"";

                const SwitchInfo *psi = node->getStatement<CaseStatement>()->getSwitchInfo();
                if (psi->switchType == SwitchType::F) { // "Fortran" style?
                    // Yes, use the table value itself
                    ost << reinterpret_cast<int *>(psi->tableAddr.value())[i];
                }
                else {
                    // Note that uTable has the address of an int array
                    ost << static_cast<int>(psi->lowerBound + i);
                }
                ost << "\"]\n";
            }
        }
        else {
            for (auto &succ : node->getSuccessors()) {
                ost << "stmt" << stmt->getNumber() << " -> stmt"
                    << succ->getStatement()->getNumber() << ";\n";
            }
        }
    }

    ost << "}";
    dest.close();
}


SharedConstStmt ControlFlowAnalyzer::findSuccessorStmt(const SharedConstStmt &stmt,
                                                       const BasicBlock *successorBB) const
{
    if (stmt == nullptr) {
        return nullptr;
    }

    std::set<const BasicBlock *> visitedBBs;
    const BasicBlock *currBB = successorBB;

    while (currBB->isEmpty()) {
        if (visitedBBs.find(currBB) != visitedBBs.end()) {
            return nullptr; // loop with empty BBs
        }

        visitedBBs.insert(currBB);
        assert(currBB->getNumSuccessors() == 1);
        currBB = currBB->getSuccessor(BTHEN);
    }

    return currBB->getFirstStmt();
}
