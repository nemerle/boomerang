#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "ExpSubscriptReplacer.h"

#include "boomerang/ssl/exp/RefExp.h"


ExpSubscriptReplacer::ExpSubscriptReplacer(const Statement *original, Statement *replacement)
    : m_orig(original)
    , m_replacement(replacement)
{
}


SharedExp ExpSubscriptReplacer::preModify(const std::shared_ptr<RefExp> &exp, bool &visitChildren)
{
    if (exp->getDef() == m_orig) {
        exp->setDef(m_replacement);
        m_modified = true;
    }

    visitChildren = true;
    return exp;
}
