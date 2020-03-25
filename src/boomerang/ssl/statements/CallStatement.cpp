#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "CallStatement.h"

#include "boomerang/db/Prog.h"
#include "boomerang/db/binary/BinaryImage.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/ifc/ICodeGenerator.h"
#include "boomerang/passes/PassManager.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/exp/RefExp.h"
#include "boomerang/ssl/exp/Terminal.h"
#include "boomerang/ssl/statements/Assign.h"
#include "boomerang/ssl/statements/ImplicitAssign.h"
#include "boomerang/ssl/statements/PhiAssign.h"
#include "boomerang/ssl/type/ArrayType.h"
#include "boomerang/ssl/type/CharType.h"
#include "boomerang/ssl/type/FloatType.h"
#include "boomerang/ssl/type/FuncType.h"
#include "boomerang/ssl/type/IntegerType.h"
#include "boomerang/ssl/type/PointerType.h"
#include "boomerang/ssl/type/VoidType.h"
#include "boomerang/util/ArgSourceProvider.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/visitor/expmodifier/ImplicitConverter.h"
#include "boomerang/visitor/expmodifier/Localiser.h"
#include "boomerang/visitor/expvisitor/ExpVisitor.h"
#include "boomerang/visitor/stmtexpvisitor/StmtExpVisitor.h"
#include "boomerang/visitor/stmtmodifier/StmtImplicitConverter.h"
#include "boomerang/visitor/stmtmodifier/StmtModifier.h"
#include "boomerang/visitor/stmtmodifier/StmtPartModifier.h"
#include "boomerang/visitor/stmtvisitor/StmtVisitor.h"

#include <QRegularExpression>
#include <QTextStreamManipulator>


CallStatement::CallStatement(Address dest)
    : GotoStatement(dest)
{
    m_kind = StmtType::Call;
}


CallStatement::CallStatement(SharedExp dest)
    : GotoStatement(dest)
{
    m_kind = StmtType::Call;
}


CallStatement::CallStatement(const CallStatement &other)
    : GotoStatement(other)
    , m_returnAfterCall(other.m_returnAfterCall)
    , m_procDest(other.m_procDest)
    , m_useCol()
    , m_defCol()
    , m_calleeReturn(other.m_calleeReturn)
{
    for (SharedStmt stmt : other.m_arguments) {
        m_arguments.append(stmt->clone());
    }

    for (SharedStmt stmt : other.m_defines) {
        m_defines.append(stmt->clone());
    }
}


CallStatement::~CallStatement()
{
}


CallStatement &CallStatement::operator=(const CallStatement &other)
{
    if (this == &other) {
        return *this;
    }

    GotoStatement::operator=(other);

    m_returnAfterCall = other.m_returnAfterCall;
    m_procDest        = other.m_procDest;
    m_calleeReturn    = other.m_calleeReturn;

    for (SharedStmt stmt : other.m_arguments) {
        m_arguments.append(stmt->clone());
    }

    for (SharedStmt stmt : other.m_defines) {
        m_defines.append(stmt->clone());
    }

    return *this;
}


SharedStmt CallStatement::clone() const
{
    return std::make_shared<CallStatement>(*this);
}


bool CallStatement::accept(StmtVisitor *visitor) const
{
    return visitor->visit(this);
}


bool CallStatement::accept(StmtExpVisitor *v)
{
    bool visitChildren = true;
    bool ret           = v->visit(shared_from_this()->as<CallStatement>(), visitChildren);

    if (!visitChildren) {
        return ret;
    }

    if (ret) {
        ret = m_dest->acceptVisitor(v->ev);
    }

    for (SharedStmt s : m_arguments) {
        ret &= s->accept(v);
    }

    // FIXME: surely collectors should be counted?
    return ret;
}


bool CallStatement::accept(StmtModifier *v)
{
    bool visitChildren = true;
    v->visit(shared_from_this()->as<CallStatement>(), visitChildren);

    if (!visitChildren) {
        return true;
    }

    if (v->m_mod) {
        setDest(m_dest->acceptModifier(v->m_mod));
    }

    if (visitChildren) {
        for (SharedStmt s : m_arguments) {
            s->accept(v);
        }
    }

    // For example: needed for CallBypasser so that a collected definition that happens to be
    // another call gets adjusted I'm thinking no at present... let the bypass and propagate while
    // possible logic take care of it, and leave the collectors as the rename logic set it Well,
    // sort it out with ignoreCollector()
    if (!v->ignoreCollector()) {
        DefCollector::iterator cc;

        for (SharedStmt s : m_defCol) {
            s->accept(v);
        }
    }

    if (visitChildren) {
        for (SharedStmt s : m_defines) {
            s->accept(v);
        }
    }
    return true;
}


bool CallStatement::accept(StmtPartModifier *v)
{
    bool visitChildren = true;
    v->visit(shared_from_this()->as<CallStatement>(), visitChildren);

    if (visitChildren) {
        setDest(m_dest->acceptModifier(v->mod));
    }

    if (visitChildren) {
        for (SharedStmt s : m_arguments) {
            s->accept(v);
        }
    }

    // For example: needed for CallBypasser so that a collected definition that happens to be
    // another call gets adjusted But now I'm thinking no, the bypass and propagate while possible
    // logic should take care of it. Then again, what about the use collectors in calls?
    // Best to do it.
    if (!v->ignoreCollector()) {
        for (SharedStmt s : m_defCol) {
            s->accept(v);
        }

        for (SharedExp exp : m_useCol) {
            // I believe that these should never change at the top level,
            // e.g. m[esp{30} + 4] -> m[esp{-} - 20]
            exp->acceptModifier(v->mod);
        }
    }

    StatementList::iterator dd;

    if (visitChildren) {
        for (SharedStmt s : m_defines) {
            s->accept(v);
        }
    }

    return true;
}


void CallStatement::setNumber(int num)
{
    m_number = num;
    // Also number any existing arguments. Important for library procedures, since these have
    // arguments set by the front end based in their signature

    for (SharedStmt stmt : m_arguments) {
        stmt->setNumber(num);
    }
}


void CallStatement::getDefinitions(LocationSet &defs, bool assumeABICompliance) const
{
    for (SharedStmt def : m_defines) {
        defs.insert(def->as<Assignment>()->getLeft());
    }

    // Childless calls are supposed to define everything.
    // In practice they don't really define things like %pc,
    // so we need some extra logic in getTypeFor()
    if (isChildless() && !assumeABICompliance) {
        defs.insert(Terminal::get(opDefineAll));
    }
}


bool CallStatement::definesLoc(SharedExp loc) const
{
    for (SharedConstStmt def : m_defines) {
        if (*def->as<const Assign>()->getLeft() == *loc) {
            return true;
        }
    }

    return false;
}


bool CallStatement::search(const Exp &pattern, SharedExp &result) const
{
    if (GotoStatement::search(pattern, result)) {
        return true;
    }

    for (const SharedStmt &stmt : m_defines) {
        if (stmt->search(pattern, result)) {
            return true;
        }
    }

    for (const SharedStmt &stmt : m_arguments) {
        if (stmt->search(pattern, result)) {
            return true;
        }
    }

    return false;
}


bool CallStatement::searchAll(const Exp &pattern, std::list<SharedExp> &result) const
{
    bool found = GotoStatement::searchAll(pattern, result);

    for (const SharedStmt &def : m_defines) {
        if (def->searchAll(pattern, result)) {
            found = true;
        }
    }

    for (const SharedStmt &arg : m_arguments) {
        if (arg->searchAll(pattern, result)) {
            found = true;
        }
    }

    return found;
}


bool CallStatement::searchAndReplace(const Exp &pattern, SharedExp replace, bool cc)
{
    bool change = GotoStatement::searchAndReplace(pattern, replace, cc);

    // FIXME: MVE: Check if we ever want to change the LHS of arguments or defines...
    for (SharedStmt ss : m_defines) {
        change |= ss->searchAndReplace(pattern, replace, cc);
    }

    for (SharedStmt ss : m_arguments) {
        change |= ss->searchAndReplace(pattern, replace, cc);
    }

    if (cc) {
        DefCollector::iterator dd;

        for (dd = m_defCol.begin(); dd != m_defCol.end(); ++dd) {
            change |= (*dd)->searchAndReplace(pattern, replace, cc);
        }
    }

    return change;
}


void CallStatement::simplify()
{
    GotoStatement::simplify();

    for (SharedStmt ss : m_arguments) {
        ss->simplify();
    }

    for (SharedStmt ss : m_defines) {
        ss->simplify();
    }
}


SharedConstType CallStatement::getTypeForExp(SharedConstExp e) const
{
    assert(e != nullptr);

    // The defines "cache" what the destination proc is defining
    std::shared_ptr<const Assignment> asgn = m_defines.findOnLeft(e);

    if (asgn != nullptr) {
        return asgn->getType();
    }

    if (e->isPC()) {
        // Special case: just return void*
        return PointerType::get(VoidType::get());
    }

    return VoidType::get();
}


SharedType CallStatement::getTypeForExp(SharedExp e)
{
    // The defines "cache" what the destination proc is defining
    std::shared_ptr<Assignment> asgn = m_defines.findOnLeft(e);

    if (asgn != nullptr) {
        return asgn->getType();
    }

    if (e->isPC()) {
        // Special case: just return void*
        return PointerType::get(VoidType::get());
    }

    return VoidType::get();
}


void CallStatement::setTypeForExp(SharedExp e, SharedType ty)
{
    std::shared_ptr<Assignment> asgn = m_defines.findOnLeft(e);

    if (asgn != nullptr) {
        return asgn->setType(ty);
    }

    // See if it is in our reaching definitions
    SharedExp ref = m_defCol.findDefFor(e);

    if ((ref == nullptr) || !ref->isSubscript()) {
        return;
    }

    SharedStmt def = ref->access<RefExp>()->getDef();

    if (def == nullptr) {
        return;
    }

    def->setTypeForExp(e, ty);
}


void CallStatement::print(OStream &os) const
{
    os << qSetFieldWidth(4) << m_number << qSetFieldWidth(0) << " ";

    // Define(s), if any
    if (!m_defines.empty()) {
        os << "{ ";

        bool first = true;

        for (const SharedStmt &def : m_defines) {
            assert(def->isAssignment());
            std::shared_ptr<const Assignment> asgn = def->as<const Assignment>();

            if (first) {
                first = false;
            }
            else {
                os << ", ";
            }

            os << "*" << asgn->getType() << "* " << asgn->getLeft();

            if (asgn->isAssign()) {
                os << " := " << asgn->as<const Assign>()->getRight();
            }
        }

        os << " } := ";
    }
    else if (isChildless()) {
        os << "<all> := ";
    }

    os << "CALL ";

    if (m_procDest) {
        os << m_procDest->getName();
    }
    else if (m_dest->isIntConst()) {
        os << getFixedDest();
    }
    else {
        m_dest->print(os); // Could still be an expression
    }

    // Print the actual arguments of the call
    if (isChildless()) {
        os << "(<all>)";
    }
    else {
        os << "(\n";

        for (const SharedStmt &arg : m_arguments) {
            os << "                ";
            std::shared_ptr<const Assignment> a = std::dynamic_pointer_cast<const Assignment>(arg);
            if (a) {
                a->printCompact(os);
            }
            os << "\n";
        }

        os << "              )";
    }

    // Collected reaching definitions
    os << "\n              Reaching definitions: ";
    m_defCol.print(os);
    os << "\n              Live variables: ";
    m_useCol.print(os);
}


void CallStatement::setArguments(const StatementList &args)
{
    m_arguments.clear();
    m_arguments.append(args);

    for (SharedStmt arg : m_arguments) {
        arg->setProc(m_proc);
        arg->setFragment(m_fragment);
        arg->setNumber(m_number);
    }
}


void CallStatement::setSigArguments()
{
    if (m_signature) {
        return; // Already done
    }

    if (m_procDest == nullptr) {
        // FIXME: Need to check this
        return;
    }

    // Clone here because each call to procDest could have a different signature,
    // modified by doEllipsisProcessing()
    m_signature = m_procDest->getSignature()->clone();
    m_procDest->addCaller(shared_from_this()->as<CallStatement>());

    if (!m_procDest->isLib()) {
        return; // Using dataflow analysis now
    }


    m_arguments.clear();

    const int n = m_signature->getNumParams();
    for (int i = 0; i < n; i++) {
        SharedExp e = m_signature->getArgumentExp(i);
        assert(e);
        auto l = std::dynamic_pointer_cast<Location>(e);

        if (l) {
            l->setProc(m_proc); // Needed?
        }

        std::shared_ptr<Assign> asgn = std::make_shared<Assign>(
            m_signature->getParamType(i)->clone(), e->clone(), e->clone());

        asgn->setProc(m_proc);
        asgn->setFragment(m_fragment);
        asgn->setNumber(m_number);

        m_arguments.append(asgn);
    }

    // initialize returns
    // FIXME: anything needed here?
}


void CallStatement::updateArguments()
{
    /*
     * If this is a library call, source = signature
     *          else if there is a callee return, source = callee parameters
     *          else
     *            if a forced callee signature, source = signature
     *            else source is def collector in this call.
     *          oldArguments = arguments
     *          clear arguments
     *          for each arg lhs in source
     *                  if exists in oldArguments, leave alone
     *                  else if not filtered append assignment lhs=lhs to oldarguments
     *          for each argument as in oldArguments in reverse order
     *                  lhs = as->getLeft
     *                  if (lhs does not exist in source) continue
     *                  if filterParams(lhs) continue
     *                  insert as into arguments, considering sig->argumentCompare
     */

    // Do not delete statements in m_arguments since they are preserved by oldArguments
    StatementList oldArguments(m_arguments);
    m_arguments.clear();

    auto sig = m_proc->getSignature();
    // Ensure everything
    //  - in the callee's signature (if this is a library call),
    //  - or the callee parameters (if available),
    //  - or the def collector if not,
    // exists in oldArguments
    ArgSourceProvider asp(this);
    SharedExp loc;

    while ((loc = asp.nextArgLoc()) != nullptr) {
        if (!m_proc->canBeParam(loc)) {
            continue;
        }

        if (!oldArguments.existsOnLeft(loc)) {
            // Check if the location is renamable. If not, localising won't work, since it relies on
            // definitions collected in the call, and you just get m[...]{-} even if there are
            // definitions.
            SharedExp rhs;

            if (m_proc->canRename(loc)) {
                rhs = asp.localise(loc->clone());
            }
            else {
                rhs = loc->clone();
            }

            SharedType ty = asp.curType(loc);
            std::shared_ptr<Assign> asgn(new Assign(ty, loc->clone(), rhs));

            // Give the assign the same statement number as the call (for now)
            asgn->setNumber(m_number);
            // as->setParent(this);
            asgn->setProc(m_proc);
            asgn->setFragment(m_fragment);

            oldArguments.append(asgn);
        }
    }

    for (SharedStmt oldArg : oldArguments) {
        // Make sure the LHS is still in the callee signature / callee parameters / use collector
        std::shared_ptr<Assign> asgn = oldArg->as<Assign>();
        SharedExp lhs                = asgn->getLeft();

        if (!asp.exists(lhs)) {
            continue;
        }

        if (!m_proc->canBeParam(lhs)) {
            // Filtered out: delete it
            continue;
        }

        m_arguments.append(asgn);
    }
}


SharedExp CallStatement::getArgumentExp(int i) const
{
    assert(Util::inRange(i, 0, getNumArguments()));

    // stmt = m_arguments[i]
    const Assign *asgn = dynamic_cast<const Assign *>(std::next(m_arguments.begin(), i)->get());
    return asgn ? asgn->getRight() : nullptr;
}


void CallStatement::setArgumentExp(int i, SharedExp e)
{
    assert(Util::inRange(i, 0, getNumArguments()));

    SharedStmt &stmt = *std::next(m_arguments.begin(), i);
    assert(stmt->isAssign());
    stmt->as<Assign>()->setRight(e->clone());
}


int CallStatement::getNumArguments() const
{
    return m_arguments.size();
}


void CallStatement::setNumArguments(int n)
{
    const int oldSize = getNumArguments();

    if (oldSize > n) {
        m_arguments.resize(n);
    }

    // MVE: check if these need extra propagation
    for (int i = oldSize; i < n; i++) {
        SharedExp a   = m_procDest->getSignature()->getArgumentExp(i);
        SharedType ty = m_procDest->getSignature()->getParamType(i);

        if (ty == nullptr && oldSize != 0) {
            ty = m_procDest->getSignature()->getParamType(oldSize - 1);
        }

        if (ty == nullptr) {
            ty = VoidType::get();
        }

        std::shared_ptr<Assign> asgn(new Assign(ty, a->clone(), a->clone()));
        asgn->setProc(m_proc);
        asgn->setFragment(m_fragment);
        m_arguments.append(asgn);
    }
}


void CallStatement::removeArgument(int i)
{
    assert(Util::inRange(i, 0, getNumArguments()));
    m_arguments.erase(std::next(m_arguments.begin(), i));
}


SharedType CallStatement::getArgumentType(int i) const
{
    assert(Util::inRange(i, 0, getNumArguments()));
    StatementList::const_iterator aa = std::next(m_arguments.begin(), i);
    assert((*aa)->isAssign());
    return (*aa)->as<Assign>()->getType();
}


void CallStatement::setArgumentType(int i, SharedType ty)
{
    assert(Util::inRange(i, 0, getNumArguments()));
    StatementList::const_iterator aa = std::next(m_arguments.begin(), i);
    assert((*aa)->isAssign());
    (*aa)->as<Assign>()->setType(ty);
}


void CallStatement::eliminateDuplicateArgs()
{
    LocationSet ls;

    for (StatementList::iterator it = m_arguments.begin(); it != m_arguments.end();) {
        SharedExp lhs = (*it)->as<const Assignment>()->getLeft();

        if (ls.contains(lhs)) {
            // This is a duplicate
            it = m_arguments.erase(it);
            continue;
        }

        ls.insert(lhs);
        ++it;
    }
}


void CallStatement::setDestProc(Function *dest)
{
    assert(dest != nullptr);
    m_procDest = dest;
}


Function *CallStatement::getDestProc()
{
    return m_procDest;
}


const Function *CallStatement::getDestProc() const
{
    return m_procDest;
}


void CallStatement::setReturnAfterCall(bool b)
{
    m_returnAfterCall = b;
}


bool CallStatement::isReturnAfterCall() const
{
    return m_returnAfterCall;
}


bool CallStatement::isChildless() const
{
    if (m_procDest == nullptr) {
        return true;
    }
    else if (m_procDest->isLib()) {
        return false;
    }

    // Early in the decompile process, recursive calls are treated as childless,
    // so they use and define all
    if (static_cast<UserProc *>(m_procDest)->isEarlyRecursive()) {
        return true;
    }

    return m_calleeReturn == nullptr;
}


bool CallStatement::isCallToMemOffset() const
{
    return m_dest && m_dest->isMemOf() && m_dest->getSubExp1()->isIntConst();
}


void CallStatement::addDefine(const std::shared_ptr<ImplicitAssign> &asgn)
{
    m_defines.append(asgn);
}


bool CallStatement::removeDefine(SharedExp e)
{
    for (StatementList::iterator ss = m_defines.begin(); ss != m_defines.end(); ++ss) {
        SharedStmt s = *ss;

        assert(s->isAssignment());
        if (*s->as<Assignment>()->getLeft() == *e) {
            m_defines.erase(ss);
            return true;
        }
    }

    LOG_WARN("Could not remove define %1 from call %2", e, shared_from_this());
    return false;
}


void CallStatement::setDefines(const StatementList &defines)
{
    if (!m_defines.empty()) {
        for (SharedConstStmt stmt : defines) {
            Q_UNUSED(stmt);
            assert(std::find(m_defines.begin(), m_defines.end(), stmt) == m_defines.end());
        }

        m_defines.clear();
    }

    m_defines = defines;
}


SharedExp CallStatement::findDefFor(SharedExp e) const
{
    assert(e != nullptr);
    return m_defCol.findDefFor(e);
}


std::unique_ptr<StatementList> CallStatement::calcResults() const
{
    std::unique_ptr<StatementList> result(new StatementList);

    if (m_procDest) {
        const SharedExp rsp = Location::regOf(Util::getStackRegisterIndex(m_proc->getProg()));

        for (SharedStmt dd : m_defines) {
            SharedExp lhs = dd->as<Assignment>()->getLeft();

            // The stack pointer is allowed as a define, so remove it here as a special case non
            // result
            if (*lhs == *rsp) {
                continue;
            }

            if (m_useCol.hasUse(lhs)) {
                result->append(dd);
            }
        }
    }
    else {
        // For a call with no destination at this late stage, use every variable live at the call
        // except for the stack pointer register. Needs to be sorted.
        const auto sig  = m_proc->getSignature();
        const RegNum sp = sig->getStackRegister();

        for (SharedExp loc : m_useCol) {
            if (!m_proc->canBeReturn(loc)) {
                continue; // Ignore filtered locations
            }
            else if (loc->isRegN(sp)) {
                continue; // Ignore the stack pointer
            }

            result->append(std::make_shared<ImplicitAssign>(loc));
        }

        result->sort([sig](const SharedConstStmt &left, const SharedConstStmt &right) {
            return sig->returnCompare(*left->as<const Assignment>(),
                                      *right->as<const Assignment>());
        });
    }

    return result;
}


SharedExp CallStatement::getProven(SharedExp e)
{
    assert(e != nullptr);

    if (!m_procDest) {
        return nullptr;
    }

    return m_procDest->getProven(e);
}


SharedExp CallStatement::localiseExp(SharedExp e)
{
    Localiser l(this);
    e = e->clone()->acceptModifier(&l);

    return e;
}


void CallStatement::localiseComp(SharedExp e)
{
    if (e->isMemOf()) {
        e->setSubExp1(localiseExp(e->getSubExp1()));
    }
}


SharedExp CallStatement::bypassRef(const std::shared_ptr<RefExp> &r, bool &changed)
{
    SharedExp base = r->getSubExp1();
    SharedExp proven;

    changed = false;

    if (m_procDest && m_procDest->isLib()) {
        auto sig = m_procDest->getSignature();
        proven   = sig->getProven(base);

        if (proven == nullptr) { // Not (known to be) preserved
            if (sig->findReturn(base) != -1) {
                return r->shared_from_this(); // Definately defined, it's the return
            }

            // Otherwise, not all that sure. Assume that library calls pass things like local
            // variables
        }
    }
    else {
        // Was using the defines to decide if something is preserved, but consider sp+4 for stack
        // based machines. Have to use the proven information for the callee (if any)
        if (m_procDest == nullptr) {
            return r->shared_from_this(); // Childless callees transmit nothing
        }

        // FIXME: temporary HACK! Ignores alias issues.
        if (!m_procDest->isLib() &&
            static_cast<const UserProc *>(m_procDest)->isLocalOrParamPattern(base)) {
            SharedExp ret = localiseExp(base->clone()); // Assume that it is proved as preserved
            changed       = true;

            LOG_VERBOSE2("%1 allowed to bypass call statement %2 ignoring aliasing; result %3",
                         base, m_number, ret);
            return ret;
        }

        proven = m_procDest->getProven(base); // e.g. r28+4
    }

    if (proven == nullptr) {
        return r->shared_from_this(); // Can't bypass, since nothing proven
    }

    SharedExp to = localiseExp(base); // e.g. r28{17}
    assert(to);
    proven = proven->clone(); // Don't modify the expressions in destProc->proven!
    proven = proven->searchReplaceAll(*base, to, changed); // e.g. r28{17} + 4

    if (changed) {
        LOG_VERBOSE2("Replacing %1 with %2", r, proven);
    }

    return proven;
}


bool CallStatement::doEllipsisProcessing()
{
    if (!m_procDest || !m_signature) {
        return false;
    }
    else if (!m_signature->hasEllipsis()) {
        return doObjCEllipsisProcessing(nullptr);
    }

    // functions like printf almost always have too many args
    const QString calleeName = m_procDest->getName();
    int formatstrIdx         = -1;

    if (getNumArguments() > 0 && (calleeName == "printf" || calleeName == "scanf")) {
        formatstrIdx = 0;
    }
    else if (getNumArguments() > 1 &&
             (calleeName == "sprintf" || calleeName == "fprintf" || calleeName == "sscanf")) {
        formatstrIdx = 1;
    }
    else if (getNumArguments() > 0) {
        formatstrIdx = getNumArguments() - 1;
    }

    if (!Util::inRange(formatstrIdx, 0, getNumArguments())) {
        return false;
    }

    LOG_VERBOSE("Ellipsis processing for %1", calleeName);

    QString formatStr;
    SharedExp formatExp = getArgumentExp(formatstrIdx);

    // We sometimes see a[m[blah{...}]]
    if (formatExp->isAddrOf()) {
        formatExp = formatExp->getSubExp1();

        if (formatExp->isSubscript()) {
            formatExp = formatExp->getSubExp1();
        }

        if (formatExp->isMemOf()) {
            formatExp = formatExp->getSubExp1();
        }
    }

    if (formatExp->isSubscript()) {
        // Maybe it's defined to be a Const string
        SharedStmt def = formatExp->access<RefExp>()->getDef();

        if (def == nullptr) {
            return false; // Not all nullptr refs get converted to implicits
        }

        if (def->isAssign()) {
            // This would be unusual; propagation would normally take care of this
            const SharedExp rhs = def->as<Assign>()->getRight();
            if (!rhs || !rhs->isStrConst()) {
                return false;
            }

            formatStr = rhs->access<Const>()->getStr();
        }
        else if (def->isPhi()) {
            // More likely. Example: switch_gcc. Only need ONE candidate format string
            std::shared_ptr<PhiAssign> pa = def->as<PhiAssign>();

            for (const std::shared_ptr<RefExp> &v : *pa) {
                def = v->getDef();

                if (!def || !def->isAssign()) {
                    continue;
                }

                SharedExp rhs = def->as<Assign>()->getRight();

                if (!rhs || !rhs->isStrConst()) {
                    continue;
                }

                formatStr = rhs->access<Const>()->getStr();
                break;
            }

            if (formatStr.isEmpty()) {
                return false;
            }
        }
        else {
            return false;
        }
    }
    else if (formatExp->isStrConst()) {
        formatStr = formatExp->access<Const>()->getStr();
    }
    else {
        return false;
    }

    if (doObjCEllipsisProcessing(formatStr)) {
        return true;
    }

    // actually have to parse it
    const bool isScanf = calleeName.contains("scanf");
    const int n        = parseFmtStr(formatStr, isScanf);

    assert(n >= 0);

    setNumArguments((formatstrIdx + 1) + n);
    m_signature->setHasEllipsis(false); // So we don't do this again

    return true;
}


int CallStatement::parseFmtStr(const QString &fmtStr, bool isScanf)
{
    // clang-format off
    const QRegularExpression re(
        "%("                                 // '%' followed by either ...
          "(?<flags>[+-0 #]*)"               //   flags (opt), ...
          "(?<width>([0-9\\*]*))"            //   width (opt, digits or *), ...
          "(?<prec>((\\.[0-9\\*]+)?))"       //   precision (opt, dot followed by digits or *), ...
          "(?<mod>[hlLzjt]*)"                //   size modifier (opt), and ...
          "(?<spec>([%diufFeEgGxXaAoscpn]))" //   the actual specifier, ...
        "|"                                  // or ...
          "(?<list>l?\\[\\^?[^\\]]+\\])"     //   a list of characters to (not) match (scanf only)
        ")");
    // clang-format on

    auto it = re.globalMatch(fmtStr);
    int n   = 0;

    while (it.hasNext()) {
        auto match = it.next();

        const QString width = match.captured("width");
        const QString prec  = match.captured("prec");
        const QString mod   = match.captured("mod");
        const QString list  = match.captured("list");
        const QString spec  = match.captured("spec");

        if (isScanf && list != "") {
            if (list[0] == "l") {
                addSigParam(ArrayType::get(IntegerType::get(16, Sign::Signed)), true); // wchar_t
                n++;
            }
            else {
                addSigParam(ArrayType::get(CharType::get()), true);
                n++;
            }

            continue;
        }

        if (isScanf && width.startsWith("*")) {
            // We have something like %*3d. No output parameter.
            continue;
        }

        if (width == "*") {
            addSigParam(IntegerType::get(32, Sign::Signed), false);
            n++;
        }

        if (prec == ".*") {
            addSigParam(IntegerType::get(32, Sign::Signed), false);
            n++;
        }

        switch (spec[0].toLatin1()) {
        case 'd':
        case 'i':
            if (mod == "") {
                addSigParam(IntegerType::get(32, Sign::Signed), isScanf);
                n++;
            }
            else if (mod == "hh") {
                addSigParam(IntegerType::get(8, Sign::Signed), isScanf);
                n++;
            }
            else if (mod == "h") {
                addSigParam(IntegerType::get(16, Sign::Signed), isScanf);
                n++;
            }
            else if (mod == "l") {
                addSigParam(IntegerType::get(32, Sign::Signed), isScanf);
                n++;
            }
            else if (mod == "ll") {
                addSigParam(IntegerType::get(64, Sign::Signed), isScanf);
                n++;
            }
            else if (mod == "j") {
                addSigParam(IntegerType::get(32, Sign::Signed), isScanf);
                n++;
            }
            else if (mod == "z") { // size_t
                addSigParam(IntegerType::get(STD_SIZE, Sign::Unsigned), isScanf);
                n++;
            }
            else if (mod == "t") { // ptrdiff_t
                addSigParam(IntegerType::get(STD_SIZE, Sign::Signed), isScanf);
                n++;
            }
            break;

        case 'X':
            if (isScanf) {
                break; // not valid for scanf
            }
            // fallthrough
        case 'u':
        case 'o':
        case 'x':
            if (mod == "") {
                addSigParam(IntegerType::get(32, Sign::Unsigned), isScanf);
                n++;
            }
            else if (mod == "hh") {
                addSigParam(IntegerType::get(8, Sign::Unsigned), isScanf);
                n++;
            }
            else if (mod == "h") {
                addSigParam(IntegerType::get(16, Sign::Unsigned), isScanf);
                n++;
            }
            else if (mod == "l") {
                addSigParam(IntegerType::get(32, Sign::Unsigned), isScanf);
                n++;
            }
            else if (mod == "ll") {
                addSigParam(IntegerType::get(64, Sign::Unsigned), isScanf);
                n++;
            }
            else if (mod == "j") {
                addSigParam(IntegerType::get(32, Sign::Unsigned), isScanf);
                n++;
            }
            else if (mod == "z") { // size_t
                addSigParam(IntegerType::get(STD_SIZE, Sign::Unsigned), isScanf);
                n++;
            }
            else if (mod == "t") { // ptrdiff_t
                addSigParam(IntegerType::get(STD_SIZE, Sign::Signed), isScanf);
                n++;
            }
            break;

        case 'A':
        case 'E':
        case 'F':
        case 'G':
            if (isScanf) {
                break; // these are not valid for scanf
            }
            // fallthrough
        case 'a':
        case 'f':
        case 'e':
        case 'g':
            if (mod == "") {
                addSigParam(FloatType::get(isScanf ? 32 : 64), isScanf);
                n++;
            }
            else if (mod == "L") {
                addSigParam(FloatType::get(128), isScanf);
                n++;
            }
            else if (mod == "l" && isScanf) {
                addSigParam(FloatType::get(64), true);
                n++;
            }
            break;

        case 'c':
            if (mod == "") {
                addSigParam(CharType::get(), isScanf);
                n++;
            }
            else if (mod == "l") {
                addSigParam(IntegerType::get(16, Sign::Signed), isScanf);
                n++;
            }
            break;

        case 's':
            if (mod == "") {
                addSigParam(ArrayType::get(CharType::get()), true);
                n++;
            }
            else if (mod == "l") {
                addSigParam(ArrayType::get(IntegerType::get(16, Sign::Signed)), true);
                n++;
            }
            break;

        case 'p':
            if (mod == "") {
                addSigParam(PointerType::get(VoidType::get()), isScanf);
                n++;
            }
            break;

        case 'n':
            if (mod == "") {
                addSigParam(IntegerType::get(32, Sign::Signed), true);
                n++;
            }
            else if (mod == "hh") {
                addSigParam(IntegerType::get(8, Sign::Signed), true);
                n++;
            }
            else if (mod == "h") {
                addSigParam(IntegerType::get(16, Sign::Signed), true);
                n++;
            }
            else if (mod == "l") {
                addSigParam(IntegerType::get(32, Sign::Signed), true);
                n++;
            }
            else if (mod == "ll") {
                addSigParam(IntegerType::get(64, Sign::Signed), true);
                n++;
            }
            else if (mod == "j") {
                addSigParam(IntegerType::get(32, Sign::Signed), true);
                n++;
            }
            else if (mod == "z") { // size_t
                addSigParam(IntegerType::get(STD_SIZE, Sign::Unsigned), true);
                n++;
            }
            else if (mod == "t") { // ptrdiff_t
                addSigParam(IntegerType::get(STD_SIZE, Sign::Signed), true);
                n++;
            }
            break;

        case '%': break;
        }
    }

    return n;
}


bool CallStatement::tryConvertToDirect()
{
    if (!m_isComputed) {
        return false;
    }

    SharedExp e = m_dest;

    if (m_dest->isSubscript()) {
        SharedStmt def = e->access<RefExp>()->getDef();

        if (def && !def->isImplicit()) {
            return false; // If an already defined global, don't convert
        }

        e = e->getSubExp1();
    }

    if (e->isArrayIndex() && e->getSubExp2()->isIntConst() &&
        (e->access<Const, 2>()->getInt() == 0)) {
        e = e->getSubExp1();
    }

    // Can actually have name{0}[0]{0} !!
    if (e->isSubscript()) {
        e = e->access<RefExp, 1>();
    }

    Address callDest = Address::INVALID;

    if (e->isIntConst()) {
        // Just convert it to a direct call!
        callDest = e->access<Const>()->getAddr();
    }
    else if (e->isMemOf()) {
        // It might be a global that has not been processed yet
        SharedExp sub = e->getSubExp1();

        if (sub->isIntConst()) {
            // m[K]: convert it to a global right here
            Address u = Address(sub->access<Const>()->getInt());
            m_proc->getProg()->markGlobalUsed(u);
            QString name = m_proc->getProg()->newGlobalName(u);
            e            = Location::global(name, m_proc);
            m_dest       = RefExp::get(e, nullptr);
        }
    }

    Prog *prog = m_proc->getProg();
    QString calleeName;

    if (e->isGlobal()) {
        BinaryImage *image = prog->getBinaryFile()->getImage();

        calleeName      = e->access<Const, 1>()->getStr();
        Address gloAddr = prog->getGlobalAddrByName(calleeName);
        if (!image->readNativeAddr4(gloAddr, callDest)) {
            return false;
        }
    }

    // Note: If gloAddr is in BSS, dest will be 0 (since we do not track
    // assignments to global variables yet), which is usually outside of text limits.
    if (!Util::inRange(callDest, prog->getLimitTextLow(), prog->getLimitTextHigh())) {
        return false; // Not a valid proc pointer
    }

    Function *p = nullptr;
    if (e->isGlobal()) {
        p = prog->getFunctionByName(calleeName);
    }

    const bool isNewProc = p == nullptr;
    if (isNewProc) {
        p          = prog->getOrCreateFunction(callDest);
        calleeName = p->getName();
    }

    assert(p != nullptr);
    LOG_VERBOSE("Found call to %1 function '%2' by converting indirect call to direct call",
                (isNewProc ? "new" : "existing"), p->getName());

    // we need to:
    // 1) replace the current return set with the return set of the new procDest
    // 2) call fixCallBypass (now CallAndPhiFix) on the enclosing procedure
    // 3) fix the arguments (this will only affect the implicit arguments, the regular arguments
    //    should be empty at this point)
    // 3a replace current arguments with those of the new proc
    // 3b copy the signature from the new proc
    // 4) change this to a non-indirect call
    m_procDest = p;
    auto sig   = p->getSignature();
    // m_dest is currently still global5{-}, but we may as well make it a constant now, since that's
    // how it will be treated now
    m_dest = Const::get(callDest);

    // 1
    // 2
    PassManager::get()->executePass(PassID::CallAndPhiFix, m_proc);

    // 3
    // 3a Do the same with the regular arguments
    m_arguments.clear();

    for (int i = 0; i < sig->getNumParams(); i++) {
        SharedExp a = sig->getParamExp(i);
        std::shared_ptr<Assign> asgn(new Assign(VoidType::get(), a->clone(), a->clone()));
        asgn->setProc(m_proc);
        asgn->setFragment(m_fragment);
        m_arguments.append(asgn);
    }

    // LOG_STREAM() << "Step 3a: arguments now: ";
    // StatementList::iterator xx; for (xx = arguments.begin(); xx != arguments.end(); ++xx) {
    //        ((Assignment*)*xx)->printCompact(LOG_STREAM()); LOG_STREAM() << ", ";
    // } LOG_STREAM() << "\n";
    // implicitArguments = newimpargs;
    // assert((int)implicitArguments.size() == sig->getNumImplicitParams());

    // 3b
    m_signature = p->getSignature()->clone();

    // 4
    m_isComputed = false;
    assert(m_fragment->isType(FragType::CompCall));
    m_fragment->setType(FragType::Call);
    m_proc->addCallee(m_procDest);

    LOG_VERBOSE("Result of convertToDirect: true");
    return true;
}


bool CallStatement::doObjCEllipsisProcessing(const QString &formatStr)
{
    if (!m_procDest) {
        return false;
    }

    const QString name = m_procDest->getName();

    if (name == "objc_msgSend") {
        if (!formatStr.isEmpty()) {
            int format = getNumArguments() - 1;
            int n      = 1;
            int p      = 0;

            while ((p = formatStr.indexOf(':', p)) != -1) {
                p++; // Point past the :
                n++;
                addSigParam(PointerType::get(VoidType::get()), false);
            }

            setNumArguments(format + n);
            m_signature->setHasEllipsis(false); // So we don't do this again
            return true;
        }
        else {
            bool change = false;
            LOG_MSG("%1", shared_from_this());

            for (int i = 0; i < getNumArguments(); i++) {
                SharedExp e   = getArgumentExp(i);
                SharedType ty = getArgumentType(i);
                LOG_MSG("arg %1 e: %2 ty: %3", i, e, ty);

                if ((!ty->isPointer() || !ty->as<PointerType>()->getPointsTo()->isChar()) &&
                    e->isIntConst()) {
                    const Address addr = Address(e->access<Const>()->getInt());
                    LOG_MSG("Addr: %1", addr);

                    if (m_procDest->getProg()->isInStringsSection(addr)) {
                        LOG_MSG("Making arg %1 of call c*", i);
                        setArgumentType(i, PointerType::get(CharType::get()));
                        change = true;
                    }
                }
            }

            return change;
        }
    }

    return false;
}


void CallStatement::addSigParam(SharedType ty, bool isScanf)
{
    if (isScanf) {
        ty = PointerType::get(ty);
    }

    m_signature->addParameter(nullptr, ty);
    SharedExp paramExp = m_signature->getParamExp(m_signature->getNumParams() - 1);

    LOG_VERBOSE("EllipsisProcessing: adding parameter %1 of type %2", paramExp, ty->getCtype());

    if (static_cast<int>(m_arguments.size()) < m_signature->getNumParams()) {
        m_arguments.append(std::shared_ptr<Assign>(makeArgAssign(ty, paramExp)));
    }
}


std::shared_ptr<Assign> CallStatement::makeArgAssign(SharedType ty, SharedExp e)
{
    SharedExp lhs = e->clone();

    localiseComp(lhs); // Localise the components of lhs (if needed)
    SharedExp rhs = localiseExp(e->clone());
    std::shared_ptr<Assign> asgn(new Assign(ty, lhs, rhs));
    asgn->setProc(m_proc);
    asgn->setFragment(m_fragment);
    // It may need implicit converting (e.g. sp{-} -> sp{0})
    ProcCFG *cfg = m_proc->getCFG();

    if (cfg->isImplicitsDone()) {
        ImplicitConverter ic(cfg);
        StmtImplicitConverter sm(&ic, cfg);
        asgn->accept(&sm);
    }

    return asgn;
}
