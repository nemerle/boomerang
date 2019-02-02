#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "DFATypeRecovery.h"

#include "DFATypeAnalyzer.h"

#include "boomerang/core/Project.h"
#include "boomerang/core/Settings.h"
#include "boomerang/db/BasicBlock.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/passes/PassManager.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/exp/RefExp.h"
#include "boomerang/ssl/exp/Terminal.h"
#include "boomerang/ssl/exp/Ternary.h"
#include "boomerang/ssl/exp/TypedExp.h"
#include "boomerang/ssl/statements/BoolAssign.h"
#include "boomerang/ssl/statements/BranchStatement.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/GotoStatement.h"
#include "boomerang/ssl/statements/ImplicitAssign.h"
#include "boomerang/ssl/statements/ReturnStatement.h"
#include "boomerang/ssl/type/ArrayType.h"
#include "boomerang/ssl/type/BooleanType.h"
#include "boomerang/ssl/type/CharType.h"
#include "boomerang/ssl/type/CompoundType.h"
#include "boomerang/ssl/type/FloatType.h"
#include "boomerang/ssl/type/FuncType.h"
#include "boomerang/ssl/type/IntegerType.h"
#include "boomerang/ssl/type/PointerType.h"
#include "boomerang/ssl/type/SizeType.h"
#include "boomerang/ssl/type/UnionType.h"
#include "boomerang/ssl/type/VoidType.h"
#include "boomerang/util/Util.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/visitor/expvisitor/ConstFinder.h"
#include "boomerang/visitor/expvisitor/ExpVisitor.h"
#include "boomerang/visitor/stmtexpvisitor/StmtConstFinder.h"

#include <cstring>
#include <sstream>
#include <utility>


#define DFA_ITER_LIMIT (100)

// idx + K; leave idx wild
static const Binary unscaledArrayPat(opPlus, Terminal::get(opWild), Terminal::get(opWildIntConst));


DFATypeRecovery::DFATypeRecovery(Project *project)
    : TypeRecoveryCommon(project, "data-flow based")
{
}


void DFATypeRecovery::printResults(StatementList &stmts, int iter)
{
    LOG_VERBOSE("%1 iterations", iter);

    for (Statement *s : stmts) {
        LOG_VERBOSE("%1", s); // Print the statement; has dest type

        // Now print type for each constant in this Statement
        std::list<std::shared_ptr<Const>> lc;
        findConstantsInStmt(s, lc);

        for (std::shared_ptr<Const> &cc : lc) {
            LOG_MSG("    %1, %2", cc->getType()->getCtype(), cc);
        }

        // If s is a call, also display its return types
        CallStatement *call = dynamic_cast<CallStatement *>(s);

        if (call && call->isCall()) {
            ReturnStatement *rs = call->getCalleeReturn();
            UseCollector *uc    = call->getUseCollector();

            if (!rs || !uc) {
                continue;
            }

            LOG_VERBOSE("  returns:");

            for (Statement *ret : *rs) {
                // Intersect the callee's returns with the live locations at the call,
                // i.e. make sure that they exist in *uc
                Assignment *assgn = dynamic_cast<Assignment *>(ret);
                if (!assgn) {
                    continue;
                }

                if (!uc->exists(assgn->getLeft())) {
                    continue; // Intersection fails
                }

                LOG_VERBOSE("    %1 %2", assgn->getType()->getCtype(), assgn->getLeft());
            }
        }
    }
}


// clang-format off
// m[idx*K1 + K2]; leave idx wild
static const Location scaledArrayPat(opMemOf,
                                     Binary::get(opPlus,
                                                 Binary::get(opMult,
                                                             Terminal::get(opWild),
                                                             Terminal::get(opWildIntConst)),
                                                 Terminal::get(opWildIntConst)),
                                     nullptr);
// clang-format on


void DFATypeRecovery::replaceArrayIndices(Statement *s)
{
    UserProc *proc = s->getProc();
    Prog *prog     = proc->getProg();

    std::list<SharedExp> result;
    s->searchAll(scaledArrayPat, result);

    // We have m[idx*stride + base]
    // Rewrite it as globalN[idx] with addr(globalN) == base
    // with a global array globalN with base type size \e stride
    for (SharedExp arrayExp : result) {
        const Address base = arrayExp->access<Const, 1, 2>()->getAddr();
        // const int stride   = arrayExp->access<Const, 1, 1, 2>()->getInt();
        SharedExp idx = arrayExp->access<Exp, 1, 1, 1>();

        // Replace with the array expression
        QString name = prog->getGlobalNameByAddr(base);
        if (name.isEmpty()) {
            name = prog->newGlobalName(base);
        }

        SharedExp array = Binary::get(opArrayIndex, Location::global(name, proc), idx);

        if (s->searchAndReplace(scaledArrayPat, array)) {
            if (s->isImplicit()) {
                // Register an array of appropriate type
                prog->markGlobalUsed(
                    base, ArrayType::get(static_cast<const ImplicitAssign *>(s)->getType()));
            }
        }
    }
}


void DFATypeRecovery::dfa_analyze_implict_assigns(Statement *s)
{
    if (!s->isImplicit()) {
        return;
    }

    UserProc *proc = s->getProc();
    assert(proc);
    Prog *prog = proc->getProg();
    assert(prog);

    SharedExp lhs = static_cast<const ImplicitAssign *>(s)->getLeft();
    // Note: parameters are not explicit any more
    // if (lhs->isParam()) { // }

    bool allZero            = false;
    SharedExp slhs          = lhs->clone()->removeSubscripts(allZero);
    SharedType implicitType = static_cast<const ImplicitAssign *>(s)->getType();
    int i                   = proc->getSignature()->findParam(slhs);

    if (i != -1) {
        proc->setParamType(i, implicitType);
    }
    else if (lhs->isMemOf()) {
        SharedExp sub = lhs->getSubExp1();

        if (sub->isIntConst()) {
            // We have a m[K] := -
            Address K = sub->access<Const>()->getAddr();
            prog->markGlobalUsed(K, implicitType);
        }
    }
    else if (lhs->isGlobal()) {
        assert(std::dynamic_pointer_cast<Location>(lhs) != nullptr);
        QString gname = lhs->access<Const, 1>()->getStr();
        prog->setGlobalType(gname, implicitType);
    }
}


void DFATypeRecovery::recoverFunctionTypes(Function *function)
{
    if (function->isLib()) {
        LOG_VERBOSE("Not using DFA type analysis on library function '%1'", function->getName());
        return;
    }

    if (function->getProg()->getProject()->getSettings()->debugTA) {
        LOG_VERBOSE("--- Start data flow based type analysis for %1 ---", getName());
    }

    bool first   = true;
    UserProc *up = dynamic_cast<UserProc *>(function);
    assert(up != nullptr);

    do {
        if (first) {
            // Subscript the discovered extra parameters
            PassManager::get()->executePass(PassID::BlockVarRename, up);
            PassManager::get()->executePass(PassID::StatementPropagation, up);
            PassManager::get()->executePass(PassID::ImplicitPlacement, up);
        }

        first = false;
        dfaTypeAnalysis(up);

        // There used to be a pass here to insert casts. This is best left until global type
        // analysis is complete, so do it just before translating from SSA form (which is the where
        // type information becomes inaccessible)
    } while (doEllipsisProcessing(up));

    PassManager::get()->executePass(PassID::BBSimplify, up); // In case there are new struct members

    if (function->getProg()->getProject()->getSettings()->debugTA) {
        LOG_VERBOSE("=== End type analysis for %1 ===", getName());
    }
}


void DFATypeRecovery::dfaTypeAnalysis(UserProc *proc)
{
    ProcCFG *cfg = proc->getCFG();
    proc->getProg()->getProject()->alertDecompileDebugPoint(proc, "Before DFA type analysis");

    // First use the type information from the signature.
    // Sometimes needed to split variables (e.g. argc as a
    // int and char* in sparc/switch_gcc)
    bool ch = dfaTypeAnalysis(proc->getSignature().get(), cfg);
    StatementList stmts;
    proc->getStatements(stmts);

    int iter = 0;

    for (iter = 1; iter <= DFA_ITER_LIMIT; ++iter) {
        ch = false;

        for (Statement *stmt : stmts) {
            Statement *before = nullptr;

            if (proc->getProg()->getProject()->getSettings()->debugTA) {
                before = stmt->clone();
            }

            DFATypeAnalyzer ana;
            stmt->accept(&ana);
            const bool thisCh = ana.hasChanged();

            if (thisCh) {
                ch = true;

                if (proc->getProg()->getProject()->getSettings()->debugTA) {
                    LOG_VERBOSE("  Caused change:\n"
                                "    FROM: %1\n"
                                "    TO:   %2",
                                before, stmt);
                }
            }

            if (proc->getProg()->getProject()->getSettings()->debugTA) {
                delete before;
            }
        }

        if (!ch) {
            // No more changes: round robin algorithm terminates
            break;
        }
    }

    if (ch) {
        LOG_VERBOSE("Iteration limit exceeded for dfaTypeAnalysis of procedure '%1'",
                    proc->getName());
    }

    if (proc->getProg()->getProject()->getSettings()->debugTA) {
        LOG_MSG("### Results for data flow based type analysis for %1 ###", proc->getName());
        printResults(stmts, iter);
        LOG_MSG("### End results for Data flow based type analysis for %1 ###", proc->getName());
    }

    // Now use the type information gathered

    proc->getProg()->getProject()->alertDecompileDebugPoint(
        proc, "Before other uses of DFA type analysis");
    proc->debugPrintAll("Before other uses of DFA type analysis");

    Prog *_prog = proc->getProg();
    DataIntervalMap localsMap(proc); // map of all local variables of proc

    for (Statement *s : stmts) {
        // 1) constants
        std::list<std::shared_ptr<Const>> constList;
        findConstantsInStmt(s, constList);

        for (const std::shared_ptr<Const> &con : constList) {
            if (!con || con->isStrConst()) {
                continue;
            }

            SharedType t = con->getType();
            int val      = con->getInt();

            if (t && t->resolvesToPointer()) {
                auto pt             = t->as<PointerType>();
                SharedType baseType = pt->getPointsTo();

                if (baseType->resolvesToChar()) {
                    // Convert to a string    MVE: check for read-only?
                    // Also, distinguish between pointer to one char, and ptr to many?
                    const char *str = _prog->getStringConstant(Address(val), true);

                    if (str) {
                        // Make a string
                        con->setOper(opStrConst);
                        con->setRawStr(str);
                    }
                }
                else if (baseType->resolvesToInteger() || baseType->resolvesToFloat() ||
                         baseType->resolvesToSize()) {
                    Address addr = Address(con->getInt()); // TODO: use getAddr
                    _prog->markGlobalUsed(addr, baseType);
                    QString gloName = _prog->getGlobalNameByAddr(addr);

                    if (!gloName.isEmpty()) {
                        Address r = addr - _prog->getGlobalAddrByName(gloName);
                        SharedExp ne;

                        if (!r.isZero()) { // TODO: what if r is NO_ADDR ?
                            SharedExp g = Location::global(gloName, proc);
                            ne          = Location::memOf(
                                Binary::get(opPlus, Unary::get(opAddrOf, g), Const::get(r)), proc);
                        }
                        else {
                            SharedType ty = _prog->getGlobalType(gloName);
                            Assign *assgn = dynamic_cast<Assign *>(s);

                            if (assgn && s->isAssign() && assgn->getType()) {
                                Type::Size bits = assgn->getType()->getSize();

                                if ((ty == nullptr) || (ty->getSize() == 0)) {
                                    _prog->setGlobalType(gloName, IntegerType::get(bits));
                                }
                            }

                            SharedExp g = Location::global(gloName, proc);

                            if (g && ty && ty->resolvesToArray()) {
                                ne = Binary::get(opArrayIndex, g, Const::get(0));
                            }
                            else {
                                ne = g;
                            }
                        }

                        SharedExp memof = Location::memOf(con);

                        if (!s->searchAndReplace(*memof, ne)) {
                            ne = nullptr;
                        }
                    }
                }
                else if (baseType->resolvesToArray()) {
                    // We have found a constant in s which has type pointer to array of alpha. We
                    // can't get the parent of con, but we can find it with the pattern
                    // unscaledArrayPat.
                    std::list<SharedExp> result;
                    s->searchAll(unscaledArrayPat, result);

                    for (auto &elem : result) {
                        // idx + K
                        auto bin_rr = std::dynamic_pointer_cast<Binary>(elem);

                        if (!bin_rr) {
                            assert(false);
                            return;
                        }

                        auto constK = bin_rr->access<Const, 2>();

                        // Note: keep searching till we find the pattern with this constant, since
                        // other constants may not be used as pointer to array type.
                        if (constK != con) {
                            continue;
                        }

                        Address K     = Address(constK->getInt());
                        SharedExp idx = bin_rr->getSubExp1();
                        SharedExp arr = Unary::get(
                            opAddrOf,
                            Binary::get(opArrayIndex,
                                        Location::global(_prog->getGlobalNameByAddr(K), proc),
                                        idx));
                        // Beware of changing expressions in implicit assignments... map can become
                        // invalid
                        bool isImplicit = s->isImplicit();

                        if (isImplicit) {
                            cfg->removeImplicitAssign(static_cast<ImplicitAssign *>(s)->getLeft());
                        }

                        if (!s->searchAndReplace(unscaledArrayPat, arr)) {
                            arr = nullptr; // remove if not emplaced in s
                        }

                        // s will likely have an m[a[array]], so simplify
                        s->simplifyAddr();

                        if (isImplicit) {
                            // Replace the implicit assignment entry. Note that s' lhs has changed
                            cfg->findOrCreateImplicitAssign(
                                static_cast<ImplicitAssign *>(s)->getLeft());
                        }

                        // Ensure that the global is declared
                        // Ugh... I think that arrays and pointers to arrays are muddled!
                        _prog->markGlobalUsed(K, baseType);
                    }
                }
            }
            else if (t->resolvesToFloat()) {
                if (con->isIntConst()) {
                    // Reinterpret as a float (and convert to double)
                    // con->setFlt(reinterpret_cast<float>(con->getInt()));
                    int tmp = con->getInt();
                    con->setFlt(*reinterpret_cast<float *>(
                        &tmp)); // Reinterpret to float, then cast to double
                    con->setOper(opFltConst);
                    con->setType(FloatType::get(64)); // TODO: why double ? float might do
                }

                // MVE: more work if double?
            }
            else { /* if (t->resolvesToArray()) */
                _prog->markGlobalUsed(Address(val), t);
            }
        }

        // 2) Search for the scaled array pattern and replace it with an array use m[idx*K1 + K2]
        replaceArrayIndices(s);

        // 3) Check implicit assigns for parameter and global types.
        dfa_analyze_implict_assigns(s);

        // 4) Add the locals (soon globals as well) to the localTable, to sort out the overlaps
        if (s->isTyping()) {
            SharedExp addrExp  = nullptr;
            SharedType typeExp = nullptr;

            assert(s->isAssignment());
            SharedExp lhs = static_cast<Assignment *>(s)->getLeft();

            if (lhs->isMemOf()) {
                addrExp = lhs->getSubExp1();
                typeExp = static_cast<Assignment *>(s)->getType();
            }

            const int spIndex = Util::getStackRegisterIndex(_prog);
            if (addrExp && proc->getSignature()->isAddrOfStackLocal(spIndex, addrExp)) {
                int localAddressOffset = 0;

                if ((addrExp->getArity() == 2) &&
                    proc->getSignature()->isOpCompatStackLocal(addrExp->getOper())) {
                    auto K = addrExp->access<Const, 2>();

                    if (K->isConst()) {
                        localAddressOffset = K->getInt();

                        if (addrExp->getOper() == opMinus) {
                            localAddressOffset = -localAddressOffset;
                        }
                    }
                }

                const SharedType &ty = static_cast<TypingStatement *>(s)->getType();
                LOG_VERBOSE2(
                    "Type analysis for '%1': Adding addrExp '%2' with type %3 to local table",
                    proc->getName(), addrExp, ty);
                SharedExp loc_mem = Location::memOf(addrExp);
                localsMap.insertItem(Address(localAddressOffset), proc->lookupSym(loc_mem, ty),
                                     typeExp);
            }
        }
    }

    proc->debugPrintAll("After application of DFA type analysis");
    proc->getProg()->getProject()->alertDecompileDebugPoint(proc, "After DFA type analysis");
}


bool DFATypeRecovery::dfaTypeAnalysis(Signature *sig, ProcCFG *cfg)
{
    bool ch = false;

    for (const std::shared_ptr<Parameter> &it : sig->getParameters()) {
        // Parameters should be defined in an implicit assignment
        Statement *def = cfg->findImplicitParamAssign(it.get());

        if (def) { // But sometimes they are not used, and hence have no implicit definition
            bool thisCh = false;
            def->meetWithFor(it->getType(), it->getExp(), thisCh);

            if (thisCh) {
                ch = true;

                if (cfg->getProc()->getProg()->getProject()->getSettings()->debugTA) {
                    LOG_MSG("  sig caused change: %1 %2", it->getType()->getCtype(), it->getName());
                }
            }
        }
    }

    return ch;
}


bool DFATypeRecovery::dfaTypeAnalysis(Statement *i)
{
    Q_UNUSED(i);
    assert(false);
    return false;
}


bool DFATypeRecovery::doEllipsisProcessing(UserProc *proc)
{
    bool ch = false;

    for (BasicBlock *bb : *proc->getCFG()) {
        BasicBlock::RTLRIterator rrit;
        StatementList::reverse_iterator srit;
        CallStatement *c = dynamic_cast<CallStatement *>(bb->getLastStmt(rrit, srit));

        // Note: we may have removed some statements, so there may no longer be a last statement!
        if (c == nullptr) {
            continue;
        }

        ch |= c->ellipsisProcessing(proc->getProg());
    }

    if (ch) {
        PassManager::get()->executePass(PassID::CallAndPhiFix, proc);
    }

    return ch;
}


void DFATypeRecovery::findConstantsInStmt(Statement *stmt,
                                          std::list<std::shared_ptr<Const>> &constants)
{
    ConstFinder cf(constants);
    StmtConstFinder scf(&cf);

    stmt->accept(&scf);
}


BOOMERANG_DEFINE_PLUGIN(PluginType::TypeRecovery, DFATypeRecovery, "DFA Type Recovery plugin",
                        BOOMERANG_VERSION, "Boomerang developers")
