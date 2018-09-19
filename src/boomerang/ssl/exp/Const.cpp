#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "Const.h"

#include "boomerang/db/proc/Proc.h"
#include "boomerang/ssl/type/CharType.h"
#include "boomerang/ssl/type/FloatType.h"
#include "boomerang/ssl/type/FuncType.h"
#include "boomerang/ssl/type/IntegerType.h"
#include "boomerang/ssl/type/PointerType.h"
#include "boomerang/ssl/type/VoidType.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/visitor/expmodifier/ExpModifier.h"
#include "boomerang/visitor/expvisitor/ExpVisitor.h"


Const::Const(uint32_t i)
    : Exp(opIntConst)
    , m_type(VoidType::get())
{
    m_value.i = i;
}


Const::Const(int i)
    : Exp(opIntConst)
    , m_type(VoidType::get())
{
    m_value.i = i;
}


Const::Const(QWord ll)
    : Exp(opLongConst)
    , m_type(VoidType::get())
{
    m_value.ll = ll;
}


Const::Const(double d)
    : Exp(opFltConst)
    , m_type(VoidType::get())
{
    m_value.d = d;
}


Const::Const(const QString &p)
    : Exp(opStrConst)
    , m_type(VoidType::get())
{
    m_string = p;
}


Const::Const(Function *p)
    : Exp(opFuncConst)
    , m_type(VoidType::get())
{
    m_value.pp = p;
}


Const::Const(Address addr)
    : Exp(opIntConst)
    , m_type(VoidType::get())
{
    m_value.ll = addr.value();
}


Const::Const(const Const &other)
    : Exp(other.m_oper)
    , m_string(other.m_string)
    , m_type(other.m_type)
{
    memcpy(&m_value, &other.m_value, sizeof(m_value));
}


bool Const::operator<(const Exp &o) const
{
    if (m_oper != o.getOper()) {
        return m_oper < o.getOper();
    }

    const Const &otherConst = static_cast<const Const &>(o);

    switch (m_oper) {
    case opIntConst: return m_value.i < otherConst.m_value.i;
    case opLongConst: return m_value.ll < otherConst.m_value.ll;
    case opFltConst: return m_value.d < otherConst.m_value.d;
    case opStrConst: return m_string < otherConst.m_string;

    default: LOG_FATAL("Invalid operator %1", operToString(m_oper));
    }

    return false;
}


bool Const::operator*=(const Exp &o) const
{
    const Exp *other = &o;

    if (o.getOper() == opSubscript) {
        other = o.getSubExp1().get();
    }

    return *this == *other;
}


QString Const::getFuncName() const
{
    return m_value.pp->getName();
}


SharedExp Const::clone() const
{
    // Note: not actually cloning the Type* type pointer. Probably doesn't matter with GC
    return Const::get(*this);
}


void Const::printNoQuotes(OStream &os) const
{
    if (m_oper == opStrConst) {
        os << m_string;
    }
    else {
        print(os);
    }
}


bool Const::operator==(const Exp &other) const
{
    // Note: the casts of o to Const& are needed, else op is protected! Duh.
    if (other.getOper() == opWild) {
        return true;
    }

    if ((other.getOper() == opWildIntConst) && (m_oper == opIntConst)) {
        return true;
    }

    if ((other.getOper() == opWildStrConst) && (m_oper == opStrConst)) {
        return true;
    }

    if (m_oper != other.getOper()) {
        return false;
    }

    switch (m_oper) {
    case opIntConst: return m_value.i == static_cast<const Const &>(other).m_value.i;
    case opLongConst: return m_value.ll == static_cast<const Const &>(other).m_value.ll;
    case opFltConst: return m_value.d == static_cast<const Const &>(other).m_value.d;
    case opStrConst: return m_string == static_cast<const Const &>(other).m_string;
    default: LOG_FATAL("Invalid operator %1", operToString(m_oper));
    }

    return false;
}


SharedType Const::ascendType()
{
    if (m_type->resolvesToVoid()) {
        switch (m_oper) {
            // could be anything, Boolean, Character, we could be bit fiddling pointers for all we
            // know - trentw
        case opIntConst: return VoidType::get();
        case opLongConst: return m_type = IntegerType::get(STD_SIZE * 2, Sign::Unknown);
        case opFltConst: return m_type = FloatType::get(64);
        case opStrConst: return m_type = PointerType::get(CharType::get());
        case opFuncConst: return m_type = PointerType::get(FuncType::get());
        default: assert(false); // Bad Const
        }
    }

    return m_type;
}


void Const::descendType(SharedType parentType, bool &changed, Statement *)
{
    bool thisCh = false;

    m_type = m_type->meetWith(parentType, thisCh);
    changed |= thisCh;

    if (thisCh) {
        // May need to change the representation
        if (m_type->resolvesToFloat()) {
            if (m_oper == opIntConst) {
                m_oper    = opFltConst;
                m_type    = FloatType::get(64);
                float f   = *reinterpret_cast<float *>(&m_value.i);
                m_value.d = static_cast<double>(f);
            }
            else if (m_oper == opLongConst) {
                m_oper    = opFltConst;
                m_type    = FloatType::get(64);
                m_value.d = *reinterpret_cast<double *>(&m_value.ll);
            }
        }

        // May be other cases
    }
}


bool Const::acceptVisitor(ExpVisitor *v)
{
    return v->visit(shared_from_base<Const>());
}


SharedExp Const::acceptPreModifier(ExpModifier *, bool &)
{
    return shared_from_this();
}


SharedExp Const::acceptPostModifier(ExpModifier *mod)
{
    return mod->postModify(access<Const>());
}
