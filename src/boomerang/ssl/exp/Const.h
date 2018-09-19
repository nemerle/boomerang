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


class Function;


/**
 * Const is a subclass of Exp, and holds either an integer,
 * floating point, string, or address constant
 */
class BOOMERANG_API Const : public Exp
{
private:
    union Data
    {
        int i;        ///< Integer
        QWord ll = 0; ///< 64 bit integer / address / pointer
        double d;     ///< Double precision float

        /// Don't store string: function could be renamed
        Function *pp; ///< Pointer to function (e.g. global function pointers)
    };
    static_assert(sizeof(Data) == 8, "Const data must be 64 bits");

public:
    // Special constructors overloaded for the various constants
    Const(uint32_t i);
    Const(int i);
    Const(QWord ll);

    /// \remark This is bad. We need a way of constructing true unsigned constants
    Const(Address a);
    Const(double d);
    Const(const QString &p);
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

    /// \copydoc Exp::operator*=
    virtual bool operator*=(const Exp &o) const override;

    // Get the constant
    int getInt() const { return m_value.i; }
    QWord getLong() const { return m_value.ll; }
    double getFlt() const { return m_value.d; }
    QString getStr() const { return m_string; }
    Address getAddr() const { return Address(static_cast<Address::value_type>(m_value.ll)); }
    QString getFuncName() const;

    // Set the constant
    void setInt(int i) { m_value.i = i; }
    void setLong(QWord ll) { m_value.ll = ll; }
    void setFlt(double d) { m_value.d = d; }
    void setStr(const QString &p) { m_string = p; }
    void setAddr(Address a) { m_value.ll = a.value(); }

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
    virtual void descendType(SharedType parentType, bool &changed, Statement *s) override;

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
    QString m_string;  ///< The string value of this constant
    SharedType m_type; ///< Constants need types during type analysis
};
