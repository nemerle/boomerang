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


#include "boomerang/ssl/Register.h"
#include "boomerang/ssl/TableEntry.h"
#include "boomerang/util/ByteUtil.h"

#include <map>
#include <set>
#include <vector>


class Exp;
class Statement;
class Type;
class OStream;


using SharedExp = std::shared_ptr<Exp>;


/**
 * The RTLInstDict represents a dictionary that maps instruction names to the
 * parameters they take and a template for the Exp list describing their
 * semantics. It handles both the parsing of the SSL file that fills in
 * the dictionary entries as well as instantiation of an Exp list for a given
 * instruction name and list of actual parameters.
 */
class BOOMERANG_API RTLInstDict
{
    friend class SSLParser;

public:
    RTLInstDict(bool verboseOutput = false);
    RTLInstDict(const RTLInstDict &) = delete;
    RTLInstDict(RTLInstDict &&)      = default;

    ~RTLInstDict() = default;

    RTLInstDict &operator=(const RTLInstDict &) = delete;
    RTLInstDict &operator=(RTLInstDict &&) = default;

public:
    /**
     * Read and parse the SSL file, and initialise the expanded instruction dictionary
     * (this object). This also reads and sets up the register map and flag functions.
     *
     * \param sslFileName the name of the file containing the SSL specification.
     * \returns           true if the file was read successfully.
     */
    bool readSSLFile(const QString &sslFileName);

    /// \returns the name and the number of operands of the instruction
    /// with name \p instructionName
    std::pair<QString, int> getSignature(const QString &instructionName) const;

    /**
     * Returns a new RTL containing the semantics of the instruction with name \p name.
     *
     * \param name    the name of the instruction (must correspond to one defined in the SSL file).
     * \param pc      address at which the named instruction is located
     * \param actuals the actual values of the instruction parameters
     */
    std::unique_ptr<RTL> instantiateRTL(const QString &name, Address pc,
                                        const std::vector<SharedExp> &actuals);

    /// Get the name of the register by its index.
    /// Returns the empty string when \p regID == -1 or the register was not found.
    QString getRegNameByID(int regID) const;

    /// Get the index of a named register by its name.
    /// Returns -1 if the register was not found.
    int getRegIDByName(const QString &regName) const;

    /// Get the size in bits of a register by its index.
    /// Returns 32 (the default register size) if the register was not found.
    int getRegSizeByID(int regID) const;

private:
    /// Reset the object to "undo" a readSSLFile()
    void reset();

    /**
     * Returns an instance of a register transfer list for the parameterized rtlist with the given
     * formals replaced with the actuals given as the third parameter.
     *
     * \param   rtls    a register transfer list
     * \param   pc      address at which the named instruction is located
     * \param   params  a list of formal parameters
     * \param   actuals the actual parameter values
     * \returns the instantiated list of Exps
     */
    std::unique_ptr<RTL> instantiateRTL(RTL &rtls, Address pc, std::list<QString> &params,
                                        const std::vector<SharedExp> &actuals);

    /**
     * Appends one RTL to the dictionary, or adds it to idict if an
     * entry does not already exist.
     *
     * \param name name of the instruction to add to
     * \param parameters list of formal parameters (as strings) for the RTL to add
     * \param rtl reference to the RTL to add
     * \returns zero for success, non-zero for failure
     */
    int insert(const QString &name, std::list<QString> &parameters, const RTL &rtl);

    /// Print a textual representation of the dictionary.
    void print(OStream &os);

    /**
     * Add a new register definition to the dictionary
     * \param name register's name
     * \param size - register size in bits
     * \param flt  - is float register?
     */
    void addRegister(const QString &name, int id, int size, bool flt);

private:
    /// Print messages when reading an SSL file or when instantiaing an instruction
    bool m_verboseOutput;

    /// Endianness of the source machine
    Endian m_endianness;

    /// A map from the symbolic representation of a register (e.g. "%g0")
    /// to its index within an array of registers.
    /// This map contains both normal and special (-> -1) registers,
    /// therefore this map contains all registers.
    std::map<QString, int> m_regIDs;

    /// Stores info about a register such as its size, its addresss etc
    /// (see register.h).
    std::map<int, Register> m_regInfo;

    /// A map from symbolic representation of a special (non-addressable) register
    /// to a Register object
    std::map<QString, Register> m_specialRegInfo;

    /// FIXME this set contains all parameters of every flag function ever defined,
    /// not only those from the current flag function
    std::set<QString> m_definedParams;

    /// All names of defined flag functions
    std::set<QString> m_flagFuncs;

    /// The actual dictionary.
    std::map<QString, TableEntry> m_instructions;
};
