#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "DebugInfo.h"

#include "boomerang/db/proc/UserProc.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/ssl/exp/Binary.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/type/ArrayType.h"
#include "boomerang/ssl/type/BooleanType.h"
#include "boomerang/ssl/type/CharType.h"
#include "boomerang/ssl/type/CompoundType.h"
#include "boomerang/ssl/type/FloatType.h"
#include "boomerang/ssl/type/FuncType.h"
#include "boomerang/ssl/type/IntegerType.h"
#include "boomerang/ssl/type/NamedType.h"
#include "boomerang/ssl/type/PointerType.h"
#include "boomerang/ssl/type/VoidType.h"
#include "boomerang/util/log/Log.h"

#include <cassert>

#ifdef _WIN32
#    include <windows.h>
#    ifndef __MINGW32__
namespace dbghelp
{
#    endif
#    include <dbghelp.h>
#    ifndef __MINGW32__
}
#    endif
#endif


#if defined(_WIN32) && !defined(__MINGW32__)

SharedType typeFromDebugInfo(int index, DWORD64 ModBase);

SharedType makeUDT(int index, DWORD64 ModBase)
{
    HANDLE hProcess = GetCurrentProcess();
    WCHAR *name;

    BOOL gotType = dbghelp::SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_GET_SYMNAME,
                                           &name);
    if (!gotType) {
        return nullptr;
    }

    char nameA[1024];
    WideCharToMultiByte(CP_ACP, 0, name, -1, nameA, sizeof(nameA), 0, nullptr);
    SharedType ty = Type::getNamedType(nameA);

    if (ty) {
        return NamedType::get(nameA);
    }

    auto cty    = CompoundType::get();
    DWORD count = 0;
    dbghelp::SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_GET_CHILDRENCOUNT, &count);
    int FindChildrenSize = sizeof(dbghelp::TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG);
    dbghelp::TI_FINDCHILDREN_PARAMS *pFC = (dbghelp::TI_FINDCHILDREN_PARAMS *)malloc(
        FindChildrenSize);
    memset(pFC, 0, FindChildrenSize);
    pFC->Count = count;
    SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_FINDCHILDREN, pFC);

    for (unsigned int i = 0; i < count; i++) {
        char fieldName[1024];
        dbghelp::SymGetTypeInfo(hProcess, ModBase, pFC->ChildId[i], dbghelp::TI_GET_SYMNAME, &name);
        WideCharToMultiByte(CP_ACP, 0, name, -1, fieldName, sizeof(fieldName), 0, nullptr);
        DWORD mytype;
        dbghelp::SymGetTypeInfo(hProcess, ModBase, pFC->ChildId[i], dbghelp::TI_GET_TYPE, &mytype);
        cty->addMember(typeFromDebugInfo(mytype, ModBase), fieldName);
    }

    Type::addNamedType(nameA, cty);
    ty = Type::getNamedType(nameA);
    assert(ty);
    return NamedType::get(nameA);
}


SharedType typeFromDebugInfo(int index, DWORD64 ModBase)
{
    HANDLE hProcess = GetCurrentProcess();

    int got;
    DWORD d;
    ULONG64 lsz = 0;

    got = dbghelp::SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_GET_SYMTAG, &d);
    assert(got);
    got    = dbghelp::SymGetTypeInfo(hProcess, ModBase, index, dbghelp::TI_GET_LENGTH, &lsz);
    int sz = (int)lsz * 8; // bits

    switch (d) {
    case 11: return makeUDT(index, ModBase);

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
        case 1: return VoidType::get();

        case 2: return CharType::get();

        case 3: return CharType::get();

        case 6:  // int
        case 13: // long
            return IntegerType::get(sz, Sign::Signed);

        case 7:  // unsigned int
        case 14: // ulong
            return IntegerType::get(sz, Sign::Unsigned);

        case 8: return FloatType::get(sz);

        case 10: return BooleanType::get();

        default: LOG_FATAL("Unhandled base type %1", (int)d);
        }

        break;

    default: LOG_FATAL("Unhandled symtag %1", (int)d);
    }

    return nullptr;
}


int debugRegister(int r)
{
    switch (r) {
    case 2: return REG_PENT_EDX;
    case 4: return REG_PENT_ECX;
    case 8: return REG_PENT_EBP;
    }

    assert(false);
    return -1;
}


BOOL CALLBACK addSymbol(dbghelp::PSYMBOL_INFO symInfo, ULONG /*SymbolSize*/, PVOID UserContext)
{
    Function *proc = static_cast<Function *>(UserContext);

    if (symInfo->Flags & SYMFLAG_PARAMETER) {
        SharedType ty = typeFromDebugInfo(symInfo->TypeIndex, symInfo->ModBase);

        if (symInfo->Flags & SYMFLAG_REGREL) {
            assert(symInfo->Register == 8); // ebp
            proc->getSignature()->addParameter(
                symInfo->Name,
                Location::memOf(Binary::get(opPlus, Location::regOf(REG_PENT_ESP),
                                            Const::get((int)symInfo->Address - 4))),
                ty);
        }
        else if (symInfo->Flags & SYMFLAG_REGISTER) {
            proc->getSignature()->addParameter(
                symInfo->Name, Location::regOf(debugRegister(symInfo->Register)), ty);
        }
    }
    else if ((symInfo->Flags & SYMFLAG_LOCAL) && !proc->isLib()) {
        UserProc *uproc = static_cast<UserProc *>(proc);
        assert(symInfo->Flags & SYMFLAG_REGREL);
        assert(symInfo->Register == 8);
        SharedExp memref = Location::memOf(Binary::get(opMinus, Location::regOf(REG_PENT_ESP),
                                                       Const::get(-((int)symInfo->Address - 4))));
        SharedType ty    = typeFromDebugInfo(symInfo->TypeIndex, symInfo->ModBase);
        uproc->addLocal(ty, symInfo->Name, memref);
    }

    return TRUE;
}

#endif

namespace DebugInfo
{
SharedType typeFromDebugInfo(const QString &name, Address addr)
{
#if defined(_WIN32) && !defined(__MINGW32__)
    HANDLE hProcess           = GetCurrentProcess();
    dbghelp::SYMBOL_INFO *sym = (dbghelp::SYMBOL_INFO *)malloc(sizeof(dbghelp::SYMBOL_INFO) + 1000);
    sym->SizeOfStruct         = sizeof(*sym);
    sym->MaxNameLen           = 1000;
    sym->Name[0]              = 0;
    BOOL got                  = dbghelp::SymFromAddr(hProcess, addr.value(), 0, sym);

    if (got && *sym->Name && sym->TypeIndex) {
        assert(name == sym->Name);
        return ::typeFromDebugInfo(sym->TypeIndex, sym->ModBase);
    }
#endif

    Q_UNUSED(name);
    Q_UNUSED(addr);
    return nullptr;
}
}
