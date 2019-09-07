#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "CallAndPhiFixPass.h"

#include "boomerang/db/proc/UserProc.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/passes/PassManager.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/PhiAssign.h"
#include "boomerang/ssl/type/Type.h"
#include "boomerang/util/StatementList.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/visitor/expmodifier/CallBypasser.h"


CallAndPhiFixPass::CallAndPhiFixPass()
    : IPass("CallAndPhiFix", PassID::CallAndPhiFix)
{
}


bool CallAndPhiFixPass::execute(UserProc *proc)
{
    /* Algorithm:
     *      for each statement s in this proc
     *        if s is a phi statement ps
     *              let r be a ref made up of lhs and s
     *              for each parameter p of ps
     *                if p == r                        // e.g. test/pentium/fromssa2 r28{56}
     *                      remove p from ps
     *              let lhs be left hand side of ps
     *              allSame = true
     *              let first be a ref built from first p
     *              do bypass but not propagation on first
     *              if result is of the form lhs{x}
     *                replace first with x
     *              for each parameter p of ps after the first
     *                let current be a ref built from p
     *                do bypass but not propagation on current
     *                if result is of form lhs{x}
     *                      replace cur with x
     *                if first != current
     *                      allSame = false
     *              if allSame
     *                let best be ref built from the "best" parameter p in ps ({-} better than
     * {assign} better than {call}) replace ps with an assignment lhs := best else (ordinary
     * statement) do bypass and propagation for s
     */
    std::map<SharedExp, int, lessExpStar> destCounts;
    StatementList stmts;
    proc->getStatements(stmts);

    // a[m[]] hack, aint nothing better.
    bool found = true;

    for (Statement *s : stmts) {
        if (!s->isCall()) {
            continue;
        }

        CallStatement *call = static_cast<CallStatement *>(s);

        for (auto &elem : call->getArguments()) {
            Assign *a = static_cast<Assign *>(elem);

            if (!a->getType()->resolvesToPointer()) {
                continue;
            }

            SharedExp e = a->getRight();

            if ((e->getOper() == opPlus) || (e->getOper() == opMinus)) {
                if (e->getSubExp2()->isIntConst()) {
                    if (e->getSubExp1()->isSubscript() &&
                        e->getSubExp1()->getSubExp1()->isRegN(
                            proc->getSignature()->getStackRegister()) &&
                        (((e->access<RefExp, 1>())->getDef() == nullptr) ||
                         (e->access<RefExp, 1>())->getDef()->isImplicit())) {
                        a->setRight(Unary::get(opAddrOf, Location::memOf(e->clone())));
                        found = true;
                    }
                }
            }
        }
    }

    if (found) {
        PassManager::get()->executePass(PassID::BlockVarRename, proc);
    }

    // Scan for situations like this:
    // 56 r28 := phi{6, 26}
    // ...
    // 26 r28 := r28{56}
    // So we can remove the second parameter,
    // then reduce the phi to an assignment, then propagate it
    for (Statement *s : stmts) {
        if (!s->isPhi()) {
            continue;
        }

        PhiAssign *phi                 = static_cast<PhiAssign *>(s);
        std::shared_ptr<RefExp> refExp = RefExp::get(phi->getLeft(), phi);

        phi->removeAllReferences(refExp);
    }

    // Second pass
    for (Statement *s : stmts) {
        if (!s->isPhi()) { // Ordinary statement
            s->bypass();
            continue;
        }

        PhiAssign *phi = static_cast<PhiAssign *>(s);

        if (phi->getNumDefs() == 0) {
            // Can happen e.g. for m[...] := phi {} when this proc is involved in a recursion group
            continue;
        }

        auto lhs     = phi->getLeft();
        bool allSame = true;
        // Let first be a reference built from the first parameter
        PhiAssign::iterator phi_iter = phi->begin();

        while (phi_iter != phi->end() && (*phi_iter)->getSubExp1() == nullptr) {
            ++phi_iter; // Skip any null parameters
        }

        assert(phi_iter != phi->end()); // Should have been deleted
        const std::shared_ptr<RefExp> &phi_inf = *phi_iter;
        SharedExp first = RefExp::get(phi_inf->getSubExp1(), phi_inf->getDef());

        // bypass to first
        CallBypasser cb(phi);
        first = first->acceptModifier(&cb);

        if (cb.isTopChanged()) {
            first = first->simplify();
        }

        first = first->propagateAll(); // Propagate everything repeatedly

        if (cb.isModified()) { // Modified?
            // if first is of the form lhs{x}
            if (first->isSubscript() && (*first->getSubExp1() == *lhs)) {
                // replace first with x
                phi_inf->setDef(first->access<RefExp>()->getDef());
            }
        }

        // For each parameter p of ps after the first
        for (++phi_iter; phi_iter != phi->end(); ++phi_iter) {
            assert((*phi_iter)->getSubExp1());
            const std::shared_ptr<RefExp> &phi_inf2 = *phi_iter;
            SharedExp current = RefExp::get(phi_inf2->getSubExp1(), phi_inf2->getDef());
            CallBypasser cb2(phi);
            current = current->acceptModifier(&cb2);

            if (cb2.isTopChanged()) {
                current = current->simplify();
            }

            current = current->propagateAll();

            if (cb2.isModified()) {
                // if current is of the form lhs{x}
                if (current->isSubscript() && (*current->getSubExp1() == *lhs)) {
                    // replace current with x
                    phi_inf2->setDef(current->access<RefExp>()->getDef());
                }
            }

            if (!(*first == *current)) {
                allSame = false;
            }
        }

        if (allSame) {
            // let best be ref built from the "best" parameter p in ps ({-} better than {assign}
            // better than {call})
            phi_iter = phi->begin();

            while (phi_iter != phi->end() && (*phi_iter)->getSubExp1() == nullptr) {
                ++phi_iter; // Skip any null parameters
            }

            assert(phi_iter != phi->end()); // Should have been deleted
            auto best = RefExp::get((*phi_iter)->getSubExp1(), (*phi_iter)->getDef());

            for (++phi_iter; phi_iter != phi->end(); ++phi_iter) {
                assert((*phi_iter)->getSubExp1());
                auto current = RefExp::get((*phi_iter)->getSubExp1(), (*phi_iter)->getDef());

                if (current->isImplicitDef()) {
                    best = current;
                    break;
                }

                if ((*phi_iter)->getDef()->isAssign()) {
                    best = current;
                }

                // If phi_iter->second.def is a call, this is the worst case; keep only (via first)
                // if all parameters are calls
            }

            Assign *asgn = proc->replacePhiByAssign(phi, best);
            LOG_VERBOSE2("Redundant phi replaced with copy assign; now %1", asgn);
        }
    }

    // Also do xxx in m[xxx] in the use collector
    for (const SharedExp &cc : proc->getUseCollector()) {
        if (!cc->isMemOf()) {
            continue;
        }

        auto addr = cc->getSubExp1();
        CallBypasser cb(nullptr);
        addr = addr->acceptModifier(&cb);

        if (cb.isModified()) {
            cc->setSubExp1(addr);
        }
    }

    return true;
}
