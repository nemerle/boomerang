#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "BinarySection.h"

#include "boomerang/util/IntervalMap.h"
#include "boomerang/util/IntervalSet.h"
#include "boomerang/util/Util.h"
#include "boomerang/util/log/Log.h"

#include <QVariantMap>


struct VariantHolder
{
    mutable QMultiMap<QString, QVariant> val;
    QVariantMap &get() const { return val; }
    VariantHolder &operator+=(const VariantHolder &other)
    {
        val = val.unite(other.val);
        return *this;
    }

    bool operator==(const VariantHolder &other) const { return val == other.val; }
};

class BinarySectionImpl
{
public:
    void clearDefinedArea() { m_hasDefinedValue.clear(); }

    void addDefinedArea(Address from, Address to) { m_hasDefinedValue.insert(from, to); }

    bool isAddressBss(Address a) const
    {
        if (m_hasDefinedValue.isEmpty()) {
            return true;
        }
        return !m_hasDefinedValue.isContained(a);
    }

    void setAttributeForRange(const QString &name, const QVariant &val, Address from, Address to)
    {
        QVariantMap vmap;

        vmap[name] = val;
        VariantHolder map{ vmap };
        m_attributeMap.insert(from, to, map);
    }

    QVariant attributeInRange(const QString &attrib, Address from, Address to) const
    {
        auto startIt = m_attributeMap.find(from);
        auto endIt   = m_attributeMap.find(to);

        if (startIt == m_attributeMap.end()) {
            return QVariant();
        }

        QList<QVariant> vals;

        for (auto iter = startIt; iter != endIt; ++iter) {
            if (iter->second.get().contains(attrib)) {
                vals << iter->second.get()[attrib];
            }
        }

        if (vals.size() == 1) {
            return vals.front();
        }

        return QVariant(vals);
    }

    QVariantMap getAttributesForRange(Address from, Address to)
    {
        QMultiMap<QString, QVariant> res;
        auto v = m_attributeMap.equalRange(from, to);

        if (v.first == m_attributeMap.end()) {
            return std::move(res);
        }

        for (auto iter = v.first; iter != v.second; ++iter) {
            res.unite(iter->second.get());
        }

        return std::move(res);
    }

public:
    IntervalSet<Address> m_hasDefinedValue;
    IntervalMap<Address, VariantHolder> m_attributeMap;
};


BinarySection::BinarySection(Address sourceAddr, uint64 size, const QString &name)
    : m_impl(new BinarySectionImpl)
    , m_sectionName(name)
    , m_nativeAddr(sourceAddr)
    , m_hostAddr(HostAddress::INVALID)
    , m_size(size)
    , m_sectionEntrySize(0)
    , m_code(false)
    , m_data(false)
    , m_bss(0)
    , m_readOnly(0)
{
}


BinarySection::~BinarySection()
{
    delete m_impl;
}


bool BinarySection::isAddressBss(Address a) const
{
    if (!Util::inRange(a, m_nativeAddr, m_nativeAddr + m_size)) {
        return false;
    }
    else if (m_bss) {
        return true;
    }
    else if (m_readOnly) {
        return false;
    }

    return m_impl->isAddressBss(a);
}


bool BinarySection::anyDefinedValues() const
{
    return !m_impl->m_hasDefinedValue.isEmpty();
}


void BinarySection::resize(uint32_t sz)
{
    LOG_VERBOSE("Function not fully implemented yet");
    m_size = sz;

    //    assert(false && "This function is not implmented yet");
    //    if(sz!=m_Size) {
    //        const BinarySection *sect =
    //        Boomerang::get()->getImage()->getSectionByAddr(uNativeAddr+sz); if(sect==nullptr ||
    //        sect==this ) {
    //        }
    //    }
}


void BinarySection::clearDefinedArea()
{
    m_impl->clearDefinedArea();
}


void BinarySection::addDefinedArea(Address from, Address to)
{
    m_impl->addDefinedArea(from, to);
}


void BinarySection::setAttributeForRange(const QString &name, const QVariant &val, Address from,
                                         Address to)
{
    m_impl->setAttributeForRange(name, val, from, to);
}


QVariantMap BinarySection::getAttributesForRange(Address from, Address to)
{
    return m_impl->getAttributesForRange(from, to);
}


bool BinarySection::isAttributeInRange(const QString &attrib, Address from, Address to) const
{
    return !m_impl->attributeInRange(attrib, from, to).isNull();
}
