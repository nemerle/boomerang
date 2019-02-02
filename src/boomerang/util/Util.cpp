#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "Util.h"

#include "boomerang/db/Prog.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/util/Types.h"

#include <QMap>
#include <QString>
#include <QTextStreamManipulator>

#include <cassert>
#include <string>


namespace Util
{
QString escapeStr(const char *inp)
{
    // clang-format off
    static const std::map<char, QString> replacements{
        { '\n', "\\n" },
        { '\t', "\\t" },
        { '\v', "\\v" },
        { '\b', "\\b" },
        { '\r', "\\r" },
        { '\f', "\\f" },
        { '\a', "\\a" },
        { '"', "\\\"" }
    };
    // clang-format on

    QString result;

    for (char c : std::string(inp)) {
        if (isprint(c) && c != '\"') {
            result += c;
            continue;
        }

        auto it = replacements.find(c);
        if (it != replacements.end()) {
            result += it->second;
        }
        else {
            result += '\\';
            result += QString::number(c & 0xFF, 8);
        }
    }

    return result;
}


OStream &alignStream(OStream &str, int align)
{
    str << qSetFieldWidth(align) << " " << qSetFieldWidth(0);
    return str;
}


int getStackOffset(SharedConstExp e, int sp)
{
    int ret = 0;

    if (e->isMemOf()) {
        SharedConstExp sub = e->getSubExp1();
        OPER op            = sub->getOper();

        if ((op == opPlus) || (op == opMinus)) {
            SharedConstExp op1 = sub->getSubExp1();

            if (op1->isSubscript()) {
                op1 = op1->getSubExp1();
            }

            if (op1->isRegN(sp)) {
                SharedConstExp op2 = sub->getSubExp2();

                if (op2->isIntConst()) {
                    ret = op2->access<const Const>()->getInt();
                }

                if (op == opMinus) {
                    ret = -ret;
                }
            }
        }
    }

    return ret;
}


int getStackRegisterIndex(const Prog *prog)
{
    switch (prog->getMachine()) {
    case Machine::SPARC: return REG_SPARC_SP;
    case Machine::PENTIUM: return REG_PENT_ESP;
    case Machine::PPC: return REG_PPC_G1;
    case Machine::ST20: return REG_ST20_SP;
    default: return -1;
    }
}
}
