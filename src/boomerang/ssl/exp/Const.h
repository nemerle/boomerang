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


#include "boomerang/ssl/exp/Exp.h"
#include "boomerang/util/Address.h"

#include <variant>


class Function;


/// Const is a terminal expression holding either an integer, floating point,
/// string, or address constant.
class BOOMERANG_API Const : public Exp
{
private:
    typedef std::variant<int,         ///< Integer
                         QWord,       ///< 64 bit integer / address / pointer
                         double,      ///< Double precision float
                         Function *,  ///< Pointer to function (e.g. global function pointers)
                         QString,     ///< The string value of this constant (for identifiers etc.)
                         const char * ///< The raw string value of this constant
                         >
        Data;

public:
    // Special constructors overloaded for the various constants
    Const(uint32_t i);
    Const(int i);
    Const(QWord ll);

    /// \remark This is bad. We need a way of constructing true unsigned constants
    Const(Address a);
    Const(double d);
    Const(const QString &p);
    Const(const char *rawString);
    Const(Function *p);

    Const(const Const &other);
    Const(Const &&other) = default;

    /// Nothing to destruct: Don't deallocate the string passed to constructor
    virtual ~Const() override = default;

    Const &operator=(const Const &) = default;
    Const &operator=(Const &&) = default;

public:
    /// \copydoc Exp::clone
    virtual SharedExp clone() const override;

    template<class T>
    static std::shared_ptr<Const> get(T i)
    {
        return std::make_shared<Const>(i);
    }

    template<class T>
    static std::shared_ptr<Const> get(T i, SharedType ty)
    {
        std::shared_ptr<Const> c = std::make_shared<Const>(i);
        c->setType(ty);
        return c;
    }

    /// \copydoc Exp::operator==
    virtual bool operator==(const Exp &o) const override;

    /// \copydoc Exp::operator<
    virtual bool operator<(const Exp &o) const override;

    /// \copydoc Exp::equalNoSubscript
    virtual bool equalNoSubscript(const Exp &o) const override;

    // Get the constant
    int getInt() const;
    QWord getLong() const;
    double getFlt() const;
    QString getStr() const;
    const char *getRawStr() const;
    Address getAddr() const;
    QString getFuncName() const;

    // Set the constant
    void setInt(int i);
    void setLong(QWord ll);
    void setFlt(double d);
    void setStr(const QString &p);
    void setRawStr(const char *p);
    void setAddr(Address a);

    /// \returns the type of the constant
    SharedType getType() { return m_type; }
    const SharedType getType() const { return m_type; }

    /// Changes the type of this constant
    void setType(SharedType ty) { m_type = ty; }

    /// Print "recursive" (extra parens not wanted at outer levels)
    void printNoQuotes(OStream &os) const;

    /// \copydoc Exp::ascendType
    virtual SharedType ascendType() override;

    /// \copydoc Exp::descendType
    virtual bool descendType(SharedType newType) override;

public:
    /// \copydoc Exp::acceptVisitor
    virtual bool acceptVisitor(ExpVisitor *v) override;

protected:
    /// \copydoc Exp::acceptPreModifier
    virtual SharedExp acceptPreModifier(ExpModifier *mod, bool &visitChildren) override;

    /// \copydoc Exp::acceptPostModifier
    virtual SharedExp acceptPostModifier(ExpModifier *mod) override;

private:
    Data m_value;      ///< The value of this constant
    SharedType m_type; ///< Constants need types during type analysis
};
