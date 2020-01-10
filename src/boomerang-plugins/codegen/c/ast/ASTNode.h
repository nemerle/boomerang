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


class BasicBlock;
class OStream;


/**
 * Base class for all nodes in the Abstract Syntax Tree.
 */
class ASTNode
{
public:
    ASTNode();
    ASTNode(const ASTNode &other) = delete;
    ASTNode(ASTNode &&other)      = default;

    virtual ~ASTNode();

    ASTNode &operator=(const ASTNode &other) = delete;
    ASTNode &operator=(ASTNode &&other) = default;

public:
    virtual bool isStmt() const { return false; }

    virtual void printAST(OStream &os) const = 0;
};
