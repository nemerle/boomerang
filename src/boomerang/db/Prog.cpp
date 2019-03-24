#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "Prog.h"

#include "boomerang/core/Project.h"
#include "boomerang/core/Settings.h"
#include "boomerang/db/DebugInfo.h"
#include "boomerang/db/Global.h"
#include "boomerang/db/binary/BinaryFile.h"
#include "boomerang/db/binary/BinaryImage.h"
#include "boomerang/db/binary/BinarySection.h"
#include "boomerang/db/binary/BinarySymbol.h"
#include "boomerang/db/binary/BinarySymbolTable.h"
#include "boomerang/db/module/Module.h"
#include "boomerang/db/proc/LibProc.h"
#include "boomerang/db/proc/ProcCFG.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/ifc/ICodeGenerator.h"
#include "boomerang/ifc/IDecoder.h"
#include "boomerang/ifc/IFrontEnd.h"
#include "boomerang/passes/PassManager.h"
#include "boomerang/ssl/RTL.h"
#include "boomerang/ssl/Register.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/exp/Terminal.h"
#include "boomerang/ssl/type/ArrayType.h"
#include "boomerang/ssl/type/CharType.h"
#include "boomerang/ssl/type/FloatType.h"
#include "boomerang/ssl/type/IntegerType.h"
#include "boomerang/ssl/type/PointerType.h"
#include "boomerang/ssl/type/SizeType.h"
#include "boomerang/util/Types.h"
#include "boomerang/util/Util.h"
#include "boomerang/util/log/Log.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <cctype>


Prog::Prog(const QString &name, Project *project)
    : m_name(name)
    , m_project(project)
    , m_binaryFile(project ? project->getLoadedBinaryFile() : nullptr)
    , m_fe(nullptr)
{
    m_rootModule = getOrInsertModule(getName());
    assert(m_rootModule != nullptr);
}


Prog::~Prog()
{
}


void Prog::setFrontEnd(IFrontEnd *frontEnd)
{
    m_fe = frontEnd;

    m_moduleList.clear();
    m_rootModule = getOrInsertModule(m_name);
}


void Prog::setName(const QString &name)
{
    m_name = name;
    if (m_rootModule) {
        m_rootModule->setName(name);
    }
}


Module *Prog::createModule(const QString &name, Module *parentModule, const IModuleFactory &factory)
{
    if (parentModule == nullptr) {
        parentModule = m_rootModule;
    }

    Module *module = m_rootModule->find(name);

    if (module && (module->getParentModule() == parentModule)) {
        // a module already exists
        return nullptr;
    }

    module = factory.create(name, this);
    parentModule->addChild(module);
    m_moduleList.push_back(std::unique_ptr<Module>(module));
    return module;
}


Module *Prog::getOrInsertModule(const QString &name, const IModuleFactory &fact)
{
    for (const auto &m : m_moduleList) {
        if (m->getName() == name) {
            return m.get();
        }
    }

    Module *m = fact.create(name, this);
    m_moduleList.push_back(std::unique_ptr<Module>(m));
    return m;
}


Module *Prog::findModule(const QString &name)
{
    auto it = std::find_if(
        m_moduleList.begin(), m_moduleList.end(),
        [name](const std::unique_ptr<Module> &mod) { return mod->getName() == name; });

    return (it != m_moduleList.end()) ? it->get() : nullptr;
}


const Module *Prog::findModule(const QString &name) const
{
    auto it = std::find_if(
        m_moduleList.begin(), m_moduleList.end(),
        [name](const std::unique_ptr<Module> &mod) { return mod->getName() == name; });

    return (it != m_moduleList.end()) ? it->get() : nullptr;
}


bool Prog::isModuleUsed(Module *module) const
{
    // TODO: maybe module can have no procedures and still be used ?
    return !module->empty();
}


Function *Prog::addEntryPoint(Address entryAddr)
{
    Function *func = getFunctionByAddr(entryAddr);
    if (!func) {
        func = getOrCreateFunction(entryAddr);
    }

    if (func && !func->isLib()) {
        UserProc *proc = static_cast<UserProc *>(func);
        if (std::find(m_entryProcs.begin(), m_entryProcs.end(), proc) == m_entryProcs.end()) {
            m_entryProcs.push_back(static_cast<UserProc *>(func));
        }
        return proc;
    }
    else {
        return nullptr;
    }
}


Function *Prog::getOrCreateFunction(Address startAddress)
{
    if (startAddress == Address::INVALID) {
        return nullptr;
    }

    // Check if we already have this proc
    Function *existingFunction = getFunctionByAddr(startAddress);

    if (existingFunction) {      // Exists already ?
        return existingFunction; // Yes, we are done
    }

    Address tgt = m_binaryFile ? m_binaryFile->getJumpTarget(startAddress) : Address::INVALID;


    if (tgt != Address::INVALID) {
        startAddress = tgt;
    }

    existingFunction = getFunctionByAddr(startAddress);

    if (existingFunction) {      // Exists already ?
        return existingFunction; // Yes, we are done
    }

    QString procName;
    bool isLibFunction      = false;
    const BinarySymbol *sym = m_binaryFile
                                  ? m_binaryFile->getSymbols()->findSymbolByAddress(startAddress)
                                  : nullptr;

    if (sym) {
        isLibFunction = sym->isImportedFunction() || sym->isStaticFunction();
        procName      = sym->getName();
    }

    if (procName.isEmpty()) {
        // No name. Give it the name of the start address.
        procName = QString("proc_%1").arg(startAddress.toString());
        LOG_VERBOSE("Assigning name %1 to address %2", procName, startAddress);
    }

    return m_rootModule->createFunction(procName, startAddress, isLibFunction);
}


LibProc *Prog::getOrCreateLibraryProc(const QString &name)
{
    if (name == "") {
        return nullptr;
    }

    Function *existingProc = getFunctionByName(name);

    if (existingProc && existingProc->isLib()) {
        return static_cast<LibProc *>(existingProc);
    }

    return static_cast<LibProc *>(m_rootModule->createFunction(name, Address::INVALID, true));
}


Function *Prog::getFunctionByAddr(Address entryAddr) const
{
    for (const auto &m : m_moduleList) {
        Function *proc = m->getFunction(entryAddr);

        if (proc != nullptr) {
            assert(proc != reinterpret_cast<Function *>(-1));
            return proc;
        }
    }

    return nullptr;
}


Function *Prog::getFunctionByName(const QString &name) const
{
    for (const auto &module : m_moduleList) {
        Function *f = module->getFunction(name);

        if (f) {
            assert(f != reinterpret_cast<Function *>(-1));
            return f;
        }
    }

    return nullptr;
}


bool Prog::removeFunction(const QString &name)
{
    Function *function = getFunctionByName(name);

    if (function) {
        function->removeFromModule();
        m_project->alertFunctionRemoved(function);
        // FIXME: this function removes the function from module, but it leaks it
        return true;
    }

    return false;
}


int Prog::getNumFunctions(bool userOnly) const
{
    int n = 0;

    if (userOnly) {
        for (const auto &m : m_moduleList) {
            for (Function *proc : *m) {
                if (!proc->isLib()) {
                    n++;
                }
            }
        }
    }
    else {
        for (const auto &module : m_moduleList) {
            n += module->size();
        }
    }

    return n;
}


bool Prog::isWellFormed() const
{
    bool wellformed = true;

    for (const auto &module : m_moduleList) {
        for (Function *func : *module) {
            if (!func->isLib()) {
                UserProc *proc = static_cast<UserProc *>(func);
                wellformed &= proc->getCFG()->isWellFormed();
            }
        }
    }

    return wellformed;
}


bool Prog::isWin32() const
{
    return m_binaryFile && m_binaryFile->getFormat() == LoadFmt::PE;
}


QString Prog::getRegNameByNum(RegNum regNum) const
{
    return m_fe->getDecoder()->getRegNameByNum(regNum);
}


int Prog::getRegSizeByNum(RegNum regNum) const
{
    return m_fe->getDecoder()->getRegSizeByNum(regNum);
}


Machine Prog::getMachine() const
{
    return m_binaryFile ? m_binaryFile->getMachine() : Machine::INVALID;
}


void Prog::readDefaultLibraryCatalogues()
{
    LOG_MSG("Reading library signatures...");

    const QDir dataDir = m_project->getSettings()->getDataDirectory();
    Plugin *plugin     = m_project->getPluginManager()->getPluginByName("C Symbol Provider plugin");
    if (!plugin) {
        LOG_ERROR("Symbol provider plugin not found!");
        return;
    }

    ISymbolProvider *prov = plugin->getIfc<ISymbolProvider>();
    prov->readLibraryCatalog(this, dataDir.absoluteFilePath("signatures/common.hs"));

    QString libCatalogName;
    switch (getMachine()) {
    case Machine::PENTIUM: libCatalogName = "signatures/pentium.hs"; break;
    case Machine::SPARC: libCatalogName = "signatures/sparc.hs"; break;
    case Machine::PPC: libCatalogName = "signatures/ppc.hs"; break;
    case Machine::ST20: libCatalogName = "signatures/st20.hs"; break;
    default: libCatalogName = ""; break;
    }

    if (!libCatalogName.isEmpty()) {
        prov->readLibraryCatalog(this, dataDir.absoluteFilePath(libCatalogName));
    }

    if (isWin32()) {
        prov->readLibraryCatalog(this, dataDir.absoluteFilePath("signatures/win32.hs"));
    }

    // TODO: change this to BinaryLayer query ("FILE_FORMAT","MACHO")
    if (m_binaryFile->getFormat() == LoadFmt::MACHO) {
        prov->readLibraryCatalog(this, dataDir.absoluteFilePath("signatures/objc.hs"));
    }
}


bool Prog::addSymbolsFromSymbolFile(const QString &fname)
{
    Plugin *plugin = m_project->getPluginManager()->getPluginByName("C Symbol Provider plugin");
    if (!plugin) {
        return false;
    }

    ISymbolProvider *prov = plugin->getIfc<ISymbolProvider>();
    return prov->addSymbolsFromSymbolFile(this, fname);
}


std::shared_ptr<Signature> Prog::getLibSignature(const QString &name)
{
    Plugin *plugin = m_project->getPluginManager()->getPluginByName("C Symbol Provider plugin");
    std::shared_ptr<Signature> signature = nullptr;

    if (plugin) {
        signature = plugin->getIfc<ISymbolProvider>()->getSignatureByName(name);
    }

    if (signature) {
        signature->setUnknown(false);
        return signature;
    }
    else {
        LOG_WARN("Unknown library function '%1', please update signatures!", name);
        return getDefaultSignature(name);
    }
}


std::shared_ptr<Signature> Prog::getDefaultSignature(const QString &name) const
{
    if (isWin32()) {
        return Signature::instantiate(getMachine(), CallConv::Pascal, name);
    }

    return Signature::instantiate(getMachine(), CallConv::C, name);
}


const char *Prog::getStringConstant(Address addr, bool knownString /* = false */) const
{
    if (!m_binaryFile || addr == Address::INVALID) {
        return nullptr;
    }

    const BinarySection *sect = m_binaryFile->getImage()->getSectionByAddr(addr);

    // Too many compilers put constants, including string constants,
    // into read/write sections, so we cannot check if the address is in a readonly section
    if (!sect || sect->isAddressBss(addr)) {
        return nullptr;
    }

    // At this stage, only support ascii, null terminated, non unicode strings.
    // At least 4 of the first 6 chars should be printable ascii
    const char *p = reinterpret_cast<const char *>(
        (sect->getHostAddr() - sect->getSourceAddr() + addr).value());

    if (knownString) {
        // No need to guess... this is hopefully a known string
        return p;
    }

    // this address is not known to be a string -> use heuristic
    int numPrintables = 0;
    int numControl    = 0; // Control characters like \n, \r, \t
    int numTotal      = 0;

    for (int i = 0; i < 6; i++, numTotal++) {
        if (p[i] == 0) {
            break;
        }
        else if (std::isprint(static_cast<Byte>(p[i]))) {
            numPrintables++;
        }
        else if (*p == '\n' || *p == '\t' || *p == '\r') {
            numControl++;
        }
    }

    if (numTotal == 0) {
        return "";
    }
    else if (numTotal - numPrintables - numControl < 2) {
        return p;
    }

    return nullptr;
}


bool Prog::getFloatConstant(Address addr, double &value, int bits) const
{
    const BinarySection *section = m_binaryFile->getImage()->getSectionByAddr(addr);
    if (!section || !section->isReadOnly()) {
        return false;
    }

    if (bits == 64) { // TODO: handle 80bit floats ?
        return m_binaryFile->getImage()->readNativeFloat8(addr, value);
    }
    else {
        assert(bits == 32);
        float val;
        if (m_binaryFile->getImage()->readNativeFloat4(addr, val)) {
            value = val;
            return true;
        }
        else {
            return false;
        }
    }
}


QString Prog::getSymbolNameByAddr(Address dest) const
{
    if (m_binaryFile == nullptr) {
        return "";
    }

    const BinarySymbol *sym = m_binaryFile->getSymbols()->findSymbolByAddress(dest);
    return sym ? sym->getName() : "";
}


const BinarySection *Prog::getSectionByAddr(Address a) const
{
    return m_binaryFile->getImage()->getSectionByAddr(a);
}


Address Prog::getLimitTextLow() const
{
    return m_binaryFile->getImage()->getLimitTextLow();
}


Address Prog::getLimitTextHigh() const
{
    return m_binaryFile->getImage()->getLimitTextHigh();
}


bool Prog::isReadOnly(Address a) const
{
    return m_binaryFile->getImage()->isReadOnly(a);
}


bool Prog::isInStringsSection(Address a) const
{
    const BinarySection *si = static_cast<const BinarySection *>(
        m_binaryFile->getImage()->getSectionByAddr(a));
    return si && si->isAttributeInRange("StringsSection", a, a + 1);
}


bool Prog::isDynamicallyLinkedProcPointer(Address dest) const
{
    const BinarySymbol *sym = m_binaryFile->getSymbols()->findSymbolByAddress(dest);
    return sym && sym->isImportedFunction();
}


Module *Prog::getOrInsertModuleForSymbol(const QString &symbolName)
{
    const BinarySymbol *sym = nullptr;
    if (m_binaryFile) {
        sym = m_binaryFile->getSymbols()->findSymbolByName(symbolName);
    }

    QString sourceFileName;
    if (sym) {
        sourceFileName = sym->belongsToSourceFile();
    }

    if (sourceFileName.isEmpty() || !sourceFileName.endsWith(".c")) {
        return m_rootModule;
    }

    LOG_VERBOSE("Got filename '%1' for symbol '%2'", sourceFileName, symbolName);
    QString moduleName = sourceFileName;
    moduleName.chop(2); // remove .c

    Module *module = findModule(moduleName);
    if (module) {
        return module;
    }

    module = getOrInsertModule(moduleName);
    m_rootModule->addChild(module);
    return module;
}


int Prog::readNative4(Address a) const
{
    return m_binaryFile->getImage()->readNative4(a);
}


void Prog::updateLibrarySignatures()
{
    for (const auto &m : m_moduleList) {
        m->updateLibrarySignatures();
    }
}


bool Prog::decodeEntryPoint(Address entryAddr)
{
    Function *func = getFunctionByAddr(entryAddr);

    if (!func || (!func->isLib() && !static_cast<UserProc *>(func)->isDecoded())) {
        if (!Util::inRange(entryAddr, m_binaryFile->getImage()->getLimitTextLow(),
                           m_binaryFile->getImage()->getLimitTextHigh())) {
            LOG_WARN("Attempt to decode entrypoint at address %1 outside text area", entryAddr);
            return false;
        }

        m_fe->decodeRecursive(entryAddr);
    }

    if (!func) {
        func = getFunctionByAddr(entryAddr);

        // Chek if there is a library thunk at entryAddr
        if (!func) {
            Address jumpTarget = m_binaryFile->getJumpTarget(entryAddr);

            if (jumpTarget != Address::INVALID) {
                func = getFunctionByAddr(jumpTarget);
            }
        }
    }

    if (!func) {
        return false;
    }

    if (!func->isLib()) { // -sf procs marked as __nodecode are treated as library procs (?)
        m_entryProcs.push_back(static_cast<UserProc *>(func));
    }

    return true;
}


bool Prog::decodeFragment(UserProc *proc, Address a)
{
    if ((a >= m_binaryFile->getImage()->getLimitTextLow()) &&
        (a < m_binaryFile->getImage()->getLimitTextHigh())) {
        return m_fe->decodeFragment(proc, a);
    }
    else {
        LOG_ERROR("Attempt to decode fragment at address %1 outside text area", a);
        return false;
    }
}


bool Prog::reDecode(UserProc *proc)
{
    if (!proc) {
        return false;
    }

    return m_fe->processProc(proc, proc->getEntryAddress());
}


Global *Prog::createGlobal(Address addr, SharedType ty, QString name)
{
    if (addr == Address::INVALID) {
        return nullptr;
    }

    if (name.isEmpty()) {
        name = newGlobalName(addr);
    }

    if (ty == nullptr) {
        ty = VoidType::get();
    }

    if (ty->isVoid()) {
        ty = guessGlobalType(name, addr);
    }

    auto pair = m_globals.insert(std::make_shared<Global>(ty, addr, name, this));
    return (pair.second) ? pair.first->get() : nullptr;
}


QString Prog::getGlobalNameByAddr(Address uaddr) const
{
    // FIXME: inefficient
    for (auto &glob : m_globals) {
        if (glob->containsAddress(uaddr)) {
            return glob->getName();
        }
    }

    return getSymbolNameByAddr(uaddr);
}


Address Prog::getGlobalAddrByName(const QString &name) const
{
    const Global *glob = getGlobalByName(name);
    if (glob) {
        return glob->getAddress();
    }

    auto symbol = m_binaryFile ? m_binaryFile->getSymbols()->findSymbolByName(name) : nullptr;
    return symbol ? symbol->getLocation() : Address::INVALID;
}


Global *Prog::getGlobalByName(const QString &name) const
{
    auto iter = std::find_if(
        m_globals.begin(), m_globals.end(),
        [&name](const std::shared_ptr<Global> &g) -> bool { return g->getName() == name; });

    return iter != m_globals.end() ? iter->get() : nullptr;
}


bool Prog::markGlobalUsed(Address uaddr, SharedType knownType)
{
    for (auto &glob : m_globals) {
        if (glob->containsAddress(uaddr)) {
            if (knownType) {
                glob->meetType(knownType);
            }

            return true;
        }
    }

    if (!m_binaryFile || m_binaryFile->getImage()->getSectionByAddr(uaddr) == nullptr) {
        LOG_VERBOSE("Refusing to create a global at address %1 "
                    "that is in no known section of the binary",
                    uaddr);
        return false;
    }

    QString name = newGlobalName(uaddr);
    SharedType ty;

    if (knownType) {
        ty = knownType;

        if (ty->resolvesToArray() && ty->as<ArrayType>()->isUnbounded()) {
            SharedType baseType = ty->as<ArrayType>()->getBaseType();
            int baseSize        = 0;

            if (baseType) {
                baseSize = baseType->getSizeInBytes();
            }

            auto symbol = m_binaryFile->getSymbols()->findSymbolByName(name);
            int sz      = symbol ? symbol->getSize() : 0;

            if (sz && baseSize) {
                // Note: since ty is a pointer and has not been cloned, this will also set the type
                // for knownType
                ty->as<ArrayType>()->setLength(sz / baseSize);
            }
        }
    }
    else {
        ty = guessGlobalType(name, uaddr);
    }

    m_globals.insert(std::make_shared<Global>(ty, uaddr, name, this));

    LOG_VERBOSE("globalUsed: name %1, address %2, %3 type %4", name, uaddr,
                knownType ? "known" : "guessed", ty->getCtype());

    return true;
}


std::shared_ptr<ArrayType> Prog::makeArrayType(Address startAddr, SharedType baseType)
{
    QString name = newGlobalName(startAddr);

    // TODO: fix the case of missing symbol table interface
    const BinarySymbol *symbol = m_binaryFile ? m_binaryFile->getSymbols()->findSymbolByName(name)
                                              : nullptr;

    if (!symbol || (symbol->getSize() == 0)) {
        return ArrayType::get(baseType); // An "unbounded" array
    }

    size_t size  = symbol->getSize();
    int elemSize = baseType->getSizeInBytes();
    if (elemSize < 1) {
        elemSize = 1;
    }

    return ArrayType::get(baseType, size / elemSize);
}


SharedType Prog::guessGlobalType(const QString &globalName, Address globAddr) const
{
    SharedType type = DebugInfo::typeFromDebugInfo(globalName, globAddr);
    if (type) {
        return type;
    }
    else if (!m_binaryFile) {
        return VoidType::get();
    }

    auto symbol = m_binaryFile->getSymbols()->findSymbolByName(globalName);
    int sz      = symbol ? symbol->getSize() : 0;

    if (sz == 0) {
        // Check if it might be a string
        const char *str = getStringConstant(globAddr);

        if (str) {
            // return char* and hope it is dealt with properly
            return PointerType::get(CharType::get());
        }
    }

    SharedType ty;

    switch (sz) {
    case 1:
    case 2:
    case 4:
    case 8: ty = IntegerType::get(sz * 8); break;

    default: ty = std::make_shared<ArrayType>(std::make_shared<CharType>(), sz);
    }

    return ty;
}


QString Prog::newGlobalName(Address uaddr)
{
    QString globalName = getGlobalNameByAddr(uaddr);

    if (!globalName.isEmpty()) {
        return globalName;
    }

    globalName = QString("global_%1").arg(uaddr.toString());
    LOG_VERBOSE("Naming new global '%1' at address %2", globalName, uaddr);
    return globalName;
}


SharedType Prog::getGlobalType(const QString &name) const
{
    for (auto &global : m_globals) {
        if (global->getName() == name) {
            return global->getType();
        }
    }

    return nullptr;
}


void Prog::setGlobalType(const QString &name, SharedType ty)
{
    // FIXME: inefficient
    for (auto &gl : m_globals) {
        if (gl->getName() == name) {
            gl->setType(ty);
            return;
        }
    }
}
