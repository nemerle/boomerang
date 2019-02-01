#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "ExpSimplifier.h"

#include "boomerang/db/Prog.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/exp/RefExp.h"
#include "boomerang/ssl/exp/Ternary.h"
#include "boomerang/ssl/exp/TypedExp.h"
#include "boomerang/ssl/type/IntegerType.h"
#include "boomerang/ssl/type/PointerType.h"
#include "boomerang/util/ByteUtil.h"
#include "boomerang/util/log/Log.h"


SharedExp ExpSimplifier::postModify(const std::shared_ptr<Unary> &exp)
{
    bool &changed = m_modified;

    if (exp->getOper() == opBitNot || exp->getOper() == opLNot) {
        OPER oper = exp->getSubExp1()->getOper();

        switch (oper) {
        case opEquals: oper = opNotEqual; break;
        case opNotEqual: oper = opEquals; break;
        case opLess: oper = opGtrEq; break;
        case opGtr: oper = opLessEq; break;
        case opLessEq: oper = opGtr; break;
        case opGtrEq: oper = opLess; break;
        case opLessUns: oper = opGtrEqUns; break;
        case opGtrUns: oper = opLessEqUns; break;
        case opLessEqUns: oper = opGtrUns; break;
        case opGtrEqUns: oper = opLessUns; break;
        default: break;
        }

        if (oper != exp->getSubExp1()->getOper()) {
            changed = true;
            exp->getSubExp1()->setOper(oper);
            return exp->getSubExp1();
        }
    }

    if (exp->getOper() == opNeg || exp->getOper() == opBitNot || exp->getOper() == opLNot) {
        if (exp->getSubExp1()->isIntConst()) {
            // -k, ~k, or !k
            int k = exp->access<Const, 1>()->getInt();

            switch (exp->getOper()) {
            case opNeg: k = -k; break;
            case opBitNot: k = ~k; break;
            case opLNot: k = !k; break;
            default: break;
            }

            changed = true;
            exp->access<Const, 1>()->setInt(k);
            return exp->getSubExp1();
        }
        else if (exp->getOper() == exp->getSubExp1()->getOper()) {
            return exp->access<Exp, 1, 1>();
        }
    }

    if ((exp->isMemOf() && exp->getSubExp1()->isAddrOf()) ||
        (exp->isAddrOf() && exp->getSubExp1()->isMemOf())) {
        changed = true;
        return exp->getSubExp1()->getSubExp1();
    }

    // Simplify e.g. ~(x comp y) -> !(x comp y)
    const OPER myOper = exp->getOper();
    if (myOper == opBitNot && exp->getSubExp1()->isLogExp()) {
        changed = true;
        exp->setOper(opLNot);
        return exp;
    }

    // if still not simplified, try De Morgan's laws
    const OPER subOper = exp->getSubExp1()->getOper();

    if (myOper == opBitNot && (subOper == opBitAnd || subOper == opBitOr)) {
        changed = true;
        if (subOper == opBitAnd) {
            return Binary::get(opBitOr, Unary::get(opBitNot, exp->access<Exp, 1, 1>()),
                               Unary::get(opBitNot, exp->access<Exp, 1, 2>()));
        }
        else {
            return Binary::get(opBitAnd, Unary::get(opBitNot, exp->access<Exp, 1, 1>()),
                               Unary::get(opBitNot, exp->access<Exp, 1, 2>()));
        }
    }
    else if (myOper == opLNot && (subOper == opAnd || subOper == opOr)) {
        changed = true;
        if (subOper == opAnd) {
            return Binary::get(opOr, Unary::get(opLNot, exp->access<Exp, 1, 1>()),
                               Unary::get(opLNot, exp->access<Exp, 1, 2>()));
        }
        else {
            return Binary::get(opAnd, Unary::get(opLNot, exp->access<Exp, 1, 1>()),
                               Unary::get(opLNot, exp->access<Exp, 1, 2>()));
        }
    }

    return exp;
}


SharedExp ExpSimplifier::postModify(const std::shared_ptr<Binary> &exp)
{
    bool &changed = m_modified;

    OPER opSub1 = exp->getSubExp1()->getOper();
    OPER opSub2 = exp->getSubExp2()->getOper();

    if (opSub1 == opIntConst && opSub2 == opIntConst) {
        // k1 op k2, where k1 and k2 are integer constants
        int k1      = exp->access<Const, 1>()->getInt();
        int k2      = exp->access<Const, 2>()->getInt();
        bool change = true;

        switch (exp->getOper()) {
        case opPlus: k1 = k1 + k2; break;
        case opMinus: k1 = k1 - k2; break;
        case opMults: k1 = k1 * k2; break;
        case opDivs: k1 = k1 / k2; break;
        case opMods: k1 = k1 % k2; break;
        case opShL: k1 = (k2 < 32) ? k1 << k2 : 0; break;
        case opShR: k1 = (k2 < 32) ? k1 >> k2 : 0; break;
        case opShRA: {
            assert(k2 < 32);
            k1 = (k1 >> k2) | (((1 << k2) - 1) << (32 - k2));
            break;
        }

        case opBitAnd: k1 = k1 & k2; break;
        case opBitOr: k1 = k1 | k2; break;
        case opBitXor: k1 = k1 ^ k2; break;
        case opEquals: k1 = (k1 == k2); break;
        case opNotEqual: k1 = (k1 != k2); break;
        case opLess: k1 = (k1 < k2); break;
        case opGtr: k1 = (k1 > k2); break;
        case opLessEq: k1 = (k1 <= k2); break;
        case opGtrEq: k1 = (k1 >= k2); break;

        case opMult:
            k1 = static_cast<int>(static_cast<unsigned>(k1) * static_cast<unsigned>(k2));
            break;
        case opDiv:
            k1 = static_cast<int>(static_cast<unsigned>(k1) / static_cast<unsigned>(k2));
            break;
        case opMod:
            k1 = static_cast<int>(static_cast<unsigned>(k1) % static_cast<unsigned>(k2));
            break;
        case opLessUns: k1 = static_cast<unsigned>(k1) < static_cast<unsigned>(k2); break;
        case opGtrUns: k1 = static_cast<unsigned>(k1) > static_cast<unsigned>(k2); break;
        case opLessEqUns: k1 = static_cast<unsigned>(k1) <= static_cast<unsigned>(k2); break;
        case opGtrEqUns: k1 = static_cast<unsigned>(k1) >= static_cast<unsigned>(k2); break;

        default: change = false;
        }

        if (change) {
            changed = true;
            return Const::get(k1);
        }
    }

    if ((exp->getOper() == opBitXor || exp->getOper() == opMinus) &&
        *exp->getSubExp1() == *exp->getSubExp2()) {
        // x ^ x or x - x: result is zero
        changed = true;
        return Const::get(0);
    }

    if ((exp->getOper() == opBitOr || exp->getOper() == opBitAnd) &&
        *exp->getSubExp1() == *exp->getSubExp2()) {
        // x | x or x & x: result is x
        changed = true;
        return exp->getSubExp1();
    }

    if (exp->getOper() == opEquals && *exp->getSubExp1() == *exp->getSubExp2()) {
        // x == x: result is true
        changed = true;
        return std::make_shared<Terminal>(opTrue);
    }
    else if (exp->getOper() == opNotEqual && *exp->getSubExp1() == *exp->getSubExp2()) {
        // x != x: result is false
        changed = true;
        return std::make_shared<Terminal>(opFalse);
    }

    // Might want to commute to put an integer constant on the RHS
    // Later simplifications can rely on this (add other ops as necessary)
    if (opSub1 == opIntConst &&
        (exp->getOper() == opPlus || exp->getOper() == opMult || exp->getOper() == opMults ||
         exp->getOper() == opBitOr || exp->getOper() == opBitAnd || exp->getOper() == opEquals ||
         exp->getOper() == opNotEqual)) {
        exp->commute();
        // Swap opSub1 and opSub2 as well
        std::swap(opSub1, opSub2);
        // This is not counted as a modification
    }

    // Similarly for boolean constants
    if (exp->getSubExp1()->isBoolConst() && !exp->getSubExp2()->isBoolConst() &&
        (exp->isOr() || exp->isAnd())) {
        exp->commute();
        // Swap opSub1 and opSub2 as well
        std::swap(opSub1, opSub2);
        // This is not counted as a modification
    }

    // Similarly for adding stuff to the addresses of globals
    if (exp->getOper() == opPlus && exp->access<Exp, 2>()->isAddrOf() &&
        exp->access<Exp, 2, 1>()->isSubscript() && exp->access<Exp, 2, 1, 1>()->isGlobal()) {
        exp->commute();
        // Swap opSub1 and opSub2 as well
        std::swap(opSub1, opSub2);
        // This is not counted as a modification
    }

    // check for (x + a) + b where a and b are constants, becomes x + a+b
    if (exp->getOper() == opPlus && opSub1 == opPlus && opSub2 == opIntConst &&
        exp->getSubExp1()->getSubExp2()->isIntConst()) {
        const int n = exp->access<Const, 2>()->getInt();
        exp->getSubExp1()->setOper(opPlus);
        exp->access<Const, 1, 2>()->setInt(exp->access<Const, 1, 2>()->getInt() + n);
        changed = true;
        return exp->getSubExp1();
    }

    // check for (x - a) + b where a and b are constants, becomes x + -a+b
    if (exp->getOper() == opPlus && opSub1 == opMinus && opSub2 == opIntConst &&
        exp->access<Exp, 1, 2>()->isIntConst()) {
        const int n = exp->access<Const, 2>()->getInt();
        exp->getSubExp1()->setOper(opPlus);
        exp->access<Const, 1, 2>()->setInt(-exp->access<Const, 1, 2>()->getInt() + n);
        changed = true;
        return exp->getSubExp1();
    }

    SharedExp res = exp->shared_from_this();

    // check for (x * k) - x, becomes x * (k-1)
    // same with +
    if ((exp->getOper() == opMinus || exp->getOper() == opPlus) &&
        (opSub1 == opMults || opSub1 == opMult) &&
        *exp->getSubExp2() == *exp->getSubExp1()->getSubExp1()) {
        res = res->getSubExp1();
        res->setSubExp2(Binary::get(exp->getOper(), res->getSubExp2(), Const::get(1)));
        changed = true;
        return res;
    }

    // check for x + (x * k), becomes x * (k+1)
    if (exp->getOper() == opPlus && (opSub2 == opMults || opSub2 == opMult) &&
        *exp->getSubExp1() == *exp->getSubExp2()->getSubExp1()) {
        res = res->getSubExp2();
        res->setSubExp2(Binary::get(opPlus, res->getSubExp2(), Const::get(1)));
        changed = true;
        return res;
    }

    // Turn a + -K into a - K (K is int const > 0)
    // Also a - -K into a + K (K is int const > 0)
    // Does not count as a change
    if ((exp->getOper() == opPlus || exp->getOper() == opMinus) && opSub2 == opIntConst &&
        exp->access<Const, 2>()->getInt() < 0) {
        exp->access<Const, 2>()->setInt(-exp->access<Const, 2>()->getInt());
        exp->setOper(exp->getOper() == opPlus ? opMinus : opPlus);
    }

    // Check for exp + 0  or  exp - 0  or  exp | 0
    if ((exp->getOper() == opPlus || exp->getOper() == opMinus || exp->getOper() == opBitOr) &&
        opSub2 == opIntConst && exp->access<Const, 2>()->getInt() == 0) {
        changed = true;
        return exp->getSubExp1();
    }

    // Check for exp or false
    if (exp->getOper() == opOr && exp->getSubExp2()->isFalse()) {
        changed = true;
        return exp->getSubExp1();
    }

    // Check for SharedExp * 0  or exp & 0
    if ((exp->getOper() == opMult || exp->getOper() == opMults || exp->getOper() == opBitAnd) &&
        opSub2 == opIntConst && exp->access<Const, 2>()->getInt() == 0) {
        changed = true;
        return Const::get(0);
    }

    // Check for exp and false
    if (exp->getOper() == opAnd && exp->getSubExp2()->isFalse()) {
        changed = true;
        return Terminal::get(opFalse);
    }

    // Check for SharedExp * 1
    if ((exp->getOper() == opMult || exp->getOper() == opMults) && opSub2 == opIntConst &&
        exp->access<Const, 2>()->getInt() == 1) {
        changed = true;
        return res->getSubExp1();
    }

    // x xor 0 = x
    if (exp->getOper() == opBitXor && opSub2 == opIntConst &&
        exp->access<Const, 2>()->getInt() == 0) {
        changed = true;
        return res->getSubExp1();
    }

    // Check for SharedExp (a * x) / x -> a
    if ((exp->getOper() == opDiv || exp->getOper() == opDivs) &&
        (opSub1 == opMult || opSub1 == opMults) &&
        *exp->getSubExp2() == *exp->getSubExp1()->getSubExp2()) {
        changed = true;
        return res->getSubExp1()->getSubExp1();
    }

    // Check for exp / 1, becomes exp
    if ((exp->getOper() == opDiv || exp->getOper() == opDivs) && opSub2 == opIntConst &&
        exp->access<Const, 2>()->getInt() == 1) {
        changed = true;
        return res->getSubExp1();
    }

    // Check for exp % 1, becomes 0
    if ((exp->getOper() == opMod || exp->getOper() == opMods) && opSub2 == opIntConst &&
        exp->access<Const, 2>()->getInt() == 1) {
        changed = true;
        return Const::get(0);
    }

    // Check for SharedExp  (a * x) % x, becomes 0
    if ((exp->getOper() == opMod || exp->getOper() == opMods) &&
        (opSub1 == opMult || opSub1 == opMults) &&
        (*exp->getSubExp2() == *exp->getSubExp1()->getSubExp2() ||
         *exp->getSubExp2() == *exp->getSubExp1()->getSubExp1())) {
        changed = true;
        return Const::get(0);
    }

    // Check for SharedExp  x % x, becomes 0
    if ((exp->getOper() == opMod || exp->getOper() == opMods) &&
        *exp->getSubExp2() == *exp->getSubExp1()) {
        changed = true;
        return Const::get(0);
    }

    // Check for exp AND -1 (bitwise AND)
    if (exp->getOper() == opBitAnd && opSub2 == opIntConst &&
        exp->access<Const, 2>()->getInt() == -1) {
        changed = true;
        return exp->getSubExp1();
    }

    // Check for exp OR -1 (bitwise OR)
    if (exp->getOper() == opBitOr && opSub2 == opIntConst &&
        exp->access<Const, 2>()->getInt() == -1) {
        changed = true;
        return exp->getSubExp2();
    }

    // Check for exp AND TRUE (logical AND)
    if (exp->getOper() == opAnd &&
        // Is the below really needed?
        ((opSub2 == opIntConst && exp->access<Const, 2>()->getInt() != 0) ||
         exp->getSubExp2()->isTrue())) {
        changed = true;
        return res->getSubExp1();
    }

    // Check for exp OR TRUE (logical OR)
    if (exp->getOper() == opOr &&
        ((opSub2 == opIntConst && exp->access<Const, 2>()->getInt() != 0) ||
         exp->getSubExp2()->isTrue())) {
        changed = true;
        return Terminal::get(opTrue);
    }

    // Check for [exp] << k where k is a positive integer const
    if (exp->getOper() == opShL && opSub2 == opIntConst) {
        const int k = exp->access<Const, 2>()->getInt();

        if (Util::inRange(k, 0, 4)) { // do not express e.g. a << 4 as multiplication
            exp->setOper(opMult);
            exp->access<Const, 2>()->setInt(1 << k);
            changed = true;
            return exp;
        }
    }

    // Check for -x compare y, becomes x compare -y
    if (exp->isComparison() && opSub1 == opNeg) {
        exp->setSubExp1(exp->access<Exp, 1, 1>());
        exp->setSubExp2(Unary::get(opNeg, exp->getSubExp2()));
        changed = true;
        return exp;
    }

    // Check for (x + y) compare 0, becomes x compare -y
    if (exp->isComparison() && opSub2 == opIntConst && exp->access<Const, 2>()->getInt() == 0) {
        if (opSub1 == opPlus) {
            exp->setSubExp2(Unary::get(opNeg, exp->access<Exp, 1, 2>()));
            exp->setSubExp1(exp->access<Exp, 1, 1>());
            changed = true;
            return exp;
        }
        else if (opSub1 == opMinus) {
            exp->setSubExp2(exp->access<Exp, 1, 2>());
            exp->setSubExp1(exp->access<Exp, 1, 1>());
            changed = true;
            return exp;
        }
    }

    // Check for 0 <=u x, or for x >=u 0, becomes true
    if ((exp->getOper() == opLessEqUns && exp->getSubExp1()->isIntConst() &&
         exp->access<Const, 1>()->getInt() == 0) ||
        (exp->getOper() == opGtrEqUns && exp->getSubExp2()->isIntConst() &&
         exp->access<Const, 2>()->getInt() == 0)) {
        changed = true;
        return Const::get(1);
    }

    // Check for 0 <u x, or for x >u 0, becomes x != 0
    if ((exp->getOper() == opLessUns && exp->getSubExp1()->isIntConst() &&
         exp->access<Const, 1>()->getInt() == 0) ||
        (exp->getOper() == opGtrUns && exp->getSubExp2()->isIntConst() &&
         exp->access<Const, 2>()->getInt() == 0)) {
        changed = true;
        exp->setOper(opNotEqual);
        return exp;
    }

    // Check for (x == y) == 1, becomes x == y
    // Check for (x == y) == 0, becomes x != y
    if (exp->getOper() == opEquals && opSub1 == opEquals && opSub2 == opIntConst) {
        const int rightConst = exp->access<Const, 2>()->getInt();
        changed              = true;

        switch (rightConst) {
        case 0: exp->getSubExp1()->setOper(opNotEqual); return exp->getSubExp1();

        case 1: return exp->getSubExp1();

        default: return Terminal::get(opFalse);
        }
    }

    // Check for (x == y) != 0, becomes x == y
    // Check for (x == y) != 1, becomes x != y
    if (exp->getOper() == opNotEqual && opSub1 == opEquals && opSub2 == opIntConst) {
        const int rightConst = exp->access<Const, 2>()->getInt();
        changed              = true;

        switch (rightConst) {
        case 0: return exp->getSubExp1();

        case 1: exp->getSubExp1()->setOper(opNotEqual); return exp->getSubExp1();

        default: return Terminal::get(opTrue);
        }
    }

    // Check for (x > y) == 0, becomes x <= y
    if (exp->getOper() == opEquals && exp->getSubExp1()->isComparison() && opSub2 == opIntConst &&
        exp->access<Const, 2>()->getInt() == 0) {
        changed = true;
        return Unary::get(opLNot, exp->getSubExp1());
    }

    auto b1 = std::dynamic_pointer_cast<Binary>(exp->getSubExp1());
    auto b2 = std::dynamic_pointer_cast<Binary>(exp->getSubExp2());

    if ((exp->getOper() == opOr) && (opSub2 == opEquals) &&
        ((opSub1 == opGtrEq) || (opSub1 == opLessEq) || (opSub1 == opGtrEqUns) ||
         (opSub1 == opLessEqUns)) &&
        (((*b1->getSubExp1() == *b2->getSubExp1()) && (*b1->getSubExp2() == *b2->getSubExp2())) ||
         ((*b1->getSubExp1() == *b2->getSubExp2()) && (*b1->getSubExp2() == *b2->getSubExp1())))) {
        res     = res->getSubExp1();
        changed = true;
        return res;
    }

    // Check for (x <  y) || (x == y), becomes x <= y
    // Check for (x <= y) || (x == y), becomes x <= y
    if (exp->isOr() && exp->getSubExp1()->isComparison() && exp->getSubExp2()->isComparison() &&
        (exp->getSubExp1()->isEquality() || exp->getSubExp2()->isEquality()) &&
        *exp->access<Exp, 1, 1>() == *exp->access<Exp, 2, 1>() && // x on left == x on right
        *exp->access<Exp, 1, 2>() == *exp->access<Exp, 2, 2>()) { // y on left == y on right
        OPER otherOper = exp->getSubExp1()->isEquality() ? exp->getSubExp2()->getOper()
                                                         : exp->getSubExp1()->getOper();

        switch (otherOper) {
        case opGtr:
        case opGtrEq: otherOper = opGtrEq; break;
        case opGtrUns:
        case opGtrEqUns: otherOper = opGtrEqUns; break;
        case opLess:
        case opLessEq: otherOper = opLessEq; break;
        case opLessUns:
        case opLessEqUns: otherOper = opLessEqUns; break;
        case opEquals:
            // x == y || x == y -> x == y
            changed = true;
            return exp->getSubExp1();
        case opNotEqual:
            // x == y || x != y -> true
            changed = true;
            return Terminal::get(opTrue);
        default: break;
        }

        changed = true;
        exp->getSubExp1()->setOper(otherOper);
        return exp->getSubExp1();
    }

    // Check for (x compare y) || (x != y), becomes x compare y
    // Note: Case (x == y) || (x != y) handled above
    if ((exp->isOr() || exp->getOper() == opBitOr) && exp->getSubExp1()->isComparison() &&
        exp->getSubExp2()->isComparison() &&
        (exp->getSubExp1()->isNotEquality() || exp->getSubExp2()->isNotEquality()) &&
        *exp->access<Exp, 1, 1>() == *exp->access<Exp, 2, 1>() && // x on left == x on right
        *exp->access<Exp, 1, 2>() == *exp->access<Exp, 2, 2>()) { // y on left == y on right
        if (exp->getSubExp1()->isNotEquality()) {
            changed = true;
            return exp->getSubExp2();
        }
        else {
            changed = true;
            return exp->getSubExp1();
        }
    }

    // For (a || b) or (a && b) recurse on a and b
    if ((exp->getOper() == opOr) || (exp->getOper() == opAnd)) {
        exp->refSubExp1() = exp->getSubExp1()->acceptModifier(this);
        exp->refSubExp2() = exp->getSubExp2()->acceptModifier(this);

        if (!m_modified && *exp->getSubExp1() == *exp->getSubExp2()) {
            m_modified = true;
            return exp->getSubExp1();
        }
        return res;
    }

    // check for (a*n)*m, becomes a*(n*m) where n and m are ints
    if ((exp->getOper() == opMult) && (opSub1 == opMult) && (opSub2 == opIntConst) &&
        exp->access<Exp, 1, 2>()->isIntConst()) {
        const int n = exp->access<const Const, 1, 2>()->getInt();
        const int m = exp->access<const Const, 2>()->getInt();

        res = res->getSubExp1();
        res->access<Const, 2>()->setInt(n * m);
        changed = true;
        return res;
    }

    if (exp->getOper() == opFMinus && exp->getSubExp1()->isFltConst() &&
        exp->access<Const, 1>()->getFlt() == 0.0) {
        res     = Unary::get(opFNeg, exp->getSubExp2());
        changed = true;
        return res;
    }

    // check for ((x * a) + (y * b)) / c where a, b and c are all integers and a and b divide evenly
    // by c becomes: (x * a/c) + (y * b/c)
    if (exp->getOper() == opDiv && exp->getSubExp1()->getOper() == opPlus &&
        exp->getSubExp2()->isIntConst()) {
        SharedExp leftOfPlus  = exp->getSubExp1()->getSubExp1();
        SharedExp rightOfPlus = exp->getSubExp1()->getSubExp2();

        if (leftOfPlus->getOper() == opMult && rightOfPlus->getOper() == opMult &&
            leftOfPlus->getSubExp2()->isIntConst() && rightOfPlus->getSubExp2()->isIntConst()) {
            const int a = leftOfPlus->access<Const, 2>()->getInt();
            const int b = rightOfPlus->access<Const, 2>()->getInt();
            const int c = exp->access<Const, 2>()->getInt();

            if ((a % c == 0) && (b % c == 0)) {
                changed = true;
                leftOfPlus->access<Const, 2>()->setInt(a / c);
                rightOfPlus->access<Const, 2>()->setInt(b / c);

                return exp->getSubExp1();
            }
        }
    }

    // check for ((x * a) + (y * b)) % c where a, b and c are all integers
    // becomes: (y * b) % c if a divides evenly by c
    // becomes: (x * a) % c if b divides evenly by c
    // becomes: 0            if both a and b divide evenly by c
    if (exp->getOper() == opMod && exp->getSubExp1()->getOper() == opPlus &&
        exp->getSubExp2()->isIntConst()) {
        SharedExp leftOfPlus  = exp->getSubExp1()->getSubExp1();
        SharedExp rightOfPlus = exp->getSubExp1()->getSubExp2();

        if (leftOfPlus->getOper() == opMult && rightOfPlus->getOper() == opMult &&
            leftOfPlus->getSubExp2()->isIntConst() && rightOfPlus->getSubExp2()->isIntConst()) {
            const int a = leftOfPlus->access<Const, 2>()->getInt();
            const int b = rightOfPlus->access<Const, 2>()->getInt();
            const int c = exp->access<Const, 2>()->getInt();

            if ((a % c == 0) && (b % c == 0)) {
                changed = true;
                return Const::get(0);
            }
            if ((a % c) == 0) {
                changed = true;
                return Binary::get(opMod, rightOfPlus, Const::get(c));
            }
            if ((b % c) == 0) {
                changed = true;
                return Binary::get(opMod, leftOfPlus, Const::get(c));
            }
        }
    }

    return res;
}


SharedExp ExpSimplifier::postModify(const std::shared_ptr<Ternary> &exp)
{
    bool &changed = m_modified;

    // p ? 1 : 0 -> p != 0
    // p ? 0 : 1 -> p == 0
    if (exp->getOper() == opTern && exp->getSubExp2()->isIntConst() &&
        exp->getSubExp3()->isIntConst()) {
        const int val2 = exp->access<Const, 2>()->getInt();
        const int val3 = exp->access<Const, 3>()->getInt();

        if (val2 == 1 && val3 == 0) {
            changed = true;
            return Binary::get(opNotEqual, exp->getSubExp1(), Const::get(0));
        }
        else if (val2 == 0 && val3 == 1) {
            changed = true;
            return Binary::get(opEquals, exp->getSubExp1(), Const::get(0));
        }
    }

    // Const ? x : y
    if (exp->getOper() == opTern && exp->getSubExp1()->isIntConst()) {
        const int val = exp->access<Const, 1>()->getInt();
        if (val != 1 && val != 0) {
            LOG_VERBOSE("Treating constant value %1 as true in Ternary '%2'", val, exp);
        }

        changed = true;
        return (val != 0) ? exp->getSubExp2() : exp->getSubExp3();
    }

    // a ? x : x
    if (exp->getOper() == opTern && *exp->getSubExp2() == *exp->getSubExp3()) {
        changed = true;
        return exp->getSubExp2();
    }

    /// sign-extend constant value
    if (exp->getOper() == opSgnEx && exp->getSubExp1()->isIntConst() &&
        exp->getSubExp2()->isIntConst() && exp->getSubExp3()->isIntConst()) {
        const int from   = exp->access<Const, 1>()->getInt();
        const int to     = exp->access<Const, 2>()->getInt();
        const int oldVal = exp->access<Const, 3>()->getInt();
        if (to <= from) {
            changed = true;
            return exp->getSubExp3();
        }
        else if (from == 0) {
            changed = true;
            return Const::get(0);
        }

        const bool sign = ((oldVal >> (from - 1)) & 1) == 1;
        if (sign) {
            changed = true;
            if (to > 32) {
                return Const::get((Util::getLowerBitMask(to - from) << from) |
                                  (oldVal & Util::getLowerBitMask(from)));
            }
            else {
                return Const::get((int)(Util::getLowerBitMask(to - from) << from) |
                                  (int)(oldVal & Util::getLowerBitMask(from)));
            }
        }
        else {
            changed = true;
            return Const::get((int)(oldVal & Util::getLowerBitMask(from)));
        }
    }

    if (exp->getOper() == opZfill && exp->getSubExp3()->isIntConst()) {
        changed = true;
        return exp->getSubExp3();
    }

    if (exp->getOper() == opFsize && exp->getSubExp3()->getOper() == opItof &&
        *exp->getSubExp1() == *exp->access<Exp, 3, 2>() &&
        *exp->getSubExp2() == *exp->access<Exp, 3, 1>()) {
        changed = true;
        return exp->getSubExp3();
    }

    if (exp->getOper() == opFsize && exp->getSubExp3()->isFltConst()) {
        changed = true;
        return exp->getSubExp3();
    }


    if (exp->getOper() == opItof && exp->getSubExp2()->isIntConst() &&
        exp->getSubExp3()->isIntConst() && exp->access<Const, 2>()->getInt() == 32) {
        changed        = true;
        unsigned int n = exp->access<Const, 3>()->getInt();
        return Const::get(*reinterpret_cast<float *>(&n));
    }

    if (exp->getOper() == opFsize && exp->getSubExp3()->isMemOf() &&
        exp->access<Exp, 3, 1>()->isIntConst()) {
        assert(exp->getSubExp3()->isLocation());
        Address u   = exp->access<Const, 3, 1>()->getAddr();
        UserProc *p = exp->access<Location, 3>()->getProc();

        if (p) {
            Prog *prog = p->getProg();
            double d;
            const bool ok = prog->getFloatConstant(u, d, exp->access<Const, 1>()->getInt());

            if (ok) {
                changed = true;
                LOG_VERBOSE("Replacing %1 with %2 in %3", exp->getSubExp3(), d,
                            exp->shared_from_this());
                return Const::get(d);
            }
        }
    }

    if (exp->getOper() == opTruncu && exp->getSubExp3()->isIntConst()) {
        int from         = exp->access<Const, 1>()->getInt();
        int to           = exp->access<Const, 2>()->getInt();
        unsigned int val = exp->access<Const, 3>()->getInt();

        if (from > to) {
            changed = true;
            return Const::get(Address(val & (int)Util::getLowerBitMask(to)));
        }
    }

    if (exp->getOper() == opTruncs && exp->getSubExp3()->isIntConst()) {
        int from = exp->access<Const, 1>()->getInt();
        int to   = exp->access<Const, 2>()->getInt();
        int val  = exp->access<Const, 3>()->getInt();

        if (from > to) {
            return Const::get(val & (int)Util::getLowerBitMask(to));
        }
    }

    return exp;
}


SharedExp ExpSimplifier::preModify(const std::shared_ptr<TypedExp> &exp, bool &visitChildren)
{
    visitChildren = true;

    if (exp->getSubExp1()->isRegOf()) {
        // type cast on a reg of.. hmm.. let's remove this
        m_modified = true;
        return exp->getSubExp1();
    }

    return exp;
}


SharedExp ExpSimplifier::postModify(const std::shared_ptr<Location> &exp)
{
    bool &changed = m_modified;

    if (exp->isMemOf() && exp->getSubExp1()->isAddrOf()) {
        changed = true;
        return exp->getSubExp1()->getSubExp1();
    }

    return exp;
}


SharedExp ExpSimplifier::postModify(const std::shared_ptr<RefExp> &exp)
{
    if (m_modified) {
        return exp;
    }

    bool &changed = m_modified;

    /*
     * This is a nasty hack.  We assume that %DF{0} is 0.  This happens
     * when string instructions are used without first clearing the direction flag.
     * By convention, the direction flag is assumed to be clear on entry to a
     * procedure.
     */
    if (exp->getSubExp1()->getOper() == opDF && exp->getDef() == nullptr) {
        changed = true;
        return Const::get(int(0));
    }

    // Was code here for bypassing phi statements that are now redundant

    return exp;
}
