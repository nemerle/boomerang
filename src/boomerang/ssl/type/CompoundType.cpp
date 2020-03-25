#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "CompoundType.h"

#include "boomerang/ssl/type/SizeType.h"
#include "boomerang/util/Util.h"


CompoundType::CompoundType()
    : Type(TypeClass::Compound)
{
}


CompoundType::~CompoundType()
{
}


SharedType CompoundType::clone() const
{
    auto t = CompoundType::get();

    for (int i = 0; i < getNumMembers(); i++) {
        t->addMember(m_types[i]->clone(), m_names[i]);
    }

    return t;
}


bool CompoundType::isCompatibleWith(const Type &other, bool all) const
{
    return isCompatible(other, all);
}


Type::Size CompoundType::getSize() const
{
    Size size = 0;

    for (const SharedType &elem : m_types) {
        // NOTE: this assumes no padding... perhaps explicit padding will be needed
        size += elem->getSize();
    }

    return size;
}


bool CompoundType::isSuperStructOf(const SharedConstType &other) const
{
    if (!other->isCompound()) {
        return false;
    }

    auto otherCmp = other->as<CompoundType>();
    if (otherCmp->getNumMembers() < getNumMembers()) {
        return false;
    }

    for (int i = 0; i < getNumMembers(); i++) {
        if (*otherCmp->m_types[i] != *m_types[i]) {
            return false;
        }
    }

    return true;
}


bool CompoundType::isSubStructOf(const SharedConstType &other) const
{
    if (!other->isCompound()) {
        return false;
    }

    return other->as<CompoundType>()->isSuperStructOf(shared_from_this());
}


SharedType CompoundType::getMemberTypeByName(const QString &name)
{
    for (int i = 0; i < getNumMembers(); i++) {
        if (m_names[i] == name) {
            return m_types[i];
        }
    }

    return nullptr;
}


SharedType CompoundType::getMemberTypeByOffset(uint64 bitOffset)
{
    uint64 offset = 0;

    for (auto &elem : m_types) {
        if (Util::inRange(bitOffset, offset, offset + elem->getSize())) {
            return elem;
        }

        offset += elem->getSize();
    }

    return nullptr;
}


void CompoundType::setMemberTypeByOffset(uint64 bitOffset, SharedType ty)
{
    uint64 offset = 0;

    for (int i = 0; i < getNumMembers(); i++) {
        if (Util::inRange(bitOffset, offset, offset + m_types[i]->getSize())) {
            unsigned oldsz = m_types[i]->getSize();
            m_types[i]     = ty;

            if (ty->getSize() < oldsz) {
                m_types.push_back(m_types[m_types.size() - 1]);
                m_names.push_back(m_names[m_names.size() - 1]);

                for (int _n = getNumMembers() - 1; _n > i; _n--) {
                    m_types[_n] = m_types[_n - 1];
                    m_names[_n] = m_names[_n - 1];
                }

                m_types[i + 1] = SizeType::get(oldsz - ty->getSize());
                m_names[i + 1] = "pad";
            }

            return;
        }

        offset += m_types[i]->getSize();
    }
}


void CompoundType::setMemberNameByOffset(uint64 bitOffset, const QString &name)
{
    uint64 offset = 0;

    for (int i = 0; i < getNumMembers(); i++) {
        if (Util::inRange(bitOffset, offset, offset + m_types[i]->getSize())) {
            m_names[i] = name;
            return;
        }

        offset += m_types[i]->getSize();
    }
}


QString CompoundType::getMemberNameByOffset(uint64 n)
{
    uint64 offset = 0;

    for (int i = 0; i < getNumMembers(); i++) {
        // if (offset >= n && n < offset + types[i]->getSize())
        if ((offset <= n) && (n < offset + m_types[i]->getSize())) {
            // return getName(offset == n ? i : i - 1);
            return m_names[i];
        }

        offset += m_types[i]->getSize();
    }

    return "";
}


uint64 CompoundType::getMemberOffsetByIdx(int n)
{
    assert(n < getNumMembers());

    uint64 offset = 0;
    for (int i = 0; i < n; i++) {
        offset += m_types[i]->getSize();
    }

    return offset;
}


uint64 CompoundType::getMemberOffsetByName(const QString &member)
{
    uint64 offset = 0;

    for (int i = 0; i < getNumMembers(); i++) {
        if (m_names[i] == member) {
            return offset;
        }

        offset += m_types[i]->getSize();
    }

    return static_cast<unsigned int>(-1);
}


uint64 CompoundType::getOffsetRemainder(uint64 bitSize)
{
    uint64 remainder = bitSize;
    uint64 offset    = 0;

    for (auto &elem : m_types) {
        offset += elem->getSize();

        if (offset > bitSize) {
            break;
        }

        remainder -= elem->getSize();
    }

    return remainder;
}


bool CompoundType::operator==(const Type &other) const
{
    if (getId() != other.getId()) {
        return false;
    }
    else if (getSize() != other.getSize()) {
        return false;
    }

    const CompoundType &otherCompound = static_cast<const CompoundType &>(other);
    if (getNumMembers() != otherCompound.getNumMembers()) {
        return false;
    }

    for (int i = 0; i < getNumMembers(); ++i) {
        if (*m_types[i] != *otherCompound.m_types[i]) {
            return false;
        }
    }

    return true;
}


bool CompoundType::operator<(const Type &other) const
{
    if (getId() != other.getId()) {
        return getId() < other.getId();
    }

    if (getSize() != other.getSize()) {
        return getSize() < other.getSize();
    }

    const CompoundType &otherCompound = static_cast<const CompoundType &>(other);
    if (getNumMembers() != otherCompound.getNumMembers()) {
        return getNumMembers() < otherCompound.getNumMembers();
    }

    for (int i = 0; i < getNumMembers(); ++i) {
        if (*m_types[i] != *otherCompound.m_types[i]) {
            return *m_types[i] < *otherCompound.m_types[i];
        }
    }

    return false; // equal
}


QString CompoundType::getCtype(bool final) const
{
    QString tmp("struct { ");

    for (unsigned i = 0; i < m_types.size(); i++) {
        tmp += m_types[i]->getCtype(final);

        if (m_names[i] != "") {
            tmp += " ";
            tmp += m_names[i];
        }

        tmp += "; ";
    }

    tmp += "}";
    return tmp;
}


void CompoundType::addMember(SharedType memberType, const QString &memberName)
{
    // check if it is a user defined type (typedef)
    SharedType existingType = getNamedType(memberType->getCtype());

    if (existingType != nullptr) {
        memberType = existingType;
    }

    m_types.push_back(memberType);
    m_names.push_back(memberName);
}


SharedType CompoundType::getMemberTypeByIdx(int idx)
{
    assert(idx < getNumMembers());
    return m_types[idx];
}


QString CompoundType::getMemberNameByIdx(int idx)
{
    assert(idx < getNumMembers());
    return m_names[idx];
}


SharedType CompoundType::meetWith(SharedType other, bool &changed, bool useHighestPtr) const
{
    if (other->resolvesToVoid()) {
        return const_cast<CompoundType *>(this)->shared_from_this();
    }

    if (!other->resolvesToCompound()) {
        if (m_types[0]->isCompatibleWith(*other)) {
            // struct meet first element = struct
            return const_cast<CompoundType *>(this)->shared_from_this();
        }

        return createUnion(other, changed, useHighestPtr);
    }

    std::shared_ptr<CompoundType> otherCmp = other->as<CompoundType>();

    if (*this == *other) {
        return const_cast<CompoundType *>(this)->shared_from_this();
    }
    else if (otherCmp->isSuperStructOf(const_cast<CompoundType *>(this)->shared_from_this())) {
        // The other structure has a superset of my struct's offsets. Preserve the names etc of the
        // bigger struct.
        changed = true;
        return other;
    }
    else if (isSubStructOf(otherCmp)) {
        // This is a superstruct of other
        changed = true;
        return const_cast<CompoundType *>(this)->shared_from_this();
    }

    // Not compatible structs. Create a union of both complete structs.
    // NOTE: may be possible to take advantage of some overlaps of the two structures some day.
    return createUnion(other, changed, useHighestPtr);
}


bool CompoundType::isCompatible(const Type &other, bool all) const
{
    if (other.resolvesToVoid()) {
        return true;
    }

    if (other.resolvesToUnion()) {
        return other.isCompatibleWith(*this, all);
    }

    if (!other.resolvesToCompound()) {
        // Used to always return false here. But in fact, a struct is compatible with its first
        // member (if all is false)
        return !all && getNumMembers() > 0 && m_types[0]->isCompatibleWith(other);
    }

    auto otherComp = other.as<CompoundType>();
    if (otherComp->getNumMembers() != getNumMembers()) {
        return false; // Is a subcompound compatible with a supercompound?
    }

    const int n = otherComp->getNumMembers();
    for (int i = 0; i < n; i++) {
        if (!m_types[i]->isCompatibleWith(*otherComp->m_types[i])) {
            return false;
        }
    }

    return true;
}
