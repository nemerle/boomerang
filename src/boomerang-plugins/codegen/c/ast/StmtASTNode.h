#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#pragma once


#include "ASTNode.h"

#include "boomerang/ssl/statements/Statement.h"

#include <vector>


class StmtASTNode : public ASTNode
{
public:
    StmtASTNode(const SharedConstStmt &stmt);
    virtual ~StmtASTNode() override;

public:
    bool isStmt() const override { return true; }

    /// \copydoc ASTNode::printAST
    void printAST(OStream &os) const override;

    template<typename T = Statement>
    const std::shared_ptr<const T> getStatement() const
    {
        return m_stmt->as<const T>();
    }

    void addPredecessor(StmtASTNode *pred);
    void addSuccessor(StmtASTNode *succ);

    int getNumPredecessors() const;
    int getNumSuccessors() const;

    StmtASTNode *getPredecessor(int i);
    StmtASTNode *getSuccessor(int i);

    const StmtASTNode *getPredecessor(int i) const;
    const StmtASTNode *getSuccessor(int i) const;

    const std::vector<StmtASTNode *> &getPredecessors() const;
    const std::vector<StmtASTNode *> &getSuccessors() const;

private:
    SharedConstStmt m_stmt;
    std::vector<StmtASTNode *> m_successors;
    std::vector<StmtASTNode *> m_predecessors;
};
