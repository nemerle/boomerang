#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "CCodeGenerator.h"

#include "boomerang/core/Project.h"
#include "boomerang/core/Settings.h"
#include "boomerang/db/BasicBlock.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/module/Module.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/passes/PassManager.h"
#include "boomerang/ssl/RTL.h"
#include "boomerang/ssl/Register.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/exp/RefExp.h"
#include "boomerang/ssl/exp/Ternary.h"
#include "boomerang/ssl/exp/TypedExp.h"
#include "boomerang/ssl/statements/BoolAssign.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/CaseStatement.h"
#include "boomerang/ssl/statements/ReturnStatement.h"
#include "boomerang/ssl/type/ArrayType.h"
#include "boomerang/ssl/type/FloatType.h"
#include "boomerang/ssl/type/FuncType.h"
#include "boomerang/ssl/type/IntegerType.h"
#include "boomerang/ssl/type/PointerType.h"
#include "boomerang/util/ByteUtil.h"
#include "boomerang/util/log/Log.h"


CCodeGenerator::CCodeGenerator(Project *project)
    : ICodeGenerator(project)
{
}


void CCodeGenerator::generateCode(const Prog *prog, Module *cluster, UserProc *proc,
                                  bool /*intermixRTL*/)
{
    const bool generate_all = cluster == nullptr || cluster == prog->getRootModule();
    bool all_procedures     = (proc == nullptr);

    // First declare prototypes
    for (const auto &module : prog->getModuleList()) {
        for (Function *func : *module) {
            if (func->isLib()) {
                continue;
            }

            UserProc *_proc = static_cast<UserProc *>(func);
            addPrototype(_proc); // May be the wrong signature if _proc has ellipsis
        }
        appendLine("");
    }

    if (generate_all) {
        if (proc == nullptr) {
            const bool global = !prog->getGlobals().empty();

            for (auto &elem : prog->getGlobals()) {
                // Check for an initial value
                SharedExp e = elem->getInitialValue();
                // if (e) {
                addGlobal(elem->getName(), elem->getType(), e);
            }

            if (global) {
                print(prog->getRootModule());
            }
        }

        appendLine(""); // Separate prototype(s) from first proc
        print(prog->getRootModule());
    }

    for (const auto &module : prog->getModuleList()) {
        if (!generate_all && (module.get() != cluster)) {
            continue;
        }

        for (Function *func : *module) {
            if (func->isLib()) {
                continue;
            }

            UserProc *_proc = static_cast<UserProc *>(func);

            if (!_proc->isDecoded()) {
                continue;
            }

            if (!all_procedures && (proc != _proc)) {
                continue;
            }

            generateCode(_proc);
            print(module.get());
        }
    }
}


void CCodeGenerator::addAssignmentStatement(const Assign *asgn)
{
    // Gerard: shouldn't these  3 types of statements be removed earlier?
    if (asgn->getLeft()->isPC()) {
        return; // Never want to see assignments to %PC
    }

    SharedExp result;

    if (asgn->getRight()->search(Terminal(opPC), result)) { // Gerard: what's this?
        return;
    }

    // ok I want this now
    // if (asgn->getLeft()->isFlags())
    //    return;

    QString tgt;
    OStream ost(&tgt);
    indent(ost, m_indent);

    SharedType asgnType = asgn->getType();
    SharedExp lhs       = asgn->getLeft();
    SharedExp rhs       = asgn->getRight();

    if (*lhs == *rhs) {
        return; // never want to see a = a;
    }

    if (lhs->isMemOf() && asgnType && !asgnType->isVoid()) {
        appendExp(ost, TypedExp(asgnType, lhs), OpPrec::Assign);
    }
    else if (lhs->isGlobal() && asgn->getType()->isArray()) {
        appendExp(ost, Binary(opArrayIndex, lhs, Const::get(0)), OpPrec::Assign);
    }
    else if ((lhs->getOper() == opAt) && lhs->getSubExp2()->isIntConst() &&
             lhs->getSubExp3()->isIntConst()) {
        // exp1@[n:m] := rhs -> exp1 = exp1 & mask | rhs << m  where mask = ~((1 << m-n+1)-1)
        SharedExp exp1 = lhs->getSubExp1();
        int n          = lhs->access<Const, 2>()->getInt();
        int m          = lhs->access<Const, 3>()->getInt();
        appendExp(ost, *exp1, OpPrec::Assign);
        ost << " = ";

        // MSVC winges without most of these parentheses
        int mask = ~(((1 << (m - n + 1)) - 1) << m);

        // clang-format off
        rhs = Binary::get(opBitAnd,
                          exp1,
                          Binary::get(opBitOr,
                                      Const::get(mask),
                                      Binary::get(opShL, rhs, Const::get(m))));
        // clang-format on
        rhs = rhs->simplify();

        appendExp(ost, *rhs, OpPrec::Assign);
        ost << ";";
        appendLine(tgt);
        return;
    }
    else {
        appendExp(ost, *lhs, OpPrec::Assign); // Ordinary LHS
    }

    // C has special syntax for this, eg += and ++
    // however it's not always acceptable for assigns to m[] (?)
    bool useIncrement = false; // use ++ / --
    bool useShortForm = false; // use += / -=

    if ((rhs->getOper() == opPlus || rhs->getOper() == opMinus) && (*rhs->getSubExp1() == *lhs)) {
        // we now have something like a = a + b -> shorten it
        useShortForm = true;

        SharedConstExp b = rhs->getSubExp2();
        if (b->isIntConst()) {
            if (b->access<const Const>()->getInt() == 1) {
                useIncrement = true;
            }
            else if (asgn->getType()->isPointer()) {
                // add ptr, 4 for 32 bit pointers in assembly is ptr++ in C code
                const Type::Size ptrSize = asgn->getType()->as<PointerType>()->getSize();
                if (ptrSize == (Type::Size)rhs->access<const Const, 2>()->getInt() * 8) {
                    useIncrement = true;
                }
            }
        }
    }

    if (useIncrement) {
        if (rhs->getOper() == opPlus) {
            ost << "++;";
        }
        else {
            ost << "--;";
        }
    }
    else if (useShortForm) {
        if (rhs->getOper() == opPlus) {
            ost << " += ";
        }
        else {
            ost << " -= ";
        }

        appendExp(ost, *rhs->getSubExp2(), OpPrec::Assign);
        ost << ";";
    }
    else {
        ost << " = ";
        appendExp(ost, *rhs, OpPrec::Assign);
        ost << ";";
    }

    appendLine(tgt);
}


void CCodeGenerator::addCallStatement(const Function *proc, const QString &name,
                                      const StatementList &args, const StatementList &results)
{
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);

    if (!results.empty()) {
        // FIXME: Needs changing if more than one real result (return a struct)
        SharedConstExp firstRet = (static_cast<const Assignment *>(*results.begin()))->getLeft();
        appendExp(s, *firstRet, OpPrec::Assign);
        s << " = ";
    }

    s << name << "(";
    bool first = true;
    int n      = 0;

    for (StatementList::const_iterator ss = args.begin(); ss != args.end(); ++ss, ++n) {
        if (first) {
            first = false;
        }
        else {
            s << ", ";
        }

        assert((*ss)->isAssignment() && dynamic_cast<Assignment *>(*ss) != nullptr);

        Assignment *arg_assign = static_cast<Assignment *>(*ss);
        SharedType t           = arg_assign->getType();
        SharedExp as_arg       = arg_assign->getRight();
        bool ok                = true;

        if (as_arg->isIntConst() && t && t->isPointer() &&
            t->as<PointerType>()->getPointsTo()->isFunc()) {
            Function *p = proc->getProg()->getFunctionByAddr(as_arg->access<Const>()->getAddr());

            if (p) {
                s << p->getName();
                ok = false;
            }
        }

        if (ok) {
            appendExp(s, *as_arg, OpPrec::Comma);
        }
    }

    s << ");";

    if (results.size() > 1) {
        first = true;
        s << " /* Warning: also results in ";

        for (auto ss = std::next(results.begin()); ss != results.end(); ++ss) {
            if (first) {
                first = false;
            }
            else {
                s << ", ";
            }

            const Assignment *assign = dynamic_cast<const Assignment *>(*ss);
            assert(assign != nullptr);
            appendExp(s, *assign->getLeft(), OpPrec::Comma);
        }

        s << " */";
    }

    appendLine(tgt);
}


void CCodeGenerator::addIndCallStatement(const SharedExp &exp, const StatementList &args,
                                         const StatementList &results)
{
    Q_UNUSED(results);
    //    FIXME: Need to use 'results', since we can infer some defines...
    QString tgt;
    OStream s(&tgt);
    indent(s, m_indent);
    s << "(*";
    appendExp(s, *exp, OpPrec::None);
    s << ")(";
    QStringList arg_strings;
    QString arg_tgt;

    for (Statement *ss : args) {
        OStream arg_str(&arg_tgt);
        SharedExp arg = static_cast<Assign *>(ss)->getRight();
        appendExp(arg_str, *arg, OpPrec::Comma);
        arg_strings << arg_tgt;
        arg_tgt.clear();
    }

    s << arg_strings.join(", ") << ");";
    appendLine(tgt);
}


void CCodeGenerator::addReturnStatement(const StatementList *rets)
{
    // FIXME: should be returning a struct of more than one real return */
    // The stack pointer is wanted as a define in calls, and so appears in returns, but needs to be
    // removed here
    QString tgt;
    OStream ost(&tgt);
    indent(ost, m_indent);
    ost << "return";
    size_t n = rets->size();

    if (n >= 1) {
        ost << " ";
        appendExp(ost, *static_cast<Assign *>(*rets->begin())->getRight(), OpPrec::None);
    }

    ost << ";";

    if (n > 0) {
        if (n > 1) {
            ost << " /* WARNING: Also returning: ";
        }

        bool first = true;
        assert(!rets->empty());

        for (auto retIt = std::next(rets->begin()); retIt != rets->end(); ++retIt) {
            if (first) {
                first = false;
            }
            else {
                ost << ", ";
            }

            appendExp(ost, *(static_cast<Assign *>(*retIt))->getLeft(), OpPrec::None);
            ost << " := ";
            appendExp(ost, *(static_cast<Assign *>(*retIt))->getRight(), OpPrec::None);
        }

        if (n > 1) {
            ost << " */";
        }
    }

    appendLine(tgt);
}


void CCodeGenerator::removeUnusedLabels()
{
    for (QStringList::iterator it = m_lines.begin(); it != m_lines.end();) {
        if (it->startsWith("bb0x") && it->contains(':')) {
            QStringRef bbAddrString = it->midRef(4, it->indexOf(':') - 4);
            bool ok                 = false;
            Address bbAddr(bbAddrString.toLongLong(&ok, 16));
            assert(ok);

            if (m_usedLabels.find(bbAddr.value()) == m_usedLabels.end()) {
                it = m_lines.erase(it);
                continue;
            }
        }

        ++it;
    }
}


void CCodeGenerator::addPrototype(UserProc *proc)
{
    m_proc = proc;
    addFunctionSignature(proc, false);
}


void CCodeGenerator::generateCode(UserProc *proc)
{
    m_lines.clear();
    m_proc = proc;

    if (!proc->getCFG() || !proc->getEntryBB()) {
        return;
    }

    m_analyzer.structureCFG(proc->getCFG());
    PassManager::get()->executePass(PassID::UnusedLocalRemoval, proc);

    // Note: don't try to remove unused statements here; that requires the
    // RefExps, which are all gone now (transformed out of SSA form)!

    if (m_proc->getProg()->getProject()->getSettings()->printRTLs) {
        LOG_VERBOSE("%1", proc->toString());
    }

    // Start generating code for this procedure.
    this->addProcStart(proc);

    // Local variables; print everything in the locals map
    std::map<QString, SharedType>::const_iterator last = proc->getLocals().end();

    if (!proc->getLocals().empty()) {
        last = std::prev(last);
    }

    for (auto it = proc->getLocals().begin(); it != proc->getLocals().end(); ++it) {
        SharedType locType = it->second;

        if ((locType == nullptr) || locType->isVoid()) {
            locType = IntegerType::get(STD_SIZE);
        }

        addLocal(it->first, locType, it == last);
    }

    // Start generating "real" code
    std::list<const BasicBlock *> followSet, gotoSet;
    generateCode(proc->getEntryBB(), nullptr, followSet, gotoSet, proc);

    addProcEnd();

    if (m_proc->getProg()->getProject()->getSettings()->removeLabels) {
        removeUnusedLabels();
    }

    proc->setStatus(ProcStatus::CodegenDone);
}


void CCodeGenerator::generateDataSectionCode(const BinaryImage *image, QString section_name,
                                             Address section_start, uint32_t size)
{
    addGlobal("start_" + section_name, IntegerType::get(32, Sign::Unsigned),
              Const::get(section_start));
    addGlobal(section_name + "_size", IntegerType::get(32, Sign::Unsigned),
              Const::get(size ? size : static_cast<uint32_t>(-1)));
    auto l = Terminal::get(opNil);

    for (unsigned int i = 0; i < size; i++) {
        int n = image->readNative1(section_start + size - 1 - i);

        l = Binary::get(opList, Const::get(n & 0xFF), l);
    }

    addGlobal(section_name, ArrayType::get(IntegerType::get(8, Sign::Unsigned), size), l);
}


void CCodeGenerator::addFunctionSignature(UserProc *proc, bool open)
{
    QString tgt;
    OStream s(&tgt);
    ReturnStatement *returns = proc->getRetStmt();
    SharedType retType;

    if (proc->getSignature()->isForced()) {
        if (proc->getSignature()->getNumReturns() == 0) {
            s << "void ";
        }
        else {
            int n       = 0;
            SharedExp e = proc->getSignature()->getReturnExp(0);

            if (e->isRegN(Util::getStackRegisterIndex(proc->getProg()))) {
                n = 1;
            }

            if (n < proc->getSignature()->getNumReturns()) {
                retType = proc->getSignature()->getReturnType(n);
            }

            if (retType == nullptr) {
                s << "void ";
            }
        }
    }
    else if ((returns == nullptr) || (returns->getNumReturns() == 0)) {
        s << "void ";
    }
    else {
        Assign *firstRet = static_cast<Assign *>(*returns->begin());
        retType          = firstRet->getType();

        if ((retType == nullptr) || retType->isVoid()) {
            // There is a real return; make it integer (Remove with AD HOC type analysis)
            retType = IntegerType::get(STD_SIZE, Sign::Unknown);
        }
    }

    if (retType) {
        appendType(s, retType);

        if (!retType->isPointer()) { // NOTE: assumes type *proc( style
            s << " ";
        }
    }

    s << proc->getName() << "(";
    StatementList &parameters = proc->getParameters();

    if ((parameters.size() > 10) && open) {
        LOG_WARN("Proc %1 has %2 parameters", proc->getName(), parameters.size());
    }

    bool first = true;

    for (auto &parameter : parameters) {
        if (first) {
            first = false;
        }
        else {
            s << ", ";
        }

        Assignment *as = static_cast<Assignment *>(parameter);
        SharedExp left = as->getLeft();
        SharedType ty  = as->getType();

        if (ty == nullptr) {
            if (proc->getProg()->getProject()->getSettings()->verboseOutput) {
                LOG_ERROR("No type for parameter %1!", left);
            }

            ty = IntegerType::get(STD_SIZE, Sign::Unknown);
        }

        QString name;

        if (left->isParam()) {
            name = left->access<Const, 1>()->getStr();
        }
        else {
            LOG_ERROR("Parameter %1 is not opParam!", left);
            name = "??";
        }

        if (ty->isPointer() && ty->as<PointerType>()->getPointsTo()->isArray()) {
            // C does this by default when you pass an array, i.e. you pass &array meaning array
            // Replace all m[param] with foo, param with foo, then foo with param
            ty = ty->as<PointerType>()->getPointsTo();

            SharedExp foo = Const::get("foo123412341234");
            m_proc->searchAndReplace(*Location::memOf(left, nullptr), foo);
            m_proc->searchAndReplace(*left, foo);
            m_proc->searchAndReplace(*foo, left);
        }

        appendTypeIdent(s, ty, name);
    }

    s << ")";

    if (open) {
        appendLine(tgt);
        appendLine("{");
        m_indent++;
    }
    else {
        s << ";";
        appendLine(tgt);
    }
}


void CCodeGenerator::addPretestedLoopHeader(const SharedExp &cond)
{
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "while (";
    appendExp(s, *cond, OpPrec::None);
    s << ") {";
    appendLine(tgt);

    m_indent++;
}


void CCodeGenerator::addPretestedLoopEnd()
{
    m_indent--;

    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "}";
    appendLine(tgt);
}


void CCodeGenerator::addEndlessLoopHeader()
{
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "for(;;) {";
    appendLine(tgt);

    m_indent++;
}


void CCodeGenerator::addEndlessLoopEnd()
{
    m_indent--;
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "}";
    appendLine(tgt);
}


void CCodeGenerator::addPostTestedLoopHeader()
{
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "do {";
    appendLine(tgt);
    m_indent++;
}


void CCodeGenerator::addPostTestedLoopEnd(const SharedExp &cond)
{
    m_indent--;

    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "} while (";
    appendExp(s, *cond, OpPrec::None);
    s << ");";
    appendLine(tgt);
}


void CCodeGenerator::addCaseCondHeader(const SharedExp &cond)
{
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "switch(";
    appendExp(s, *cond, OpPrec::None);
    s << ") {";
    appendLine(tgt);

    m_indent++;
}


void CCodeGenerator::addCaseCondOption(Exp &opt)
{
    m_indent--;

    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "case ";
    appendExp(s, opt, OpPrec::None);
    s << ":";
    appendLine(tgt);

    m_indent++;
}


void CCodeGenerator::addCaseCondOptionEnd()
{
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "break;";
    appendLine(tgt);
}


void CCodeGenerator::addCaseCondElse()
{
    m_indent--;
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "default:";
    appendLine(tgt);

    m_indent++;
}


void CCodeGenerator::addCaseCondEnd()
{
    m_indent--;

    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "}";
    appendLine(tgt);
}


void CCodeGenerator::addIfCondHeader(const SharedExp &cond)
{
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "if (";
    appendExp(s, *cond, OpPrec::None);
    s << ") {";
    appendLine(tgt);

    m_indent++;
}


void CCodeGenerator::addIfCondEnd()
{
    m_indent--;

    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "}";
    appendLine(tgt);
}


void CCodeGenerator::addIfElseCondHeader(const SharedExp &cond)
{
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "if (";
    appendExp(s, *cond, OpPrec::None);
    s << ") {";
    appendLine(tgt);

    m_indent++;
}


void CCodeGenerator::addIfElseCondOption()
{
    m_indent--;

    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "}";
    appendLine(tgt);

    tgt = "";
    indent(s, m_indent);
    s << "else {";
    appendLine(tgt);

    m_indent++;
}


void CCodeGenerator::addIfElseCondEnd()
{
    m_indent--;

    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "}";
    appendLine(tgt);
}


void CCodeGenerator::addGoto(const BasicBlock *bb)
{
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "goto bb0x" << QString::number(bb->getLowAddr().value(), 16) << ";";
    appendLine(tgt);
    m_usedLabels.insert(bb->getLowAddr().value());
}


void CCodeGenerator::addContinue()
{
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "continue;";
    appendLine(tgt);
}


void CCodeGenerator::addBreak()
{
    QString tgt;
    OStream s(&tgt);

    indent(s, m_indent);
    s << "break;";
    appendLine(tgt);
}


void CCodeGenerator::addLabel(const BasicBlock *bb)
{
    QString tgt;
    OStream s(&tgt);

    s << "bb0x" << QString::number(bb->getLowAddr().value(), 16) << ":";
    appendLine(tgt);
}


void CCodeGenerator::addProcStart(UserProc *proc)
{
    QString tgt;
    OStream s(&tgt);

    s << "/** address: " << proc->getEntryAddress() << " */";
    appendLine(tgt);
    addFunctionSignature(proc, true);
}


void CCodeGenerator::addProcEnd()
{
    m_indent--;
    appendLine("}");
    appendLine("");
}


void CCodeGenerator::addLocal(const QString &name, SharedType type, bool last)
{
    QString tgt;
    OStream ost(&tgt);

    indent(ost, 1);
    appendTypeIdent(ost, type, name);
    SharedConstExp e = m_proc->expFromSymbol(name);

    if (e) {
        // ? Should never see subscripts in the back end!
        if (e->isSubscript() && (e->getSubExp1()->isParam() || e->getSubExp1()->isGlobal()) &&
            e->access<const RefExp>()->isImplicitDef()) {
            ost << " = ";
            appendExp(ost, *e->getSubExp1(), OpPrec::None);
            ost << ";";
        }
        else {
            ost << "; \t\t// " << e;
        }
    }
    else {
        ost << ";";
    }

    appendLine(tgt);
    m_locals[name] = type->clone();

    if (last) {
        appendLine("");
    }
}


void CCodeGenerator::addGlobal(const QString &name, SharedType type, const SharedExp &init)
{
    QString tgt;
    OStream s(&tgt);

    // Check for array types. These are declared differently in C than
    // they are printed
    if (type->isArray()) {
        // Get the component type
        SharedType base = type->as<ArrayType>()->getBaseType();
        appendType(s, base);

        s << " " << name << "[";
        if (!type->as<ArrayType>()->isUnbounded()) {
            s << type->as<ArrayType>()->getLength();
        }
        s << "]";
    }
    else if (type->isPointer() && type->as<PointerType>()->getPointsTo()->resolvesToFunc()) {
        // These are even more different to declare than to print. Example:
        // void (void)* global0 = foo__1B;     ->
        // void (*global0)(void) = foo__1B;
        std::shared_ptr<FuncType> ft = type->as<PointerType>()->getPointsTo()->as<FuncType>();
        QString ret, param;
        ft->getReturnAndParam(ret, param);
        s << ret << "(*" << name << ")" << param;
    }
    else {
        appendType(s, type);
        s << " " << name;
    }

    if (init && !init->isNil()) {
        s << " = ";
        SharedType base_type = type->isArray() ? type->as<ArrayType>()->getBaseType() : type;
        appendExp(s, *init, OpPrec::Assign,
                  base_type->isInteger() ? base_type->as<IntegerType>()->isUnsigned() : false);
    }

    s << ";";

    if (type->isSize()) {
        s << "// " << type->getSize() / 8 << " bytes";
    }

    appendLine(tgt);
}


void CCodeGenerator::addLineComment(const QString &cmt)
{
    appendLine(QString("/* %1 */").arg(cmt));
}


void CCodeGenerator::appendExp(OStream &str, const Exp &exp, OpPrec curPrec, bool uns /* = false */)
{
    const OPER op = exp.getOper();
    const Const &constExp(static_cast<const Const &>(exp));
    const Unary &unaryExp(static_cast<const Unary &>(exp));
    const Binary &binaryExp(static_cast<const Binary &>(exp));
    const Ternary &ternaryExp(static_cast<const Ternary &>(exp));

    switch (op) {
    case opIntConst: {
        int K = constExp.getInt();

        if (uns && (K < 0)) {
            // An unsigned constant. Use some heuristics
            unsigned rem = static_cast<unsigned int>(K) % 100;

            if ((rem == 0) || (rem == 99) || (K > -128)) {
                // A multiple of 100, or one less; use 4000000000U style
                char num[16];
                sprintf(num, "%u", static_cast<unsigned int>(K));
                str << num << "U";
            }
            else {
                // Output it in 0xF0000000 style
                str << "0x" << QString::number(uint32_t(K), 16);
            }
        }
        else {
            if (constExp.getType() && constExp.getType()->isChar()) {
                switch (K) {
                case '\a': str << "'\\a'"; break;
                case '\b': str << "'\\b'"; break;
                case '\f': str << "'\\f'"; break;
                case '\n': str << "'\\n'"; break;
                case '\r': str << "'\\r'"; break;
                case '\t': str << "'\\t'"; break;
                case '\v': str << "'\\v'"; break;
                case '\\': str << "'\\\\'"; break;
                case '\?': str << "'\\?'"; break;
                case '\'': str << "'\\''"; break;
                case '\"': str << "'\\\"'"; break;
                default: str << "'" << static_cast<char>(K) << "'";
                }
            }
            else {
                // More heuristics
                if ((-2048 < K) && (K < 2048)) {
                    str << K; // Just a plain vanilla int
                }
                else {
                    str << "0x" << QString::number(uint32_t(K), 16); // 0x2000 style
                }
            }
        }

        break;
    }

    case opLongConst:
        // str << std::dec << c->getLong() << "LL"; break;
        if ((static_cast<long long>(constExp.getLong()) < -1000LL) ||
            (constExp.getLong() > 1000ULL)) {
            str << "0x" << QString::number(constExp.getLong(), 16) << "LL";
        }
        else {
            str << constExp.getLong() << "LL";
        }
        break;

    case opFltConst: {
        // str.precision(4);     // What to do with precision here? Would be nice to avoid 1.00000
        // or 0.99999
        QString flt_val = QString::number(constExp.getFlt(), 'g', 8);

        if (!flt_val.contains('.')) {
            flt_val += '.';
        }

        str << flt_val;
        break;
    }

    case opStrConst:
        // escape string:
        str << "\"" << Util::escapeStr(constExp.getRawStr()) << "\"";
        break;

    case opFuncConst: str << constExp.getFuncName(); break;

    case opAddrOf: {
        SharedConstExp sub = unaryExp.getSubExp1();

        if (sub->isGlobal()) {
            Prog *prog    = m_proc->getProg();
            SharedType gt = prog->getGlobalType(sub->access<const Const, 1>()->getStr());

            if (gt && (gt->resolvesToArray() || gt->isCString())) {
                // Special C requirement: don't emit "&" for address of an array or char*
                appendExp(str, *sub, curPrec);
                break;
            }
        }

        if (sub->isMemOf()) {
            // Avoid &*(type*)sub, just emit sub
            appendExp(str, *sub->getSubExp1(), OpPrec::Unary);
        }
        else {
            openParen(str, curPrec, OpPrec::Unary);
            str << "&";
            appendExp(str, *sub, OpPrec::Unary);
            closeParen(str, curPrec, OpPrec::Unary);
        }

        break;
    }

    case opParam:
    case opGlobal:
    case opLocal: {
        auto c1 = std::dynamic_pointer_cast<const Const>(unaryExp.getSubExp1());
        assert(c1 && c1->isStrConst());
        str << c1->getStr();
    } break;

    case opEquals: {
        openParen(str, curPrec, OpPrec::Equal);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Equal);
        str << " == ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Equal);
        closeParen(str, curPrec, OpPrec::Equal);
    } break;

    case opNotEqual: {
        openParen(str, curPrec, OpPrec::Equal);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Equal);
        str << " != ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Equal);
        closeParen(str, curPrec, OpPrec::Equal);
    } break;

    case opLess:
    case opLessUns:
        openParen(str, curPrec, OpPrec::Rel);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Rel, op == opLessUns);
        str << " < ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Rel, op == opLessUns);
        closeParen(str, curPrec, OpPrec::Rel);
        break;

    case opGtr:
    case opGtrUns:
        openParen(str, curPrec, OpPrec::Rel);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Rel, op == opGtrUns);
        str << " > ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Rel, op == opGtrUns);
        closeParen(str, curPrec, OpPrec::Rel);
        break;

    case opLessEq:
    case opLessEqUns:
        openParen(str, curPrec, OpPrec::Rel);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Rel, op == opLessEqUns);
        str << " <= ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Rel, op == opLessEqUns);
        closeParen(str, curPrec, OpPrec::Rel);
        break;

    case opGtrEq:
    case opGtrEqUns:
        openParen(str, curPrec, OpPrec::Rel);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Rel, op == opGtrEqUns);
        str << " >= ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Rel, op == opGtrEqUns);
        closeParen(str, curPrec, OpPrec::Rel);
        break;

    case opAnd:
        openParen(str, curPrec, OpPrec::LogAnd);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::LogAnd);
        str << " && ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::LogAnd);
        closeParen(str, curPrec, OpPrec::LogAnd);
        break;

    case opOr:
        openParen(str, curPrec, OpPrec::LogOr);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::LogOr);
        str << " || ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::LogOr);
        closeParen(str, curPrec, OpPrec::LogOr);
        break;

    case opBitAnd:
        openParen(str, curPrec, OpPrec::BitAnd);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::BitAnd);
        str << " & ";

        if (binaryExp.getSubExp2()->isIntConst()) {
            // print it 0x2000 style
            uint32_t val    = uint32_t(binaryExp.access<const Const, 2>()->getInt());
            QString vanilla = QString("0x") + QString::number(val, 16);
            QString negated = QString("~0x") + QString::number(~val, 16);

            if (negated.size() < vanilla.size()) {
                str << negated;
            }
            else {
                str << vanilla;
            }
        }
        else {
            appendExp(str, *binaryExp.getSubExp2(), OpPrec::BitAnd);
        }

        closeParen(str, curPrec, OpPrec::BitAnd);
        break;

    case opBitOr:
        openParen(str, curPrec, OpPrec::BitOr);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::BitOr);
        str << " | ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::BitOr);
        closeParen(str, curPrec, OpPrec::BitOr);
        break;

    case opBitXor:
        openParen(str, curPrec, OpPrec::BitXor);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::BitXor);
        str << " ^ ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::BitXor);
        closeParen(str, curPrec, OpPrec::BitXor);
        break;

    case opBitNot:
        openParen(str, curPrec, OpPrec::Unary);
        str << "~";
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::Unary);
        closeParen(str, curPrec, OpPrec::Unary);
        break;

    case opLNot:
        openParen(str, curPrec, OpPrec::Unary);
        str << "!";
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::Unary);
        closeParen(str, curPrec, OpPrec::Unary);
        break;

    case opNeg:
    case opFNeg:
        openParen(str, curPrec, OpPrec::Unary);
        str << "-";
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::Unary);
        closeParen(str, curPrec, OpPrec::Unary);
        break;

    case opAt: {
        // I guess that most people will find this easier to read
        // s1 >> last & 0xMASK
        openParen(str, curPrec, OpPrec::BitAnd);
        appendExp(str, *ternaryExp.getSubExp1(), OpPrec::BitShift);
        auto first = ternaryExp.access<const Const, 2>();
        auto last  = ternaryExp.access<const Const, 3>();

        str << " >> ";
        appendExp(str, *last, OpPrec::BitShift);
        str << " & ";

        SharedExp maskExp = Binary::get(opPlus, Binary::get(opMinus, first->clone(), last->clone()),
                                        Const::get(1))
                                ->simplify();

        if (maskExp->isIntConst()) {
            const unsigned int mask = Util::getLowerBitMask(maskExp->access<Const>()->getInt());

            // print 0x3 as 3
            if (mask < 10) {
                str << mask;
            }
            else {
                str << "0x" << QString::number(mask, 16);
            }
        }
        else {
            maskExp = Binary::get(opMinus, Binary::get(opShL, Const::get(1), maskExp),
                                  Const::get(1));
            appendExp(str, *maskExp, OpPrec::BitAnd);
        }

        closeParen(str, curPrec, OpPrec::BitAnd);
        break;
    }

    case opPlus:
        openParen(str, curPrec, OpPrec::Add);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Add);
        str << " + ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Add);
        closeParen(str, curPrec, OpPrec::Add);
        break;

    case opMinus:
        openParen(str, curPrec, OpPrec::Add);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Add);
        str << " - ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Add);
        closeParen(str, curPrec, OpPrec::Add);
        break;

    case opMemOf:
        openParen(str, curPrec, OpPrec::Unary);
        // annotateMemofs should have added a cast if it was needed
        str << "*";
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::Unary);
        closeParen(str, curPrec, OpPrec::Unary);
        break;

    case opRegOf: {
        // MVE: this can likely go
        LOG_VERBOSE("Case opRegOf is deprecated");

        if (unaryExp.getSubExp1()->isTemp()) {
            // The great debate: r[tmpb] vs tmpb
            str << "tmp";
            break;
        }

        assert(unaryExp.getSubExp1()->isIntConst());
        const RegNum regNum = unaryExp.access<const Const, 1>()->getInt();
        QString regName     = m_proc->getProg()->getRegNameByNum(regNum);

        if (!regName.isEmpty()) {
            str << regName;
        }
        else {
            // What is this doing in the back end???
            str << "r[";
            appendExp(str, *unaryExp.getSubExp1(), OpPrec::None);
            str << "]";
        }
    } break;

    case opTemp:
        // Should never see this; temps should be mapped to locals now so that they get declared
        LOG_VERBOSE("Case opTemp is deprecated");
        // Emit the temp name, e.g. "tmp1"
        str << unaryExp.access<Const, 1>()->getStr();
        break;

    case opItof:
        // TODO: MVE: needs work: float/double/long double.
        str << "(float)";
        openParen(str, curPrec, OpPrec::Unary);
        appendExp(str, *ternaryExp.getSubExp3(), OpPrec::Unary);
        closeParen(str, curPrec, OpPrec::Unary);
        break;

    case opFsize:

        // TODO: needs work!
        if (ternaryExp.getSubExp3()->isMemOf()) {
            assert(ternaryExp.getSubExp1()->isIntConst());
            int float_bits = ternaryExp.access<Const, 1>()->getInt();

            switch (float_bits) {
            case 32: str << "*((float *)&"; break;

            case 64: str << "*((double *)&"; break;

            case 80: str << "*((long double*)&"; break;
            }

            openParen(str, curPrec, curPrec);
            appendExp(str, *ternaryExp.getSubExp3(), curPrec);
            closeParen(str, curPrec, curPrec);
            str << ")";
        }
        else {
            appendExp(str, *ternaryExp.getSubExp3(), curPrec);
        }

        break;

    case opMult:
    case opMults: // FIXME: check types
        openParen(str, curPrec, OpPrec::Mult);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Mult);
        str << " * ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Mult);
        closeParen(str, curPrec, OpPrec::Mult);
        break;

    case opDiv:
    case opDivs: // FIXME: check types
        openParen(str, curPrec, OpPrec::Mult);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Mult);
        str << " / ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Mult);
        closeParen(str, curPrec, OpPrec::Mult);
        break;

    case opMod:
    case opMods: // Fixme: check types
        openParen(str, curPrec, OpPrec::Mult);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Mult);
        str << " % ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Mult);
        closeParen(str, curPrec, OpPrec::Mult);
        break;

    case opShL:
        openParen(str, curPrec, OpPrec::BitShift);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::BitShift);
        str << " << ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::BitShift);
        closeParen(str, curPrec, OpPrec::BitShift);
        break;

    case opShR:
    case opShRA:
        openParen(str, curPrec, OpPrec::BitShift);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::BitShift);
        str << " >> ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::BitShift);
        closeParen(str, curPrec, OpPrec::BitShift);
        break;

    case opTern:
        openParen(str, curPrec, OpPrec::Cond);
        str << " (";
        appendExp(str, *ternaryExp.getSubExp1(), OpPrec::None);
        str << ") ? ";
        appendExp(str, *ternaryExp.getSubExp2(), OpPrec::Cond);
        str << " : ";
        appendExp(str, *ternaryExp.getSubExp3(), OpPrec::Cond);
        closeParen(str, curPrec, OpPrec::Cond);
        break;

    case opFPlus:
        openParen(str, curPrec, OpPrec::Add);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Add);
        str << " + ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Add);
        closeParen(str, curPrec, OpPrec::Add);
        break;

    case opFMinus:
        openParen(str, curPrec, OpPrec::Add);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Add);
        str << " - ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Add);
        closeParen(str, curPrec, OpPrec::Add);
        break;

    case opFMult:
        openParen(str, curPrec, OpPrec::Mult);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Mult);
        str << " * ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Mult);
        closeParen(str, curPrec, OpPrec::Mult);
        break;

    case opFDiv:
        openParen(str, curPrec, OpPrec::Mult);
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Mult);
        str << " / ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Mult);
        closeParen(str, curPrec, OpPrec::Mult);
        break;

    case opFround:
        // Note: we need roundf or roundl depending on size of operands
        str << "round("; // Note: math.h required
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::None);
        str << ")";
        break;

    case opFtrunc:
        // Note: we need truncf or truncl depending on size of operands
        str << "trunc("; // Note: math.h required
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::None);
        str << ")";
        break;

    case opFabs:
        str << "fabs(";
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::None);
        str << ")";
        break;

    case opFtoi:
        // Should check size!
        str << "(int)";
        appendExp(str, *unaryExp.getSubExp3(), OpPrec::Unary);
        break;

    case opRotL:
        str << "ROTL(";
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Unary);
        str << ", ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Unary);
        str << ")";
        break;

    case opRotR:
        str << "ROTR(";
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Unary);
        str << ", ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Unary);
        str << ")";
        break;

    case opRotLC:
        str << "ROTLC(";
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Unary);
        str << ", ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Unary);
        str << ")";
        break;

    case opRotRC:
        str << "ROTRC(";
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Unary);
        str << ", ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Unary);
        str << ")";
        break;

    case opLoge:
        // not implemented
        LOG_WARN("Case %1 not implemented", operToString(exp.getOper()));
        // assert(false);
        break;

    case opFlagCall: {
        assert(binaryExp.getSubExp1()->isStrConst());
        str << binaryExp.access<Const, 1>()->getStr();
        str << "(";
        auto l = binaryExp.getSubExp2();

        for (; l && l->getOper() == opList; l = l->getSubExp2()) {
            appendExp(str, *l->getSubExp1(), OpPrec::None);

            if (l->getSubExp2()->getOper() == opList) {
                str << ", ";
            }
        }

        str << ")";
    } break;

    case opList: {
        int elems_on_line = 0; // try to limit line lengths
        SharedConstExp b2 = binaryExp.shared_from_this();
        SharedConstExp e2 = binaryExp.getSubExp2();
        str << "{ ";

        if (binaryExp.getSubExp1()->getOper() == opList) {
            str << "\n ";
        }

        while (e2->getOper() == opList) {
            appendExp(str, *b2->getSubExp1(), OpPrec::None, uns);
            ++elems_on_line;

            if ((b2->getSubExp1()->getOper() == opList) ||
                (elems_on_line >= 16) /* completely arbitrary, but better than nothing*/) {
                str << ",\n ";
                elems_on_line = 0;
            }
            else {
                str << ", ";
            }

            b2 = e2;
            e2 = b2->getSubExp2();
        }

        appendExp(str, *b2->getSubExp1(), OpPrec::None, uns);
        str << " }";
    } break;

    case opFlags: str << "flags"; break;

    case opPC: str << "pc"; break;

    case opZfill:

        // MVE: this is a temporary hack... needs cast?
        // sprintf(s, "/* zfill %d->%d */ ",
        //    ((Const*)t.getSubExp1())->getInt(),
        //    ((Const*)t.getSubExp2())->getInt());
        // strcat(str, s); */
        if (ternaryExp.getSubExp3()->isMemOf() && ternaryExp.getSubExp1()->isIntConst() &&
            ternaryExp.getSubExp2()->isIntConst() &&
            (ternaryExp.access<Const, 2>()->getInt() == 32)) {
            int sz = ternaryExp.access<Const, 1>()->getInt();

            if ((sz == 8) || (sz == 16)) {
                str << "*";
                str << "(unsigned ";

                if (sz == 8) {
                    str << "char";
                }
                else {
                    str << "short";
                }

                str << "*)";
                openParen(str, curPrec, OpPrec::Unary);
                appendExp(str, *ternaryExp.getSubExp3()->getSubExp1(), OpPrec::Unary);
                closeParen(str, curPrec, OpPrec::Unary);

                break;
            }
        }

        LOG_VERBOSE("Case opZfill is deprecated");
        str << "(";
        appendExp(str, *ternaryExp.getSubExp3(), OpPrec::None);
        str << ")";
        break;

    case opTypedExp: {
        if (unaryExp.getSubExp1()->isTypedExp() &&
            (*static_cast<const TypedExp &>(unaryExp).getType() ==
             *unaryExp.access<TypedExp, 1>()->getType())) {
            // We have (type)(type)x: recurse with type(x)
            appendExp(str, *unaryExp.getSubExp1(), curPrec);
        }
        else if (unaryExp.getSubExp1()->isMemOf()) {
            // We have (tt)m[x]
            SharedConstType tt = static_cast<const TypedExp &>(unaryExp).getType();
            SharedConstExp x   = unaryExp.getSubExp1()->getSubExp1();
            std::shared_ptr<const PointerType> xType = nullptr; // type of "x"
            if (x->isTypedExp()) {
                // x is of type pointer-to-type
                auto ptrTy = x->access<const TypedExp>()->getType();
                xType      = ptrTy->isPointer() ? ptrTy->as<const PointerType>() : nullptr;
            }

            if (xType && (*tt == *xType->getPointsTo() ||
                          (tt->isSize() && (xType->getPointsTo()->getSize() == tt->getSize())))) {
                str << "*"; // memof degrades to dereference if types match
            }
            else { //  *(T *)
                str << "*(";
                appendType(str, tt);
                str << "*)";
            }

            openParen(str, curPrec, OpPrec::Unary);
            // Emit x
            // was : ((Location*)((TypedExp&)u).getSubExp1())->getSubExp1()
            appendExp(str, *unaryExp.getSubExp1()->getSubExp1(), OpPrec::Unary);
            closeParen(str, curPrec, OpPrec::Unary);
        }
        else {
            SharedConstType tt = static_cast<const TypedExp &>(unaryExp).getType();
            str << "(";
            appendType(str, tt);
            str << ")";
            openParen(str, curPrec, OpPrec::Unary);
            appendExp(str, *unaryExp.getSubExp1(), OpPrec::Unary);
            closeParen(str, curPrec, OpPrec::Unary);
        }

        break;
    }

    case opSgnEx:
    case opTruncs: {
        SharedConstExp s = ternaryExp.getSubExp3();
        int toSize       = ternaryExp.access<Const, 2>()->getInt();

        switch (toSize) {
        case 8: str << "(char) "; break;
        case 16: str << "(short) "; break;
        case 64: str << "(long long) "; break;
        default: str << "(int) "; break;
        }

        appendExp(str, *s, curPrec);
        break;
    }

    case opTruncu: {
        SharedConstExp s = ternaryExp.getSubExp3();
        int toSize       = ternaryExp.access<Const, 2>()->getInt();

        switch (toSize) {
        case 8: str << "(unsigned char) "; break;
        case 16: str << "(unsigned short) "; break;
        case 64: str << "(unsigned long long) "; break;
        default: str << "(unsigned int) "; break;
        }

        appendExp(str, *s, curPrec);
        break;
    }

    case opMachFtr: {
        str << "/* machine specific */ (int) ";
        auto sub = unaryExp.access<Const, 1>();
        assert(sub->isStrConst());
        QString s = sub->getStr();

        if (s[0] == '%') {   // e.g. %Y
            str << s.mid(1); // Just use Y
        }
        else {
            str << s;
        }

        break;
    }

    case opFflags: str << "/* Fflags() */ "; break;

    case opPow:
        str << "pow(";
        appendExp(str, *binaryExp.getSubExp1(), OpPrec::Comma);
        str << ", ";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Comma);
        str << ")";
        break;

    case opLog2:
        str << "log2(";
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::None);
        str << ")";
        break;

    case opLog10:
        str << "log10(";
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::None);
        str << ")";
        break;

    case opSin:
        str << "sin(";
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::None);
        str << ")";
        break;

    case opCos:
        str << "cos(";
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::None);
        str << ")";
        break;

    case opSqrt:
        str << "sqrt(";
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::None);
        str << ")";
        break;

    case opTan:
        str << "tan(";
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::None);
        str << ")";
        break;

    case opArcTan:
        str << "atan(";
        appendExp(str, *unaryExp.getSubExp1(), OpPrec::None);
        str << ")";
        break;

    case opSubscript:
        appendExp(str, *unaryExp.getSubExp1(), curPrec);
        LOG_ERROR("Subscript in code generation of proc %1", m_proc->getName());
        break;

    case opMemberAccess: {
        SharedType ty = nullptr;

        //             if (ty == nullptr) {
        LOG_MSG("Type failure: no type for subexp1 of %1", binaryExp.shared_from_this());

        // ty = b.getSubExp1()->getType();
        // No idea why this is hitting! - trentw
        // str << "/* type failure */ ";
        // break;
        //             }

        // Trent: what were you thinking here? Fails for things like
        // local11.lhHeight (where local11 is a register)
        // Mike: it shouldn't!  local11 should have a compound type
        // assert(ty->resolvesToCompound());
        if (binaryExp.getSubExp1()->isMemOf()) {
            appendExp(str, *binaryExp.getSubExp1()->getSubExp1(), OpPrec::Prim);
            str << "->";
        }
        else {
            appendExp(str, *binaryExp.getSubExp1(), OpPrec::Prim);
            str << ".";
        }

        str << binaryExp.access<const Const, 2>()->getStr();
    } break;

    case opArrayIndex:
        openParen(str, curPrec, OpPrec::Prim);

        if (binaryExp.getSubExp1()->isMemOf()) {
            SharedType ty = nullptr;

            if (ty && ty->resolvesToPointer() &&
                ty->as<PointerType>()->getPointsTo()->resolvesToArray()) {
                // a pointer to an array is automatically dereferenced in C
                appendExp(str, *binaryExp.getSubExp1()->getSubExp1(), OpPrec::Prim);
            }
            else {
                appendExp(str, *binaryExp.getSubExp1(), OpPrec::Prim);
            }
        }
        else {
            appendExp(str, *binaryExp.getSubExp1(), OpPrec::Prim);
        }

        closeParen(str, curPrec, OpPrec::Prim);
        str << "[";
        appendExp(str, *binaryExp.getSubExp2(), OpPrec::Prim);
        str << "]";
        break;

    case opDefineAll:
        str << "<all>";
        LOG_ERROR("Should not see opDefineAll in codegen");
        break;

    case opTrue: str << "true"; break;

    case opFalse: str << "false"; break;

    default:
        // others
        OPER other_op = exp.getOper();

        if (other_op >= opZF) {
            // Machine flags; can occasionally be manipulated individually
            // Chop off the "op" part
            str << operToString(exp.getOper()) + 2;
            break;
        }

        LOG_ERROR("case %1 not implemented", operToString(exp.getOper()));
    }
}


void CCodeGenerator::appendType(OStream &str, SharedConstType typ)
{
    if (!typ) {
        str << "int"; // Default type for C
        return;
    }

    if (typ->resolvesToPointer() && typ->as<PointerType>()->getPointsTo()->resolvesToArray()) {
        // C programmers prefer to see pointers to arrays as pointers
        // to the first element of the array.  They then use syntactic
        // sugar to access a pointer as if it were an array.
        typ = PointerType::get(
            typ->as<PointerType>()->getPointsTo()->as<ArrayType>()->getBaseType());
    }

    str << typ->getCtype(true);
}


void CCodeGenerator::appendTypeIdent(OStream &str, SharedConstType typ, QString ident)
{
    if (typ == nullptr) {
        return;
    }

    if (typ->isPointer() && typ->as<PointerType>()->getPointsTo()->isArray()) {
        appendType(str, typ->as<PointerType>()->getPointsTo()->as<ArrayType>()->getBaseType());
        str << " *" << ident;
    }
    else if (typ->isPointer()) {
        appendType(str, typ);
        str << ident;
    }
    else if (typ->isArray()) {
        auto a = typ->as<ArrayType>();
        appendTypeIdent(str, a->getBaseType(), ident);
        str << "[";

        if (!a->isUnbounded()) {
            str << a->getLength();
        }

        str << "]";
    }
    // Can happen in e.g. twoproc, where really need global parameter and
    // return analysis
    else if (typ->isVoid()) {
        // TMN: Stop crashes by this workaround
        if (ident.isEmpty()) {
            ident = "unknownVoidType";
        }

        LOG_WARN("Declaring type void as int for %1", ident);
        str << "int " << ident;
    }
    else {
        appendType(str, typ);
        str << " " << (!ident.isEmpty() ? ident : "<null>");
    }
}


void CCodeGenerator::openParen(OStream &str, OpPrec outer, OpPrec inner)
{
    if (inner > outer) {
        str << "(";
    }
}


void CCodeGenerator::closeParen(OStream &str, OpPrec outer, OpPrec inner)
{
    if (inner > outer) {
        str << ")";
    }
}


void CCodeGenerator::generateCode(const BasicBlock *bb, const BasicBlock *latch,
                                  std::list<const BasicBlock *> &followSet,
                                  std::list<const BasicBlock *> &gotoSet, UserProc *proc)
{
    // If this is the follow for the most nested enclosing conditional, then don't generate
    // anything. Otherwise if it is in the follow set generate a goto to the follow
    const BasicBlock *enclFollow = followSet.empty() ? nullptr : followSet.back();

    if (Util::isContained(gotoSet, bb) && !m_analyzer.isLatchNode(bb) &&
        ((latch && m_analyzer.getLoopHead(latch) &&
          (bb == m_analyzer.getLoopFollow(m_analyzer.getLoopHead(latch)))) ||
         !isAllParentsGenerated(bb))) {
        emitGotoAndLabel(bb, bb);
        return;
    }
    else if (Util::isContained(followSet, bb)) {
        if (bb != enclFollow) {
            emitGotoAndLabel(bb, bb);
        }

        return;
    }

    if (isGenerated(bb)) {
        // this should only occur for a loop over a single block
        return;
    }
    else {
        m_generatedBBs.insert(bb);
    }

    //
    // if this is a latchNode, there are 2 possibilities:
    // 1) The current indentation level is the same as the indentation of the first node of a loop.
    //    Just write out the BB.
    // 2) The indentation level is different.
    //    Can happen (?) when processing a "parent continue" of a double for loop, e.g.
    //    \code
    //        for (...) {
    //            for (...) {
    //               if (condition) {
    //                   goto label;
    //               }
    //               /* code ... */
    //            }
    //            /* code ... */
    //        label:
    //        }
    //    \endcode
    //
    if (m_analyzer.isLatchNode(bb)) {
        // FIXME
        //         if (latch && latch->getLoopHead() &&
        //             (m_indent == latch->getLoopHead()->m_indentLevel +
        //             ((latch->m_loopHead->m_loopHeaderType == LoopType::PreTested) ? 1 : 0))) {
        //             bb->WriteBB(this);
        //         }
        //         else {
        //             // unset its traversed flag
        //             bb->m_traversed = TravType::Untraversed;
        //             emitGotoAndLabel(this, bb);
        //         }
        writeBB(bb);
        return;
    }

    switch (m_analyzer.getStructType(bb)) {
    case StructType::Loop:
    case StructType::LoopCond: generateCode_Loop(bb, gotoSet, proc, latch, followSet); break;

    case StructType::Cond: // if-else / case
        generateCode_Branch(bb, gotoSet, proc, latch, followSet);
        break;

    case StructType::Seq: generateCode_Seq(bb, gotoSet, proc, latch, followSet); break;

    default:
        LOG_ERROR("Unhandled structuring type %1", static_cast<int>(m_analyzer.getStructType(bb)));
    }
}


void CCodeGenerator::generateCode_Loop(const BasicBlock *bb, std::list<const BasicBlock *> &gotoSet,
                                       UserProc *proc, const BasicBlock *latch,
                                       std::list<const BasicBlock *> &followSet)
{
    // add the follow of the loop (if it exists) to the follow set
    if (m_analyzer.getLoopFollow(bb)) {
        followSet.push_back(m_analyzer.getLoopFollow(bb));
    }

    if (m_analyzer.getLoopType(bb) == LoopType::PreTested) {
        assert(m_analyzer.getLatchNode(bb)->getNumSuccessors() == 1);

        // write the body of the block (excluding the predicate)
        writeBB(bb);

        // write the 'while' predicate
        SharedExp cond = bb->getCond();

        if (bb->getSuccessor(BTHEN) == m_analyzer.getLoopFollow(bb)) {
            cond = Unary::get(opLNot, cond)->simplify();
        }

        addPretestedLoopHeader(cond);

        // write the code for the body of the loop
        const BasicBlock *loopBody = (bb->getSuccessor(BELSE) == m_analyzer.getLoopFollow(bb))
                                         ? bb->getSuccessor(BTHEN)
                                         : bb->getSuccessor(BELSE);
        generateCode(loopBody, m_analyzer.getLatchNode(bb), followSet, gotoSet, proc);

        // if code has not been generated for the latch node, generate it now
        if (!isGenerated(m_analyzer.getLatchNode(bb))) {
            m_generatedBBs.insert(m_analyzer.getLatchNode(bb));
            writeBB(m_analyzer.getLatchNode(bb));
        }

        // rewrite the body of the block (excluding the predicate) at the next nesting level after
        // making sure another label won't be generated
        writeBB(bb);

        // write the loop tail
        addPretestedLoopEnd();
    }
    else {
        // write the loop header
        if (m_analyzer.getLoopType(bb) == LoopType::Endless) {
            addEndlessLoopHeader();
        }
        else {
            addPostTestedLoopHeader();
        }

        // if this is also a conditional header, then generate code for the conditional. Otherwise
        // generate code for the loop body.
        if (m_analyzer.getStructType(bb) == StructType::LoopCond) {
            // set the necessary flags so that generateCode can successfully be called again on this
            // node
            m_analyzer.setStructType(bb, StructType::Cond);
            m_analyzer.setTravType(bb, TravType::Untraversed);
            m_generatedBBs.erase(bb);
            generateCode(bb, m_analyzer.getLatchNode(bb), followSet, gotoSet, proc);
        }
        else {
            writeBB(bb);

            // write the code for the body of the loop
            generateCode(bb->getSuccessor(0), m_analyzer.getLatchNode(bb), followSet, gotoSet,
                         proc);
        }

        if (m_analyzer.getLoopType(bb) == LoopType::PostTested) {
            // if code has not been generated for the latch node, generate it now
            if (!isGenerated(m_analyzer.getLatchNode(bb))) {
                m_generatedBBs.insert(m_analyzer.getLatchNode(bb));
                writeBB(m_analyzer.getLatchNode(bb));
            }

            const BasicBlock *myLatch = m_analyzer.getLatchNode(bb);
            const BasicBlock *myHead  = m_analyzer.getLoopHead(myLatch);
            assert(myLatch->isType(BBType::Twoway));

            SharedExp cond = myLatch->getCond();
            if (myLatch->getSuccessor(BELSE) == myHead) {
                addPostTestedLoopEnd(Unary::get(opLNot, cond)->simplify());
            }
            else {
                addPostTestedLoopEnd(cond->simplify());
            }
        }
        else {
            assert(m_analyzer.getLoopType(bb) == LoopType::Endless);

            // if code has not been generated for the latch node, generate it now
            if (!isGenerated(m_analyzer.getLatchNode(bb))) {
                m_generatedBBs.insert(m_analyzer.getLatchNode(bb));
                writeBB(m_analyzer.getLatchNode(bb));
            }

            // write the closing bracket for an endless loop
            addEndlessLoopEnd();
        }
    }

    // write the code for the follow of the loop (if it exists)
    if (m_analyzer.getLoopFollow(bb)) {
        // remove the follow from the follow set
        followSet.pop_back();

        if (!isGenerated(m_analyzer.getLoopFollow(bb))) {
            generateCode(m_analyzer.getLoopFollow(bb), latch, followSet, gotoSet, proc);
        }
        else {
            emitGotoAndLabel(bb, m_analyzer.getLoopFollow(bb));
        }
    }
}


void CCodeGenerator::generateCode_Branch(const BasicBlock *bb,
                                         std::list<const BasicBlock *> &gotoSet, UserProc *proc,
                                         const BasicBlock *latch,
                                         std::list<const BasicBlock *> &followSet)
{
    // reset this back to LoopCond if it was originally of this type
    if (m_analyzer.getLatchNode(bb) != nullptr) {
        m_analyzer.setStructType(bb, StructType::LoopCond);
    }

    // for 2 way conditional headers that are effectively jumps into
    // or out of a loop or case body, we will need a new follow node
    const BasicBlock *tmpCondFollow = nullptr;

    // keep track of how many nodes were added to the goto set so that
    // the correct number are removed
    int gotoTotal = 0;

    // add the follow to the follow set if this is a case header
    if (m_analyzer.getCondType(bb) == CondType::Case) {
        followSet.push_back(m_analyzer.getCondFollow(bb));
    }
    else if (m_analyzer.getCondFollow(bb) != nullptr) {
        // For a structured two conditional header,
        // its follow is added to the follow set
        // myLoopHead = (sType == LoopCond ? this : loopHead);

        if (m_analyzer.getUnstructType(bb) == UnstructType::Structured) {
            followSet.push_back(m_analyzer.getCondFollow(bb));
        }

        // Otherwise, for a jump into/outof a loop body, the follow is added to the goto set.
        // The temporary follow is set for any unstructured conditional header branch that is within
        // the same loop and case.
        else {
            if (m_analyzer.getUnstructType(bb) == UnstructType::JumpInOutLoop) {
                // define the loop header to be compared against
                const BasicBlock *myLoopHead = (m_analyzer.getStructType(bb) == StructType::LoopCond
                                                    ? bb
                                                    : m_analyzer.getLoopHead(bb));
                gotoSet.push_back(m_analyzer.getCondFollow(bb));
                gotoTotal++;

                // also add the current latch node, and the loop header of the follow if they exist
                if (latch) {
                    gotoSet.push_back(latch);
                    gotoTotal++;
                }

                if (m_analyzer.getLoopHead(m_analyzer.getCondFollow(bb)) &&
                    m_analyzer.getLoopHead(m_analyzer.getCondFollow(bb)) != myLoopHead) {
                    gotoSet.push_back(m_analyzer.getLoopHead(m_analyzer.getCondFollow(bb)));
                    gotoTotal++;
                }
            }

            tmpCondFollow = bb->getSuccessor(
                (m_analyzer.getCondType(bb) == CondType::IfThen) ? BELSE : BTHEN);

            // for a jump into a case, the temp follow is added to the follow set
            if (m_analyzer.getUnstructType(bb) == UnstructType::JumpIntoCase) {
                followSet.push_back(tmpCondFollow);
            }
        }
    }

    // write the body of the block (excluding the predicate)
    writeBB(bb);

    // write the conditional header
    SwitchInfo *psi = nullptr; // Init to nullptr to suppress a warning

    if (m_analyzer.getCondType(bb) == CondType::Case) {
        // The CaseStatement will be in the last RTL this BB
        RTL *last         = bb->getRTLs()->back().get();
        CaseStatement *cs = (CaseStatement *)last->getHlStmt();
        psi               = cs->getSwitchInfo();

        // Write the switch header (i.e. "switch (var) {")
        addCaseCondHeader(psi->switchExp);
    }
    else {
        SharedExp cond = bb->getCond();

        if (!cond) {
            cond = Const::get(Address(0xfeedface)); // hack, but better than a crash
        }

        if (m_analyzer.getCondType(bb) == CondType::IfElse) {
            cond = Unary::get(opLNot, cond->clone());
            cond = cond->simplify();
        }

        if (m_analyzer.getCondType(bb) == CondType::IfThenElse) {
            addIfElseCondHeader(cond);
        }
        else {
            addIfCondHeader(cond);
        }
    }

    // write code for the body of the conditional
    if (m_analyzer.getCondType(bb) != CondType::Case) {
        const BasicBlock *succ = bb->getSuccessor(
            (m_analyzer.getCondType(bb) == CondType::IfElse) ? BELSE : BTHEN);
        assert(succ != nullptr);

        // emit a goto statement if the first clause has already been
        // generated or it is the follow of this node's enclosing loop
        if (isGenerated(succ) || (m_analyzer.getLoopHead(bb) &&
                                  succ == m_analyzer.getLoopFollow(m_analyzer.getLoopHead(bb)))) {
            emitGotoAndLabel(bb, succ);
        }
        else {
            generateCode(succ, latch, followSet, gotoSet, proc);
        }

        // generate the else clause if necessary
        if (m_analyzer.getCondType(bb) == CondType::IfThenElse) {
            // generate the 'else' keyword and matching brackets
            addIfElseCondOption();

            succ = bb->getSuccessor(BELSE);

            // emit a goto statement if the second clause has already
            // been generated
            if (isGenerated(succ)) {
                emitGotoAndLabel(bb, succ);
            }
            else {
                generateCode(succ, latch, followSet, gotoSet, proc);
            }

            // generate the closing bracket
            addIfElseCondEnd();
        }
        else {
            // generate the closing bracket
            addIfCondEnd();
        }
    }
    else {
        // case header

        // first, determine the optimal fall-through ordering
        std::list<std::pair<SharedExp, const BasicBlock *>>
            switchDests = computeOptimalCaseOrdering(bb, psi);

        for (auto it = switchDests.begin(); it != switchDests.end(); ++it) {
            SharedExp caseValue    = it->first;
            const BasicBlock *succ = it->second;

            addCaseCondOption(*caseValue);
            if (std::next(it) != switchDests.end() && std::next(it)->second == succ) {
                // multiple case values; generate the BB only for the last case value
                continue;
            }

            if (isGenerated(succ)) {
                emitGotoAndLabel(bb, succ);
            }
            else {
                generateCode(succ, latch, followSet, gotoSet, proc);
            }
        }

        // generate the closing bracket
        addCaseCondEnd();
    }

    // do all the follow stuff if this conditional had one
    if (m_analyzer.getCondFollow(bb)) {
        // remove the original follow from the follow set if it was
        // added by this header
        if ((m_analyzer.getUnstructType(bb) == UnstructType::Structured) ||
            (m_analyzer.getUnstructType(bb) == UnstructType::JumpIntoCase)) {
            assert(gotoTotal == 0);
            followSet.resize(followSet.size() - 1);
        }
        else { // remove all the nodes added to the goto set
            gotoSet.resize(std::max((int)gotoSet.size() - gotoTotal, 0));
        }

        // do the code generation (or goto emitting) for the new conditional follow if it exists,
        // otherwise do it for the original follow
        if (!tmpCondFollow) {
            tmpCondFollow = m_analyzer.getCondFollow(bb);
        }

        if (isGenerated(tmpCondFollow)) {
            emitGotoAndLabel(bb, tmpCondFollow);
        }
        else {
            generateCode(tmpCondFollow, latch, followSet, gotoSet, proc);
        }
    }
}


void CCodeGenerator::generateCode_Seq(const BasicBlock *bb, std::list<const BasicBlock *> &gotoSet,
                                      UserProc *proc, const BasicBlock *latch,
                                      std::list<const BasicBlock *> &followSet)
{
    // generate code for the body of this block
    writeBB(bb);

    // return if this is the 'return' block (i.e. has no out edges) after emitting a 'return'
    // statement
    if (bb->getType() == BBType::Ret) {
        // This should be emitted now, like a normal statement
        // addReturnStatement(getReturnVal());
        return;
    }

    // return if this doesn't have any out edges (emit a warning)
    if (bb->getNumSuccessors() == 0) {
        LOG_WARN("No out edge for BB at address %1, in proc %2", bb->getLowAddr(), proc->getName());

        if (bb->getType() == BBType::CompJump) {
            assert(!bb->getRTLs()->empty());
            RTL *lastRTL = bb->getRTLs()->back().get();
            assert(!lastRTL->empty());

            GotoStatement *gs = static_cast<GotoStatement *>(lastRTL->back());
            if (gs && gs->getDest()) {
                addLineComment("goto " + gs->getDest()->toString());
            }
            else {
                addLineComment("goto <unknown_dest>");
            }
        }

        return;
    }

    const BasicBlock *succ = bb->getSuccessor(0);

    if (bb->getNumSuccessors() > 1) {
        const BasicBlock *other = bb->getSuccessor(1);
        LOG_MSG("Found seq with more than one outedge!");
        std::shared_ptr<Const> constDest = bb->getDest()->access<Const>();

        if (constDest && constDest->isIntConst() && (constDest->getAddr() == succ->getLowAddr())) {
            std::swap(other, succ);
            LOG_MSG("Taken branch is first out edge");
        }

        SharedExp cond = bb->getCond();

        if (cond) {
            addIfCondHeader(bb->getCond());

            if (isGenerated(other)) {
                emitGotoAndLabel(bb, other);
            }
            else {
                generateCode(other, latch, followSet, gotoSet, proc);
            }

            addIfCondEnd();
        }
        else {
            LOG_ERROR("Last statement is not a cond, don't know what to do with this.");
        }
    }

    // Generate code for its successor if
    //  - it hasn't already been visited and
    //  - is in the same loop/case and
    //  - is not the latch for the current most enclosing loop.
    // The only exception for generating it when it is not in
    // the same loop is when it is only reached from this node
    if (isGenerated(succ)) {
        emitGotoAndLabel(bb, succ);
    }
    else if (m_analyzer.getLoopHead(succ) != m_analyzer.getLoopHead(bb) &&
             (!isAllParentsGenerated(succ) || Util::isContained(followSet, succ))) {
        emitGotoAndLabel(bb, succ);
    }
    else if (latch && m_analyzer.getLoopHead(latch) &&
             (m_analyzer.getLoopFollow(m_analyzer.getLoopHead(latch)) == succ)) {
        emitGotoAndLabel(bb, succ);
    }
    else if (m_analyzer.getCaseHead(succ) && m_analyzer.getCaseHead(bb) &&
             m_analyzer.getCaseHead(bb) != m_analyzer.getCaseHead(succ) &&
             m_analyzer.getCondFollow(m_analyzer.getCaseHead(bb))) {
        emitGotoAndLabel(bb, succ);
    }
    else {
        if (m_analyzer.getCaseHead(bb) &&
            (succ == m_analyzer.getCondFollow(m_analyzer.getCaseHead(bb)))) {
            // generate the 'break' statement
            addCaseCondOptionEnd();
        }
        else if ((m_analyzer.getCaseHead(bb) == nullptr) ||
                 (m_analyzer.getCaseHead(bb) != m_analyzer.getCaseHead(succ)) ||
                 !m_analyzer.isCaseOption(succ)) {
            generateCode(succ, latch, followSet, gotoSet, proc);
        }
    }
}


void CCodeGenerator::emitGotoAndLabel(const BasicBlock *bb, const BasicBlock *dest)
{
    if (m_analyzer.getLoopHead(bb) &&
        ((m_analyzer.getLoopHead(bb) == dest) ||
         (m_analyzer.getLoopFollow(m_analyzer.getLoopHead(bb)) == dest))) {
        if (m_analyzer.getLoopHead(bb) == dest) {
            addContinue();
        }
        else {
            addBreak();
        }
    }
    else if (dest->isType(BBType::Ret)) {
        // a goto to a return -> just emit the return statement
        writeBB(dest);
    }
    else {
        addGoto(dest);
    }
}


void CCodeGenerator::writeBB(const BasicBlock *bb)
{
    if (m_proc->getProg()->getProject()->getSettings()->debugGen) {
        LOG_MSG("Generating code for BB at address %1", bb->getLowAddr());
    }

    // Allocate space for a label to be generated for this node and add this to the generated code.
    // The actual label can then be generated now or back patched later
    addLabel(bb);

    if (bb->getRTLs()) {
        for (const auto &rtl : *(bb->getRTLs())) {
            if (m_proc->getProg()->getProject()->getSettings()->debugGen) {
                LOG_MSG("%1", rtl->getAddress());
            }

            for (const Statement *st : *rtl) {
                emitCodeForStmt(st);
            }
        }
    }
}


void CCodeGenerator::print(const Module *module)
{
    m_writer.writeCode(module, m_lines);
    m_lines.clear();
}


void CCodeGenerator::indent(OStream &str, int indLevel)
{
    // Can probably do more efficiently
    for (int i = 0; i < indLevel; i++) {
        str << "    ";
    }
}


void CCodeGenerator::appendLine(const QString &s)
{
    m_lines.push_back(s);
}


bool CCodeGenerator::isAllParentsGenerated(const BasicBlock *bb) const
{
    for (BasicBlock *pred : bb->getPredecessors()) {
        if (!m_analyzer.isBackEdge(pred, bb) && !isGenerated(pred)) {
            return false;
        }
    }

    return true;
}


bool CCodeGenerator::isGenerated(const BasicBlock *bb) const
{
    return m_generatedBBs.find(bb) != m_generatedBBs.end();
}


void CCodeGenerator::emitCodeForStmt(const Statement *st)
{
    switch (st->getKind()) {
    case StmtType::Assign: {
        this->addAssignmentStatement(static_cast<const Assign *>(st));
        break;
    }
    case StmtType::Call: {
        const CallStatement *call = static_cast<const CallStatement *>(st);
        const Function *dest      = call->getDestProc();

        if ((dest == nullptr) && call->isComputed()) {
            addIndCallStatement(call->getDest(), call->getArguments(), *call->calcResults());
            return;
        }

        std::unique_ptr<StatementList> results = call->calcResults();
        assert(dest);

        if (dest->isLib() && !dest->getSignature()->getPreferredName().isEmpty()) {
            addCallStatement(dest, dest->getSignature()->getPreferredName(), call->getArguments(),
                             *results);
        }
        else {
            addCallStatement(dest, dest->getName(), call->getArguments(), *results);
        }
        break;
    }
    case StmtType::Ret: {
        addReturnStatement(&static_cast<const ReturnStatement *>(st)->getReturns());
        break;
    }
    case StmtType::BoolAssign: {
        const BoolAssign *bas = static_cast<const BoolAssign *>(st);

        // lhs := (m_cond) ? 1 : 0
        Assign as(bas->getLeft()->clone(),
                  Ternary::get(opTern, bas->getCondExpr()->clone(), Const::get(1), Const::get(0)));
        addAssignmentStatement(&as);
        break;
    }
    case StmtType::Branch:
    case StmtType::Goto:
    case StmtType::Case:
        // these will be handled by the BB
        break;
    case StmtType::PhiAssign: LOG_ERROR("Encountered Phi Assign in back end"); break;
    case StmtType::ImpAssign: LOG_ERROR("Encountered Implicit Assign in back end"); break;
    case StmtType::INVALID: LOG_ERROR("Encountered Invalid Statement in back end"); break;
    }
}


std::list<std::pair<SharedExp, const BasicBlock *>>
CCodeGenerator::computeOptimalCaseOrdering(const BasicBlock *caseHead, SwitchInfo *psi)
{
    using CaseEntry = std::pair<SharedExp, const BasicBlock *>;
    std::list<CaseEntry> result;

    for (int i = 0; i < caseHead->getNumSuccessors(); ++i) {
        const BasicBlock *origSucc = caseHead->getSuccessor(i);
        SharedExp caseVal;
        if (psi->switchType == SwitchType::F) { // "Fortran" style?
            // Yes, use the table value itself
            caseVal = Const::get(reinterpret_cast<int *>(psi->tableAddr.value())[i]);
        }
        else {
            // Note that uTable has the address of an int array
            caseVal = Const::get(static_cast<int>(psi->lowerBound + i));
        }

        const BasicBlock *realSucc = origSucc;
        while (realSucc->getNumSuccessors() == 1 &&
               (realSucc->isEmpty() || realSucc->isEmptyJump())) {
            realSucc = realSucc->getSuccessor(0);
        }

        result.push_back({ caseVal, realSucc });
    }

    result.sort([](const CaseEntry &left, const CaseEntry &right) {
        const BasicBlock *leftBB  = left.second;
        const BasicBlock *rightBB = right.second;

        const BasicBlock *leftSucc = leftBB;

        while (leftSucc->getType() != BBType::Ret) {
            if (leftSucc == rightBB) {
                return true; // the left case is a fallthrough to the right case
            }
            else if (leftSucc->getNumSuccessors() != 1) {
                break;
            }

            leftSucc = leftSucc->getSuccessor(0);
        }

        // No fallthrough found; compare by address
        return leftBB->getLowAddr() < rightBB->getLowAddr();
    });

    return result;
}


BOOMERANG_DEFINE_PLUGIN(PluginType::CodeGenerator, CCodeGenerator, "C Code Generator plugin",
                        BOOMERANG_VERSION, "Boomerang developers")
