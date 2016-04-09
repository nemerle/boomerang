/**
 * \file types.h
 * \brief Contains some often used basic type definitions
 */
#ifndef __TYPES_H__
#define __TYPES_H__

#include <QObject>
#include <iosfwd>
#include <stdint.h>
#include "util.h"
class QTextStream;
// Machine types
typedef uint8_t Byte;   /* 8 bits */
typedef uint16_t SWord; /* 16 bits */
typedef uint32_t DWord; /* 32 bits */
struct ADDRESS {        /* pointer. size depends on platform */
                        //    ADDRESS() {}
                        //    ADDRESS(uint32_t v) : m_value(v) {}
    typedef uintptr_t value_type;
    value_type m_value;
    static ADDRESS g(value_type x) { // construct host/native oblivious address
        ADDRESS z;
        z.m_value = x;
        return z;
    }
    static ADDRESS n(value_type x) { // construct native address
        ADDRESS z;
        z.m_value = x;
        return z.native();
    }
    //! query if the ADDRESS is the source, if it's host address returns false
    bool isSourceAddr() const { return sizeof(m_value) == 4 || (uint64_t(m_value) >> 32) == 0; }
    ADDRESS native() const { return ADDRESS::g(m_value & 0xFFFFFFFF); }
    static ADDRESS host_ptr(const void *x) {
        ADDRESS z;
        z.m_value = value_type(x);
        return z;
    }
    bool isZero() const { return m_value == 0; }
    bool operator==(const ADDRESS &other) const { return m_value == other.m_value; }
    bool operator!=(const ADDRESS &other) const { return m_value != other.m_value; }
    bool operator<(const ADDRESS &other) const { return m_value < other.m_value; }
    bool operator>(const ADDRESS &other) const { return m_value > other.m_value; }
    bool operator>=(const ADDRESS &other) const { return m_value >= other.m_value; }
    bool operator<=(const ADDRESS &other) const { return m_value <= other.m_value; }

    ADDRESS operator+(const ADDRESS &other) const { return ADDRESS::g(m_value + other.m_value); }
    ADDRESS operator++() {
        ++m_value;
        return *this;
    }
    ADDRESS operator++(int) {
        ADDRESS res = *this;
        ++m_value;
        return res;
    }
    ADDRESS operator--() {
        --m_value;
        return *this;
    }
    ADDRESS operator--(int) {
        ADDRESS res = *this;
        --m_value;
        return res;
    }
    ADDRESS operator+=(const ADDRESS &other) {
        m_value += other.m_value;
        return *this;
    }
    ADDRESS operator+=(intptr_t other) {
        m_value += other;
        return *this;
    }
    ADDRESS &operator=(intptr_t v) {
        m_value = v;
        return *this;
    }
    ADDRESS operator+(intptr_t val) const { return ADDRESS::g(m_value + val); }
    ADDRESS operator-(const ADDRESS &other) const { return ADDRESS::g(m_value - other.m_value); }
    ADDRESS operator-=(intptr_t v) {
        m_value -= v;
        return *this;
    }
    ADDRESS operator-(intptr_t other) const { return ADDRESS::g(m_value - other); }
    friend QTextStream& operator<< (QTextStream& os, const ADDRESS& mdv);\
    QString toString(bool zerofill=false) const {
        if(zerofill)
            return QString("%1").arg(m_value,8,16,QChar('0'));
        return QString::number(m_value,16);
    }
    // operator intptr_t() const {return int(m_value);}
};
template <class T, class U> bool IN_RANGE(const T &val, const U &range_start, const U &range_end) {
    return ((val >= range_start) && (val < range_end));
}
Q_DECLARE_METATYPE(ADDRESS)

#define STD_SIZE 32 // Standard size
// Note: there is a known name collision with NO_ADDRESS in WinSock.h
#ifdef NO_ADDRESS
#undef NO_ADDRESS
#endif
#define NO_ADDRESS ADDRESS::g(~0u) // For invalid ADDRESSes

typedef uint64_t QWord; // 64 bits

#endif // #ifndef __TYPES_H__
