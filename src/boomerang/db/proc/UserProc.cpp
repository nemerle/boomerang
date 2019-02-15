#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "UserProc.h"

#include "boomerang/core/Project.h"
#include "boomerang/core/Settings.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/module/Module.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/decomp/ProcDecompiler.h"
#include "boomerang/ifc/ITypeRecovery.h"
#include "boomerang/passes/PassManager.h"
#include "boomerang/ssl/RTL.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/exp/RefExp.h"
#include "boomerang/ssl/exp/Ternary.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/ImplicitAssign.h"
#include "boomerang/ssl/statements/PhiAssign.h"
#include "boomerang/ssl/statements/ReturnStatement.h"
#include "boomerang/ssl/type/IntegerType.h"
#include "boomerang/util/DFGWriter.h"
#include "boomerang/util/UseGraphWriter.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/util/log/SeparateLogger.h"


UserProc::UserProc(Address address, const QString &name, Module *module)
    : Function(address, std::make_shared<Signature>(name), module)
    , m_cfg(new ProcCFG(this))
    , m_df(this)
{
}


UserProc::~UserProc()
{
    qDeleteAll(m_parameters);
}


bool UserProc::isNoReturn() const
{
    std::set<const Function *> visited;
    return isNoReturnInternal(visited);
}


SharedExp UserProc::getProven(SharedExp left)
{
    // Note: proven information is in the form r28 mapsto (r28 + 4)
    auto it = m_provenTrue.find(left);

    if (it != m_provenTrue.end()) {
        return it->second;
    }

    //     not found, try the signature
    // No! The below should only be for library functions!
    // return signature->getProven(left);
    return nullptr;
}


SharedExp UserProc::getPremised(SharedExp left)
{
    auto it = m_recurPremises.find(left);
    return it != m_recurPremises.end() ? it->second : nullptr;
}


bool UserProc::isPreserved(SharedExp e)
{
    return preservesExp(e);
}


void UserProc::setStatus(ProcStatus s)
{
    if (m_status != s) {
        m_status = s;
        if (m_prog) {
            m_prog->getProject()->alertProcStatusChanged(this);
        }
    }
}


void UserProc::setDecoded()
{
    setStatus(ProcStatus::Decoded);
}


BasicBlock *UserProc::getEntryBB()
{
    return m_cfg->getEntryBB();
}


void UserProc::setEntryBB()
{
    BasicBlock *entryBB = m_cfg->getBBStartingAt(m_entryAddress);
    m_cfg->setEntryAndExitBB(entryBB);
}


void UserProc::decompileRecursive()
{
    ProcDecompiler().decompileRecursive(this);
}


void UserProc::numberStatements() const
{
    int stmtNumber = 0;

    for (BasicBlock *bb : *m_cfg) {
        BasicBlock::RTLIterator rit;
        StatementList::iterator sit;
        for (Statement *s = bb->getFirstStmt(rit, sit); s; s = bb->getNextStmt(rit, sit)) {
            s->setNumber(++stmtNumber);
        }
    }
}


void UserProc::getStatements(StatementList &stmts) const
{
    for (const BasicBlock *bb : *m_cfg) {
        bb->appendStatementsTo(stmts);
    }

    for (Statement *s : stmts) {
        if (s->getProc() == nullptr) {
            s->setProc(const_cast<UserProc *>(this));
        }
    }
}


bool UserProc::removeStatement(Statement *stmt)
{
    if (!stmt) {
        return false;
    }

    // remove anything proven about this statement
    for (auto provenIt = m_provenTrue.begin(); provenIt != m_provenTrue.end();) {
        LocationSet refs;
        provenIt->second->addUsedLocs(refs);
        provenIt->first->addUsedLocs(
            refs); // Could be say m[esp{99} - 4] on LHS and we are deleting stmt 99

        auto it = std::find_if(refs.begin(), refs.end(), [stmt](const SharedExp &ref) {
            return ref->isSubscript() && ref->access<RefExp>()->getDef() == stmt;
        });

        if (it != refs.end()) {
            LOG_VERBOSE("Removing proven true exp %1 = %2 that uses statement being removed.",
                        provenIt->first, provenIt->second);

            provenIt = m_provenTrue.erase(provenIt);
            continue;
        }

        ++provenIt;
    }

    // remove from BB/RTL
    BasicBlock *bb = stmt->getBB(); // Get our enclosing BB
    if (!bb) {
        return false;
    }

    for (auto &rtl : *bb->getRTLs()) {
        for (RTL::iterator it = rtl->begin(); it != rtl->end(); ++it) {
            if (*it == stmt) {
                rtl->erase(it);
                return true;
            }
        }
    }

    return false;
}


Assign *UserProc::insertAssignAfter(Statement *s, SharedExp left, SharedExp right)
{
    BasicBlock *bb = nullptr;
    Assign *as     = new Assign(left, right);

    if (s == nullptr) {
        // This means right is supposed to be a parameter.
        // We can insert the assignment at the start of the entryBB
        bb = m_cfg->getEntryBB();
    }
    else {
        // An ordinary definition; put the assignment right after s
        bb = s->getBB();
    }

    as->setProc(this);
    as->setBB(bb);

    if (s) {
        // Insert the new assignment directly after s,
        // or near the end of the existing BB if s has been removed already.
        for (auto &rtl : *bb->getRTLs()) {
            for (auto it = rtl->begin(); it != rtl->end(); ++it) {
                if (*it == s) {
                    rtl->insert(++it, as);
                    return as;
                }
            }
        }
    }

    auto &lastRTL = bb->getRTLs()->back();
    if (lastRTL->empty() || lastRTL->back()->isAssignment()) {
        lastRTL->append(as);
    }
    else {
        // do not insert after a Branch statement etc.
        lastRTL->insert(std::prev(lastRTL->end()), as);
    }
    return as;
}


bool UserProc::insertStatementAfter(Statement *afterThis, Statement *stmt)
{
    assert(!afterThis->isBranch());

    for (BasicBlock *bb : *m_cfg) {
        RTLList *rtls = bb->getRTLs();

        if (rtls == nullptr) {
            continue; // e.g. bb is (as yet) invalid
        }

        for (const auto &rtl : *rtls) {
            for (RTL::iterator ss = rtl->begin(); ss != rtl->end(); ++ss) {
                if (*ss == afterThis) {
                    rtl->insert(std::next(ss), stmt);
                    stmt->setBB(bb);
                    return true;
                }
            }
        }
    }

    return false;
}


void UserProc::addParameterToSignature(SharedExp e, SharedType ty)
{
    // In case it's already an implicit argument:
    removeParameterFromSignature(e);

    m_signature->addParameter(e, ty);
}


void UserProc::insertParameter(SharedExp e, SharedType ty)
{
    if (filterParams(e)) {
        return; // Filtered out
    }

    // Used to filter out preserved locations here: no!
    // Propagation and dead code elimination solve the problem.
    // See test/pentium/restoredparam for an example
    // where you must not remove restored locations

    // Wrap it in an implicit assignment; DFA based TA should update the type later
    ImplicitAssign *as = new ImplicitAssign(ty->clone(), e->clone());

    auto it = std::lower_bound(m_parameters.begin(), m_parameters.end(), as,
                               [this](const Statement *stmt, const Assignment *a) {
                                   return m_signature->argumentCompare(
                                       static_cast<const Assignment &>(*stmt), *a);
                               });

    if (it == m_parameters.end() ||
        *static_cast<ImplicitAssign *>(*it)->getLeft() != *as->getLeft()) {
        // not a duplicate
        m_parameters.insert(it, as);
    }

    // update the signature
    m_signature->setNumParams(0);
    int i = 1;

    for (Statement *param : m_parameters) {
        Assignment *a     = static_cast<Assignment *>(param);
        QString paramName = QString("param%1").arg(i++);

        m_signature->addParameter(paramName, a->getLeft(), a->getType());
    }
}


SharedConstType UserProc::getParamType(const QString &name) const
{
    for (int i = 0; i < m_signature->getNumParams(); i++) {
        if (name == m_signature->getParamName(i)) {
            return m_signature->getParamType(i);
        }
    }

    return nullptr;
}


SharedType UserProc::getParamType(const QString &name)
{
    for (int i = 0; i < m_signature->getNumParams(); i++) {
        if (name == m_signature->getParamName(i)) {
            return m_signature->getParamType(i);
        }
    }

    return nullptr;
}


void UserProc::setParamType(const QString &name, SharedType ty)
{
    m_signature->setParamType(name, ty);
}


void UserProc::setParamType(int idx, SharedType ty)
{
    if (static_cast<size_t>(idx) >= m_parameters.size()) {
        // index out of range
        return;
    }

    auto it = std::next(m_parameters.begin(), idx);
    assert(it != m_parameters.end());
    assert((*it)->isAssignment());

    Assignment *a = static_cast<Assignment *>(*it);
    a->setType(ty);
    // Sometimes the signature isn't up to date with the latest parameters
    m_signature->setParamType(idx, ty);
}


QString UserProc::lookupParam(SharedConstExp e) const
{
    // Originally e.g. m[esp+K]
    Statement *def = m_cfg->findTheImplicitAssign(e);

    if (def == nullptr) {
        LOG_ERROR("No implicit definition for parameter %1!", e);
        return "";
    }

    SharedConstType ty = def->getTypeForExp(e);
    return lookupSym(RefExp::get(std::const_pointer_cast<Exp>(e), def), ty);
}


bool UserProc::filterParams(SharedExp e)
{
    switch (e->getOper()) {
    case opPC: return true;
    case opTemp: return true;
    case opRegOf: {
        if (!e->isRegOfConst()) {
            return false;
        }
        const int sp = Util::getStackRegisterIndex(m_prog);
        return e->access<Const, 1>()->getInt() == sp;
    }

    case opMemOf: {
        SharedExp addr = e->getSubExp1();

        if (addr->isIntConst()) {
            return true; // Global memory location
        }

        if (addr->isSubscript() && addr->access<RefExp>()->isImplicitDef()) {
            auto reg = addr->getSubExp1();
            int sp   = 999;

            if (m_signature) {
                sp = Util::getStackRegisterIndex(m_prog);
            }

            if (reg->isRegN(sp)) {
                return true; // Filter out m[sp{-}] assuming it is the return address
            }
        }

        return false; // Might be some weird memory expression that is not a local
    }

    case opGlobal:
        return true; // Never use globals as argument locations (could appear on RHS of args)

    default: return false;
    }
}


Address UserProc::getRetAddr()
{
    return m_retStatement != nullptr ? m_retStatement->getRetAddr() : Address::INVALID;
}


void UserProc::setRetStmt(ReturnStatement *s, Address r)
{
    assert(m_retStatement == nullptr);
    m_retStatement = s;
    m_retStatement->setRetAddr(r);
}


bool UserProc::filterReturns(SharedExp e)
{
    if (isPreserved(e)) {
        // If it is preserved, then it can't be a return (since we don't change it)
        return true;
    }

    switch (e->getOper()) {
    case opPC: return true; // Ignore %pc

    case opDefineAll: return true; // Ignore <all>

    case opTemp:
        return true; // Ignore all temps (should be local to one instruction)

        // Would like to handle at least %ZF, %CF one day. For now, filter them out
    case opZF:
    case opCF:
    case opNF:
    case opOF:
    case opFlags: return true;

    case opMemOf:
        // Actually, surely all sensible architectures will only every return
        // in registers. So for now, just filter out all mem-ofs
        // return signature->isStackLocal(prog, e); // Filter out local variables
        return true;

    case opGlobal: return true; // Never return in globals

    default: return false;
    }
}


SharedExp UserProc::createLocal(SharedType ty, const SharedExp &e, const QString &name)
{
    const QString localName = (name != "") ? name : newLocalName(e);

    if (ty == nullptr) {
        LOG_FATAL("Null type passed to newLocal");
    }

    LOG_VERBOSE2("Assigning type %1 to new %2", ty->getCtype(), localName);
    m_locals[localName] = ty;

    return Location::local(localName, this);
}


void UserProc::addLocal(SharedType ty, const QString &name, SharedExp e)
{
    // symbolMap is a multimap now; you might have r8->o0 for integers and r8->o0_1 for char*
    // assert(symbolMap.find(e) == symbolMap.end());
    mapSymbolTo(e, Location::local(name, this));
    // assert(locals.find(name) == locals.end());        // Could be r10{20} -> o2, r10{30}->o2 now
    m_locals[name] = ty;
}


void UserProc::ensureExpIsMappedToLocal(const std::shared_ptr<RefExp> &ref)
{
    if (!lookupSymFromRefAny(ref).isEmpty()) {
        return; // Already have a symbol for ref
    }

    Statement *def = ref->getDef();

    if (!def) {
        return; // TODO: should this be logged ?
    }

    SharedExp base = ref->getSubExp1();
    SharedType ty  = def->getTypeForExp(base);
    // No, get its name from the front end
    QString locName = nullptr;

    if (base->isRegOf()) {
        locName = getRegName(base);

        // Create a new local, for the base name if it doesn't exist yet,
        // so we don't need several names for the same combination of location
        // and type. However if it does already exist, addLocal will allocate
        // a new name.
        // Example: r8{0}->argc type int, r8->o0 type int, now r8->o0_1 type char*.
        if (existsLocal(locName)) {
            locName = newLocalName(ref);
        }
    }
    else {
        locName = newLocalName(ref);
    }

    addLocal(ty, locName, base);
}


SharedExp UserProc::getSymbolExp(SharedExp le, SharedType ty, bool lastPass)
{
    assert(ty != nullptr);

    SharedExp e = nullptr;

    // check for references to the middle of a local
    if (le->isMemOf() && le->getSubExp1()->getOper() == opMinus &&
        le->getSubExp1()->getSubExp1()->isSubscript() &&
        le->getSubExp1()->getSubExp1()->getSubExp1()->isRegN(m_signature->getStackRegister()) &&
        le->getSubExp1()->getSubExp2()->isIntConst()) {
        for (auto &elem : m_symbolMap) {
            if (!(elem).second->isLocal()) {
                continue;
            }

            QString name = elem.second->access<Const, 1>()->getStr();

            if (m_locals.find(name) == m_locals.end()) {
                continue;
            }

            SharedType lty     = m_locals[name];
            SharedConstExp loc = elem.first;

            if (loc->isMemOf() && loc->getSubExp1()->getOper() == opMinus &&
                loc->access<Exp, 1, 1>()->isSubscript() &&
                loc->access<Exp, 1, 1, 1>()->isRegN(m_signature->getStackRegister()) &&
                loc->access<Exp, 1, 2>()->isIntConst()) {
                const int byteOffset = le->access<Const, 1, 2>()->getInt() -
                                       loc->access<Const, 1, 2>()->getInt();

                if (Util::inRange(byteOffset, 1, static_cast<int>(lty->getSize() / 8))) {
                    e = Location::memOf(Binary::get(opPlus,
                                                    Unary::get(opAddrOf, elem.second->clone()),
                                                    Const::get(byteOffset)));
                    LOG_VERBOSE("Seems %1 is in the middle of %2 returning %3", le, loc, e);
                    return e;
                }
            }
        }
    }

    if (m_symbolMap.find(le) != m_symbolMap.end()) {
        return getSymbolFor(le, ty);
    }

    if (*ty == *VoidType::get() && lastPass) {
        ty = IntegerType::get(STD_SIZE);
    }

    // the default of just assigning an int type is bad..
    // if the locals is not an int then assigning it this
    // type early results in aliases to this local not being recognised

    //            Exp* base = le;
    //            if (le->isSubscript())
    //                base = ((RefExp*)le)->getSubExp1();
    // NOTE: using base below instead of le does not enhance anything, but causes us to lose def
    // information
    e = createLocal(ty->clone(), le);
    mapSymbolTo(le->clone(), e);
    return e->clone();
}


QString UserProc::findLocal(const SharedExp &e, SharedType ty)
{
    if (e->isLocal()) {
        return e->access<Const, 1>()->getStr();
    }

    // Look it up in the symbol map
    QString name = lookupSym(e, ty);

    if (name.isEmpty()) {
        return name;
    }

    // Now make sure it is a local; some symbols (e.g. parameters) are in the symbol map but not
    // locals
    if (m_locals.find(name) != m_locals.end()) {
        return name;
    }

    return "";
}


SharedConstType UserProc::getLocalType(const QString &name) const
{
    auto it = m_locals.find(name);
    return (it != m_locals.end()) ? it->second : nullptr;
}


void UserProc::setLocalType(const QString &name, SharedType ty)
{
    const auto it = m_locals.find(name);
    if (it != m_locals.end()) {
        it->second = ty;
        LOG_VERBOSE("Updating type of '%1' to %2", name, ty->getCtype());
    }
}


bool UserProc::isLocalOrParamPattern(SharedConstExp e) const
{
    if (!e->isMemOf()) {
        return false; // Don't want say a register
    }
    else if (!m_signature || !m_signature->isPromoted()) {
        return false; // Prevent an assert failure if using -E
    }

    const int sp = m_signature->getStackRegister();
    const auto initSp(RefExp::get(Location::regOf(sp), nullptr)); // sp{-}

    SharedConstExp addrExp = e->getSubExp1();
    if (*addrExp == *initSp) {
        return true; // Accept m[sp{-}]
    }
    else if (addrExp->getOper() != opPlus && addrExp->getOper() != opMinus) {
        return false;
    }

    return (*addrExp->getSubExp1() == *initSp) && addrExp->getSubExp2()->isIntConst();
}


SharedConstExp UserProc::expFromSymbol(const QString &name) const
{
    for (const std::pair<SharedConstExp, SharedExp> &it : m_symbolMap) {
        const SharedConstExp exp = it.second;
        if (exp->isLocal() && (exp->access<Const, 1>()->getStr() == name)) {
            return it.first;
        }
    }

    return nullptr;
}


void UserProc::mapSymbolTo(const SharedConstExp &from, SharedExp to)
{
    assert(from && to);

    SymbolMap::iterator it = m_symbolMap.find(from);

    while (it != m_symbolMap.end() && *it->first == *from) {
        if (*it->second == *to) {
            return; // Already in the multimap
        }

        ++it;
    }

    std::pair<SharedConstExp, SharedExp> pr = { from, to };
    m_symbolMap.insert(pr);
}


QString UserProc::lookupSym(const SharedConstExp &arg, SharedConstType ty) const
{
    SharedConstExp e = arg;

    if (arg->isTypedExp()) {
        e = e->getSubExp1();
    }

    SymbolMap::const_iterator it = m_symbolMap.find(e);

    while (it != m_symbolMap.end() && *it->first == *e) {
        SharedExp sym = it->second;
        assert(sym->isLocal() || sym->isParam());

        const QString name   = sym->access<Const, 1>()->getStr();
        SharedConstType type = getLocalType(name);

        if (type == nullptr) {
            type = getParamType(name); // Ick currently linear search
        }

        if (type && type->isCompatibleWith(*ty)) {
            return name;
        }

        ++it;
    }

    // Else there is no symbol
    return "";
}


QString UserProc::lookupSymFromRef(const std::shared_ptr<const RefExp> &ref) const
{
    const Statement *def = ref->getDef();

    if (!def) {
        LOG_WARN("Unknown def for RefExp '%1' in '%2'", ref, getName());
        return "";
    }

    SharedConstExp base = ref->getSubExp1();
    SharedConstType ty  = def->getTypeForExp(base);
    return lookupSym(ref, ty);
}


QString UserProc::lookupSymFromRefAny(const std::shared_ptr<const RefExp> &ref) const
{
    const Statement *def = ref->getDef();

    if (!def) {
        LOG_WARN("Unknown def for RefExp '%1' in '%2'", ref, getName());
        return "";
    }

    SharedConstExp base = ref->getSubExp1();
    SharedConstType ty  = def->getTypeForExp(base);

    // Check for specific symbol
    const QString ret = lookupSym(ref, ty);
    if (!ret.isEmpty()) {
        return ret;
    }

    return lookupSym(base, ty); // Check for a general symbol
}


void UserProc::markAsNonChildless(const std::shared_ptr<ProcSet> &cs)
{
    assert(cs);

    BasicBlock::RTLRIterator rrit;
    StatementList::reverse_iterator srit;

    for (BasicBlock *bb : *m_cfg) {
        CallStatement *c = dynamic_cast<CallStatement *>(bb->getLastStmt(rrit, srit));

        if (c && c->isChildless()) {
            UserProc *dest = static_cast<UserProc *>(c->getDestProc());

            if (cs->find(dest) != cs->end()) { // Part of the cycle?
                // Yes, set the callee return statement (making it non childless)
                c->setCalleeReturn(dest->getRetStmt());
            }
        }
    }
}


void UserProc::addCallee(Function *callee)
{
    assert(callee != nullptr);

    // is it already in? (this is much slower than using a set)
    if (std::find(m_calleeList.begin(), m_calleeList.end(), callee) == m_calleeList.end()) {
        m_calleeList.push_back(callee);
    }
}


bool UserProc::preservesExp(const SharedExp &exp)
{
    if (!m_prog->getProject()->getSettings()->useProof) {
        return false;
    }

    return proveEqual(exp, exp, false);
}


bool UserProc::preservesExpWithOffset(const SharedExp &exp, int offset)
{
    return proveEqual(exp, Binary::get(opPlus, exp, Const::get(offset)), false);
}


void UserProc::promoteSignature()
{
    m_signature = m_signature->promote(this);
}


QString UserProc::findFirstSymbol(const SharedConstExp &exp) const
{
    auto it = m_symbolMap.find(exp);
    if (it != m_symbolMap.end()) {
        return it->second->access<Const, 1>()->getStr();
    }
    return "";
}


bool UserProc::searchAndReplace(const Exp &search, SharedExp replace)
{
    bool ch = false;
    StatementList stmts;
    getStatements(stmts);

    for (Statement *s : stmts) {
        ch |= s->searchAndReplace(search, replace);
    }

    return ch;
}


void UserProc::markAsInitialParam(const SharedExp &loc)
{
    m_procUseCollector.insert(loc);
}


bool UserProc::allPhisHaveDefs() const
{
    StatementList stmts;
    getStatements(stmts);

    for (const Statement *stmt : stmts) {
        if (!stmt->isPhi()) {
            continue; // Might be able to optimise this a bit
        }

        const PhiAssign *pa = static_cast<const PhiAssign *>(stmt);

        for (const auto &ref : *pa) {
            if (!ref.getDef()) {
                return false;
            }
        }
    }

    return true;
}


QString UserProc::toString() const
{
    QString tgt;
    OStream ost(&tgt);

    this->print(ost);
    return tgt;
}


void UserProc::print(OStream &out) const
{
    numberStatements();

    QString tgt1;
    QString tgt2;

    OStream ost1(&tgt1);
    OStream ost2(&tgt2);


    printParams(ost1);
    printLocals(ost1);
    m_procUseCollector.print(ost2);

    m_signature->print(out);
    out << "\n";
    out << "in module " << m_module->getName() << "\n";
    out << tgt1;

    printSymbolMap(out);

    out << "live variables:\n";

    if (tgt2.isEmpty()) {
        out << "  <None>\n";
    }
    else {
        out << "  " << tgt2 << "\n";
    }

    QString tgt3;
    OStream ost3(&tgt3);
    m_cfg->print(ost3);
    out << tgt3 << "\n";
}


void UserProc::printParams(OStream &out) const
{
    out << "parameters: ";

    if (!m_parameters.empty()) {
        bool first = true;

        for (auto const &elem : m_parameters) {
            if (first) {
                first = false;
            }
            else {
                out << ", ";
            }

            out << static_cast<Assignment *>(elem)->getType() << " "
                << static_cast<Assignment *>(elem)->getLeft();
        }
    }
    else {
        out << "<None>";
    }

    out << "\n";
}


void UserProc::printSymbolMap(OStream &out) const
{
    out << "symbols:\n";

    if (m_symbolMap.empty()) {
        out << "  <None>\n";
    }
    else {
        for (const std::pair<SharedConstExp, SharedExp> &it : m_symbolMap) {
            SharedConstType ty = getTypeForLocation(it.second);
            out << "  " << it.first << " maps to " << it.second << " type "
                << (ty ? qPrintable(ty->getCtype()) : "<unknown>") << "\n";
        }
    }
}


void UserProc::printLocals(OStream &os) const
{
    os << "locals:\n";

    if (m_locals.empty()) {
        os << "  <None>\n";
    }
    else {
        for (const std::pair<QString, SharedType> &local_entry : m_locals) {
            os << local_entry.second->getCtype() << " " << local_entry.first << " ";
            SharedConstExp e = expFromSymbol(local_entry.first);

            // Beware: for some locals, expFromSymbol() returns nullptr (? No longer?)
            if (e) {
                os << "  " << e << "\n";
            }
            else {
                os << "  -\n";
            }
        }
    }
}


void UserProc::debugPrintAll(const QString &stepName)
{
    if (m_prog->getProject()->getSettings()->verboseOutput) {
        numberStatements();

        QDir outputDir   = m_prog->getProject()->getSettings()->getOutputDirectory();
        QString filePath = outputDir.absoluteFilePath(getName());

        LOG_SEPARATE(filePath, "--- debug print %1 for %2 ---", stepName, getName());
        LOG_SEPARATE(filePath, "%1", this->toString());
        LOG_SEPARATE(filePath, "=== end debug print %1 for %2 ===", stepName, getName());
    }
}


bool UserProc::existsLocal(const QString &name) const
{
    return m_locals.find(name) != m_locals.end();
}


QString UserProc::newLocalName(const SharedExp &e)
{
    QString localName;

    if (e->isSubscript() && e->getSubExp1()->isRegOf()) {
        // Assume that it's better to know what register this location was created from
        QString regName = getRegName(e->getSubExp1());
        int tag         = 0;

        do {
            localName = QString("%1_%2").arg(regName).arg(++tag);
        } while (m_locals.find(localName) != m_locals.end());

        return localName;
    }

    return QString("local%1").arg(m_nextLocal++);
}


QString UserProc::getRegName(SharedExp r)
{
    assert(r->isRegOf());

    // assert(r->getSubExp1()->isConst());
    if (r->getSubExp1()->isConst()) {
        const RegNum regNum = r->access<Const, 1>()->getInt();

        if (regNum == (RegNum)-1) {
            LOG_WARN("Tried to get name of special register!");
            return "r[-1]";
        }

        QString regName = m_prog->getRegNameByNum(regNum);
        if (regName[0] == '%') {
            return regName.mid(1); // Skip % if %eax
        }

        return regName;
    }

    LOG_WARN("Will try to build register name from [tmp+X]!");

    // TODO: how to handle register file lookups ?
    // in some cases the form might be r[tmp+value]
    // just return this expression :(
    // WARN: this is a hack to prevent crashing when r->subExp1 is not const
    QString tgt;
    OStream ostr(&tgt);

    r->getSubExp1()->print(ostr);

    return tgt;
}


SharedType UserProc::getTypeForLocation(const SharedExp &e)
{
    const QString name = e->access<Const, 1>()->getStr();
    if (e->isLocal()) {
        auto it = m_locals.find(name);
        if (it != m_locals.end()) {
            return it->second;
        }
    }

    // Sometimes parameters use opLocal, so fall through
    return getParamType(name);
}


SharedConstType UserProc::getTypeForLocation(const SharedConstExp &e) const
{
    const QString name = e->access<Const, 1>()->getStr();
    if (e->isLocal()) {
        auto it = m_locals.find(name);
        if (it != m_locals.end()) {
            return it->second;
        }
    }

    // Sometimes parameters use opLocal, so fall through
    return getParamType(name);
}


static const SharedExp defAll = Terminal::get(opDefineAll);


bool UserProc::proveEqual(const SharedExp &queryLeft, const SharedExp &queryRight, bool conditional)
{
    if ((m_provenTrue.find(queryLeft) != m_provenTrue.end()) &&
        (*m_provenTrue[queryLeft] == *queryRight)) {
        if (m_prog->getProject()->getSettings()->debugProof) {
            LOG_MSG("found true in provenTrue cache %1 in %2",
                    Binary::get(opEquals, queryLeft, queryRight), getName());
        }

        return true;
    }

    const SharedExp origLeft  = queryLeft;
    const SharedExp origRight = queryRight;

    SharedExp query = Binary::get(opEquals, queryLeft->clone(), queryRight->clone());

    // subscript locs on the right with {-} (nullptr reference)
    LocationSet locs;
    query->getSubExp2()->addUsedLocs(locs);

    for (const SharedExp &xx : locs) {
        query->setSubExp2(query->getSubExp2()->expSubscriptValNull(xx));
    }

    if (!query->getSubExp1()->isSubscript()) {
        bool gotDef = false;

        // replace expression from return set with expression in the collector of the return
        if (m_retStatement) {
            auto def = m_retStatement->findDefFor(query->getSubExp1());

            if (def) {
                query->setSubExp1(def);
                gotDef = true;
            }
        }

        if (!gotDef) {
            // OK, the thing I'm looking for isn't in the return collector, but perhaps there is an
            // entry for <all> If this is proved, then it is safe to say that x == x for any x with
            // no definition reaching the exit
            auto right = origRight->clone()->simplify(); // In case it's sp+0

            if ((*origLeft == *right) &&                // x == x
                (origLeft->getOper() != opDefineAll) && // Beware infinite recursion
                proveEqual(defAll, defAll)) {           // Recurse in case <all> not proven yet
                if (m_prog->getProject()->getSettings()->debugProof) {
                    LOG_MSG("Using all=all for %1", query->getSubExp1());
                    LOG_MSG("Prove returns true");
                }

                m_provenTrue[origLeft->clone()] = right;
                return true;
            }

            if (m_prog->getProject()->getSettings()->debugProof) {
                LOG_MSG("Not in return collector: %1", query->getSubExp1());
                LOG_MSG("Prove returns false");
            }

            return false;
        }
    }

    if (m_recursionGroup) { // If in involved in a recursion cycle
        //    then save the original query as a premise for bypassing calls
        m_recurPremises[origLeft->clone()] = origRight;
    }

    std::set<PhiAssign *> lastPhis;
    std::map<PhiAssign *, SharedExp> cache;
    bool result = prover(query, lastPhis, cache);

    if (m_recursionGroup) {
        killPremise(origLeft); // Remove the premise, regardless of result
    }

    if (m_prog->getProject()->getSettings()->debugProof) {
        LOG_MSG("Prove returns %1 for %2 in %3", (result ? "true" : "false"), query,
                this->getName());
    }

    if (result && !conditional) {
        m_provenTrue[origLeft] = origRight; // Save the now proven equation
    }

    return result;
}


bool UserProc::prover(SharedExp query, std::set<PhiAssign *> &lastPhis,
                      std::map<PhiAssign *, SharedExp> &cache, PhiAssign *lastPhi /* = nullptr */)
{
    // A map that seems to be used to detect loops in the call graph:
    std::map<CallStatement *, SharedExp> called;
    auto phiInd = query->getSubExp2()->clone();

    if (lastPhi && (cache.find(lastPhi) != cache.end()) && (*cache[lastPhi] == *phiInd)) {
        if (m_prog->getProject()->getSettings()->debugProof) {
            LOG_MSG("true - in the phi cache");
        }

        return true;
    }

    std::set<Statement *> refsTo;

    query        = query->clone();
    bool change  = true;
    bool swapped = false;

    while (change) {
        if (m_prog->getProject()->getSettings()->debugProof) {
            LOG_MSG("%1", query);
        }

        change = false;

        if (query->isEquality()) {
            // same left and right means true
            if (*query->getSubExp1() == *query->getSubExp2()) {
                query  = Terminal::get(opTrue);
                change = true;
            }

            // move constants to the right
            if (!change) {
                auto plus = query->getSubExp1();
                auto s1s2 = plus ? plus->getSubExp2() : nullptr;

                if (plus && s1s2) {
                    if ((plus->getOper() == opPlus) && s1s2->isIntConst()) {
                        query->setSubExp2(Binary::get(opPlus, query->getSubExp2(),
                                                      Unary::get(opNeg, s1s2->clone())));
                        query->setSubExp1(plus->getSubExp1());
                        change = true;
                    }

                    if ((plus->getOper() == opMinus) && s1s2->isIntConst()) {
                        query->setSubExp2(Binary::get(opPlus, query->getSubExp2(), s1s2->clone()));
                        query->setSubExp1(plus->getSubExp1());
                        change = true;
                    }
                }
            }

            // substitute using a statement that has the same left as the query
            if (!change && (query->getSubExp1()->isSubscript())) {
                auto r              = query->access<RefExp, 1>();
                Statement *s        = r->getDef();
                CallStatement *call = dynamic_cast<CallStatement *>(s);

                if (call) {
                    // See if we can prove something about this register.
                    UserProc *destProc = dynamic_cast<UserProc *>(call->getDestProc());
                    SharedExp base     = r->getSubExp1();

                    if (destProc && !destProc->isLib() && (destProc->m_recursionGroup != nullptr) &&
                        (destProc->m_recursionGroup->find(this) !=
                         destProc->m_recursionGroup->end())) {
                        // The destination procedure may not have preservation proved as yet,
                        // because it is involved in our recursion group. Use the conditional
                        // preservation logic to determine whether query is true for this procedure
                        SharedExp provenTo = destProc->getProven(base);

                        if (provenTo) {
                            // There is a proven preservation. Use it to bypass the call
                            auto queryLeft = call->localiseExp(provenTo->clone());
                            query->setSubExp1(queryLeft);

                            // Now try everything on the result
                            return prover(query, lastPhis, cache, lastPhi);
                        }
                        else {
                            // Check if the required preservation is one of the premises already
                            // assumed
                            SharedExp premisedTo = destProc->getPremised(base);

                            if (premisedTo) {
                                if (m_prog->getProject()->getSettings()->debugProof) {
                                    LOG_MSG("Conditional preservation for call from %1 to %2, "
                                            "allows bypassing",
                                            getName(), destProc->getName());
                                }

                                auto queryLeft = call->localiseExp(premisedTo->clone());
                                query->setSubExp1(queryLeft);
                                return prover(query, lastPhis, cache, lastPhi);
                            }
                            else {
                                // There is no proof, and it's not one of the premises. It may yet
                                // succeed, by making another premise! Example: try to prove esp,
                                // depends on whether ebp is preserved, so recurse to check ebp's
                                // preservation. Won't infinitely loop because of the premise map
                                // FIXME: what if it needs a rx = rx + K preservation?
                                destProc->setPremise(base);

                                if (m_prog->getProject()->getSettings()->debugProof) {
                                    LOG_MSG("New required premise '%1' for %2",
                                            Binary::get(opEquals, base, base), destProc->getName());
                                }

                                // Pass conditional as true, since even if proven, this is
                                // conditional on other things
                                bool result = destProc->proveEqual(base->clone(), base->clone(),
                                                                   true);
                                destProc->killPremise(base);

                                if (result) {
                                    if (m_prog->getProject()->getSettings()->debugProof) {
                                        LOG_MSG("Conditional preservation with new premise '%1' "
                                                "succeeds for %2",
                                                Binary::get(opEquals, base, base),
                                                destProc->getName());
                                    }

                                    // Use the new conditionally proven result
                                    auto queryLeft = call->localiseExp(base->clone());
                                    query->setSubExp1(queryLeft);
                                    return destProc->prover(query, lastPhis, cache, lastPhi);
                                }
                                else {
                                    if (m_prog->getProject()->getSettings()->debugProof) {
                                        LOG_MSG(
                                            "Conditional preservation required premise '%1' fails!",
                                            Binary::get(opEquals, base, base));
                                    }

                                    // Do nothing else; the outer proof will likely fail
                                }
                            }
                        }
                    } // End call involved in this recursion group

                    // Seems reasonable that recursive procs need protection from call loops too
                    // getProven returns the right side of what is
                    auto right = call->getProven(r->getSubExp1());

                    if (right) { //    proven about r (the LHS of query)
                        right = right->clone();

                        if ((called.find(call) != called.end()) && (*called[call] == *query)) {
                            LOG_MSG("Found call loop to %1 %2", call->getDestProc()->getName(),
                                    query);
                            query  = Terminal::get(opFalse);
                            change = true;
                        }
                        else {
                            called[call] = query->clone();

                            if (m_prog->getProject()->getSettings()->debugProof) {
                                LOG_MSG("Using proven for %1 %2 = %3",
                                        call->getDestProc()->getName(), r->getSubExp1(), right);
                            }

                            right = call->localiseExp(right);

                            if (m_prog->getProject()->getSettings()->debugProof) {
                                LOG_MSG("Right with subs: %1", right);
                            }

                            query->setSubExp1(right); // Replace LHS of query with right
                            change = true;
                        }
                    }
                }
                else if (s && s->isPhi()) {
                    // for a phi, we have to prove the query for every statement
                    PhiAssign *pa = static_cast<PhiAssign *>(s);
                    bool ok       = true;

                    if ((lastPhis.find(pa) != lastPhis.end()) || (pa == lastPhi)) {
                        ok = (*query->getSubExp2() == *phiInd);

                        if (m_prog->getProject()->getSettings()->debugProof) {
                            if (ok) {
                                // FIXME: induction??!
                                LOG_MSG("Phi loop detected (set true due to induction)");
                            }
                            else {
                                LOG_MSG("Phi loop detected (set false: %1 != %2)",
                                        query->getSubExp2(), phiInd);
                            }
                        }
                    }
                    else {
                        if (m_prog->getProject()->getSettings()->debugProof) {
                            LOG_MSG("Found %1 prove for each, ", s);
                        }

                        for (RefExp &pi : *pa) {
                            auto e  = query->clone();
                            auto r1 = e->access<RefExp, 1>();
                            r1->setDef(pi.getDef());

                            if (m_prog->getProject()->getSettings()->debugProof) {
                                LOG_MSG("proving for %1", e);
                            }

                            lastPhis.insert(lastPhi);

                            if (!prover(e, lastPhis, cache, pa)) {
                                ok = false;
                                // delete e;
                                break;
                            }

                            lastPhis.erase(lastPhi);
                            // delete e;
                        }

                        if (ok) {
                            cache[pa] = query->getSubExp2()->clone();
                        }
                    }

                    if (ok) {
                        query = Terminal::get(opTrue);
                    }
                    else {
                        query = Terminal::get(opFalse);
                    }

                    change = true;
                }
                else if (s && s->isAssign()) {
                    if (refsTo.find(s) != refsTo.end()) {
                        LOG_ERROR("Detected ref loop %1", s);
                        LOG_ERROR("refsTo: ");

                        for (Statement *ins : refsTo) {
                            LOG_MSG("  %1, ", ins->toString());
                        }

                        return false;
                    }
                    else {
                        refsTo.insert(s);
                        query->setSubExp1(static_cast<Assign *>(s)->getRight()->clone());
                        change = true;
                    }
                }
            }

            // remove memofs from both sides if possible
            if (!change && query->getSubExp1()->isMemOf() && query->getSubExp2()->isMemOf()) {
                query->setSubExp1(query->access<Exp, 1, 1>());
                query->setSubExp2(query->access<Exp, 2, 1>());
                change = true;
            }

            // is ok if both of the memofs are subscripted with nullptr
            if (!change && query->getSubExp1()->isSubscript() &&
                query->access<Exp, 1, 1>()->isMemOf() &&
                query->access<RefExp, 1>()->getDef() == nullptr &&
                query->access<Exp, 2>()->isSubscript() && query->access<Exp, 2, 1>()->isMemOf() &&
                query->access<RefExp, 2>()->getDef() == nullptr) {
                query->setSubExp1(query->access<Exp, 1, 1, 1>());
                query->setSubExp2(query->access<Exp, 2, 1, 1>());
                change = true;
            }

            // find a memory def for the right if there is a memof on the left
            // FIXME: this seems pretty much like a bad hack!
            if (!change && query->getSubExp1()->isMemOf()) {
                StatementList stmts;
                getStatements(stmts);

                for (Statement *s : stmts) {
                    Assign *as = dynamic_cast<Assign *>(s);

                    if (as && (*as->getRight() == *query->getSubExp2()) &&
                        as->getLeft()->isMemOf()) {
                        query->setSubExp2(as->getLeft()->clone());
                        change = true;
                        break;
                    }
                }
            }

            // last chance, swap left and right if haven't swapped before
            if (!change && !swapped) {
                auto e = query->getSubExp1();
                query->setSubExp1(query->getSubExp2());
                query->setSubExp2(e);
                change  = true;
                swapped = true;
                refsTo.clear();
            }
        }
        else if (query->isIntConst()) {
            auto c = query->access<Const>();
            query  = Terminal::get(c->getInt() ? opTrue : opFalse);
        }

        auto old = query->clone();

        auto query_prev = query;
        query           = query->clone()->simplify();

        if (change && !(*old == *query) && m_prog->getProject()->getSettings()->debugProof) {
            LOG_MSG("%1", old);
        }
    }

    return query->isTrue();
}


SharedExp UserProc::getSymbolFor(const SharedConstExp &from, const SharedConstType &ty) const
{
    assert(ty != nullptr);

    SymbolMap::const_iterator ff = m_symbolMap.find(from);

    while (ff != m_symbolMap.end() && *ff->first == *from) {
        SharedExp currTo = ff->second;
        assert(currTo->isLocal() || currTo->isParam());
        QString name           = currTo->access<Const, 1>()->getStr();
        SharedConstType currTy = getLocalType(name);

        if (currTy == nullptr) {
            currTy = getParamType(name);
        }

        if (currTy && currTy->isCompatibleWith(*ty)) {
            return currTo;
        }

        ++ff;
    }

    return nullptr;
}


void UserProc::setPremise(const SharedExp &e)
{
    SharedExp premise  = e->clone();
    m_recurPremises[e] = e;
}


void UserProc::killPremise(const SharedExp &e)
{
    m_recurPremises.erase(e);
}


bool UserProc::isNoReturnInternal(std::set<const Function *> &visited) const
{
    // undecoded procs are assumed to always return (and define everything)
    if (!this->isDecoded()) {
        return false;
    }

    BasicBlock *exitbb = m_cfg->getExitBB();

    if (exitbb == nullptr) {
        return true;
    }

    if (exitbb->getNumPredecessors() == 1) {
        Statement *s = exitbb->getPredecessor(0)->getLastStmt();

        if (!s || !s->isCall()) {
            return false;
        }

        const CallStatement *call = static_cast<const CallStatement *>(s);
        const Function *callee    = call->getDestProc();

        if (callee) {
            visited.insert(this);

            if (visited.find(callee) != visited.end()) {
                // we have found a procedure involved in tail recursion (either self or mutual).
                // Assume we have not found all the BBs yet that reach the return statement.
                return false;
            }
            else if (callee->isLib()) {
                return callee->isNoReturn();
            }
            else {
                return static_cast<const UserProc *>(callee)->isNoReturnInternal(visited);
            }
        }
    }

    return false;
}
