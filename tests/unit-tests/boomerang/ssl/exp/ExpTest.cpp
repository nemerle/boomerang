#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "ExpTest.h"


#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/exp/RefExp.h"
#include "boomerang/ssl/exp/Terminal.h"
#include "boomerang/ssl/exp/Ternary.h"
#include "boomerang/ssl/exp/TypedExp.h"
#include "boomerang/ssl/statements/Assign.h"
#include "boomerang/visitor/expvisitor/FlagsFinder.h"
#include "boomerang/ssl/type/IntegerType.h"
#include "boomerang/ssl/type/CharType.h"
#include "boomerang/ssl/type/PointerType.h"
#include "boomerang/ssl/type/FloatType.h"
#include "boomerang/util/LocationSet.h"

#include <map>


Q_DECLARE_METATYPE(LocationSet)


void ExpTest::initTestCase()
{
    BoomerangTest::initTestCase();

    m_99 = Const::get(99);
    m_rof2.reset(new Location(opRegOf, Const::get(REG_SPARC_G2), nullptr));
}


void ExpTest::test99()
{
    QString     actual;
    OStream ost(&actual);

    m_99->print(ost);
    QCOMPARE(actual, QString("99"));
}


void ExpTest::testFlt()
{
    QString     actual;
    OStream ost(&actual);

    std::shared_ptr<Const> c = Const::get(3.14);
    c->print(ost);
    QCOMPARE(actual, QString("%1").arg(3.14));
}


void ExpTest::testRegOf2()
{
    QString     actual;
    OStream ost(&actual);

    ost << m_rof2;
    QCOMPARE(actual, QString("r2"));
}


void ExpTest::testBinaries()
{
    QCOMPARE(Binary::get(opPlus,  m_99->clone(), m_rof2->clone())->toString(), QString("99 + r2"));
    QCOMPARE(Binary::get(opMinus, m_99->clone(), m_rof2->clone())->toString(), QString("99 - r2"));
    QCOMPARE(Binary::get(opMult,  m_99->clone(), m_rof2->clone())->toString(), QString("99 * r2"));
    QCOMPARE(Binary::get(opDiv,   m_99->clone(), m_rof2->clone())->toString(), QString("99 / r2"));
    QCOMPARE(Binary::get(opMults, m_99->clone(), m_rof2->clone())->toString(), QString("99 *! r2"));
    QCOMPARE(Binary::get(opDivs,  m_99->clone(), m_rof2->clone())->toString(), QString("99 /! r2"));
    QCOMPARE(Binary::get(opMod,   m_99->clone(), m_rof2->clone())->toString(), QString("99 % r2"));
    QCOMPARE(Binary::get(opMods,  m_99->clone(), m_rof2->clone())->toString(), QString("99 %! r2"));
}


void ExpTest::testUnaries()
{
    QCOMPARE(Unary::get(opNot,  Terminal::get(opZF))->toString(), QString("~%ZF"));
    QCOMPARE(Unary::get(opLNot, Terminal::get(opZF))->toString(), QString("L~%ZF"));
    QCOMPARE(Unary::get(opNeg,  Terminal::get(opZF))->toString(), QString("-%ZF"));
}


void ExpTest::testIsAfpTerm()
{
    SharedExp afp   = Terminal::get(opAFP);
    SharedExp plus  = Binary::get(opPlus, afp->clone(), Const::get(-99));
    SharedExp minus = Binary::get(opMinus, afp->clone(), m_99->clone());

    QVERIFY(afp->isAfpTerm());
    QVERIFY(plus->isAfpTerm());
    QVERIFY(minus->isAfpTerm());
    QVERIFY(!m_99->isAfpTerm());
    QVERIFY(!m_rof2->isAfpTerm());

    // Now with typed expressions
    SharedExp tafp = std::make_shared<TypedExp>(IntegerType::get(Address::getSourceBits()), afp->clone());
    // Unary tafp  (opTypedExp, afp.clone());
    SharedExp tplus  = Unary::get(opTypedExp, plus->clone());
    SharedExp tminus = Unary::get(opTypedExp, minus->clone());

    QVERIFY(tafp->isAfpTerm());
    QVERIFY(tplus->isAfpTerm());
    QVERIFY(tminus->isAfpTerm());
}


void ExpTest::testCompare1()
{
    QVERIFY(!(*m_99 == *m_rof2));
}


void ExpTest::testCompare2()
{
    QCOMPARE(*m_99, *Const::get(99));
}


void ExpTest::testCompare3()
{
    QVERIFY(!(*m_99 == *Const::get(-99)));
}


void ExpTest::testCompare4()
{
    QVERIFY(*m_rof2 == *Location::regOf(REG_SPARC_G2));
}


void ExpTest::testCompare5()
{
    SharedExp original  = Binary::get(opMult, m_99->clone(), m_rof2->clone());
    SharedExp commuted  = Binary::get(opMult, m_rof2->clone(), m_99->clone());
    SharedExp original2 = Binary::get(opMult, m_99->clone(), m_rof2->clone());

    QVERIFY(*original != *commuted);
    QVERIFY(*original == *original2);
}


void ExpTest::testSearchReplace1()
{
    // Null test: should not replace.
    SharedExp p = Ternary::get(opAt, m_rof2->clone(), Const::get(8), Const::get(15));

    bool change;
    SharedExp result = p->searchReplace(*m_99, m_rof2, change);
    QCOMPARE(*result, *p);
}


void ExpTest::testSearchReplace2()
{
    // Whole expression replacements
    bool      change;
    SharedExp p1 = Const::get(55);
    SharedExp c2 = Const::get(1234);

    p1 = p1->searchReplace(*p1, c2, change);
    QVERIFY(*p1 == *c2);
    QVERIFY(p1 != c2); // Pointers should not be same

    p1 = p1->searchReplace(*c2, m_rof2, change);
    QVERIFY(*p1 == *m_rof2);
}


void ExpTest::testSearchReplace3()
{
    // Subexpression replacement
    bool      change;
    SharedExp two   = Const::get(2);
    SharedExp three = Const::get(3);
    SharedExp p     = m_rof2->clone();

    SharedExp result = p->searchReplaceAll(*two, three, change);
    QCOMPARE(*result, *Location::regOf(3));
}


void ExpTest::testSearchReplace4()
{
    // Subexpression replacement with different subexpression form
    bool      change;
    SharedExp p = m_rof2->clone();

    // Note recursion. OK to use the all function,
    // since it does the search first.
    SharedExp result = p->searchReplaceAll(*Const::get(2), m_rof2, change);

    QCOMPARE(*result, *Location::regOf(Location::regOf(2)));
}


void ExpTest::testSearch1()
{
    SharedExp two = Const::get(2);
    SharedExp result;

    // Search inside r2 for const 2
    QVERIFY(m_rof2->search(*two, result));
    QVERIFY(*result == *two);

    // Test for false positives
    QVERIFY(!m_99->search(*two, result));
    QVERIFY(!m_rof2->search(*m_99, result));
    QVERIFY(result == nullptr);

    SharedExp e = Binary::get(opDiv, m_rof2->clone(), m_99->clone()); // r2 / 99
    Const     three(3);
    QVERIFY(!e->search(three, result));
    QVERIFY(result == nullptr);
}


void ExpTest::testSearch2()
{
    // Search using wildcards
    SharedExp e = Binary::get(opDivs, m_rof2->clone(), m_99->clone());         // r2 /! 99
    SharedExp result;
    SharedExp search = Location::get(opRegOf, Terminal::get(opWild), nullptr); // r[?]

    QVERIFY(e->search(*search, result));
    QVERIFY(result != nullptr);
    QVERIFY(*result == *m_rof2); // Should be r2

    QVERIFY(!e->search(*Const::get(3), result));
    QVERIFY(result == nullptr);

    QVERIFY(e->search(*m_99, result));
    QVERIFY(result != nullptr);
}


void ExpTest::testSearch3()
{
    // A more complex expression:
    // (r2 * 99) + (m[1000] * 4)
    SharedExp result;
    SharedExp e = Binary::get(opPlus,
                              Binary::get(opMult,
                                          m_rof2->clone(),
                                          m_99->clone()),
                              Binary::get(opMult,
                                          Location::memOf(Const::get(1000)),
                                          Const::get(4)));

    SharedExp mem1000 = Location::memOf(Const::get(1000), nullptr);
    SharedExp prod    = Binary::get(opMult, m_rof2->clone(), m_99->clone());

    QVERIFY(e->search(*Const::get(4), result));
    QVERIFY(result != nullptr);

    QVERIFY(e->search(*mem1000, result));
    QVERIFY(result != nullptr);

    QVERIFY(e->search(*prod, result));
    QVERIFY(result != nullptr);

    QVERIFY(e->search(*m_99, result));
    QVERIFY(result != nullptr);

    QVERIFY(!e->search(*Const::get(3), result));
    QVERIFY(result == nullptr);
}


void ExpTest::testSearchAll()
{
    SharedExp search = Location::get(opRegOf, Terminal::get(opWild), nullptr); // r[?]

    // A more complex expression:
    // (r2 * 99) + (r8 * 4)
    std::list<SharedExp> result;
    SharedExp            e = Binary::get(opPlus,
                                         Binary::get(opMult,
                                                     m_rof2->clone(),
                                                     m_99->clone()),
                                         Binary::get(opMult,
                                                     Location::regOf(REG_PENT_AL),
                                                     Const::get(4)));

    QVERIFY(e->searchAll(*search, result));
    QVERIFY(result.size() == 2);
    QVERIFY(*result.front() == *m_rof2);
    Location rof8(opRegOf, Const::get(REG_SPARC_O0), nullptr);
    QVERIFY(*result.back() == rof8);
}


void ExpTest::testAccumulate()
{
    SharedExp rof2     = Location::get(opRegOf, Const::get(REG_SPARC_G2), nullptr);
    SharedExp nineNine = Const::get(99);

    // Zero terms
    std::list<SharedExp> le;
    QVERIFY(*Exp::accumulate(le) == *Const::get(0));

    // One term
    le.push_back(rof2);
    QVERIFY(*Exp::accumulate(le) == *rof2);

    // Two terms
    SharedExp nn = nineNine->clone();
    le.push_back(nn);

    Binary expected2(opPlus, rof2->clone(), nineNine->clone());
    QVERIFY(*Exp::accumulate(le) == expected2);

    // Three terms, one repeated
    le.push_back(nineNine);
    Binary expected3(opPlus,
                     rof2->clone(),
                     Binary::get(opPlus,
                                 nineNine->clone(),
                                 nineNine->clone()));
    QVERIFY(*Exp::accumulate(le) == expected3);

    // Four terms, one repeated
    le.push_back(Terminal::get(opAFP));
    Binary expected4(opPlus, rof2->clone(),
                     Binary::get(opPlus,
                                 nineNine->clone(),
                                 Binary::get(opPlus,
                                             nineNine->clone(),
                                             Terminal::get(opAFP))));
    QVERIFY(*Exp::accumulate(le) == expected4);
}


void ExpTest::testPartitionTerms()
{
    // afp + 108 + n - (afp + 92)
    Binary e(opMinus,
             Binary::get(opPlus,
                         Binary::get(opPlus,
                                     Terminal::get(opAFP),
                                     Const::get(108)),
                         Unary::get(opParam, Const::get("n"))),
             Binary::get(opPlus,
                         Terminal::get(opAFP),
                         Const::get(92)));

    std::list<SharedExp> positives, negatives;
    std::vector<int>     integers;

    e.partitionTerms(positives, negatives, integers, false);
    SharedExp res = Exp::accumulate(positives);
    Binary    expected1(opPlus,
                        Terminal::get(opAFP),
                        Unary::get(opParam, Const::get("n")));
    QVERIFY(*res == expected1);

    res = Exp::accumulate(negatives);
    Terminal expected2(opAFP);
    QVERIFY(*res == expected2);

    QCOMPARE(integers.size(), static_cast<size_t>(2));
    QCOMPARE(integers.front(), 108);
    QCOMPARE(integers.back(), -92);
}


void ExpTest::testSimplify()
{
    QFETCH(SharedExpWrapper, exp);
    QFETCH(SharedExpWrapper, expectedResult);

    SharedConstExp actual = exp->simplify();
    SharedConstExp expected = *expectedResult;

    QCOMPARE(*actual, *expected);

    // make sure to not clone the expression if it does not change
    if (**exp == *actual) {
        QVERIFY(*exp == actual);
    }
}


#define TEST_SIMPLIFY(name, exp, result) \
    QTest::newRow((name)) << SharedExpWrapper(exp) << SharedExpWrapper(result)

void ExpTest::testSimplify_data()
{
    QTest::addColumn<SharedExpWrapper>("exp");
    QTest::addColumn<SharedExpWrapper>("expectedResult");

    TEST_SIMPLIFY("negConst",  Unary::get(opNeg, Const::get(55)),    Const::get(-55));
    TEST_SIMPLIFY("notConst",  Unary::get(opNot, Const::get(0x55AA)), Const::get(0xFFFFAA55));
    TEST_SIMPLIFY("LNotConst", Unary::get(opLNot, Const::get(55)),   Const::get(0));
    TEST_SIMPLIFY("LNotZero",  Unary::get(opLNot, Const::get(0)),    Const::get(1));
    TEST_SIMPLIFY("NegLocal",  Unary::get(opNeg, Unary::get(opLocal, Const::get("abc"))),
                  Unary::get(opNeg, Unary::get(opLocal, Const::get("abc"))));

    TEST_SIMPLIFY("plusConst", Binary::get(opPlus,    Const::get(2),    Const::get(3)),    Const::get(5));
    TEST_SIMPLIFY("multConst", Binary::get(opMult,    Const::get(2),    Const::get(3)),    Const::get(6));
    TEST_SIMPLIFY("shlConst",  Binary::get(opShiftL,  Const::get(2),    Const::get(3)),    Const::get(16));
    TEST_SIMPLIFY("sarConst",  Binary::get(opShiftRA, Const::get(-144), Const::get(3)),    Const::get(-18));
    TEST_SIMPLIFY("xorConst",  Binary::get(opBitXor,  Const::get(0x55), Const::get(0x0F)), Const::get(0x5A));
    TEST_SIMPLIFY("xorSelf",   Binary::get(opBitXor,  m_rof2->clone(),  m_rof2->clone()),  Const::get(0));
    TEST_SIMPLIFY("commute",   Binary::get(opMults,   Const::get(77),   m_rof2->clone()),  Binary::get(opMults, m_rof2->clone(), Const::get(77)));
    TEST_SIMPLIFY("mult1",     Binary::get(opMult,    m_rof2->clone(),  Const::get(1)),    m_rof2->clone());
    TEST_SIMPLIFY("or0",       Binary::get(opBitOr,   Const::get(0),    m_rof2->clone()),  m_rof2->clone());
    TEST_SIMPLIFY("shlZero",   Binary::get(opShiftL,  m_rof2->clone(),  Const::get(0)),    m_rof2->clone());
    TEST_SIMPLIFY("shlMult",   Binary::get(opShiftL,  m_rof2->clone(),  Const::get(2)),    Binary::get(opMult, m_rof2->clone(), Const::get(4)));

    // As of June 2003, I've decided to go the old way. esp + -4 is just
    // too ugly, and all the code has to cope with pluses and minuses anyway,
    // just in case
    TEST_SIMPLIFY("addNeg", Binary::get(opPlus,    m_rof2->clone(),  Const::get(-99)),     Binary::get(opMinus, m_rof2->clone(), Const::get(99)));

    SharedExp   e = Binary::get(opBitOr,
                                Binary::get(opMinus,
                                            Binary::get(opPlus,
                                                        Const::get(0),
                                                        Unary::get(opParam,
                                                                   Const::get("a"))),
                                            Const::get(0)),
                                Const::get(0));
    TEST_SIMPLIFY("complex", e, Unary::get(opParam, Const::get("a")));
}


void ExpTest::testSimplifyBinary()
{
    // r27 := m[r29 + -4]
    std::shared_ptr<Assign> as(new Assign(Location::regOf(REG_PENT_EBX),
                                          Location::memOf(Binary::get(opPlus,
                                                                      Location::regOf(REG_PENT_EBP),
                                                                      Const::get(-4)))));
    as->simplify();
    QCOMPARE(as->prints(), QString("   0 *v* r27 := m[r29 - 4]"));
}


void ExpTest::testSimplifyAddr()
{
    // a[m[1000]] - a[m[r2]{64}]@0:15
    SharedExp e = Binary::get(opMinus,
                              Unary::get(opAddrOf, Location::memOf(Const::get(1000))),
                              Ternary::get(opAt,
                                           Unary::get(opAddrOf,
                                                      Binary::get(opSize,
                                                                  Const::get(64),
                                                                  Location::memOf(Location::regOf(REG_SPARC_G2)))),
                                           Const::get(0),
                                           Const::get(15)));
    QCOMPARE(QString(e->simplifyAddr()->toString()), QString("1000 - (r2@0:15)"));

    // Now test at top level
    e = Unary::get(opAddrOf, Location::memOf(Const::get(1000)));
    QCOMPARE(QString(e->simplifyAddr()->toString()), QString("1000"));
}


void ExpTest::testLess()
{
    // Simple constants
    Const two(2), three(3), minusThree(-3), twoPointTwo(2.2), threePointThree(3.3);
    Const minusThreePointThree(-3.3);

    QVERIFY(two < three);
    QVERIFY(minusThree < two);
    QVERIFY(twoPointTwo < threePointThree);
    QVERIFY(minusThreePointThree < twoPointTwo);

    // Terminal
    Terminal afp(opAFP), agp(opAGP);
    QVERIFY((opAFP < opAGP) == (afp < agp));

    // Unary
    Unary negTwo(opNeg, Const::get(2)), negThree(opNeg, Const::get(3));
    // Note that the ordering is not arithmetic!
    QVERIFY(negTwo < negThree);
    // Binary
    Binary twoByThr(opMult, Const::get(2), Const::get(3));
    Binary twoByFou(opMult, Const::get(2), Const::get(4));
    Binary thrByThr(opMult, Const::get(3), Const::get(3));
    QVERIFY(twoByThr < twoByFou);
    QVERIFY(twoByThr < thrByThr);

    // Ternary
    Ternary twoAtThrToFou(opAt, Const::get(2), Const::get(3), Const::get(4));
    Ternary twoAtThrToFiv(opAt, Const::get(2), Const::get(3), Const::get(5));
    QVERIFY(twoAtThrToFou < twoAtThrToFiv);
    // TypedExp later
}


void ExpTest::testMapOfExp()
{
    std::map<SharedExp, int, lessExpStar> m;
    m[m_rof2] = 200;
    m[m_99]   = 99;
    SharedExp e = Binary::get(opPlus,
                              Const::get(0),
                              Binary::get(opMinus,
                                          Binary::get(opMult,
                                                      Const::get(2),
                                                      Const::get(3)),
                                          Binary::get(opMult,
                                                      Const::get(4),
                                                      Const::get(5))));

    m[e] = -100;
    SharedExp rof2 = Location::get(opRegOf, Const::get(REG_SPARC_G2), nullptr);
    m[rof2] = 2; // Should overwrite

    QCOMPARE(m.size(), static_cast<size_t>(3));
    QCOMPARE(m[m_rof2], 2);
    QCOMPARE(m[m_rof2], 2);
    QCOMPARE(m[m_99],   99);
    QCOMPARE(m[e],      -100);
}


void ExpTest::testList()
{
    QCOMPARE(Binary::get(opList, Terminal::get(opNil), Terminal::get(opNil))->toString(), QString(""));

    // 1 element list
    SharedExp e = Binary::get(opList,
                              Location::get(opParam, Const::get("a"), nullptr),
                              Terminal::get(opNil));
    QCOMPARE(e->toString(), QString("a"));

    // 2 element list
    e = Binary::get(opList,
                    Location::get(opParam, Const::get("a"), nullptr),
                    Binary::get(opList,
                                Location::get(opParam, Const::get("b"), nullptr),
                                Terminal::get(opNil)));
    QCOMPARE(e->toString(), QString("a, b"));

    // 3 element list
    e = Binary::get(opList,
                    Location::get(opParam, Const::get("a"), nullptr),
                    Binary::get(opList,
                                Location::get(opParam, Const::get("b"), nullptr),
                                Binary::get(opList,
                                            Location::get(opParam, Const::get("c"), nullptr),
                                            Terminal::get(opNil))));
    QCOMPARE(e->toString(), QString("a, b, c"));

    // 4 element list
    e = Binary::get(opList,
                    Location::get(opParam, Const::get("a"), nullptr),
                    Binary::get(opList,
                                Location::get(opParam, Const::get("b"), nullptr),
                                Binary::get(opList,
                                            Location::get(opParam, Const::get("c"), nullptr),
                                            Binary::get(opList,
                                                        Location::get(opParam, Const::get("d"), nullptr),
                                                        Terminal::get(opNil)))));
    QCOMPARE(e->toString(), QString("a, b, c, d"));
}


void ExpTest::testParen()
{
    Assign a(Location::regOf(Location::get(opParam, Const::get("rd"), nullptr)),
             Binary::get(opBitAnd,
                         Location::regOf(Location::get(opParam, Const::get("rs1"), nullptr)),
                         Binary::get(opMinus,
                                     Binary::get(opMinus,
                                                 Const::get(0),
                                                 Location::get(opParam, Const::get("reg_or_imm"), nullptr)),
                                     Const::get(1))));


    QCOMPARE(a.prints(), QString("   0 *v* r[rd] := r[rs1] & ((0 - reg_or_imm) - 1)"));
}


void ExpTest::testFixSuccessor()
{
    // Trivial test (should not affect)
    SharedExp b = Binary::get(opMinus,
                              m_99->clone(),
                              m_rof2->clone());
    QCOMPARE(*b->fixSuccessor(), *b);

    b = Unary::get(opSuccessor, Location::regOf(REG_SPARC_G2));
    QCOMPARE(*b->fixSuccessor(), *Location::regOf(REG_SPARC_G3));
}


void ExpTest::testAssociativity()
{
    // (r8 + m[m[r8 + 12] + -12]) + 12
    SharedExp e1 = Binary::get(opPlus,
                               Binary::get(opPlus,
                                           Location::regOf(REG_SPARC_O0),
                                           Location::memOf(Binary::get(opPlus,
                                                                       Location::memOf(Binary::get(opPlus,
                                                                                                   Location::regOf(REG_SPARC_O0),
                                                                                                   Const::get(12))),
                                                                       Const::get(-12)))),
                               Const::get(12));

    // (r8 + 12) + m[m[r8 + 12] + -12]
    SharedExp e2 = Binary::get(opPlus,
                               Binary::get(opPlus,
                                           Location::regOf(REG_SPARC_O0),
                                           Const::get(12)),
                               Location::memOf(Binary::get(opPlus,
                                                           Location::memOf(Binary::get(opPlus,
                                                                                       Location::regOf(REG_SPARC_O0),
                                                                                       Const::get(12))),
                                                           Const::get(-12))));

    // Note: at one stage, simplifyArith was part of simplify().
    // Now call simplifyArith() explicitly only where needed
    QCOMPARE(*e1->simplify()->simplifyArith(), *e2->simplify()->simplifyArith());
}


void ExpTest::testSubscriptVar()
{
    // m[r28 - 4] := r28 + r29
    SharedExp lhs = Location::memOf(Binary::get(opMinus,
                                                 Location::regOf(REG_PENT_ESP),
                                                 Const::get(4)));

    Assign    *ae  = new Assign(lhs->clone(),
                                Binary::get(opPlus,
                                            Location::regOf(REG_PENT_ESP),
                                            Location::regOf(REG_PENT_EBP)));

    // Subtest 1: should do nothing
    SharedExp r28   = Location::regOf(REG_PENT_ESP);
    Statement *def1 = new Assign(r28->clone(), r28->clone());

    def1->setNumber(12);
    def1->subscriptVar(lhs, def1); // Should do nothing

    QCOMPARE(ae->prints(), QString("   0 *v* m[r28 - 4] := r28 + r29"));

    // m[r28 - 4]

    // Subtest 2: Ordinary substitution, on LHS and RHS
    ae->subscriptVar(r28, def1);
    QCOMPARE(ae->prints(), QString("   0 *v* m[r28{12} - 4] := r28{12} + r29"));


    // Subtest 3: change to a different definition
    // 99: r28 := 0
    // Note: behaviour has changed. Now, we don't allow re-renaming, so it should stay the same
    Statement *def3 = new Assign(Location::regOf(REG_PENT_ESP), Const::get(0));
    def3->setNumber(99);
    ae->subscriptVar(r28, def3);
    QCOMPARE(ae->prints(), QString("   0 *v* m[r28{12} - 4] := r28{12} + r29"));

    delete def1;
    delete def3;
    delete ae;
}


void ExpTest::testTypeOf()
{
    // T[r24{5}] = T[r25{9}]
    Statement *s5 = new Assign;
    Statement *s9 = new Assign;

    s5->setNumber(5);
    s9->setNumber(9);
    SharedExp e = Binary::get(opEquals,
        Unary::get(opTypeOf, RefExp::get(Location::regOf(REG_PENT_EAX), s5)),
        Unary::get(opTypeOf, RefExp::get(Location::regOf(REG_PENT_ECX), s9)));

    QCOMPARE(e->toString(), QString("T[r24{5}] = T[r25{9}]"));

    delete s5;
    delete s9;
}


void ExpTest::testAddUsedLocs()
{
    QFETCH(SharedExpWrapper, usedExp);
    QFETCH(LocationSet,      usedLocs);

    LocationSet locSet;
    (*usedExp)->addUsedLocs(locSet);
    QCOMPARE(locSet, usedLocs);
}


#define TEST_ADDUSEDLOCS(name, usedExp, locs) \
    QTest::newRow(name) << SharedExpWrapper(usedExp) << locs;


void ExpTest::testAddUsedLocs_data()
{
    QTest::addColumn<SharedExpWrapper>("usedExp");
    QTest::addColumn<LocationSet>("usedLocs");

    TEST_ADDUSEDLOCS("nil",         Terminal::get(opNil),           LocationSet());
    TEST_ADDUSEDLOCS("strConst",    Const::get("foo"),              LocationSet());
    TEST_ADDUSEDLOCS("pc",          Terminal::get(opPC),            LocationSet({ Terminal::get(opPC) }));
    TEST_ADDUSEDLOCS("reg",         Location::regOf(REG_PENT_ESP),  LocationSet({ Location::regOf(REG_PENT_ESP) }));

    TEST_ADDUSEDLOCS("memof",       Location::memOf(Binary::get(opMinus,
                                                                Location::regOf(REG_PENT_ESP),
                                                                Const::get(4))),
                                    LocationSet({ Location::regOf(REG_PENT_ESP),
                                                  Location::memOf(Binary::get(opMinus,
                                                                              Location::regOf(REG_PENT_ESP),
                                                                              Const::get(4))) }));
    TEST_ADDUSEDLOCS("addrofMemof", Unary::get(opAddrOf,
                                               Location::memOf(Binary::get(opMinus,
                                                               Location::regOf(REG_PENT_ESP),
                                                               Const::get(4)))),
                                    LocationSet({ Location::regOf(REG_PENT_ESP),
                                                  Location::memOf(Binary::get(opMinus,
                                                                              Location::regOf(REG_PENT_ESP),
                                                                              Const::get(4))) }));

    TEST_ADDUSEDLOCS("binary",      Binary::get(opPlus,
                                                Location::regOf(REG_PENT_EAX),
                                                Location::regOf(REG_PENT_ECX)),
                                    LocationSet({ Location::regOf(REG_PENT_EAX), Location::regOf(REG_PENT_ECX) }));

    TEST_ADDUSEDLOCS("ternary",     Ternary::get(opAt,
                                                 Location::regOf(REG_PENT_EAX),
                                                 Location::regOf(REG_PENT_ECX),
                                                 Location::regOf(REG_PENT_EDX)),
                                    LocationSet({ Location::regOf(REG_PENT_EAX),
                                                  Location::regOf(REG_PENT_ECX),
                                                  Location::regOf(REG_PENT_EDX) }));

    SharedExp e = Location::regOf(REG_PENT_ESP);
    Assign a(e, e);
    a.setNumber(1);

    SharedExp ref = RefExp::get(e->clone(), &a);
    TEST_ADDUSEDLOCS("refexp",      ref, LocationSet({ ref }));

    SharedExp memof = Location::memOf(Binary::get(opMinus, ref, Const::get(4)));
    Assign a2(e, e);
    a2.setNumber(2);

    TEST_ADDUSEDLOCS("memofRef", RefExp::get(memof, &a2),
                                 LocationSet({ ref, RefExp::get(memof, &a2)}));
}


void ExpTest::testSubscriptVars()
{
    // Null case: %pc
    Assign s9(Terminal::get(opNil), Terminal::get(opNil));

    s9.setNumber(9);
    SharedExp search = Location::regOf(REG_PENT_ESP);
    SharedExp e      = Terminal::get(opPC);
    e = e->expSubscriptVar(search, &s9);
    QCOMPARE(e->toString(), QString("%pc"));

    // Simple case: r28
    e      = search->clone();
    e      = e->expSubscriptVar(search, &s9);
    QCOMPARE(e->toString(), QString("r28{9}"));


    // A temp
    e      = Location::tempOf(Const::get("tmp1"));
    e      = e->expSubscriptVar(e->clone(), &s9);
    QCOMPARE(e->toString(), QString("tmp1{9}"));

    // m[r28] + r28
    e      = Binary::get(opPlus, Location::memOf(Location::regOf(REG_PENT_ESP)), Location::regOf(REG_PENT_ESP));
    e      = e->expSubscriptVar(search, &s9);
    QCOMPARE(e->toString(), QString("m[r28{9}] + r28{9}"));

    // RefExp: r28{7} -> r28{9}
    // Again, changed behaviour: don't resubscript any location
    Assign s7(Terminal::get(opNil), Terminal::get(opNil));
    s7.setNumber(7);
    e = RefExp::get(search->clone(), &s7);
    e = e->expSubscriptVar(search, &s9);
    QCOMPARE(e->toString(), QString("r28{7}"));

    // m[r28{7} + 4]{8}
    Assign s8(Terminal::get(opNil), Terminal::get(opNil));
    s8.setNumber(8);
    e = RefExp::get(Location::memOf(Binary::get(opPlus, RefExp::get(Location::regOf(REG_PENT_ESP), &s7), Const::get(4))), &s8);
    e = e->expSubscriptVar(search, &s9);
    QCOMPARE(e->toString(), QString("m[r28{7} + 4]{8}"));

    // r24{7} with r24{7} and 0: should not change: RefExps should not compare
    // at the top level, only with their base expression (here r24, not r24{7})
    e      = RefExp::get(Location::regOf(REG_PENT_EAX), &s7);
    e      = e->expSubscriptVar(e->clone(), nullptr);
    QCOMPARE(e->toString(), QString("r24{7}"));
}


void ExpTest::testVisitors()
{
    Assign s7(Terminal::get(opNil), Terminal::get(opNil));

    // m[SETTFLAGS(m[1000], r8)]{7}
    s7.setNumber(7);
    FlagsFinder ff;
    SharedExp   e1 = RefExp::get(Location::memOf(Binary::get(opFlagCall,
                                                             Const::get("SETFFLAGS"),
                                                             Binary::get(opList,
                                                                         Location::memOf( // A bare memof
                                                                             Const::get(0x1000)),
                                                                         Binary::get(opList, Location::regOf(REG_PENT_AL), Terminal::get(opNil))))),
                                 &s7);

    // m[0x2000]
    SharedExp e2 = Location::memOf(Const::get(0x2000));

    // r1+m[1000]{7}*4
    SharedExp e3 = Binary::get(opPlus,
                               Location::regOf(REG_PENT_AX),
                               Binary::get(opMult, RefExp::get(Location::memOf(Const::get(1000)), &s7), Const::get(4)));

    QVERIFY(e1->containsFlags());
    QVERIFY(!e2->containsFlags());
    QVERIFY(!e3->containsFlags());
}


QTEST_GUILESS_MAIN(ExpTest)
