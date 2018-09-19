#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "RTLTest.h"



#include "boomerang/ssl/RTL.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/module/Module.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/exp/Ternary.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/ssl/statements/BoolAssign.h"
#include "boomerang/ssl/statements/GotoStatement.h"
#include "boomerang/ssl/statements/BranchStatement.h"
#include "boomerang/ssl/statements/Assign.h"
#include "boomerang/ssl/statements/CaseStatement.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/ReturnStatement.h"
#include "boomerang/frontend/pentium/PentiumFrontEnd.h"
#include "boomerang/frontend/sparc/SPARCFrontEnd.h"
#include "boomerang/ssl/type/IntegerType.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/visitor/stmtvisitor/StmtVisitor.h"

#include <sstream>


void RTLTest::testAppend()
{
    Assign *a = new Assign(Location::regOf(REG_SPARC_O0), Binary::get(opPlus, Location::regOf(REG_SPARC_O1), Const::get(99)));
    RTL    r(Address::ZERO, { a });

    QString     res;
    OStream ost(&res);
    r.print(ost);

    QCOMPARE(res, QString("0x00000000    0 *v* r8 := r9 + 99\n"));
}


class StmtVisitorStub : public StmtVisitor
{
public:
    bool a, b, c, d, e, f, g, h;

    void clear() { a = b = c = d = e = f = g = h = false; }
    StmtVisitorStub() { clear(); }
    virtual ~StmtVisitorStub() {}
    virtual bool visit(const RTL *) override
    {
        a = true;
        return false;
    }

    virtual bool visit(const GotoStatement *) override
    {
        b = true;
        return false;
    }

    virtual bool visit(const BranchStatement *) override
    {
        c = true;
        return false;
    }

    virtual bool visit(const CaseStatement *) override
    {
        d = true;
        return false;
    }

    virtual bool visit(const CallStatement *) override
    {
        e = true;
        return false;
    }

    virtual bool visit(const ReturnStatement *) override
    {
        f = true;
        return false;
    }

    virtual bool visit(const BoolAssign *) override
    {
        g = true;
        return false;
    }

    virtual bool visit(const Assign *) override
    {
        h = true;
        return false;
    }
};


void RTLTest::testVisitor()
{
    StmtVisitorStub *visitor = new StmtVisitorStub();

    /* jump stmt */
    GotoStatement *jump = new GotoStatement;

    jump->accept(visitor);
    QVERIFY(visitor->b);
    delete jump;

    /* branch stmt */
    BranchStatement *jcond = new BranchStatement;
    jcond->accept(visitor);
    QVERIFY(visitor->c);
    delete jcond;

    /* nway jump stmt */
    CaseStatement *nwayjump = new CaseStatement;
    nwayjump->accept(visitor);
    QVERIFY(visitor->d);
    delete nwayjump;

    /* call stmt */
    CallStatement *call = new CallStatement;
    call->accept(visitor);
    QVERIFY(visitor->e);
    delete call;

    /* return stmt */
    ReturnStatement *ret = new ReturnStatement;
    ret->accept(visitor);
    QVERIFY(visitor->f);
    delete ret;

    /* "bool" assgn */
    BoolAssign *scond = new BoolAssign(0);
    scond->accept(visitor);
    QVERIFY(visitor->g);
    delete scond;

    /* assignment stmt */
    Assign *as = new Assign;
    as->accept(visitor);
    QVERIFY(visitor->h);
    delete as;

    /* polymorphic */
    Statement *s = new CallStatement;
    s->accept(visitor);
    QVERIFY(visitor->e);
    delete s;

    /* cleanup */
    delete visitor;
}


// void RtlTest::testIsCompare() {
//    BinaryFileFactory bff;
//    BinaryFile *pBF = bff.Load(SWITCH_SPARC);
//    QVERIFY(pBF != 0);
//    QVERIFY(pBF->GetMachine() == MACHINE_SPARC);
//    Prog* prog = new Prog;
//    FrontEnd *pFE = new SPARCFrontEnd(pBF, prog, &bff);
//    prog->setFrontEnd(pFE);

//    // Decode second instruction: "sub        %i0, 2, %o1"
//    int iReg;
//    Exp* eOperand = nullptr;
//    DecodeResult inst = pFE->decodeInstruction(Address(0x10910));
//    QVERIFY(inst.rtl != nullptr);
//    QVERIFY(inst.rtl->isCompare(iReg, eOperand) == false);

//    // Decode fifth instruction: "cmp        %o1, 5"
//    inst = pFE->decodeInstruction(0x1091c);
//    QVERIFY(inst.rtl != nullptr);
//    QVERIFY(inst.rtl->isCompare(iReg, eOperand) == true);
//    QCOMPARE(iReg,9);
//    QString expected("5");
//    std::ostringstream ost1;
//    eOperand->print(ost1);
//    QString actual(ost1.str());
//    QCOMPARE(actual,expected);

//    pBF->UnLoad();
//    delete pBF;
//    delete pFE;
//    pBF = bff.Load(SWITCH_PENT);
//    QVERIFY(pBF != 0);
//    QVERIFY(pBF->GetMachine() == MACHINE_PENTIUM);
//    pFE = new PentiumFrontEnd(pBF, prog, &bff);
//    prog->setFrontEnd(pFE);

//    // Decode fifth instruction: "cmp    $0x5,%eax"
//    inst = pFE->decodeInstruction(0x80488fb);
//    QVERIFY(inst.rtl != nullptr);
//    QVERIFY(inst.rtl->isCompare(iReg, eOperand) == true);
//    QCOMPARE(iReg,24);
//    std::ostringstream ost2;
//    eOperand->print(ost2);
//    actual = ost2.str();
//    QCOMPARE(actual,expected);

//    // Decode instruction: "add        $0x4,%esp"
//    inst = pFE->decodeInstruction(0x804890c);
//    QVERIFY(inst.rtl != nullptr);
//    QVERIFY(inst.rtl->isCompare(iReg, eOperand) == false);
//    pBF->UnLoad();
//    delete pFE;
// }


QTEST_GUILESS_MAIN(RTLTest)
