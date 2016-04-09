#include "module.h"

#include "boomerang.h"
#include "proc.h"
#include "prog.h"

#include "signature.h"

#include <QDir>
#include <QString>

#if defined(_WIN32) && !defined(__MINGW32__)
#undef NO_ADDRESS
#include <windows.h>
namespace dbghelp {
#include <dbghelp.h>
// dbghelp.h can define ADDRESS
#ifdef ADDRESS
#undef ADDRESS
#endif
}
#undef NO_ADDRESS
#define NO_ADDRESS ADDRESS::g(~0u)
#include <iostream>
#include "log.h"
#endif

#if defined(_WIN32) && !defined(__MINGW32__)
// From prog.cpp
BOOL CALLBACK addSymbol(dbghelp::PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext);
SharedType typeFromDebugInfo(int index, DWORD64 ModBase);
#endif

void Module::onLibrarySignaturesChanged()
{
    CurrentFrontend->readLibraryCatalog();
    for (Function *pProc : FunctionList) {
        if (pProc->isLib()) {
            pProc->setSignature(getLibSignature(pProc->getName()));
            for (CallStatement *call_stmt : pProc->getCallers())
                call_stmt->setSigArguments();
            Boomerang::get()->alertUpdateSignature(pProc);
        }
    }
}

Module::Module() { strm.setDevice(&out); }

Module::Module(const QString &_name, Prog *_parent, FrontEnd *fe) : CurrentFrontend(fe),Name(_name),Parent(_parent) {
    strm.setDevice(&out);
}

Module::~Module()
{
    for (Function *proc : FunctionList) {
        delete proc;
    }
}

size_t Module::getNumChildren() { return Children.size(); }

Module *Module::getChild(size_t n) { return Children[n]; }

void Module::addChild(Module *n) {
    if (n->Upstream)
        n->Upstream->removeChild(n);
    Children.push_back(n);
    n->Upstream = this;
}

void Module::removeChild(Module *n) {
    auto it = Children.begin();
    for (; it != Children.end(); it++)
        if (*it == n)
            break;
    assert(it != Children.end());
    Children.erase(it);
}

Module *Module::getUpstream() { return Upstream; }

bool Module::hasChildren() { return Children.size() > 0; }

void Module::openStream(const char *ext) {
    if (out.isOpen())
        return;
    out.setFileName(getOutPath(ext));
    out.open(QFile::WriteOnly | QFile::Text);
    stream_ext = ext;
}

void Module::openStreams(const char *ext) {
    openStream(ext);
    for (Module *child : Children)
        child->openStreams(ext);
}

void Module::closeStreams() {
    if (out.isOpen()) {
        strm.flush();
        out.close();
    }
    for (Module *child : Children)
        child->closeStreams();
}

QString Module::makeDirs() {
    QString path;
    if (Upstream)
        path = Upstream->makeDirs();
    else
        path = Boomerang::get()->getOutputPath();
    QDir dr(path);
    if (getNumChildren() > 0 || Upstream == nullptr) {
        dr.mkpath(Name);
        dr.cd(Name);
    }
    return dr.absolutePath();
}

QString Module::getOutPath(const char *ext) {
    QString basedir = makeDirs();
    QDir dr(basedir);
    return dr.absoluteFilePath(Name + "." + ext);
}

Module *Module::find(const QString &nam) {
    if (Name == nam)
        return this;
    for (Module *child : Children) {
        Module *c = child->find(nam);
        if (c)
            return c;
    }
    return nullptr;
}
/**
 * Prints a tree graph.
 */
void Module::printTree(QTextStream &ostr) {
    ostr << "\t\t" << Name << "\n";
    for (Module *elem : Children)
        elem->printTree(ostr);
}
/// Record the \a fnc location in the ADDRESS->Function map
/// If \a fnc is nullptr - remove given function from the map.
void Module::setLocationMap(ADDRESS loc, Function *fnc)
{
    if(fnc==nullptr) {
        size_t count = LabelsToProcs.erase(loc);
        assert(count==1);
    }
    else
        LabelsToProcs[loc] = fnc;
}

void Module::eraseFromParent()
{
    Parent->getModuleList().remove(this);
    delete this;
}
/***************************************************************************/ /**
  *
  * \brief    Creates a new Function object, adds it to the list of procs in this Module, and adds the address to
  * the list
  * \param name - Name for the proc
  * \param uNative - Native address of the entry point of the proc
  * \param bLib - If true, this will be a libProc; else a UserProc
  * \returns        A pointer to the new Function object
  ******************************************************************************/
Function *Module::getOrInsertFunction(const QString &name, ADDRESS uNative, bool bLib)
{
    Function *pProc;
    if (bLib)
        pProc = new LibProc(this, name, uNative);
    else
        pProc = new UserProc(this, name, uNative);
    if(NO_ADDRESS!=uNative) {
        assert(LabelsToProcs.find(uNative)==LabelsToProcs.end());
        LabelsToProcs[uNative] = pProc;
    }
    FunctionList.push_back(pProc); // Append this to list of procs
    // alert the watchers of a new proc
    emit newFunction(pProc);
    Boomerang::get()->alertNew(pProc);
    // TODO: add platform agnostic way of using debug information, should be moved to Loaders, Prog should just collect info
    // from Loader

#if defined(_WIN32) && !defined(__MINGW32__)
    if (CurrentFrontend->isWin32()) {
        // use debugging information
        HANDLE hProcess = GetCurrentProcess();
        dbghelp::SYMBOL_INFO *sym = (dbghelp::SYMBOL_INFO *)malloc(sizeof(dbghelp::SYMBOL_INFO) + 1000);
        sym->SizeOfStruct = sizeof(*sym);
        sym->MaxNameLen = 1000;
        sym->Name[0] = 0;
        BOOL got = dbghelp::SymFromAddr(hProcess, uNative.m_value, 0, sym);
        DWORD retType;
        if (got && *sym->Name &&
                dbghelp::SymGetTypeInfo(hProcess, sym->ModBase, sym->TypeIndex, dbghelp::TI_GET_TYPE, &retType)) {
            DWORD d;
            // get a calling convention
            got =
                    dbghelp::SymGetTypeInfo(hProcess, sym->ModBase, sym->TypeIndex, dbghelp::TI_GET_CALLING_CONVENTION, &d);
            if (got) {
                std::cout << "calling convention: " << d << "\n";
                // TODO: use it
            } else {
                // assume we're stdc calling convention, remove r28, r24 returns
                pProc->setSignature(Signature::instantiate(PLAT_PENTIUM, CONV_C, name));
            }

            // get a return type
            SharedType rtype = typeFromDebugInfo(retType, sym->ModBase);
            if (!rtype->isVoid()) {
                pProc->getSignature()->addReturn(rtype, Location::regOf(24));
            }

            // find params and locals
            dbghelp::IMAGEHLP_STACK_FRAME stack;
            stack.InstructionOffset = uNative.m_value;
            dbghelp::SymSetContext(hProcess, &stack, 0);
            dbghelp::SymEnumSymbols(hProcess, 0, nullptr, addSymbol, pProc);

            LOG << "final signature: ";
            pProc->getSignature()->printToLog();
            LOG << "\n";
        }
    }
#endif

    return pProc;
}

Function *Module::getFunction(const QString &name)
{
    for(Function *f : FunctionList) {
        if ( f->getName()==name)
            return f;
    }
    return nullptr;
}

Function *Module::getFunction(ADDRESS loc)
{
    auto iter = LabelsToProcs.find(loc);
    if( iter == LabelsToProcs.end() )
        return nullptr;
    return iter->second;
}

//! Get a library signature for a given name (used when creating a new library proc).
Signature *Module::getLibSignature(const QString &name) {
    return CurrentFrontend->getLibSignature(name);
}
