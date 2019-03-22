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


#include "boomerang/visitor/stmtexpvisitor/StmtExpVisitor.h"


class ConstFinder;


/**
 *
 */
class BOOMERANG_API StmtConstFinder : public StmtExpVisitor
{
public:
    StmtConstFinder(ConstFinder *v);
    virtual ~StmtConstFinder() = default;
};
