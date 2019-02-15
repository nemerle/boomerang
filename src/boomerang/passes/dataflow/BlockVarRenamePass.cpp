#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "BlockVarRenamePass.h"

#include "boomerang/core/Project.h"
#include "boomerang/core/Settings.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Terminal.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/PhiAssign.h"
#include "boomerang/ssl/statements/ReturnStatement.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/visitor/expmodifier/ExpSubscripter.h"
#include "boomerang/visitor/stmtmodifier/StmtSubscripter.h"


BlockVarRenamePass::BlockVarRenamePass()
    : IPass("BlockVarRename", PassID::BlockVarRename)
{
}


static SharedExp defineAll = Terminal::get(opDefineAll); // An expression representing <all>

// There is an entry in stacks[defineAll] that represents the latest definition
// from a define-all source. It is needed for variables that don't have a definition as yet
// (i.e. stacks[x].empty() is true). As soon as a real definition to x appears,
// stacks[defineAll] does not apply for variable x. This is needed to get correct
// operation of the use collectors in calls.

// Care with the Stacks object (a map from expression to stack);
// using Stacks[q].empty() can needlessly insert an empty stack
#define STACKS_EMPTY(q) (stacks.find(q) == stacks.end() || stacks[q].empty())


// Subscript dataflow variables
bool BlockVarRenamePass::renameBlockVars(
    UserProc *proc, int n, std::map<SharedExp, std::deque<Statement *>, lessExpStar> &stacks)
{
    if (proc->getCFG()->getNumBBs() == 0) {
        return false;
    }

    bool changed                   = false;
    const bool assumeABICompliance = proc->getProg()->getProject()->getSettings()->assumeABI;

    // For each statement S in block n
    BasicBlock::RTLIterator rit;
    StatementList::iterator sit;
    BasicBlock *bb = proc->getDataFlow()->nodeToBB(n);

    for (Statement *S = bb->getFirstStmt(rit, sit); S; S = bb->getNextStmt(rit, sit)) {
        {
            // For each use of some variable x in S (not just assignments)
            LocationSet locs;

            if (S->isPhi()) {
                PhiAssign *pa     = static_cast<PhiAssign *>(S);
                SharedExp phiLeft = pa->getLeft();

                if (phiLeft->isMemOf() || phiLeft->isRegOf()) {
                    phiLeft->getSubExp1()->addUsedLocs(locs);
                }

                // A phi statement may use a location defined in a childless call,
                // in which case its use collector needs updating
                for (auto &pp : *pa) {
                    Statement *def = pp.getDef();

                    if (def && def->isCall()) {
                        static_cast<CallStatement *>(def)->useBeforeDefine(phiLeft->clone());
                    }
                }
            }
            else { // Not a phi assignment
                S->addUsedLocs(locs);
            }

            for (SharedExp location : locs) {
                // Don't rename memOfs that are not renamable according to the current policy
                if (!proc->canRename(location)) {
                    continue;
                }

                Statement *def = nullptr;

                if (location->isSubscript()) { // Already subscripted?
                    // No renaming required, but redo the usage analysis, in case this is a new
                    // return, and also because we may have just removed all call livenesses
                    // Update use information in calls, and in the proc (for parameters)
                    SharedExp base = location->getSubExp1();
                    def            = location->access<RefExp>()->getDef();

                    if (def && def->isCall()) {
                        // Calls have UseCollectors for locations that are used before definition at
                        // the call
                        static_cast<CallStatement *>(def)->useBeforeDefine(base->clone());
                        continue;
                    }

                    // Update use collector in the proc (for parameters)
                    if (def == nullptr) {
                        proc->markAsInitialParam(base->clone());
                    }

                    continue; // Don't re-rename the renamed variable
                }

                if (!STACKS_EMPTY(location)) {
                    def = stacks[location].back();
                }
                else if (!STACKS_EMPTY(defineAll)) {
                    def = stacks[defineAll].back();
                }
                else {
                    // If the both stacks are empty, use a nullptr definition. This will be changed
                    // into a pointer to an implicit definition at the start of type analysis, but
                    // not until all the m[...] have stopped changing their expressions (complicates
                    // implicit assignments considerably).
                    def = nullptr;
                    // Update the collector at the start of the UserProc
                    proc->markAsInitialParam(location->clone());
                }


                if (def && def->isCall()) {
                    // Calls have UseCollectors for locations that are used before definition
                    // at the call
                    static_cast<CallStatement *>(def)->useBeforeDefine(location->clone());
                }

                // Replace the use of x with x{def} in S
                changed = true;

                if (S->isPhi()) {
                    SharedExp phiLeft = static_cast<PhiAssign *>(S)->getLeft();
                    phiLeft->setSubExp1(phiLeft->getSubExp1()->expSubscriptVar(location, def));
                }
                else {
                    subscriptVar(S, location, def);
                }
            }
        }

        // MVE: Check for Call and Return Statements; these have DefCollector objects that need to
        // be updated Do before the below, so CallStatements have not yet processed their defines
        if (S->isCall() || S->isReturn()) {
            DefCollector *col;

            if (S->isCall()) {
                col = static_cast<CallStatement *>(S)->getDefCollector();
            }
            else {
                col = static_cast<ReturnStatement *>(S)->getCollector();
            }

            col->updateDefs(stacks, proc);
        }

        // For each definition of some variable a in S
        LocationSet defs;
        S->getDefinitions(defs, assumeABICompliance);

        for (SharedExp a : defs) {
            // Don't consider a if it cannot be renamed
            const bool suitable = proc->canRename(a);

            if (suitable) {
                // Push i onto Stacks[a]
                // Note: we clone a because otherwise it could be an expression
                // that gets deleted through various modifications.
                // This is necessary because we do several passes of this algorithm
                // to sort out the memory expressions.
                if (stacks.find(a) != stacks.end()) { // expression exists, no need for clone ?
                    stacks[a].push_back(S);
                }
                else {
                    stacks[a->clone()].push_back(S);
                }

                // Replace definition of 'a' with definition of a_i in S (we don't do this)
            }

            // FIXME: MVE: do we need this awful hack?
            if (a->isLocal()) {
                SharedConstExp a1 = S->getProc()->expFromSymbol(a->access<Const, 1>()->getStr());
                assert(a1);

                // Stacks already has a definition for a (as just the bare local)
                if (suitable) {
                    stacks[a1->clone()].push_back(S);
                }
            }
        }

        // Special processing for define-alls (presently, only childless calls).
        // But note that only 'everythings' at the current memory level are defined!
        if (S->isCall() && static_cast<const CallStatement *>(S)->isChildless() &&
            !proc->getProg()->getProject()->getSettings()->assumeABI) {
            // S is a childless call (and we're not assuming ABI compliance)
            stacks[defineAll]; // Ensure that there is an entry for defineAll

            for (auto &elem : stacks) {
                // if (dd->first->isMemDepth(memDepth))
                elem.second.push_back(S); // Add a definition for all vars
            }
        }
    }

    // For each successor Y of block n
    for (BasicBlock *Ybb : bb->getSuccessors()) {
        // For each phi-function in Y
        for (Statement *St = Ybb->getFirstStmt(rit, sit); St; St = Ybb->getNextStmt(rit, sit)) {
            if (!St->isPhi()) {
                continue;
            }

            PhiAssign *pa = static_cast<PhiAssign *>(St);

            // Suppose the jth operand of the phi is 'a'
            // For now, just get the LHS
            SharedExp a = pa->getLeft();

            // Only consider variables that can be renamed
            if (!proc->canRename(a)) {
                continue;
            }

            Statement *def = nullptr; // assume No reaching definition

            if (!STACKS_EMPTY(a)) {
                def = stacks[a].back();
            }

            // "Replace jth operand with a_i"
            pa->putAt(bb, def, a);
        }
    }

    // For each child X of n
    // Note: linear search!
    const int numBB = proc->getCFG()->getNumBBs();

    for (int X = 0; X < numBB; X++) {
        if (proc->getDataFlow()->getIdom(X) == n) { // if 'n' is immediate dominator of X
            renameBlockVars(proc, X, stacks);
        }
    }

    // For each statement S in block n
    // NOTE: Because of the need to pop childless calls from the Stacks, it is important in my
    // algorithm to process the statments in the BB *backwards*. (It is not important in Appel's
    // algorithm, since he always pushes a definition for every variable defined on the Stacks).
    BasicBlock::RTLRIterator rrit;
    StatementList::reverse_iterator srit;

    for (Statement *S = bb->getLastStmt(rrit, srit); S; S = bb->getPrevStmt(rrit, srit)) {
        // For each definition of some variable a in S
        LocationSet defs;
        S->getDefinitions(defs, assumeABICompliance);

        for (const auto &def : defs) {
            if (!proc->canRename(def)) {
                continue;
            }

            auto stackIt = stacks.find(def);

            if (stackIt == stacks.end()) {
                LOG_FATAL("Tried to pop '%1' from Stacks; does not exist", def);
            }

            stackIt->second.pop_back();
        }

        // Pop all defs due to childless calls
        if (S->isCall() && static_cast<const CallStatement *>(S)->isChildless()) {
            for (auto &stack : stacks) {
                if (!stack.second.empty() && (stack.second.back() == S)) {
                    stack.second.pop_back();
                }
            }
        }
    }

    return changed;
}


bool BlockVarRenamePass::execute(UserProc *proc)
{
    /// The stack which remembers the last definition of an expression.
    std::map<SharedExp, std::deque<Statement *>, lessExpStar> stacks;
    return renameBlockVars(proc, 0, stacks);
}


void BlockVarRenamePass::subscriptVar(Statement *stmt, SharedExp var, Statement *varDef)
{
    ExpSubscripter es(var, varDef);
    StmtSubscripter ss(&es);

    stmt->accept(&ss);
}
