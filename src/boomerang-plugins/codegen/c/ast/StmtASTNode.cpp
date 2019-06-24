#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "StmtASTNode.h"

#include "boomerang/ssl/statements/Statement.h"


StmtASTNode::StmtASTNode(const SharedConstStmt &stmt)
    : m_stmt(stmt)
{
}


StmtASTNode::~StmtASTNode()
{
}


void StmtASTNode::addPredecessor(StmtASTNode *pred)
{
    m_predecessors.push_back(pred);
}


void StmtASTNode::addSuccessor(StmtASTNode *succ)
{
    m_successors.push_back(succ);
}


int StmtASTNode::getNumPredecessors() const
{
    return m_predecessors.size();
}


int StmtASTNode::getNumSuccessors() const
{
    return m_successors.size();
}


StmtASTNode *StmtASTNode::getPredecessor(int i)
{
    return m_predecessors.at(i);
}


StmtASTNode *StmtASTNode::getSuccessor(int i)
{
    return m_successors.at(i);
}


const StmtASTNode *StmtASTNode::getPredecessor(int i) const
{
    return m_predecessors.at(i);
}


const StmtASTNode *StmtASTNode::getSuccessor(int i) const
{
    return m_successors.at(i);
}


const std::vector<StmtASTNode *> &StmtASTNode::getPredecessors() const
{
    return m_predecessors;
}


const std::vector<StmtASTNode *> &StmtASTNode::getSuccessors() const
{
    return m_successors;
}


void StmtASTNode::printAST(OStream &os) const
{
    m_stmt->print(os);
}
