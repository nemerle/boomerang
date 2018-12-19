#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "Operator.h"

#include <cassert>

#define HANDLE_OPER(op)                                                                            \
    case (op): return #op

const char *operToString(OPER oper)
{
    switch (oper) {
        HANDLE_OPER(opWild);
        HANDLE_OPER(opPlus);
        HANDLE_OPER(opMinus);
        HANDLE_OPER(opMult);
        HANDLE_OPER(opDiv);
        HANDLE_OPER(opFPlus);
        HANDLE_OPER(opFMinus);
        HANDLE_OPER(opFMult);
        HANDLE_OPER(opFDiv);
        HANDLE_OPER(opFNeg);
        HANDLE_OPER(opMults);
        HANDLE_OPER(opDivs);
        HANDLE_OPER(opMod);
        HANDLE_OPER(opMods);
        HANDLE_OPER(opNeg);
        HANDLE_OPER(opAnd);
        HANDLE_OPER(opOr);
        HANDLE_OPER(opEquals);
        HANDLE_OPER(opNotEqual);
        HANDLE_OPER(opLess);
        HANDLE_OPER(opGtr);
        HANDLE_OPER(opLessEq);
        HANDLE_OPER(opGtrEq);
        HANDLE_OPER(opLessUns);
        HANDLE_OPER(opGtrUns);
        HANDLE_OPER(opLessEqUns);
        HANDLE_OPER(opGtrEqUns);
        HANDLE_OPER(opNot);
        HANDLE_OPER(opLNot);
        HANDLE_OPER(opBitAnd);
        HANDLE_OPER(opBitOr);
        HANDLE_OPER(opBitXor);
        HANDLE_OPER(opShiftL);
        HANDLE_OPER(opShiftR);
        HANDLE_OPER(opShiftRA);
        HANDLE_OPER(opRotateL);
        HANDLE_OPER(opRotateR);
        HANDLE_OPER(opRotateLC);
        HANDLE_OPER(opRotateRC);
        HANDLE_OPER(opTypedExp);
        HANDLE_OPER(opFlagCall);
        HANDLE_OPER(opList);
        HANDLE_OPER(opExpTable);
        HANDLE_OPER(opSuccessor);
        HANDLE_OPER(opTern);
        HANDLE_OPER(opAt);
        HANDLE_OPER(opRegOf);
        HANDLE_OPER(opMemOf);
        HANDLE_OPER(opAddrOf);
        HANDLE_OPER(opWildMemOf);
        HANDLE_OPER(opWildRegOf);
        HANDLE_OPER(opWildAddrOf);
        HANDLE_OPER(opDefineAll);
        HANDLE_OPER(opSubscript);
        HANDLE_OPER(opParam);
        HANDLE_OPER(opLocal);
        HANDLE_OPER(opGlobal);
        HANDLE_OPER(opMemberAccess);
        HANDLE_OPER(opArrayIndex);
        HANDLE_OPER(opTemp);
        HANDLE_OPER(opSize);
        HANDLE_OPER(opMachFtr);
        HANDLE_OPER(opTruncu);
        HANDLE_OPER(opTruncs);
        HANDLE_OPER(opZfill);
        HANDLE_OPER(opSgnEx);
        HANDLE_OPER(opFsize);
        HANDLE_OPER(opItof);
        HANDLE_OPER(opFtoi);
        HANDLE_OPER(opFround);
        HANDLE_OPER(opFtrunc);
        HANDLE_OPER(opFabs);
        HANDLE_OPER(opSin);
        HANDLE_OPER(opCos);
        HANDLE_OPER(opTan);
        HANDLE_OPER(opArcTan);
        HANDLE_OPER(opLog2);
        HANDLE_OPER(opLog10);
        HANDLE_OPER(opLoge);
        HANDLE_OPER(opPow);
        HANDLE_OPER(opSqrt);
        HANDLE_OPER(opIntConst);
        HANDLE_OPER(opLongConst);
        HANDLE_OPER(opFltConst);
        HANDLE_OPER(opStrConst);
        HANDLE_OPER(opFuncConst);
        HANDLE_OPER(opWildIntConst);
        HANDLE_OPER(opWildStrConst);
        HANDLE_OPER(opPC);
        HANDLE_OPER(opAFP);
        HANDLE_OPER(opAGP);
        HANDLE_OPER(opNil);
        HANDLE_OPER(opFlags);
        HANDLE_OPER(opFflags);
        HANDLE_OPER(opTrue);
        HANDLE_OPER(opFalse);
        HANDLE_OPER(opTypeOf);
        HANDLE_OPER(opZF);
        HANDLE_OPER(opCF);
        HANDLE_OPER(opNF);
        HANDLE_OPER(opOF);
        HANDLE_OPER(opDF);
        HANDLE_OPER(opFZF);
        HANDLE_OPER(opFLF);

    case opInvalid: break;
    }

    assert(!"operToString: Invalid operator.");

    return "";
}
