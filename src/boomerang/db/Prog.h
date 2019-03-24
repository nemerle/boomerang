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
#include "boomerang/db/Global.h"
#include "boomerang/db/binary/BinaryFile.h"
#include "boomerang/db/module/ModuleFactory.h"
#include "boomerang/frontend/SigEnum.h"
#include "boomerang/ssl/Register.h"
#include "boomerang/type/DataIntervalMap.h"
#include "boomerang/util/Address.h"

#include <QString>

#include <list>
#include <map>
#include <memory>
#include <set>


class ArrayType;
class BinaryFile;
class BinarySection;
class BinarySymbol;
class Function;
class IFrontEnd;
class LibProc;
class Module;
class Project;
class Signature;
class ISymbolProvider;


class BOOMERANG_API Prog
{
public:
    /// The type for the list of functions.
    typedef std::list<std::unique_ptr<Module>> ModuleList;
    typedef std::map<Address, BinarySymbol *> AddressToSymbolMap;

    typedef std::set<std::shared_ptr<Global>, GlobalComparator> GlobalSet;

public:
    Prog(const QString &name, Project *project);
    Prog(const Prog &other) = delete;
    Prog(Prog &&other)      = delete;

    ~Prog();

    Prog &operator=(const Prog &other) = delete;
    Prog &operator=(Prog &&other) = delete;

public:
    void setFrontEnd(IFrontEnd *fe);
    IFrontEnd *getFrontEnd() const { return m_fe; }

    Project *getProject() { return m_project; }
    const Project *getProject() const { return m_project; }

    /// Assign a new name to this program
    void setName(const QString &name);
    QString getName() const { return m_name; }

    BinaryFile *getBinaryFile() { return m_binaryFile; }
    const BinaryFile *getBinaryFile() const { return m_binaryFile; }

    /**
     * Creates a new empty module.
     * \param name   The name of the new module.
     * \param parent The parent of the new module.
     * \param modFactory Determines the type of Module to be created.
     * \returns the new module, or nullptr if there already exists a module with the same name and
     * parent.
     */
    Module *createModule(const QString &name, Module *parent = nullptr,
                         const IModuleFactory &modFactory = DefaultModFactory());

    /**
     * Create or retrieve existing module
     * \param name retrieve/create module with this name.
     * \param fact abstract factory object that creates Module instance
     */
    Module *getOrInsertModule(const QString &name,
                              const IModuleFactory &fact = DefaultModFactory());

    Module *getRootModule() { return m_rootModule; }
    Module *getRootModule() const { return m_rootModule; }

    Module *findModule(const QString &name);
    const Module *findModule(const QString &name) const;

    bool isModuleUsed(Module *module) const;

    const ModuleList &getModuleList() const { return m_moduleList; }

    /// Add an entry procedure at the specified address.
    /// This will fail if \p entryAddr is already the entry address of a LibProc.
    /// \returns the new or exising entry procedure, or nullptr on failure.
    Function *addEntryPoint(Address entryAddr);

    /**
     * Create a new unnamed function at address \p addr.
     * Call this method when a function is discovered (usually by
     * decoding a call instruction). That way, it is given a name
     * that can be displayed in the dot file, etc. If we assign it
     * a number now, then it will retain this number always.
     *
     * \param entryAddr Address of the entry point of the function
     * \returns Pointer to the Function, or nullptr if this is a deleted
     * (not to be decoded) address
     */
    Function *getOrCreateFunction(Address entryAddr);

    /// lookup a library procedure by name; create if does not exist
    LibProc *getOrCreateLibraryProc(const QString &name);

    /// \returns the function with entry address \p entryAddr,
    /// or nullptr if no such function exists.
    Function *getFunctionByAddr(Address entryAddr) const;

    /// \returns the function with name \p name,
    /// or nullptr if no such function exists.
    Function *getFunctionByName(const QString &name) const;

    /// Removes the function with name \p name.
    /// If there is no such function, nothing happens.
    /// \returns true if function was found and removed.
    bool removeFunction(const QString &name);

    /// \param userOnly If true, only count user functions, not library functions.
    /// \returns the number of functions in this program.
    int getNumFunctions(bool userOnly = true) const;

    /// Check the wellformedness of all the procedures/ProcCFGs in this program
    bool isWellFormed() const;

    /// \returns true if this program was loaded from a PE executable file.
    bool isWin32() const;

    QString getRegNameByNum(RegNum regNum) const;
    int getRegSizeByNum(RegNum regNum) const;

    /// Get a code for the machine e.g. MACHINE_SPARC
    Machine getMachine() const;

    void readDefaultLibraryCatalogues();
    bool addSymbolsFromSymbolFile(const QString &fname);
    std::shared_ptr<Signature> getLibSignature(const QString &name);

    std::shared_ptr<Signature> getDefaultSignature(const QString &name) const;


    /// get a string constant at a given address if appropriate
    /// if knownString, it is already known to be a char*
    /// get a string constant at a give address if appropriate
    const char *getStringConstant(Address addr, bool knownString = false) const;
    bool getFloatConstant(Address addr, double &value, int bits = 64) const;

    /// Get a symbol from an address
    QString getSymbolNameByAddr(Address dest) const;

    const BinarySection *getSectionByAddr(Address addr) const;
    Address getLimitTextLow() const;
    Address getLimitTextHigh() const;

    bool isReadOnly(Address a) const;
    bool isInStringsSection(Address a) const;
    bool isDynamicallyLinkedProcPointer(Address dest) const;

    /// \returns the default module for a symbol with name \p name.
    Module *getOrInsertModuleForSymbol(const QString &symbolName);

    int readNative4(Address a) const;

    void updateLibrarySignatures();


    // Decompilation related

    /// Decode from entry point given as an agrument
    bool decodeEntryPoint(Address entryAddr);

    /// Decode a procedure fragment of \p proc starting at address \p addr.
    bool decodeFragment(UserProc *proc, Address addr);

    /// Re-decode this proc from scratch
    bool reDecode(UserProc *proc);

    const std::list<UserProc *> &getEntryProcs() const { return m_entryProcs; }

    // globals

    /**
     * Create a new global variable at address \p addr.
     * If \p ty and \p name are not specified, they are assigned sensible values automatically
     * using heuristics.
     * This function will fail if the global already exists.
     * \returns the newly created global on success, or nullptr on failure.
     */
    Global *createGlobal(Address addr, SharedType ty = nullptr, QString name = "");

    GlobalSet &getGlobals() { return m_globals; }
    const GlobalSet &getGlobals() const { return m_globals; }

    /// Get a global variable if possible, looking up the loader's symbol table if necessary
    QString getGlobalNameByAddr(Address addr) const;

    /// Get a named global variable if possible, looking up the loader's symbol table if necessary
    Address getGlobalAddrByName(const QString &name) const;
    Global *getGlobalByName(const QString &name) const;

    /**
     * Indicate that a given global is used in the program.
     * The global variable will be created if it does not yet exist.
     *
     * \param uaddr the start address of the global variable in memory
     * \param knownType If not nullptr, update type information of the global by meeting types.
     *
     * \returns true on success, false if the global variable could not be created.
     */
    bool markGlobalUsed(Address uaddr, SharedType knownType = nullptr);

    /// Make an array type for the global array starting at \p startAddr.
    /// Mainly, set the length sensibly
    std::shared_ptr<ArrayType> makeArrayType(Address startAddr, SharedType baseType);

    /// Guess a global's type based on its name and address
    SharedType guessGlobalType(const QString &name, Address addr) const;

    /// Make up a name for a new global at address \a uaddr
    /// (or return an existing name if address already used)
    QString newGlobalName(Address uaddr);

    /// Get the type of a global variable
    SharedType getGlobalType(const QString &name) const;

    /// Set the type of a global variable
    void setGlobalType(const QString &name, SharedType ty);

private:
    QString m_name; ///< name of the program
    Project *m_project       = nullptr;
    BinaryFile *m_binaryFile = nullptr;
    IFrontEnd *m_fe          = nullptr; ///< Pointer to the FrontEnd object for the project
    Module *m_rootModule     = nullptr; ///< Root of the module tree
    ModuleList m_moduleList;            ///< The Modules that make up this program

    /// list of UserProcs for entry point(s)
    std::list<UserProc *> m_entryProcs;

    // FIXME: is a set of Globals the most appropriate data structure? Surely not.
    GlobalSet m_globals;         ///< globals to print at code generation time
    DataIntervalMap m_globalMap; ///< Map from address to DataInterval (has size, name, type)
};
