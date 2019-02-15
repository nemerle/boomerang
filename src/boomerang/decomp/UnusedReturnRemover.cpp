#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "UnusedReturnRemover.h"

#include "boomerang/core/Project.h"
#include "boomerang/core/Settings.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/module/Module.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/passes/PassManager.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/PhiAssign.h"
#include "boomerang/ssl/statements/ReturnStatement.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/visitor/expmodifier/ImplicitConverter.h"


UnusedReturnRemover::UnusedReturnRemover(Prog *prog)
    : m_prog(prog)
{
}


bool UnusedReturnRemover::removeUnusedReturns()
{
    for (const auto &module : m_prog->getModuleList()) {
        for (Function *proc : *module) {
            if (proc && !proc->isLib() && static_cast<UserProc *>(proc)->isDecoded()) {
                m_removeRetSet.insert(static_cast<UserProc *>(proc));
            }
            // else e.g. use -sf file to just prototype the proc
        }
    }

    bool change = false;
    // The workset is processed in arbitrary order. May be able to do better,
    // but note that sometimes changes propagate down the call tree
    // (no caller uses potential returns for child), and sometimes up the call tree
    // (removal of returns and/or dead code removes parameters, which affects all callers).
    while (!m_removeRetSet.empty()) {
        auto it                   = m_removeRetSet.begin(); // Pick the first element of the set
        const bool removedReturns = removeUnusedParamsAndReturns(*it);

        if (removedReturns) {
            // Removing returns changes the uses of the callee.
            // So we have to do type analyis to update the use information.
            PassManager::get()->executePass(PassID::LocalTypeAnalysis, *it);

            // type analysis might propagate statements that could not be propagated before
            PassManager::get()->executePass(PassID::UnusedStatementRemoval, *it);
        }
        change |= removedReturns;

        // Note: removing the currently processed item here should prevent
        // unnecessary reprocessing of self recursive procedures
        m_removeRetSet.erase(it);
    }

    return change;
}


bool UnusedReturnRemover::removeUnusedParamsAndReturns(UserProc *proc)
{
    assert(m_removeRetSet.find(proc) != m_removeRetSet.end());

    m_prog->getProject()->alertDecompiling(proc);
    m_prog->getProject()->alertDecompileDebugPoint(proc, "before removing unused returns");

    // First remove the unused parameters
    bool removedParams = PassManager::get()->executePass(PassID::UnusedParamRemoval, proc);

    if (proc->getRetStmt() == nullptr) {
        return removedParams;
    }

    if (m_prog->getProject()->getSettings()->debugUnused) {
        LOG_MSG("%%% removing unused returns for %1 %%%", proc->getName());
    }

    if (proc->getSignature()->isForced()) {
        // Respect the forced signature, but use it to remove returns if necessary
        bool removedRets = false;

        for (auto retIt = proc->getRetStmt()->begin(); retIt != proc->getRetStmt()->end();) {
            assert(*retIt != nullptr && (*retIt)->isAssign());
            Assign *retDef = static_cast<Assign *>(*retIt);
            SharedExp lhs  = retDef->getLeft();

            // For each location in the returns, check if in the signature
            bool found = false;

            for (int i = 0; i < proc->getSignature()->getNumReturns(); i++) {
                if (*proc->getSignature()->getReturnExp(i) == *lhs) {
                    found = true;
                    break;
                }
            }

            if (found) {
                ++retIt; // Yes, in signature; OK
            }
            else {
                if (m_prog->getProject()->getSettings()->debugUnused) {
                    LOG_MSG("%%%  removing unused return %1 from proc %2 (forced signature)",
                            retDef, proc->getName());
                }

                // This return is not in the signature. Remove it
                retIt       = proc->getRetStmt()->erase(retIt);
                removedRets = true;
            }
        }

        if (removedRets) {
            // Still may have effects on calls or now unused statements
            updateForUseChange(proc);
        }

        return removedRets;
    }

    // FIXME: this needs to be more sensible when we don't decompile down from main! Probably should
    // assume just the first return is valid, for example (presently assume none are valid)
    LocationSet unionOfCallerLiveLocs;

    if (proc->getName() == "main") { // Probably not needed: main is forced so handled above
        // Just insert one return for main. Note: at present, the first parameter is still the stack
        // pointer
        if (proc->getSignature()->getNumReturns() <= 1) {
            // handle the case of missing main() signature
            LOG_WARN("main signature definition is missing; assuming void main()");
        }
        else {
            unionOfCallerLiveLocs.insert(proc->getSignature()->getReturnExp(1));
        }
    }
    else {
        for (CallStatement *cc : proc->getCallers()) {
            // TODO: prevent function from blocking its own removals
            UseCollector *useCol = cc->getUseCollector();
            unionOfCallerLiveLocs.makeUnion(useCol->getLocSet());
        }
    }

    // Intersect with the current returns
    bool removedRets = false;

    for (auto retIt = proc->getRetStmt()->begin(); retIt != proc->getRetStmt()->end();) {
        Assign *retDef = static_cast<Assign *>(*retIt);

        // Check if the location defined by the return is actually used by any callee.
        if (unionOfCallerLiveLocs.contains(retDef->getLeft())) {
            ++retIt;
            continue;
        }

        if (m_prog->getProject()->getSettings()->debugUnused) {
            LOG_MSG("%%%  removing unused return %1 from proc %2", retDef, proc->getName());
        }

        // If a component of the RHS referenced a call statement, the liveness used to be killed
        // here. This was wrong; you need to notice the liveness changing inside
        // updateForUseChange() to correctly recurse to callee
        retIt       = proc->getRetStmt()->erase(retIt);
        removedRets = true;
    }

    if (m_prog->getProject()->getSettings()->debugUnused) {
        QString tgt;
        OStream ost(&tgt);
        unionOfCallerLiveLocs.print(ost);
        LOG_MSG("%%%  union of caller live locations for %1: %2", proc->getName(), tgt);
        LOG_MSG("%%%  final returns for %1: %2", proc->getName(),
                proc->getRetStmt()->getReturns().toString());
    }

    // removing returns might result in params that can be removed, might as well do it now.
    removedParams |= PassManager::get()->executePass(PassID::UnusedParamRemoval, proc);

    ProcSet updateSet; // Set of procs to update

    if (removedParams || removedRets) {
        // Update the statements that call us
        for (CallStatement *call : proc->getCallers()) {
            PassManager::get()->executePass(PassID::CallArgumentUpdate, proc);
            updateSet.insert(call->getProc());      // Make sure we redo the dataflow
            m_removeRetSet.insert(call->getProc()); // Also schedule caller proc for more analysis
        }

        // Now update myself
        updateForUseChange(proc);

        // Update any other procs that need updating
        updateSet.erase(proc); // Already done this proc

        while (!updateSet.empty()) {
            UserProc *_proc = *updateSet.begin();
            updateSet.erase(_proc);
            updateForUseChange(_proc);
        }
    }

    m_prog->getProject()->alertDecompileDebugPoint(proc,
                                                   "after removing unused and redundant returns");
    return removedRets || removedParams;
}


void UnusedReturnRemover::updateForUseChange(UserProc *proc)
{
    // We need to remember the parameters, and all the livenesses for all the calls, to see if these
    // are changed by removing returns
    if (m_prog->getProject()->getSettings()->debugUnused) {
        LOG_MSG("%%% updating %1 for changes to uses (returns or arguments)", proc->getName());
        LOG_MSG("%%% updating dataflow:");
    }

    // Save the old parameters and call liveness
    const size_t oldNumParameters = proc->getParameters().size();
    std::map<CallStatement *, UseCollector> callLiveness;

    for (BasicBlock *bb : *proc->getCFG()) {
        BasicBlock::RTLRIterator rrit;
        StatementList::reverse_iterator srit;
        CallStatement *c = dynamic_cast<CallStatement *>(bb->getLastStmt(rrit, srit));

        // Note: we may have removed some statements, so there may no longer be a last statement!
        if (c == nullptr) {
            continue;
        }

        UserProc *dest = dynamic_cast<UserProc *>(c->getDestProc());

        // Not interested in unanalysed indirect calls (not sure) or calls to lib procs
        if (dest == nullptr) {
            continue;
        }

        callLiveness[c].makeCloneOf(*c->getUseCollector());
    }

    // Have to redo dataflow to get the liveness at the calls correct
    PassManager::get()->executePass(PassID::CallLivenessRemoval, proc);
    PassManager::get()->executePass(PassID::BlockVarRename, proc);

    // Perform type analysis. If we are relying (as we are at present) on TA to perform ellipsis
    // processing, do the local TA pass now. Ellipsis processing often reveals additional uses (e.g.
    // additional parameters to printf/scanf), and removing unused statements is unsafe without full
    // use information
    if (proc->getStatus() < ProcStatus::FinalDone) {
        PassManager::get()->executePass(PassID::LocalTypeAnalysis, proc);

        // Now that locals are identified, redo the dataflow
        PassManager::get()->executePass(PassID::PhiPlacement, proc);

        PassManager::get()->executePass(PassID::BlockVarRename, proc); // Rename the locals
        PassManager::get()->executePass(PassID::StatementPropagation, proc);

        if (m_prog->getProject()->getSettings()->verboseOutput) {
            proc->debugPrintAll("after propagating locals");
        }
    }

    PassManager::get()->executePass(PassID::UnusedStatementRemoval, proc);
    PassManager::get()->executePass(PassID::FinalParameterSearch, proc);

    if (m_prog->getProject()->getSettings()->nameParameters) {
        // Replace the existing temporary parameters with the final ones:
        // mapExpressionsToParameters();
        PassManager::get()->executePass(PassID::ParameterSymbolMap, proc);
        proc->debugPrintAll("after adding new parameters");
    }

    // Or just CallArgumentUpdate?
    PassManager::get()->executePass(PassID::CallDefineUpdate, proc);
    PassManager::get()->executePass(PassID::CallArgumentUpdate, proc);

    // Have the parameters changed? If so, then all callers will need to update their arguments, and
    // do similar analysis to the removal of returns
    PassManager::get()->executePass(PassID::UnusedParamRemoval, proc);

    if (proc->getParameters().size() != oldNumParameters) {
        if (m_prog->getProject()->getSettings()->debugUnused) {
            LOG_MSG("%%%  parameters changed for %1", proc->getName());
        }

        std::set<CallStatement *> &callers = proc->getCallers();
        const bool experimental            = m_prog->getProject()->getSettings()->experimental;

        for (CallStatement *cc : callers) {
            cc->updateArguments(experimental);
            // Schedule the callers for analysis
            m_removeRetSet.insert(cc->getProc());
        }
    }

    // Check if the liveness of any calls has changed
    for (auto &[call, oldLiveness] : callLiveness) {
        const UseCollector &newLiveness = *call->getUseCollector();

        if (newLiveness != oldLiveness) {
            if (m_prog->getProject()->getSettings()->debugUnused) {
                LOG_MSG("%%%  Liveness for call to %1 in %2 changed",
                        call->getDestProc()->getName(), proc->getName());
            }

            m_removeRetSet.insert(static_cast<UserProc *>(call->getDestProc()));
        }
    }
}
