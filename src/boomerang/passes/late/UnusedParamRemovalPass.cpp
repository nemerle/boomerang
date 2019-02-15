#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "UnusedParamRemovalPass.h"

#include "boomerang/core/Project.h"
#include "boomerang/core/Settings.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/PhiAssign.h"
#include "boomerang/ssl/statements/ReturnStatement.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/visitor/expmodifier/ImplicitConverter.h"


UnusedParamRemovalPass::UnusedParamRemovalPass()
    : IPass("UnusedParamRemoval", PassID::UnusedParamRemoval)
{
}


bool UnusedParamRemovalPass::execute(UserProc *proc)
{
    if (proc->getSignature()->isForced()) {
        // Assume that no extra parameters would have been inserted... not sure always valid
        return false;
    }

    bool ret = false;
    StatementList newParameters;

    proc->getProg()->getProject()->alertDecompileDebugPoint(proc,
                                                            "Before removing redundant parameters");

    if (proc->getProg()->getProject()->getSettings()->debugUnused) {
        LOG_MSG("%%% removing unused parameters for %1", proc->getName());
    }

    // Note: this would be far more efficient if we had def-use information
    for (StatementList::iterator pp = proc->getParameters().begin();
         pp != proc->getParameters().end(); ++pp) {
        SharedExp param = static_cast<Assignment *>(*pp)->getLeft();
        bool az;
        SharedExp bparam = param->clone()->removeSubscripts(
            az); // FIXME: why does main have subscripts on parameters?
        // Memory parameters will be of the form m[sp + K]; convert to m[sp{0} + K] as will be found
        // in uses
        bparam = bparam->expSubscriptAllNull(); // Now m[sp{-}+K]{-}
        ImplicitConverter ic(proc->getCFG());
        bparam = bparam->acceptModifier(&ic); // Now m[sp{0}+K]{0}
        assert(bparam->isSubscript());
        bparam = bparam->access<Exp, 1>(); // now m[sp{0}+K] (bare parameter)

        ProcSet visited;

        if (checkForGainfulUse(proc, bparam, visited)) {
            newParameters.append(*pp); // Keep this parameter
        }
        else {
            // Remove the parameter
            ret = true;

            if (proc->getProg()->getProject()->getSettings()->debugUnused) {
                LOG_MSG(" %%% removing unused parameter %1 in %2", param, proc->getName());
            }

            // Check if it is in the symbol map. If so, delete it; a local will be created later
            UserProc::SymbolMap::iterator ss = proc->getSymbolMap().find(param);

            if (ss != proc->getSymbolMap().end()) {
                proc->getSymbolMap().erase(ss); // Kill the symbol
            }

            proc->getSignature()->removeParameter(param); // Also remove from the signature
            proc->getCFG()->removeImplicitAssign(
                param); // Remove the implicit assignment so it doesn't come back
        }
    }

    proc->getParameters() = newParameters;

    if (proc->getProg()->getProject()->getSettings()->debugUnused) {
        LOG_MSG("%%% end removing unused parameters for %1", proc->getName());
    }

    proc->getProg()->getProject()->alertDecompileDebugPoint(proc,
                                                            "after removing redundant parameters");

    return ret;
}


bool UnusedParamRemovalPass::checkForGainfulUse(UserProc *proc, SharedExp bparam, ProcSet &visited)
{
    visited.insert(proc); // Prevent infinite recursion

    StatementList stmts;
    proc->getStatements(stmts);

    for (Statement *s : stmts) {
        // Special checking for recursive calls
        if (s->isCall()) {
            CallStatement *c = static_cast<CallStatement *>(s);
            UserProc *dest   = dynamic_cast<UserProc *>(c->getDestProc());

            if (dest && dest->doesRecurseTo(proc)) {
                // In the destination expression?
                LocationSet u;
                c->getDest()->addUsedLocs(u);

                if (u.containsImplicit(bparam)) {
                    return true; // Used by the destination expression
                }

                // Else check for arguments of the form lloc := f(bparam{0})
                const StatementList &args = c->getArguments();

                for (const Statement *arg : args) {
                    const Assign *a = dynamic_cast<const Assign *>(arg);
                    SharedExp rhs   = a ? a->getRight() : nullptr;
                    if (!rhs) {
                        continue;
                    }

                    LocationSet argUses;
                    rhs->addUsedLocs(argUses);

                    if (argUses.containsImplicit(bparam)) {
                        SharedExp lloc = static_cast<const Assign *>(arg)->getLeft();

                        if ((visited.find(dest) == visited.end()) &&
                            checkForGainfulUse(dest, lloc, visited)) {
                            return true;
                        }
                    }
                }

                // If get to here, then none of the arguments is of this form,
                // and we can ignore this call
                continue;
            }
        }
        else if (s->isReturn()) {
            if (proc->getRecursionGroup() && !proc->getRecursionGroup()->empty()) {
                // If this functions is involved in recursion, then ignore this return statement
                continue;
            }
        }
        else if (s->isPhi() && (proc->getRetStmt() != nullptr) && proc->getRecursionGroup() &&
                 !proc->getRecursionGroup()->empty()) {
            SharedExp phiLeft = static_cast<PhiAssign *>(s)->getLeft();
            auto refPhi       = RefExp::get(phiLeft, s);
            bool foundPhi     = false;

            for (Statement *stmt : *proc->getRetStmt()) {
                SharedExp rhs = static_cast<Assign *>(stmt)->getRight();
                LocationSet uses;
                rhs->addUsedLocs(uses);

                if (uses.contains(refPhi)) {
                    // s is a phi that defines a component of a recursive return. Ignore it
                    foundPhi = true;
                    break;
                }
            }

            if (foundPhi) {
                continue; // Ignore this phi
            }
        }

        // Otherwise, consider uses in s
        LocationSet uses;
        s->addUsedLocs(uses);

        if (uses.containsImplicit(bparam)) {
            return true; // A gainful use
        }
    } // for each statement s

    return false;
}
