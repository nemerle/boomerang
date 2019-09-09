#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "StmtSubscriptReplacer.h"

#include "boomerang/ssl/statements/PhiAssign.h"
#include "boomerang/visitor/expmodifier/ExpSubscriptReplacer.h"


StmtSubscriptReplacer::StmtSubscriptReplacer(const Statement *original, Statement *replacement)
    : StmtModifier(new ExpSubscriptReplacer(original, replacement), false)
{
}


void StmtSubscriptReplacer::visit(PhiAssign *stmt, bool &visitChildren)
{
    // replace only the refs on the RHS of the phi, since the ExpModifier already replaces the LHS.
    for (auto it = stmt->begin(); it != stmt->end(); ++it) {
        SharedExp result = (*it)->acceptModifier(m_mod);
        assert(result->isSubscript());
        *it = result->access<RefExp>();
    }

    visitChildren = true;
}
