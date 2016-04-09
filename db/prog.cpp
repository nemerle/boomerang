/*
 * Copyright (C) 1998-2001, The University of Queensland
 * Copyright (C) 2001, Sun Microsystems, Inc
 * Copyright (C) 2002-2003, Trent Waddington
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 *
 */

/***************************************************************************/ /**
  * \file     prog.cpp
  * \brief    Implementation of the program class. Holds information of
  *                interest to the whole program.
  ******************************************************************************/

/***************************************************************************/ /**
  * Dependencies.
  ******************************************************************************/
#include "prog.h"

#include "type.h"
#include "module.h"
#include "types.h"
#include "statement.h"
#include "hllcode.h"
#include "exp.h"
#include "cfg.h"
#include "proc.h"
#include "util.h" // For lockFileWrite etc
#include "register.h"
#include "rtl.h"
#include "BinaryFile.h"
#include "frontend.h"
#include "signature.h"
#include "boomerang.h"
#include "ansi-c-parser.h"
#include "config.h"
#include "managed.h"
#include "log.h"
#include "BinaryImage.h"
#include "db/SymTab.h"

#include <QtCore/QFileInfo>
#include <QtCore/QDebug>
#include <QtCore/QXmlStreamWriter>
#include <QtCore/QDir>
#include <QtCore/QString>
#include <QtCore/QLockFile>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>
#ifdef _WIN32
#undef NO_ADDRESS
#include <windows.h>
#ifndef __MINGW32__
namespace dbghelp {
#include <dbghelp.h>
#ifdef ADDRESS
#undef ADDRESS
#endif
}
#endif
#undef NO_ADDRESS
#define NO_ADDRESS ADDRESS::g(~0u)
#endif

#include <sys/types.h>

Prog::Prog(const QString &name) : pLoaderPlugin(nullptr), DefaultFrontend(nullptr), m_name(name), m_iNumberedProc(1) {
    m_rootCluster = getOrInsertModule("prog");
    Image = Boomerang::get()->getImage();
    BinarySymbols = (SymTab *)Boomerang::get()->getSymbols();
    // Default constructor
}
/// Create or retrieve existing module
/// \param frontend for the module, if nullptr set it to program's default frontend.
/// \param fact abstract factory object that creates Module instance
/// \param name retrieve/create module with this name.
Module *Prog::getOrInsertModule(const QString &name,const ModuleFactory &fact,FrontEnd *frontend) {
    for(Module *m : ModuleList) {
        if(m->getName()==name)
            return m;
    }
    Module *m = fact.create(name,this,frontend ? frontend : DefaultFrontend);
    ModuleList.push_back(m);
    connect(this,SIGNAL(rereadLibSignatures()),m,SLOT(onLibrarySignaturesChanged()));
    return m;
}

void Prog::setFrontEnd(FrontEnd *_pFE) {
    pLoaderPlugin = _pFE->getBinaryFile();
    pLoaderIface = qobject_cast<LoaderInterface *>(pLoaderPlugin);
    DefaultFrontend = _pFE;
    for(Module *m : ModuleList)
        delete m;
    ModuleList.clear();
    m_rootCluster = nullptr;
    if (pLoaderIface && !pLoaderIface->getFilename().isEmpty()) {
        if(m_rootCluster)
            m_rootCluster->eraseFromParent();
        m_name = pLoaderIface->getFilename();
        m_rootCluster = this->getOrInsertModule(getNameNoPathNoExt());
    }
}

Prog::~Prog() {
    pLoaderPlugin->deleteLater();
    delete DefaultFrontend;
    for (Module *m : ModuleList) {
        delete m;
    }
}
//! Assign a name to this program
void Prog::setName(const char *name) {
    m_name = name;
    m_rootCluster->setName(name);
}

QString Prog::getName() { return m_name; }

//! Well form all the procedures/cfgs in this program
bool Prog::wellForm() {
    bool wellformed = true;
    for (Module *module : ModuleList) {
        for (Function *func : *module) {
            if (!func->isLib()) {
                UserProc *u = (UserProc *)func;
                wellformed &= u->getCFG()->wellFormCfg();
            }
        }
    }

    return wellformed;
}

// was in analysis.cpp
//! last fixes after decoding everything
void Prog::finishDecode() {
    for (Module *module : ModuleList) {
        for (Function *func : *module) {
            if (func->isLib())
                continue;
            UserProc *p = (UserProc *)func;
            if (!p->isDecoded())
                continue;
            p->assignProcsToCalls();
            p->finalSimplify();
        }
    }
}
//! Generate dotty file
void Prog::generateDotFile() {
    assert(!Boomerang::get()->dotFile.isEmpty());
    QFile tgt(Boomerang::get()->dotFile);
    if(!tgt.open(QFile::WriteOnly|QFile::Text))
        return;

    QTextStream of(&tgt);
    of << "digraph Cfg {\n";

    for (Module *module : ModuleList) {
        for (Function *func : *module) {
            if (func->isLib())
                continue;
            UserProc *p = (UserProc *)func;
            if (!p->isDecoded())
                continue;
            // Subgraph for the proc name
            of << "\nsubgraph cluster_" << p->getName() << " {\n"
               << "       color=gray;\n    label=" << p->getName() << ";\n";
            // Generate dotty CFG for this proc
            p->getCFG()->generateDotFile(of);
        }
    }
    of << "}";
}

void Prog::generateDataSectionCode(QString section_name, ADDRESS section_start, uint32_t size, HLLCode *code)
{
    code->AddGlobal("start_" + section_name, IntegerType::get(32, -1), new Const(section_start));
    code->AddGlobal(section_name + "_size", IntegerType::get(32, -1), new Const(size ? size : (unsigned int)-1));
    Exp *l = new Terminal(opNil);
    for (unsigned int i = 0; i < size; i++) {
        int n = Image->readNative1(section_start + size - 1 - i);
        if (n < 0)
            n = 256 + n;
        l = Binary::get(opList, new Const(n), l);
    }
    code->AddGlobal(section_name, ArrayType::get(IntegerType::get(8, -1), size), l);
}

void Prog::generateCode(Module *cluster, UserProc *proc, bool /*intermixRTL*/) {
    // QString basedir = m_rootCluster->makeDirs();
    QTextStream *os;
    if (cluster) {
        cluster->openStream("c");
        cluster->closeStreams();
    }
    bool generate_all = cluster == nullptr || cluster == m_rootCluster;
    bool all_procedures = proc==nullptr;
    if (generate_all) {
        m_rootCluster->openStream("c");
        os = &m_rootCluster->getStream();
        if (proc == nullptr) {
            HLLCode *code = Boomerang::get()->getHLLCode();
            bool global = false;
            if (Boomerang::get()->noDecompile) {
                const char *sections[] = {"rodata", "data", "data1", nullptr};
                for (int j = 0; sections[j]; j++) {
                    QString str = ".";
                    str += sections[j];
                    IBinarySection * info = Image->GetSectionInfoByName(str);
                    if(info)
                        generateDataSectionCode(sections[j], info->sourceAddr(), info->size(), code);
                    else
                        generateDataSectionCode(sections[j], NO_ADDRESS, 0, code);

                }
                code->AddGlobal("source_endianness", IntegerType::get(STD_SIZE),
                                new Const(getFrontEndId() != PLAT_PENTIUM));
                (*os) << "#include \"boomerang.h\"\n\n";
                global = true;
            }
            for (Global *elem : globals) {
                // Check for an initial value
                Exp *e = elem->getInitialValue(this);
                //                if (e) {
                code->AddGlobal(elem->getName(), elem->getType(), e);
                global = true;
            }
            if (global)
                code->print(*os); // Avoid blank line if no globals
        }
    }

    // First declare prototypes for all but the first proc
    bool first = true, proto = false;
    for ( Module *module : ModuleList) {
        for (Function *func : *module) {
            if (func->isLib())
                continue;
            if (first) {
                first = false;
                continue;
            }
            proto = true;
            UserProc *up = (UserProc *)func;
            HLLCode *code = Boomerang::get()->getHLLCode(up);
            code->AddPrototype(up); // May be the wrong signature if up has ellipsis
            if (generate_all)
                code->print(*os);
            delete code;
        }
    }
    if (proto && generate_all)
        *os << "\n"; // Separate prototype(s) from first proc

    for ( Module *module : ModuleList) {
        if(!generate_all && cluster!=module) {
            continue;
        }
        module->openStream("c");
        for (Function *func : *module) {
            if (func->isLib())
                continue;
            UserProc *up = (UserProc *)func;
            if (!up->isDecoded())
                continue;
            if (!all_procedures && up != proc)
                continue;
            up->getCFG()->compressCfg();
            up->getCFG()->removeOrphanBBs();

            HLLCode *code = Boomerang::get()->getHLLCode(up);
            up->generateCode(code);
            code->print(module->getStream());
            delete code;
        }
    }
    for ( Module *module : ModuleList)
        module->closeStreams();
}

void Prog::generateRTL(Module *cluster, UserProc *proc) {
    bool generate_all = cluster==nullptr;
    bool all_procedures = proc==nullptr;
    for(Module *module : ModuleList) {
        if(!generate_all && module!=cluster)
            continue;
        cluster->openStream("rtl");
        for (Function *func : *module) {
            if (!all_procedures && func != proc)
                continue;
            if (func->isLib())
                continue;
            UserProc *p = (UserProc *)func;
            if (!p->isDecoded())
                continue;
            p->print(module->getStream());
        }
    }
    for ( Module *module : ModuleList)
        module->closeStreams();
}

Instruction *Prog::getStmtAtLex(Module *cluster, unsigned int begin, unsigned int end) {
    bool search_all = cluster==nullptr;
    for(Module *m : ModuleList) {
        if(!search_all && m!=cluster)
            continue;
        for (Function *pProc : *m) {
            if (pProc->isLib())
                continue;
            UserProc *p = (UserProc *)pProc;
            if (!p->isDecoded())
                continue;
            Instruction *s = p->getStmtAtLex(begin, end);
            if (s)
                return s;
        }
    }
    return nullptr;
}

bool Prog::moduleUsed(Module *c) {
    return !c->empty(); // TODO: maybe module can have no procedures and still be used ?
}

Module *Prog::getDefaultModule(const QString &name) {
    QString cfname;
    const IBinarySymbol *bsym = BinarySymbols->find(name);
    if (bsym)
        cfname = bsym->belongsToSourceFile();

    if (cfname.isEmpty() || !cfname.endsWith(".c"))
        return m_rootCluster;
    LOG << "got filename " << cfname << " for " << name << "\n";
    cfname.chop(2); // remove .c
    Module *c = findModule(cfname);
    if (c == nullptr) {
        c = getOrInsertModule(cfname);
        m_rootCluster->addChild(c);
    }
    return c;
}

void Prog::generateCode(QTextStream &os) {
    HLLCode *code = Boomerang::get()->getHLLCode();
    for (Global *glob : globals) {
        // Check for an initial value
        Exp *e = nullptr;
        e = glob->getInitialValue(this);
        if (e)
            code->AddGlobal(glob->getName(), glob->getType(), e);
    }
    code->print(os);
    delete code;
    for (Module * module : ModuleList) {
        for (Function *pProc : *module) {
            if (pProc->isLib())
                continue;
            UserProc *p = (UserProc *)pProc;
            if (!p->isDecoded())
                continue;
            p->getCFG()->compressCfg();
            code = Boomerang::get()->getHLLCode(p);
            p->generateCode(code);
            code->print(os);
            delete code;
        }
    }
}

//! Print this program (primarily for debugging)
void Prog::print(QTextStream &out) {
    for (Module * module : ModuleList) {
        for (Function *pProc : *module) {
            if (pProc->isLib())
                continue;
            UserProc *p = (UserProc *)pProc;
            if (!p->isDecoded())
                continue;

            // decoded userproc.. print it
            p->print(out);
        }
    }
}

//! clear the prog object \note deletes everything!
void Prog::clear() {
    m_name = "";
    for (Module * module : ModuleList)
        delete module;
    ModuleList.clear();
    pLoaderPlugin->deleteLater();
    pLoaderPlugin = nullptr;
    delete DefaultFrontend;
    DefaultFrontend = nullptr;
}

/***************************************************************************/ /**
  *
  * \note     Formally Frontend::newProc
  * \brief    Call this function when a procedure is discovered (usually by
  *                  decoding a call instruction). That way, it is given a name
  *                  that can be displayed in the dot file, etc. If we assign it
  *                  a number now, then it will retain this number always
  * \param uAddr - Native address of the procedure entry point
  * \returns        Pointer to the Proc object, or 0 if this is a deleted (not to
  *                  be decoded) address
  ******************************************************************************/
Function *Prog::setNewProc(ADDRESS uAddr) {
    // this test fails when decoding sparc, why?  Please investigate - trent
    // Likely because it is in the Procedure Linkage Table (.plt), which for Sparc is in the data section
    // assert(uAddr >= limitTextLow && uAddr < limitTextHigh);
    // Check if we already have this proc
    Function *pProc = findProc(uAddr);
    if (pProc == (Function *)-1) // Already decoded and deleted?
        return nullptr;      // Yes, exit with 0
    if (pProc) // Exists already ?
        return pProc; // Yes, we are done
    ADDRESS other = pLoaderIface->IsJumpToAnotherAddr(uAddr);
    if (other != NO_ADDRESS)
        uAddr = other;
    pProc = findProc(uAddr);
    if (pProc) // Exists already ?
        return pProc; // Yes, we are done
    QString pName;
    const IBinarySymbol *sym = BinarySymbols->find(uAddr);
    bool bLib = false;
    if(sym) {
        bLib = sym->isImportedFunction() || sym->isStaticFunction();
        pName = sym->getName();
    }
    if (pName.isEmpty()) {
        // No name. Give it a numbered name
        pName = QString("proc%1").arg(m_iNumberedProc++);
        LOG_VERBOSE(1) << "assigning name " << pName << " to addr " << uAddr << "\n";
    }
    pProc = m_rootCluster->getOrInsertFunction(pName,uAddr, bLib);
    return pProc;
}

#if defined(_WIN32) && !defined(__MINGW32__)

SharedType typeFromDebugInfo(int index, DWORD64 ModBase);

SharedType makeUDT(int index, DWORD64 ModBase) {
    HANDLE hProcess = GetCurrentProcess();
    int got;
    WCHAR *name;
    got = dbghelp::SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_GET_SYMNAME, &name);
    assert(got);
    if (got) {
        char nameA[1024];
        WideCharToMultiByte(CP_ACP, 0, name, -1, nameA, sizeof(nameA), 0, nullptr);
        SharedType ty = Type::getNamedType(nameA);
        if (ty)
            return NamedType::get(nameA);
        std::shared_ptr<CompoundType> cty = CompoundType::get();
        DWORD count = 0;
        got = dbghelp::SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_GET_CHILDRENCOUNT, &count);
        int FindChildrenSize = sizeof(dbghelp::TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG);
        dbghelp::TI_FINDCHILDREN_PARAMS *pFC = (dbghelp::TI_FINDCHILDREN_PARAMS *)malloc(FindChildrenSize);
        memset(pFC, 0, FindChildrenSize);
        pFC->Count = count;
        got = SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_FINDCHILDREN, pFC);
        for (unsigned int i = 0; i < count; i++) {
            char fieldName[1024];
            got = dbghelp::SymGetTypeInfo(hProcess, ModBase, pFC->ChildId[i], dbghelp::TI_GET_SYMNAME, &name);
            WideCharToMultiByte(CP_ACP, 0, name, -1, fieldName, sizeof(fieldName), 0, nullptr);
            DWORD mytype;
            got = dbghelp::SymGetTypeInfo(hProcess, ModBase, pFC->ChildId[i], dbghelp::TI_GET_TYPE, &mytype);
            cty->addType(typeFromDebugInfo(mytype, ModBase), fieldName);
        }
        Type::addNamedType(nameA, cty);
        ty = Type::getNamedType(nameA);
        assert(ty);
        return NamedType::get(nameA);
    }
    return nullptr;
}

SharedType typeFromDebugInfo(int index, DWORD64 ModBase) {
    HANDLE hProcess = GetCurrentProcess();

    int got;
    DWORD d;
    ULONG64 lsz = 0;
    got = dbghelp::SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_GET_SYMTAG, &d);
    assert(got);
    got = dbghelp::SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_GET_LENGTH, &lsz);
    int sz = (int)lsz * 8; // bits

    switch (d) {
    case 11:
        return makeUDT(index, ModBase);
    case 13:
        // TODO: signature
        return FuncType::get();
    case 14:
        got = dbghelp::SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_GET_TYPE, &d);
        assert(got);
        return PointerType::get(typeFromDebugInfo(d, ModBase));
    case 15:
        got = dbghelp::SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_GET_TYPE, &d);
        assert(got);
        got = dbghelp::SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_GET_LENGTH, &lsz);
        assert(got);
        return ArrayType::get(typeFromDebugInfo(d, ModBase), (unsigned)lsz);
        break;
    case 16:
        got = dbghelp::SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_GET_BASETYPE, &d);
        assert(got);
        switch (d) {
        case 1:
            return VoidType::get();
        case 2:
            return CharType::get();
        case 3:
            return CharType::get();
        case 6:  // int
        case 13: // long
            return IntegerType::get(sz, 1);
        case 7:  // unsigned int
        case 14: // ulong
            return IntegerType::get(sz, -1);
        case 8:
            return SharedType(new FloatType(sz));
        case 10:
            return SharedType(new BooleanType());
        default:
            LOG_STREAM() << "unhandled base type " << d << "\n";
            assert(false);
        }
        break;
    default:
        LOG_STREAM() << "unhandled symtag " << d << "\n";
        assert(false);
    }
    return nullptr;
}

int debugRegister(int r) {
    switch (r) {
    case 2:
        return 26; // edx
    case 4:
        return 25; // ecx
    case 8:
        return 29; // ebp
    }
    assert(false);
    return -1;
}

BOOL CALLBACK addSymbol(dbghelp::PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext) {
    Function *proc = (Function *)UserContext;
    if (pSymInfo->Flags & SYMFLAG_PARAMETER) {
        SharedType ty = typeFromDebugInfo(pSymInfo->TypeIndex, pSymInfo->ModBase);
        if (pSymInfo->Flags & SYMFLAG_REGREL) {
            assert(pSymInfo->Register == 8); // ebp
            proc->getSignature()->addParameter(
                        ty, pSymInfo->Name,
                        Location::memOf(Binary::get(opPlus, Location::regOf(28), new Const((int)pSymInfo->Address - 4))));
        } else if (pSymInfo->Flags & SYMFLAG_REGISTER) {
            proc->getSignature()->addParameter(ty, pSymInfo->Name, Location::regOf(debugRegister(pSymInfo->Register)));
        }
    } else if ((pSymInfo->Flags & SYMFLAG_LOCAL) && !proc->isLib()) {
        UserProc *u = (UserProc *)proc;
        assert(pSymInfo->Flags & SYMFLAG_REGREL);
        assert(pSymInfo->Register == 8);
        Exp *memref =
                Location::memOf(Binary::get(opMinus, Location::regOf(28), new Const(-((int)pSymInfo->Address - 4))));
        SharedType ty = typeFromDebugInfo(pSymInfo->TypeIndex, pSymInfo->ModBase);
        u->addLocal(ty, pSymInfo->Name, memref);
    }
    return TRUE;
}

#endif

/***************************************************************************/ /**
  *
  * \brief Removes the named Function
  * \param name - is the function's name
  * \note this does not destroy the removed function.
  ******************************************************************************/
void Prog::removeProc(const QString &name) {
    Function *f = findProc(name);
    if(f && f!=(Function *)-1) {
        f->removeFromParent();
        Boomerang::get()->alertRemove(f);
        //FIXME: this function removes the function from module, but it leaks it
    }
}

/***************************************************************************/ /**
  *
  * \brief    Return the number of user (non deleted, non library) procedures
  * \returns  The number of procedures
  ******************************************************************************/
int Prog::getNumProcs(bool user_only) {
    int n = 0;
    if(user_only) {
        for(Module *m : ModuleList)
            for (Function *pProc : *m)
                if (!pProc->isLib())
                    n++;
    }
    else {
        for(Module *m : ModuleList)
            n += m->size();

    }
    return n;
}

/***************************************************************************/ /**
  *
  * \brief    Return a pointer to the associated Proc object, or nullptr if none
  * \note        Could return -1 for a deleted Proc
  * \param uAddr - Native address of the procedure entry point
  * \returns Pointer to the Proc object, or 0 if none, or -1 if deleted
  ******************************************************************************/
Function *Prog::findProc(ADDRESS uAddr) const {
    for(Module *m : ModuleList)
    {
        Function *r = m->getFunction(uAddr);
        if(r!=nullptr)
            return r;
    }
    return nullptr;
}
/***************************************************************************/ /**
  * \brief    Return a pointer to the associated Proc object, or nullptr if none
  * \note        Could return -1 for a deleted Proc
  * \param name - name of the searched-for procedure
  * \returns Pointer to the Proc object, or 0 if none, or -1 if deleted
  ******************************************************************************/
Function *Prog::findProc(const QString &name) const {
    for(Module *m : ModuleList) {
        Function *f = m->getFunction(name);
        if(f)
            return f;
    }
    return nullptr;
}

//! lookup a library procedure by name; create if does not exist
LibProc *Prog::getLibraryProc(const QString &nam) {
    Function *p = findProc(nam);
    if (p && p->isLib())
        return (LibProc *)p;
    return (LibProc *)m_rootCluster->getOrInsertFunction(nam,NO_ADDRESS,true);
}

//! Get the front end id used to make this prog
platform Prog::getFrontEndId() { return DefaultFrontend->getFrontEndId(); }

Signature *Prog::getDefaultSignature(const char *name) { return DefaultFrontend->getDefaultSignature(name); }

std::vector<Exp *> &Prog::getDefaultParams() { return DefaultFrontend->getDefaultParams(); }

std::vector<Exp *> &Prog::getDefaultReturns() { return DefaultFrontend->getDefaultReturns(); }
//! Returns true if this is a win32 program
bool Prog::isWin32() {
    if (!DefaultFrontend)
        return false;
    return DefaultFrontend->isWin32();
}
//! Get a global variable if possible, looking up the loader's symbol table if necessary
QString Prog::getGlobalName(ADDRESS uaddr) {
    // FIXME: inefficient
    for (Global *glob : globals) {
        if (glob->addressWithinGlobal(uaddr))
            return glob->getName();
    }
    return symbolByAddress(uaddr);
}
//! Dump the globals to stderr for debugging
void Prog::dumpGlobals() {
    for (Global *glob : globals) {
        LOG_STREAM() << glob->toString() << "\n";
    }
}
//! Get a named global variable if possible, looking up the loader's symbol table if necessary
ADDRESS Prog::getGlobalAddr(const QString &nam) {
    Global *glob = getGlobal(nam);
    if (glob)
        return glob->getAddress();
    auto symbol = BinarySymbols->find(nam);
    return symbol ? symbol->getLocation() : NO_ADDRESS;
}

Global *Prog::getGlobal(const QString &nam) {
    auto iter =
            std::find_if(globals.begin(), globals.end(), [nam](Global *g) -> bool { return g->getName()==nam; });
    if (iter == globals.end())
        return nullptr;
    return *iter;
}
//! Indicate that a given global has been seen used in the program.
bool Prog::globalUsed(ADDRESS uaddr, SharedType knownType) {
    for (Global *glob : globals) {
        if (glob->addressWithinGlobal(uaddr)) {
            if (knownType)
                glob->meetType(knownType);
            return true;
        }
    }

    if (Image->getSectionInfoByAddr(uaddr) == nullptr) {
        LOG_VERBOSE(1) << "refusing to create a global at address that is in no known section of the binary: " << uaddr
                       << "\n";
        return false;
    }

    QString nam = newGlobalName(uaddr);
    SharedType ty;
    if (knownType) {
        ty = knownType;
        if (ty->resolvesToArray() && ty->asArray()->isUnbounded()) {
            SharedType baseType = ty->asArray()->getBaseType();
            int baseSize = 0;
            if (baseType)
                baseSize = baseType->getBytes();
            auto symbol = BinarySymbols->find(nam);
            int sz = symbol ? symbol->getSize() : 0;
            if (sz && baseSize)
                // Note: since ty is a pointer and has not been cloned, this will also set the type for knownType
                ty->asArray()->setLength(sz / baseSize);
        }
    } else
        ty = guessGlobalType(nam, uaddr);

    Global *global = new Global(ty, uaddr, nam,this);
    globals.insert(global);

    if (VERBOSE) {
        LOG << "globalUsed: name " << nam << ", address " << uaddr;
        if (knownType)
            LOG << ", known type " << ty->getCtype() << "\n";
        else
            LOG << ", guessed type " << ty->getCtype() << "\n";
    }
    return true;
}

//! Make an array type for the global array at u. Mainly, set the length sensibly
std::shared_ptr<ArrayType> Prog::makeArrayType(ADDRESS u, SharedType t) {
    QString nam = newGlobalName(u);
    assert(pLoaderIface);
    // TODO: fix the case of missing symbol table interface
    auto symbol = BinarySymbols->find(nam);
    if (!symbol || symbol->getSize()==0)
        return ArrayType::get(t); // An "unbounded" array
    unsigned int sz = symbol->getSize();
    int n = t->getBytes();    // TODO: use baseType->getBytes()
    if (n == 0)
        n = 1;
    return ArrayType::get(t, sz / n);
}
//! Guess a global's type based on its name and address
SharedType Prog::guessGlobalType(const QString &nam, ADDRESS u) {
#if defined(_WIN32) && !defined(__MINGW32__)
    HANDLE hProcess = GetCurrentProcess();
    dbghelp::SYMBOL_INFO *sym = (dbghelp::SYMBOL_INFO *)malloc(sizeof(dbghelp::SYMBOL_INFO) + 1000);
    sym->SizeOfStruct = sizeof(*sym);
    sym->MaxNameLen = 1000;
    sym->Name[0] = 0;
    BOOL got = dbghelp::SymFromAddr(hProcess, u.m_value, 0, sym);
    if (got && *sym->Name && sym->TypeIndex) {
        assert(nam == sym->Name);
        return typeFromDebugInfo(sym->TypeIndex, sym->ModBase);
    }
#endif
    auto symbol = BinarySymbols->find(nam);
    int sz = symbol ? symbol->getSize() : 0;
    if (sz == 0) {
        // Check if it might be a string
        const char *str = getStringConstant(u);
        if (str)
            // return char* and hope it is dealt with properly
            return PointerType::get(CharType::get());
    }
    SharedType ty;
    switch (sz) {
    case 1:
    case 2:
    case 4:
    case 8:
        ty = IntegerType::get(sz * 8);
        break;
    default:
        ty = std::make_shared<ArrayType>(std::make_shared<CharType>(), sz);
    }
    return ty;
}
//! Make up a name for a new global at address \a uaddr (or return an existing name if address already used)
QString Prog::newGlobalName(ADDRESS uaddr) {
    QString nam = getGlobalName(uaddr);
    if (!nam.isEmpty())
        return nam;
    nam = QString("global%1_%2").arg(globals.size()).arg(uaddr.m_value,0,16);
    LOG_VERBOSE(1) << "naming new global: " << nam << " at address " << uaddr << "\n";
    return nam;
}
//! Get the type of a global variable
SharedType Prog::getGlobalType(const QString &nam) {
    for (Global *gl : globals)
        if (gl->getName()==nam)
            return gl->getType();
    return nullptr;
}
//! Set the type of a global variable
void Prog::setGlobalType(const QString &nam, SharedType ty) {
    // FIXME: inefficient
    for (Global *gl : globals) {
        if (gl->getName()!=nam)
            continue;
        gl->setType(ty);
        return;
    }
}

// get a string constant at a given address if appropriate
// if knownString, it is already known to be a char*
//! get a string constant at a give address if appropriate
const char *Prog::getStringConstant(ADDRESS uaddr, bool knownString /* = false */) {
    const IBinarySection *si = Image->getSectionInfoByAddr(uaddr);
    // Too many compilers put constants, including string constants, into read/write sections
    // if (si && si->bReadOnly)
    if (si && !si->isAddressBss(uaddr)) {
        // At this stage, only support ascii, null terminated, non unicode strings.
        // At least 4 of the first 6 chars should be printable ascii
        char *p = (char *)(uaddr + si->hostAddr() - si->sourceAddr()).m_value;
        if (knownString)
            // No need to guess... this is hopefully a known string
            return p;
        int printable = 0;
        char last = 0;
        for (int i = 0; i < 6; i++) {
            char c = p[i];
            if (c == 0)
                break;
            if (c >= ' ' && c < '\x7F')
                printable++;
            last = c;
        }
        if (printable >= 4)
            return p;
        // Just a hack while type propagations are not yet ready
        if (last == '\n' && printable >= 2)
            return p;
    }
    return nullptr;
}

double Prog::getFloatConstant(ADDRESS uaddr, bool &ok, int bits) {
    ok = true;
    const IBinarySection *si = Image->getSectionInfoByAddr(uaddr);
    if (si && si->isReadOnly()) {
        if (bits == 64) { // TODO: handle 80bit floats ?
            return Image->readNativeFloat8(uaddr);
        } else {
            assert(bits == 32);
            return Image->readNativeFloat4(uaddr);
        }
    }
    ok = false;
    return 0.0;
}

QString Prog::symbolByAddress(ADDRESS dest) {
    auto sym = BinarySymbols->find(dest);
    return sym ? sym->getName() : "";
}
const IBinarySection *Prog::getSectionInfoByAddr(ADDRESS a) {
    return Image->getSectionInfoByAddr(a);
}

ADDRESS Prog::getLimitTextLow() {
    return Boomerang::get()->getImage()->getLimitTextLow();
}

ADDRESS Prog::getLimitTextHigh() {
    return Boomerang::get()->getImage()->getLimitTextHigh();
}

bool Prog::isReadOnly(ADDRESS a) {
    return Image->isReadOnly(a);
}

int Prog::readNative1(ADDRESS a) {
    return Image->readNative1(a);
}

int Prog::readNative2(ADDRESS a) {
    return Image->readNative2(a);
}

int Prog::readNative4(ADDRESS a) {
    return Image->readNative4(a);
}

/***************************************************************************/ /**
  *
  * \brief    Return a pointer to the Proc object containing uAddr, or 0 if none
  * \note     Could return nullptr for a deleted Proc
  * \param uAddr - Native address to search for
  * \returns        Pointer to the Proc object, or 0 if none, or -1 if deleted
  ******************************************************************************/
Function *Prog::findContainingProc(ADDRESS uAddr) const {
    for(Module *module : ModuleList) {
        for (Function *p : *module) {
            if (p->getNativeAddress() == uAddr)
                return p;
            if (p->isLib())
                continue;
            UserProc *u = (UserProc *)p;
            if (u->containsAddr(uAddr))
                return p;
        }
    }
    return nullptr;
}

/***************************************************************************/ /**
  *
  * \brief    Return true if this is a real procedure
  * \param addr   Native address of the procedure entry point
  * \returns        True if a real (non deleted) proc
  ******************************************************************************/
bool Prog::isProcLabel(ADDRESS addr) {

    for(Module *m : ModuleList) {
        if(m->getFunction(addr))
            return true;
    }
    return false;
}

/***************************************************************************/ /**
  *
  * \brief Get the name for the progam, without any path at the front
  * \returns A string with the name
  ******************************************************************************/
QString Prog::getNameNoPath() const { return QFileInfo(m_name).fileName(); }

/***************************************************************************/ /**
  *
  * \brief Get the name for the progam, without any path at the front, and no extension
  * \sa Prog::getNameNoPath
  * \returns A string with the name
  ******************************************************************************/
QString Prog::getNameNoPathNoExt() const { return QFileInfo(m_name).baseName(); }

/***************************************************************************/ /**
  *
  * \brief    Lookup the given native address in the code section, returning a host pointer corresponding to the same
  *                 address
  * \param uAddr Native address of the candidate string or constant
  * \param last will be set to one past end of the code section (host)
  * \param delta will be set to the difference between the host and native addresses
  * \returns        Host pointer if in range; nullptr if not
  *                Also sets 2 reference parameters (see above)
  ******************************************************************************/
const void *Prog::getCodeInfo(ADDRESS uAddr, const char *&last, int &delta) {
    delta = 0;
    last = nullptr;
    const IBinarySection *pSect = Image->getSectionInfoByAddr(uAddr);
    if(!pSect)
        return nullptr;
    if ((!pSect->isCode()) && (!pSect->isReadOnly()))
        return nullptr;
    // Search all code and read-only sections
    delta = (pSect->hostAddr() - pSect->sourceAddr()).m_value;
    last = (const char *)(pSect->hostAddr() + pSect->size()).m_value;
    const char *p = (const char *)(uAddr + delta).m_value;
    return p;
}

/***************************************************************************/ /**
  *
  * \brief    Decode from entry point given as an agrument
  * \param a -  Native address of the entry point
  *
  ******************************************************************************/
void Prog::decodeEntryPoint(ADDRESS a) {
    Function *p = (UserProc *)findProc(a);
    if (p == nullptr || (!p->isLib() && !((UserProc *)p)->isDecoded())) {
        if (a < Image->getLimitTextLow() || a >= Image->getLimitTextHigh()) {
            LOG_STREAM(LL_Warn) << "attempt to decode entrypoint at address outside text area, addr=" << a << "\n";
            return;
        }
        DefaultFrontend->decode(this, a);
        finishDecode();
    }
    if (p == nullptr)
        p = findProc(a);
    assert(p);
    if (!p->isLib()) // -sf procs marked as __nodecode are treated as library procs (?)
        entryProcs.push_back((UserProc *)p);
}
/***************************************************************************/ /**
  *
  * \brief    Add entry point given as an agrument to the list of entryProcs
  * \param a -  Native address of the entry point
  *
  ******************************************************************************/
void Prog::setEntryPoint(ADDRESS a) {
    Function *p = (UserProc *)findProc(a);
    if (p != nullptr && !p->isLib())
        entryProcs.push_back((UserProc *)p);
}
bool Prog::isDynamicLinkedProcPointer(ADDRESS dest) {
    auto sym = BinarySymbols->find(dest);
    return sym && sym->isImportedFunction();
}

const QString &Prog::GetDynamicProcName(ADDRESS uNative) {
    static QString dyn("dynamic");
    auto sym = BinarySymbols->find(uNative);
    return sym ? sym->getName() : dyn;
}

void Prog::decodeEverythingUndecoded() {
    for(Module *module : ModuleList) {
        for (Function *pp : *module) {
            UserProc *up = (UserProc *)pp;
            if (!pp || pp->isLib())
                continue;
            if (up->isDecoded())
                continue;
            DefaultFrontend->decode(this, pp->getNativeAddress());
        }
    }
    finishDecode();
}
//! Do the main non-global decompilation steps
void Prog::decompile() {
    Boomerang * boom=Boomerang::get();
    assert(!ModuleList.empty());
    getNumProcs();
    LOG_VERBOSE(1) << getNumProcs(false) << " procedures\n";

    // Start decompiling each entry point
    for (UserProc *up : entryProcs) {
        ProcList call_path;
        LOG_VERBOSE(1) << "decompiling entry point " << up->getName() << "\n";
        int indent = 0;
        up->decompile(&call_path, indent);
    }

    // Just in case there are any Procs not in the call graph.

    if (boom->decodeMain && !boom->noDecodeChildren) {
        bool foundone = true;
        while (foundone) {
            foundone = false;
            for(Module *module : ModuleList) {
                for (Function *pp : *module) {
                    if (pp->isLib())
                        continue;
                    UserProc *proc = (UserProc *)pp;
                    if (proc->isDecompiled())
                        continue;
                    int indent = 0;
                    proc->decompile(new ProcList, indent);
                    foundone = true;
                }
            }
        }
    }

    // Type analysis, if requested
    if (Boomerang::get()->conTypeAnalysis && Boomerang::get()->dfaTypeAnalysis) {
        LOG_STREAM() << "can't use two types of type analysis at once!\n";
        Boomerang::get()->conTypeAnalysis = false;
    }
    globalTypeAnalysis();

    if (!Boomerang::get()->noDecompile) {
        if (!Boomerang::get()->noRemoveReturns) {
            // A final pass to remove returns not used by any caller
            LOG_VERBOSE(1) << "prog: global removing unused returns\n";
            // Repeat until no change. Note 100% sure if needed.
            while (removeUnusedReturns())
                ;
        }

        // print XML after removing returns
        for(Module *m :ModuleList) {
            for (Function *pp : *m) {
                if (pp->isLib())
                    continue;
                UserProc *proc = (UserProc *)pp;
                proc->printXML();
            }
        }
    }
    LOG_VERBOSE(1) << "transforming from SSA\n";

    // Now it is OK to transform out of SSA form
    fromSSAform();

    // removeUnusedLocals(); Note: is now in UserProc::generateCode()
    removeUnusedGlobals();
}
//! As the name suggests, removes globals unused in the decompiled code.
void Prog::removeUnusedGlobals() {

    LOG_VERBOSE(1) << "removing unused globals\n";

    // seach for used globals
    std::list<Exp *> usedGlobals;
    for(Module *module : ModuleList) {
        for (Function *pp : *module) {
            if (pp->isLib())
                continue;
            UserProc *u = (UserProc *)pp;
            Location search(opGlobal, Terminal::get(opWild), u);
            // Search each statement in u, excepting implicit assignments (their uses don't count, since they don't really
            // exist in the program representation)
            StatementList stmts;
            StatementList::iterator ss;
            u->getStatements(stmts);
            for (Instruction *s : stmts) {
                if (s->isImplicit())
                    continue; // Ignore the uses in ImplicitAssigns
                bool found = s->searchAll(search, usedGlobals);
                if (found && DEBUG_UNUSED)
                    LOG << " a global is used by stmt " << s->getNumber() << "\n";
            }
        }
    }
    // make a map to find a global by its name (could be a global var too)
    QMap<QString, Global *> namedGlobals;
    for (Global *g : globals)
        namedGlobals[g->getName()] = g;

    // rebuild the globals vector
    Global *usedGlobal;

    globals.clear();
    for (Exp *e : usedGlobals) {
        if (DEBUG_UNUSED)
            LOG << " " << e << " is used\n";
        QString name(((Const *)e->getSubExp1())->getStr());
        usedGlobal = namedGlobals[name];
        if (usedGlobal) {
            globals.insert(usedGlobal);
        } else {
            LOG << "warning: an expression refers to a nonexistent global\n";
        }
    }
}

/***************************************************************************/ /**
  *
  * \brief    Remove unused return locations
  *
  * This is the global removing of unused and redundant returns. The initial idea
  * is simple enough: remove some returns according to the formula:
  * returns(p) = modifieds(p) isect union(live at c) for all c calling p.
  * However, removing returns reduces the uses, leading to three effects:
  * 1) The statement that defines the return, if only used by that return, becomes unused
  * 2) if the return is implicitly defined, then the parameters may be reduced, which affects all callers
  * 3) if the return is defined at a call, the location may no longer be live at the call. If not, you need to check
  *    the child, and do the union again (hence needing a list of callers) to find out if this change also affects that
  *    child.
  * \returns true if any change
  *
  ******************************************************************************/
bool Prog::removeUnusedReturns() {
    // For each UserProc. Each proc may process many others, so this may duplicate some work. Really need a worklist of
    // procedures not yet processed.
    // Define a workset for the procedures who have to have their returns checked
    // This will be all user procs, except those undecoded (-sf says just trust the given signature)
    std::set<UserProc *> removeRetSet;
    bool change = false;
    for(Module *module : ModuleList) {
        for (Function *pp : *module) {
            UserProc *proc = dynamic_cast<UserProc *>(pp);
            if (nullptr==proc || !proc->isDecoded())
                continue; // e.g. use -sf file to just prototype the proc
            removeRetSet.insert(proc);
        }
    }
    // The workset is processed in arbitrary order. May be able to do better, but note that sometimes changes propagate
    // down the call tree (no caller uses potential returns for child), and sometimes up the call tree (removal of
    // returns and/or dead code removes parameters, which affects all callers).
    while (!removeRetSet.empty()) {
        auto it = removeRetSet.begin(); // Pick the first element of the set
        change |= (*it)->removeRedundantReturns(removeRetSet);
        // Note: removing the currently processed item here should prevent unnecessary reprocessing of self recursive
        // procedures
        removeRetSet.erase(it); // Remove the current element (may no longer be the first)
    }
    return change;
}

// Have to transform out of SSA form after the above final pass
//! Convert from SSA form
void Prog::fromSSAform() {
    for(Module *module : ModuleList) {
        for (Function *pp : *module) {
            if (pp->isLib())
                continue;
            UserProc *proc = (UserProc *)pp;
            if (VERBOSE) {
                LOG << "===== before transformation from SSA form for " << proc->getName() << " =====\n" << *proc
                    << "===== end before transformation from SSA for " << proc->getName() << " =====\n\n";
                if (!Boomerang::get()->dotFile.isEmpty())
                    proc->printDFG();
            }
            proc->fromSSAform();
            LOG_VERBOSE(1) << "===== after transformation from SSA form for " << proc->getName() << " =====\n" << *proc
                           << "===== end after transformation from SSA for " << proc->getName() << " =====\n\n";
        }
    }
}
//! Constraint based type analysis
void Prog::conTypeAnalysis() {
    if (VERBOSE || DEBUG_TA)
        LOG << "=== start constraint-based type analysis ===\n";
    // FIXME: This needs to be done bottom of the call-tree first, with repeat until no change for cycles
    // in the call graph
    for(Module *module : ModuleList) {
        for (Function *pp : *module) {
            UserProc *proc = (UserProc *)pp;
            if (proc->isLib() || !proc->isDecoded())
                continue;
            proc->conTypeAnalysis();
        }
    }
    if (VERBOSE || DEBUG_TA)
        LOG << "=== end type analysis ===\n";
}

void Prog::globalTypeAnalysis() {
    if (VERBOSE || DEBUG_TA)
        LOG << "### start global data-flow-based type analysis ###\n";
    for(Module *module : ModuleList) {
        for (Function *pp : *module) {
            UserProc *proc = dynamic_cast<UserProc *>(pp);
            if ( nullptr==proc || !proc->isDecoded())
                continue;
            // FIXME: this just does local TA again. Need to meet types for all parameter/arguments, and return/results!
            // This will require a repeat until no change loop
            LOG_STREAM() << "global type analysis for " << proc->getName() << "\n";
            proc->typeAnalysis();
        }
    }
    if (VERBOSE || DEBUG_TA)
        LOG << "### end type analysis ###\n";
}
#include "passes/RangeAnalysis.h"
void Prog::rangeAnalysis() {
    for(Module *module : ModuleList) {
        RangeAnalysis ra;
        for (Function *pp : *module) {
            UserProc *proc = (UserProc *)pp;
            if (proc->isLib() || !proc->isDecoded())
                continue;
            ra.runOnFunction(*proc);
        }
    }
}

void Prog::printCallGraph() {
    QString fname1 = Boomerang::get()->getOutputPath() + "callgraph.out";
    QString fname2 = Boomerang::get()->getOutputPath() + "callgraph.dot";
    QLockFile lockFile1(fname1);
    QLockFile lockFile2(fname2);
    lockFile1.lock();
    lockFile2.lock();
    QFile file1(fname1);
    QFile file2(fname2);
    if( !(file1.open(QFile::WriteOnly) && file2.open(QFile::WriteOnly)) ) {
        LOG_STREAM() << "Cannot open output files for callgraph output";
        return;
    }
    QTextStream f1(&file1);
    QTextStream f2(&file2);
    std::set<Function *> seen;
    std::map<Function *, int> spaces;
    std::map<Function *, Function *> parent;
    std::list<Function *> procList;
    f2 << "digraph callgraph {\n";
    std::copy(entryProcs.begin(), entryProcs.end(), std::back_inserter(procList));

    spaces[procList.front()] = 0;
    while (procList.size()) {
        Function *p = procList.front();
        procList.erase(procList.begin());
        if (ADDRESS::host_ptr(p) == NO_ADDRESS)
            continue;
        if (seen.find(p) != seen.end())
            continue;
        seen.insert(p);
        int n = spaces[p];
        for (int i = 0; i < n; i++)
            f1 << "     ";
        f1 << p->getName() << " @ " << p->getNativeAddress();
        if (parent.find(p) != parent.end())
            f1 << " [parent=" << parent[p]->getName() << "]";
        f1 << '\n';
        if (!p->isLib()) {
            n++;
            UserProc *u = (UserProc *)p;
            std::list<Function *> &calleeList = u->getCallees();
            for (auto it1 = calleeList.rbegin(); it1 != calleeList.rend(); it1++) {
                procList.push_front(*it1);
                spaces[*it1] = n;
                parent[*it1] = p;
                f2 << p->getName() << " -> " << (*it1)->getName() << ";\n";
            }
        }
    }
    f2 << "}\n";
    f1.flush();
    f2.flush();
}

void printProcsRecursive(Function *proc, int indent, QTextStream &f, std::set<Function *> &seen) {
    bool fisttime = false;
    if (seen.find(proc) == seen.end()) {
        seen.insert(proc);
        fisttime = true;
    }
    for (int i = 0; i < indent; i++)
        f << "     ";

    if (!proc->isLib() && fisttime) { // seen lib proc
        f << proc->getNativeAddress();
        f << " __nodecode __incomplete void " << proc->getName() << "();\n";

        UserProc *u = (UserProc *)proc;
        for (Function *callee : u->getCallees()) {
            printProcsRecursive(callee, indent + 1, f, seen);
        }
        for (int i = 0; i < indent; i++)
            f << "     ";
        f << "// End of " << proc->getName() << "\n";
    } else {
        f << "// " << proc->getName() << "();\n";
    }
}

void Prog::printSymbolsToFile() {
    LOG_STREAM() << "entering Prog::printSymbolsToFile\n";
    QString fname = Boomerang::get()->getOutputPath() + "symbols.h";
    QLockFile lockFile(fname);
    lockFile.lock();
    QFile tgt(fname);
    if(!tgt.open(QFile::WriteOnly)) {
        LOG_STREAM() << " Cannot open " << fname << " for writing\n";
        return;
    }
    QTextStream f(&tgt);

    /* Print procs */
    f << "/* Functions: */\n";
    std::set<Function *> seen;
    for (UserProc *up : entryProcs)
        printProcsRecursive(up, 0, f, seen);

    f << "/* Leftovers: */\n";
    for(Module *m : ModuleList) {
        for (Function *pp : *m) {
            if (!pp->isLib() && seen.find(pp) == seen.end()) {
                printProcsRecursive(pp, 0, f, seen);
            }
        }
    }
    f.flush();
    LOG_STREAM() << "leaving Prog::printSymbolsToFile\n";
}

void Prog::printCallGraphXML() {
    if (!Boomerang::get()->dumpXML)
        return;

    for(Module *m : ModuleList) {
        for (Function *it : *m)
            it->clearVisited();
    }
    QString fname = Boomerang::get()->getOutputPath() + "callgraph.xml";
    QLockFile lockFile(fname);
    QFile CallGraphFile(fname);
    QTextStream f(&CallGraphFile);
    f << "<prog name=\"" << getName() << "\">\n";
    f << "     <callgraph>\n";

    for (UserProc *up : entryProcs)
        up->printCallGraphXML(f, 2);
    for(Module *m : ModuleList) {
        for (Function *pp : *m) {
            if (!pp->isVisited() && !pp->isLib()) {
                pp->printCallGraphXML(f, 2);
            }
        }
    }
    f << "     </callgraph>\n";
    f << "</prog>\n";
    f.flush();
}

Module *Prog::findModule(const QString &name) {
    for(Module *m : ModuleList)
        if(m->getName()==name)
            return m;
    return nullptr;
}

void Prog::readSymbolFile(const QString &fname) {
    std::ifstream ifs;

    ifs.open(fname.toStdString());

    if (!ifs.good()) {
        LOG << "can't open `" << fname << "'\n";
        exit(1);
    }

    AnsiCParser *par = new AnsiCParser(ifs, false);
    platform plat = getFrontEndId();
    callconv cc = CONV_C;
    if (isWin32())
        cc = CONV_PASCAL;
    par->yyparse(plat, cc);
    Module *tgt_mod = getRootCluster();

    for (Symbol *sym : par->symbols) {
        if (sym->sig) {
            QString name = sym->sig->getName();
            tgt_mod = getDefaultModule(name);
            auto bin_sym = BinarySymbols->find(sym->addr);
            bool do_not_decode = (bin_sym && bin_sym->isImportedFunction()) ||
                    // NODECODE isn't really the right modifier; perhaps we should have a LIB modifier,
                    // to specifically specify that this function obeys library calling conventions
                    sym->mods->noDecode;
            Function *p = tgt_mod->getOrInsertFunction(name, sym->addr,do_not_decode);
            if (!sym->mods->incomplete) {
                p->setSignature(sym->sig->clone());
                p->getSignature()->setForced(true);
            }
        } else {
            QString nam=sym->nam;
            if (sym->nam.isEmpty()) {
                nam = newGlobalName(sym->addr);
            }
            SharedType ty = sym->ty;
            if (ty == nullptr) {
                ty = guessGlobalType(nam, sym->addr);
            }
            globals.insert(new Global(ty, sym->addr, nam,this));
        }
    }

    for (SymbolRef *ref : par->refs) {
        DefaultFrontend->addRefHint(ref->addr, ref->nam);
    }

    delete par;
    ifs.close();
}

Global::~Global() {
    // Do-nothing d'tor
}
//! Get the initial value as an expression (or nullptr if not initialised)
Exp *Global::getInitialValue(Prog *prog) const {
    const IBinarySection *si = prog->getSectionInfoByAddr(uaddr);
    // TODO: see what happens when we skip Bss check here
    if (si && si->isAddressBss(uaddr))
        // This global is in the BSS, so it can't be initialised
        // NOTE: this is not actually correct. at least for typing, BSS data can have a type assigned
        return nullptr;
    if (si == nullptr)
        return nullptr;
    return prog->readNativeAs(uaddr, type);
}

QString Global::toString() const {
    Exp *init = getInitialValue(Parent);
    QString res = QString("%1 %2 at %3 initial value %4")
            .arg(type->toString())
            .arg(nam)
            .arg(uaddr.toString())
            .arg((init ? init->prints() : "<none>"));
    delete init;
    return res;
}
Exp *Prog::readNativeAs(ADDRESS uaddr, SharedType type) {
    Exp *e = nullptr;
    const IBinarySection *si = getSectionInfoByAddr(uaddr);
    if (si == nullptr)
        return nullptr;
    if (type->resolvesToPointer()) {
        ADDRESS init = ADDRESS::g(readNative4(uaddr));
        if (init.isZero())
            return new Const(0);
        QString nam = getGlobalName(init);
        if (!nam.isEmpty())
            // TODO: typecast?
            return Location::global(nam, nullptr);
        if (type->asPointer()->getPointsTo()->resolvesToChar()) {
            const char *str = getStringConstant(init);
            if (str != nullptr)
                return new Const(str);
        }
    }
    if (type->resolvesToCompound()) {
        std::shared_ptr<CompoundType> c = type->asCompound();
        Exp *n = e = new Terminal(opNil);
        for (unsigned int i = 0; i < c->getNumTypes(); i++) {
            ADDRESS addr = uaddr + c->getOffsetTo(i) / 8;
            SharedType t = c->getType(i);
            Exp *v = readNativeAs(addr, t);
            if (v == nullptr) {
                LOG << "unable to read native address " << addr << " as type " << t->getCtype() << "\n";
                v = new Const(-1);
            }
            if (n->isNil()) {
                n = Binary::get(opList, v, n);
                e = n;
            } else {
                assert(n->getSubExp2()->getOper() == opNil);
                n->setSubExp2(Binary::get(opList, v, n->getSubExp2()));
                n = n->getSubExp2();
            }
        }
        return e;
    }
    if (type->resolvesToArray() && type->asArray()->getBaseType()->resolvesToChar()) {
        const char *str = getStringConstant(uaddr, true);
        if (str) {
            // Make a global string
            return new Const(str);
        }
    }
    if (type->resolvesToArray()) {
        int nelems = -1;
        QString nam = getGlobalName(uaddr);
        int base_sz = type->asArray()->getBaseType()->getSize() / 8;
        if (!nam.isEmpty()) {
            auto symbol = BinarySymbols->find(nam);
            nelems = symbol ? symbol->getSize() : 0;
            assert(base_sz);
            nelems /= base_sz;
        }
        Exp *n = e = new Terminal(opNil);
        for (int i = 0; nelems == -1 || i < nelems; i++) {
            Exp *v = readNativeAs(uaddr + i * base_sz, type->asArray()->getBaseType());
            if (v == nullptr)
                break;
            if (n->isNil()) {
                n = Binary::get(opList, v, n);
                e = n;
            } else {
                assert(n->getSubExp2()->getOper() == opNil);
                n->setSubExp2(Binary::get(opList, v, n->getSubExp2()));
                n = n->getSubExp2();
            }
            // "null" terminated
            if (nelems == -1 && v->isConst() && ((Const *)v)->getInt() == 0)
                break;
        }
    }
    if (type->resolvesToInteger() || type->resolvesToSize()) {
        int size;
        if (type->resolvesToInteger())
            size = type->asInteger()->getSize();
        else
            size = type->asSize()->getSize();
        switch (size) {
        case 8:
            return new Const(Image->readNative1(uaddr));
        case 16:
            // Note: must respect endianness
            return new Const(Image->readNative2(uaddr));
        case 32:
            return new Const(Image->readNative4(uaddr));
        case 64:
            return new Const(Image->readNative8(uaddr));
        }
    }
    if (!type->resolvesToFloat())
        return e;
    switch (type->asFloat()->getSize()) {
    case 32:
        return new Const(Image->readNativeFloat4(uaddr));
    case 64:
        return new Const(Image->readNativeFloat8(uaddr));
    }
    return e;
}

void Global::meetType(SharedType ty) {
    bool ch=false;
    type = type->meetWith(ty, ch);
}
//! Re-decode this proc from scratch
void Prog::reDecode(UserProc *proc) {
    QTextStream os(stderr); // rtl output target
    DefaultFrontend->processProc(proc->getNativeAddress(), proc, os);
}

void Prog::decodeFragment(UserProc *proc, ADDRESS a) {
    if (a >= Image->getLimitTextLow() && a < Image->getLimitTextHigh())
        DefaultFrontend->decodeFragment(proc, a);
    else {
        LOG_STREAM() << "attempt to decode fragment outside text area, addr=" << a << "\n";
        if (VERBOSE)
            LOG << "attempt to decode fragment outside text area, addr=" << a << "\n";
    }
}
//
//
/***************************************************************************/ /**
  *
  * \brief This does extra processing on a constant.
  *
  * The Exp* \a e is expected to be a Const, and the ADDRESS \a lc is the native
  * location from which the constant was read.
  *
  * \returns processed Exp
  ******************************************************************************/
Exp *Prog::addReloc(Exp *e, ADDRESS lc) {
    assert(e->isConst());

    if (!pLoaderIface->IsRelocationAt(lc))
        return e;

    Const *c = (Const *)e;
    // relocations have been applied to the constant, so if there is a
    // relocation for this lc then we should be able to replace the constant
    // with a symbol.
    ADDRESS c_addr = c->getAddr();
    const IBinarySymbol *bin_sym = BinarySymbols->find(c_addr);
    if (bin_sym != nullptr) {
        unsigned int sz = bin_sym->getSize(); // TODO: fix the case of missing symbol table interface
        if (getGlobal(bin_sym->getName()) == nullptr) {
            Global *global = new Global(SizeType::get(sz * 8), c_addr, bin_sym->getName(),this);
            globals.insert(global);
        }
        return new Unary(opAddrOf, Location::global(bin_sym->getName(), nullptr));
    } else {
        const char *str = getStringConstant(c_addr);
        if (str)
            e = new Const(str);
        else {
            // check for accesses into the middle of symbols
            for (IBinarySymbol *it : *BinarySymbols) {
                unsigned int sz = it->getSize();
                if (it->getLocation() < c_addr && (it->getLocation() + sz) > c_addr) {
                    int off = (c->getAddr() - it->getLocation()).m_value;
                    e = Binary::get(opPlus, new Unary(opAddrOf, Location::global(it->getName(), nullptr)),
                                    new Const(off));
                    break;
                }
            }
        }
    }
    return e;
}


bool Prog::isStringConstant(ADDRESS a) {
    const SectionInfo *si = static_cast<const SectionInfo *>(Image->getSectionInfoByAddr(a));
    if(!si)
        return false;
    QVariant qv = si->attributeInRange("StringsSection",a,a+1);
    return !qv.isNull();
}

bool Prog::isCFStringConstant(ADDRESS a) { return isStringConstant(a); }
