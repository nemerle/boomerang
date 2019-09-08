#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License


#include "boomerang/visitor/stmtmodifier/StmtModifier.h"


class Statement;


class BOOMERANG_API StmtSubscriptReplacer : public StmtModifier
{
public:
    StmtSubscriptReplacer(const Statement *original, Statement *replacement);
    virtual ~StmtSubscriptReplacer() override = default;

public:
    /// \copydoc StmtModifier::visit
    virtual void visit(PhiAssign *stmt, bool &visitChildren) override;
};
