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


/**
 * \file operator.h Declares the enum OPER,
 * which is used within class Exp to denote what the top level operator is
 */

/// The OPER (and integer representation) of expressions (they can be a fair
/// bit different from operators)
/// \sa operToString
enum OPER
{
    // Operators
    opWild    = -1, ///< Wildcard (Terminal for search Exps only)
    opInvalid = 0,  ///< Invalid operator

    // Integer operations
    opPlus,  ///< Binary addition
    opMinus, ///< Binary subtraction
    opMult,  ///< Multiplication
    opMults, ///< Multiply signed
    opDiv,   ///< Integer division
    opDivs,  ///< Divide signed
    opMod,   ///< Remainder of integer division
    opMods,  ///< Remainder of signed integer division
    opNeg,   ///< Unary minus

    // float operations
    opFPlus,  ///< Float addition
    opFMinus, ///< Float subtraction
    opFMult,  ///< Float multiply
    opFDiv,   ///< Float divide
    opFNeg,   ///< Floating point negate

    // logical operations
    opAnd,       ///< Logical and
    opOr,        ///< Logical or
    opEquals,    ///< Equality (logical)
    opNotEqual,  ///< Logical !=
    opLess,      ///< Logical less than (signed)
    opGtr,       ///< Logical greater than (signed)
    opLessEq,    ///< Logical <= (signed)
    opGtrEq,     ///< Logical >= (signed)
    opLessUns,   ///< Logical less than (unsigned)
    opGtrUns,    ///< Logical greater than (unsigned)
    opLessEqUns, ///< Logical <= (unsigned)
    opGtrEqUns,  ///< Logical >= (unsigned)
    opLNot,      ///< Logical not

    // bit manipulation operations
    opNot,      ///< Bitwise inversion
    opBitAnd,   ///< Bitwise and
    opBitOr,    ///< Bitwise or
    opBitXor,   ///< Bitwise xor
    opShiftL,   ///< Left shift
    opShiftR,   ///< Right shift
    opShiftRA,  ///< Right shift arithmetic
    opRotateL,  ///< Rotate left
    opRotateR,  ///< Rotate right
    opRotateLC, ///< Rotate left through carry
    opRotateRC, ///< Rotate right through carry

    // other operations
    opTypedExp, ///< Typed expression
    opFlagCall, ///< A flag call (Binary with string and params)
    opList,     ///< A binary, with expression (1) and next element
                ///< in chain (2). Last element in chain is opNil

    // Next two are for parser use only. Binary with name of table and name
    // of string as Const string subexpressions. Actual table info held in the
    // TableDict object
    opExpTable, ///< A table of expressions

    opSuccessor,    ///< Get the successor register of this parameter
    opTern,         ///< Ternary (i.e. ? : )
    opAt,           ///< Bit extraction (expr@[first:last] in that order)
    opRegOf,        ///< Represents r[]
    opMemOf,        ///< Represents m[]
    opAddrOf,       ///< Represents a[]
    opWildMemOf,    ///< m[wild],
    opWildRegOf,    ///< r[wild],
    opWildAddrOf,   ///< a[wild],
    opDefineAll,    ///< A wild definition
    opSubscript,    ///< Represents subscript(e, n) .. ie SSA renaming
    opParam,        ///< SSL parameter param`'
    opLocal,        ///< used to represent a local, takes a string
    opGlobal,       ///< used to represent a global, takes a string
    opMemberAccess, ///< . and -> in C
    opArrayIndex,   ///< [] in C
    opTemp,         ///< Temp register name
    opSize,         ///< Size specifier
    opMachFtr,      ///< A Unary with Const(string) representing a machine specific feature
                    ///< (register, instruction or whatever); the analysis better understand it
                    ///< and transform it away)
    opTruncu,       ///< Integer truncate (unsigned)
    opTruncs,       ///< Integer truncate (signed)
    opZfill,        ///< Integer zero fill
    opSgnEx,        ///< Integer sign extend
    opFsize,        ///< Floating point size conversion
    opItof,         ///< Integer to floating point (and size) conversion
    opFtoi,         ///< Floating point to integer (and size) conversion
    opFround,       ///< Floating point to nearest float conversion
    opFtrunc,       ///< chop float to int, e.g. 3.99 -> 3.00
    opFabs,         ///< floating point absolute function
    opSin,          ///< sine
    opCos,          ///< cosine
    opTan,          ///< tangent
    opArcTan,       ///< inverse tangent
    opLog2,         ///< logarithm to base 2
    opLog10,        ///< logarithm to base 10
    opLoge,         ///< logarithm to base e
    opPow,          ///< raise to a power
    opSqrt,         ///< square root

    // constants
    opIntConst,     ///< integer constant TODO: differentiate IntConst by adding AddressConst ?
    opLongConst,    ///< long integer constant
    opFltConst,     ///< floating point constant
    opStrConst,     ///< string constant
    opFuncConst,    ///< a function constant (address of named function)
    opWildIntConst, ///< Terminal integer constant whose value is wild
    opWildStrConst, ///< Terminal string constant whose value is wild

    // Terminals (zero parameter special locations)
    // All machines are assumed to have these following registers:
    opPC,     ///< program counter
    opAFP,    ///< This is the abstract frame pointer register (CSR/PAL analysis)
    opAGP,    ///< This is the abstract global pointer register (CSR/PAL analysis)
    opNil,    ///< This is a "nil list" terminal (e.g. no parameters)
    opFlags,  ///< This is the abstracted integer flags register terminal
    opFflags, ///< This is the abstracted floating point flags terminal

    opTrue,
    opFalse,
    opTypeOf, ///< Unary: takes a location, makes a type variable

    // ---------------------- "The line" --------------------------//
    // All id's greater or equal to idMachSpec are assumed to be source machine
    // specific. If any of these are left by the time the back end is called,
    // then these need to have local variables assigned for them
    opZF,  ///< zero flag
    opCF,  ///< carry flag
    opNF,  ///< negative flag
    opOF,  ///< overflow flag
    opDF,  ///< Pentium Direction (=Down) flag
    opFZF, ///< floating point zero flag
    opFLF  ///< floating point less flag
};

/// Convert operator to string
const char *operToString(OPER oper);
