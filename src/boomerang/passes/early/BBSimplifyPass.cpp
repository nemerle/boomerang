#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "BBSimplifyPass.h"

#include "boomerang/db/BasicBlock.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/ssl/statements/PhiAssign.h"
#include "boomerang/passes/PassManager.h"
#include "boomerang/util/log/Log.h"


BBSimplifyPass::BBSimplifyPass()
    : IPass("BBSimplify", PassID::BBSimplify)
{
}


bool BBSimplifyPass::execute(UserProc *proc)
{
    for (BasicBlock *bb : *proc->getCFG()) {
        bb->simplify();
    }

    StatementList stmts;
    proc->getStatements(stmts);

    for (Statement *stmt : stmts) {
        if (!stmt->isPhi()) {
            continue;
        }

        PhiAssign *phi = static_cast<PhiAssign *>(stmt);

        if (phi->getDefs().empty()) {
            continue;
        }

        /// Convert r24 -> phi{ r25, r25 }  ->  r24 := r25
        bool allSame        = true;
        Statement *firstDef = (*phi->begin())->getDef();

        for (auto &refExp : *phi) {
            if (refExp->getDef() != firstDef) {
                allSame = false;
                break;
            }
        }

        if (allSame) {
            LOG_VERBOSE("all the same in %1", phi);
            proc->replacePhiByAssign(phi, RefExp::get(phi->getLeft(), firstDef));
            PassManager::get()->executePass(PassID::StatementPropagation, proc);
            continue;
        }

        /// Convert r24 := phi{ r24, r24, r25 }  ->  r24 := r25
        bool onlyOneNotThis = true;
        Statement *notthis  = STMT_WILD;

        for (const std::shared_ptr<RefExp> &ref : *phi) {
            Statement *def = ref->getDef();
            if (def == phi) {
                continue; // ok
            }
            else if (notthis == STMT_WILD) {
                notthis = def;
            }
            else {
                onlyOneNotThis = false;
                break;
            }
        }

        if (onlyOneNotThis && (notthis != STMT_WILD)) {
            LOG_VERBOSE("All but one not this in %1", phi);

            proc->replacePhiByAssign(phi, RefExp::get(phi->getLeft(), notthis));
            PassManager::get()->executePass(PassID::StatementPropagation, proc);
            continue;
        }
    }

    return true;
}
