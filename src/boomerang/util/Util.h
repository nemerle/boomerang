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


#include "boomerang/core/BoomerangAPI.h"

#include <QString>

#include <memory>


class Prog;
class OStream;


using SharedConstExp = std::shared_ptr<const class Exp>;


namespace Util
{
/**
 * Escape strings properly for code generation.
 * Turns things like newline, return, tab into \n, \r, \t etc
 * \note Assumes a C or C++ back end
 */
QString BOOMERANG_API escapeStr(const char *str);

OStream &alignStream(OStream &str, int align);


/// Check if \p value is in [\p rangeStart, \p rangeEnd)
template<class T, class U1, class U2>
bool inRange(const T &value, const U1 &rangeStart, const U2 &rangeEnd)
{
    return (value >= rangeStart) && (value < rangeEnd);
}


/// Check if a value is in a container
template<typename Cont, typename T>
bool isContained(const Cont &cont, const T &value)
{
    return std::find(cont.begin(), cont.end(), value) != cont.end();
}


template<class Container>
void clone(const Container &from, Container &to)
{
    if (&from == &to) {
        return;
    }

    to.resize(from.size());

    std::transform(from.begin(), from.end(), to.begin(),
                   [](typename Container::value_type val) { return val->clone(); });
}


// From m[sp +- K] return K (or -K for subtract). sp could be subscripted with {-}
int getStackOffset(SharedConstExp e, int sp);


/**
 * Return the internal index of the stack register
 * of an architecture, or -1 if the architecture does not have a stack register.
 */
BOOMERANG_API int getStackRegisterIndex(const Prog *prog);
}
