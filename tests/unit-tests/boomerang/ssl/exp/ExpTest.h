#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#pragma once


#include "TestUtils.h"


class Const;
class Location;


/**
 * Test the Exp class and subclasses
 */
class ExpTest : public BoomerangTest
{
    Q_OBJECT

protected:
    std::shared_ptr<Const> m_99;
    std::shared_ptr<Location> m_rof2;

private slots:
    void initTestCase();

    /// Test integer constant
    void test99();

    /// Test float constant
    void testFlt();

    /**
     * Tests r[2], which is used in many tests. Also tests opRegOf,
     * and ostream::operator&(Exp*)
     * \note r[2] prints as r2, as of June 2003
     */
    void testRegOf2();

    /// Test opPlus, opMinus, etc
    void testBinaries();

    /// Test LNot, unary minus, etc
    void testUnaries();

    /// Test [ a[m[ ] %afp [+|- const]
    void testIsAfpTerm();

    /**
     * ExpTest::testCompare1-5
     * Test the operator== function
     */
    void testCompare1();
    void testCompare2();
    void testCompare3();
    void testCompare4();
    void testCompare5();


    /**
     * ExpTest::testSearchReplace1-4
     * Test the searchReplace function
     */
    void testSearchReplace1();
    void testSearchReplace2();
    void testSearchReplace3();
    void testSearchReplace4();

    /**
     * ExpTest::testSearch1-4
     * Test the search function, including wildcards
     */
    void testSearch1();
    void testSearch2();
    void testSearch3();
    void testSearchAll();

    /// Test the partitionTerms function
    void testPartitionTerms();

    /// Test the Accumulate function
    void testAccumulate();

    void testSimplify();
    void testSimplify_data();

    /// Test the simplifyArith function
    void testSimplifyBinary();

    void testSimplifyAddr();

    /// Various tests of the operator< function
    void testLess();

    /// Test maps of Exp*s; exercises some comparison operators
    void testMapOfExp();

    /// Test the opList creating and printing
    void testList();

    /// Test the printing of parentheses in complex expressions
    void testParen();

    /// Test succ(r[k]) == r[k+1]
    void testFixSuccessor();

    /// Test that a+K+b is the same as a+b+K when each is simplified
    void testAssociativity();

    /// Test Assign::subscriptVar and thereby Exp::expSubscriptVar
    void testSubscriptVar();

    /// Test opTypeOf and TypeVal (type values)
    void testTypeOf();

    /// Test finding the locations used by an expression
    void testAddUsedLocs();
    void testAddUsedLocs_data();

    /// Test the subscripting of variables (locations)
    void testSubscriptVars();

    /// Test the FlagsFinder and BareMemofFinder visitors
    void testVisitors();
};
