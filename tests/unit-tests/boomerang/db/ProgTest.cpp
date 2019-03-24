#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "ProgTest.h"

#include "boomerang-plugins/frontend/x86/PentiumFrontEnd.h"

#include "boomerang/core/Settings.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/binary/BinarySection.h"
#include "boomerang/db/binary/BinarySymbol.h"
#include "boomerang/db/binary/BinarySymbolTable.h"
#include "boomerang/db/module/Module.h"
#include "boomerang/db/proc/LibProc.h"
#include "boomerang/db/signature/Signature.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/type/ArrayType.h"
#include "boomerang/ssl/type/CharType.h"
#include "boomerang/ssl/type/FloatType.h"
#include "boomerang/ssl/type/IntegerType.h"
#include "boomerang/ssl/type/PointerType.h"
#include "boomerang/ssl/type/VoidType.h"


#define HELLO_PENTIUM   getFullSamplePath("pentium/hello")
#define FBRANCH_PENTIUM getFullSamplePath("pentium/fbranch")
#define HELLO_WIN       getFullSamplePath("windows/hello.exe")


void ProgTest::testFrontend()
{
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));

    m_project.getProg()->setFrontEnd(nullptr);
    QVERIFY(m_project.getProg()->getFrontEnd() == nullptr);
    QVERIFY(m_project.getProg()->getModuleList().size() == 1);
    QVERIFY(m_project.getProg()->getRootModule() != nullptr);
    QVERIFY(m_project.getProg()->getRootModule()->getName() == m_project.getProg()->getName());

    m_project.getProg()->createModule("foo");
    QVERIFY(m_project.getProg()->getModuleList().size() == 2);

    m_project.getProg()->setFrontEnd(new PentiumFrontEnd(&m_project));
    QVERIFY(m_project.getProg()->getFrontEnd() != nullptr);
    QVERIFY(m_project.getProg()->getModuleList().size() == 1);
    QVERIFY(m_project.getProg()->getRootModule() != nullptr);
    QVERIFY(m_project.getProg()->getRootModule()->getName() == m_project.getProg()->getName());
}


void ProgTest::testName()
{
    QString progName = HELLO_PENTIUM;
    Prog prog(progName, nullptr);

    QCOMPARE(prog.getName(), progName);

    progName = "Happy Prog";
    prog.setName(progName);
    QCOMPARE(prog.getName(), progName);
}


void ProgTest::testCreateModule()
{
    Prog prog("test", nullptr);

    Module *mod = prog.createModule("");
    QVERIFY(mod != nullptr);
    QCOMPARE(mod->getParentModule(), prog.getRootModule());
    QCOMPARE(mod->getName(), QString(""));
    QCOMPARE(prog.getRootModule()->getNumChildren(), size_t(1));
    QCOMPARE(prog.getRootModule()->getChild(0), mod);
    QCOMPARE(prog.getModuleList().size(), size_t(2));

    // create exisiting module
    Module *existing = prog.createModule("");
    QVERIFY(existing == nullptr);

    Module *sub = prog.createModule("", mod);
    QVERIFY(sub != nullptr);
    QCOMPARE(sub->getParentModule(), mod);
    QCOMPARE(mod->getNumChildren(), size_t(1));
    QCOMPARE(prog.getRootModule()->getNumChildren(), size_t(1));
    QCOMPARE(mod->getChild(0), sub);
}


void ProgTest::testGetOrInsertModule()
{
    Prog prog("test", nullptr);

    // get module
    QCOMPARE(prog.getOrInsertModule("test"), prog.getRootModule());

    Module *mod = prog.getOrInsertModule("");
    QVERIFY(mod != nullptr);
    QVERIFY(mod != prog.getRootModule());

    QCOMPARE(prog.getOrInsertModule("foo"), prog.getOrInsertModule("foo"));
}


void ProgTest::testGetRootModule()
{
    Prog prog("test", nullptr);
    QVERIFY(prog.getRootModule() != nullptr);
}


void ProgTest::testFindModule()
{
    Prog prog("test", nullptr);
    QCOMPARE(prog.findModule("test"), prog.getRootModule());

    QVERIFY(prog.findModule("foo") == nullptr);

    Module *foo = prog.getOrInsertModule("foo");
    QCOMPARE(prog.findModule("foo"), foo);
}


void ProgTest::testIsModuleUsed()
{
    Prog prog("test", &m_project);

    QVERIFY(!prog.isModuleUsed(prog.getRootModule())); // no functions present in module

    prog.getOrCreateFunction(Address(0x1000));
    QVERIFY(prog.isModuleUsed(prog.getRootModule()));
}


void ProgTest::testAddEntryPoint()
{
    Prog prog("test", &m_project);

    QVERIFY(prog.addEntryPoint(Address::INVALID) == nullptr);

    Function *entry = prog.addEntryPoint(Address(0x1000));
    QVERIFY(entry != nullptr);

    // add existing entry point
    QCOMPARE(prog.addEntryPoint(Address(0x1000)), entry);

    // add entry point that is blocked by a lib proc
    LibProc *libProc = prog.getOrCreateLibraryProc("testProc");
    libProc->setEntryAddress(Address(0x2000));
    QVERIFY(prog.addEntryPoint(Address(0x2000)) == nullptr);
}


void ProgTest::testGetOrCreateFunction()
{
    Prog prog("test", &m_project);

    Function *func = prog.getOrCreateFunction(Address::INVALID);
    QVERIFY(func == nullptr);

    func = prog.getOrCreateFunction(Address(0x1000));
    QVERIFY(func != nullptr);
    QCOMPARE(func->getName(), QString("proc_0x00001000"));
    QCOMPARE(func->getEntryAddress(), Address(0x1000));
}


void ProgTest::testGetOrCreateLibraryProc()
{
    Prog prog("test", &m_project);

    LibProc *libProc = prog.getOrCreateLibraryProc("");
    QVERIFY(libProc == nullptr);

    libProc = prog.getOrCreateLibraryProc("testProc");
    QVERIFY(libProc != nullptr);
    QCOMPARE(libProc->getEntryAddress(), Address::INVALID);
    QCOMPARE(prog.getOrCreateLibraryProc("testProc"), libProc);
}


void ProgTest::testGetFunctionByAddr()
{
    Prog prog("test", &m_project);
    QVERIFY(prog.getFunctionByAddr(Address::INVALID) == nullptr);

    Function *func = prog.getOrCreateFunction(Address(0x1000));
    QVERIFY(prog.getFunctionByAddr(Address(0x1000)) == func);
}


void ProgTest::testGetFunctionByName()
{
    Prog prog("test", &m_project);
    QVERIFY(prog.getFunctionByName("test") == nullptr);

    Function *func = prog.getOrCreateFunction(Address(0x1000));
    func->setName("testFunc");
    QVERIFY(prog.getFunctionByName("testFunc") == func);
}


void ProgTest::testRemoveFunction()
{
    Prog prog("test", &m_project);

    QVERIFY(prog.removeFunction("") == false);

    Function *func = prog.getOrCreateFunction(Address(0x1000));
    QVERIFY(prog.removeFunction(func->getName()) == true);
    QVERIFY(prog.getFunctionByAddr(Address(0x1000)) == nullptr);

    func = prog.getOrCreateFunction(Address(0x1000));
    QVERIFY(func != nullptr);
    func->setName("testFunc");
    QVERIFY(prog.removeFunction(func->getName()) == true);
}


void ProgTest::testGetNumFunctions()
{
    Prog prog("test", &m_project);

    QCOMPARE(prog.getNumFunctions(true), 0);
    QCOMPARE(prog.getNumFunctions(false), 0);

    prog.getOrCreateFunction(Address(0x1000));

    QCOMPARE(prog.getNumFunctions(true), 1);
    QCOMPARE(prog.getNumFunctions(false), 1);

    prog.getOrCreateLibraryProc("foo");

    QCOMPARE(prog.getNumFunctions(true), 1);
    QCOMPARE(prog.getNumFunctions(false), 2);
}


void ProgTest::testIsWellFormed()
{
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));

    const Prog *hello = m_project.getProg();
    QVERIFY(hello->isWellFormed());
    QVERIFY(m_project.decodeBinaryFile());
    QVERIFY(hello->isWellFormed());
    QVERIFY(m_project.decompileBinaryFile());
    QVERIFY(hello->isWellFormed());

    Prog testProg("test", nullptr);
    QVERIFY(testProg.isWellFormed());
}


void ProgTest::testIsWin32()
{
    Prog testProg("test", nullptr);
    QVERIFY(!testProg.isWin32());

    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));
    QVERIFY(!m_project.getProg()->isWin32());
    QVERIFY(m_project.loadBinaryFile(HELLO_WIN));
    QVERIFY(m_project.getProg()->isWin32());
}


void ProgTest::testGetRegNameByNum()
{
    QSKIP("Not implemented.");
}


void ProgTest::testGetRegSizeByNum()
{
    QSKIP("Not implemented.");
}


void ProgTest::testGetMachine()
{
    Prog testProg("test", nullptr);
    QCOMPARE(testProg.getMachine(), Machine::INVALID);

    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));
    QCOMPARE(m_project.getProg()->getMachine(), Machine::PENTIUM);
}


void ProgTest::testGetDefaultSignature()
{
    Prog testProg("test", nullptr);
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));
    auto sig = m_project.getProg()->getDefaultSignature("foo");
    QVERIFY(sig != nullptr);
    QCOMPARE(sig->getName(), QString("foo"));
}


void ProgTest::testGetStringConstant()
{
    Prog testProg("test", nullptr);
    QVERIFY(testProg.getStringConstant(Address(0x1000), true ) == nullptr);
    QVERIFY(testProg.getStringConstant(Address(0x1000), false) == nullptr);
    QVERIFY(testProg.getStringConstant(Address::INVALID) == nullptr);

    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));
    const char *hello1 = m_project.getProg()->getStringConstant(Address(0x80483FC), false);
    QVERIFY(hello1 != nullptr);
    QCOMPARE(hello1, "Hello, world!\n");

    const char *hello2 = m_project.getProg()->getStringConstant(Address(0x80483FC), true);
    QCOMPARE(hello2, hello1);

    // zero length string
    const char *world1 = m_project.getProg()->getStringConstant(Address(0x804840A), false);
    QVERIFY(world1 != nullptr);
    QCOMPARE(world1, "");

    // with control characters
    const char *ld1 = m_project.getProg()->getStringConstant(Address(0x08048406), false);
    QVERIFY(ld1 != nullptr);
    QCOMPARE(ld1, "ld!\n");

    // no string constant can be extracted from .bss
    QVERIFY(m_project.getProg()->getStringConstant(Address(0x08049510), false) == nullptr);
    QVERIFY(m_project.getProg()->getStringConstant(Address(0x08049510), true) == nullptr);
}


void ProgTest::testGetFloatConstant()
{
    QVERIFY(m_project.loadBinaryFile(FBRANCH_PENTIUM));
    QVERIFY(m_project.decodeBinaryFile());

    double result;
    QVERIFY(!m_project.getProg()->getFloatConstant(Address::INVALID, result, 32));
    QVERIFY(m_project.getProg()->getFloatConstant(Address(0x080485CC), result, 32));
    QCOMPARE(result, 5.0);
}


void ProgTest::testGetSymbolNameByAddr()
{
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));

    // "Hello, world!" string is not a symbol
    QCOMPARE(m_project.getProg()->getSymbolNameByAddr(Address(0x080483FC)), QString(""));
    QCOMPARE(m_project.getProg()->getSymbolNameByAddr(Address(0x08048268)), QString("printf"));
}


void ProgTest::testGetSectionByAddr()
{
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));

    QVERIFY(m_project.getProg()->getSectionByAddr(Address::INVALID) == nullptr);

    const BinarySection *sect = m_project.getProg()->getSectionByAddr(Address(0x08048331));
    QVERIFY(sect != nullptr);
    QCOMPARE(sect->getName(), QString(".text"));
}


void ProgTest::testGetLimitText()
{
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));

    QCOMPARE(m_project.getProg()->getLimitTextLow(),  Address(0x08048230));
    QCOMPARE(m_project.getProg()->getLimitTextHigh(), Address(0x080483f3));
}


void ProgTest::testIsReadOnly()
{
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));

    QVERIFY(!m_project.getProg()->isReadOnly(Address::INVALID));
    QVERIFY(!m_project.getProg()->isReadOnly(Address(0x080496a8))); // address in .ctors
    QVERIFY( m_project.getProg()->isReadOnly(Address(0x080483f4))); // address in .rodata
}


void ProgTest::testIsInStringsSection()
{
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));

    QVERIFY(!m_project.getProg()->isInStringsSection(Address::INVALID));
    QVERIFY(!m_project.getProg()->isInStringsSection(Address(0x080483f4))); // address in .rodata
    QVERIFY( m_project.getProg()->isInStringsSection(Address(0x080481a0))); // address in .dynstr
}


void ProgTest::testIsDynamicallyLinkedProcPointer()
{
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));

    QVERIFY(!m_project.getProg()->isDynamicallyLinkedProcPointer(Address::INVALID));
    QVERIFY(!m_project.getProg()->isDynamicallyLinkedProcPointer(Address(0x080483f4)));
    QVERIFY( m_project.getProg()->isDynamicallyLinkedProcPointer(Address(0x08048268))); // address of printf
}


void ProgTest::testGetOrInsertModuleForSymbol()
{
    Prog prog("test", nullptr);
    QCOMPARE(prog.getOrInsertModuleForSymbol(""),     prog.getRootModule());
    QCOMPARE(prog.getOrInsertModuleForSymbol("test"), prog.getRootModule());

    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));
    QCOMPARE(m_project.getProg()->getOrInsertModuleForSymbol(""), m_project.getProg()->getRootModule());
    QCOMPARE(m_project.getProg()->getOrInsertModuleForSymbol("printf"), m_project.getProg()->getRootModule());

    BinarySymbol *mainSym = m_project.getLoadedBinaryFile()->getSymbols()->findSymbolByName("main");
    QVERIFY(mainSym != nullptr);
    mainSym->setAttribute("SourceFile", "foo.c");

    QCOMPARE(m_project.getProg()->getOrInsertModuleForSymbol(mainSym->getName()), m_project.getProg()->getOrInsertModule("foo"));
}


void ProgTest::testReadNative4()
{
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));

    QCOMPARE(m_project.getProg()->readNative4(Address::INVALID), 0);
    QCOMPARE(m_project.getProg()->readNative4(Address(0x80483FC)), 0x6c6c6548);
}


void ProgTest::testDecodeEntryPoint()
{
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));
    QVERIFY(!m_project.getProg()->decodeEntryPoint(Address::INVALID));
    QVERIFY(m_project.getProg()->decodeEntryPoint(m_project.getLoadedBinaryFile()->getEntryPoint()));
}


void ProgTest::testDecodeFragment()
{
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));
    UserProc *mainProc = (UserProc *)m_project.getProg()->getOrCreateFunction(Address(0x08048328));
    QVERIFY(!m_project.getProg()->decodeFragment(mainProc, Address::INVALID));
    QVERIFY(m_project.getProg()->decodeFragment(mainProc, Address(0x08048328)));
}


void ProgTest::testReDecode()
{
    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));
    UserProc *mainProc = (UserProc *)m_project.getProg()->getOrCreateFunction(Address(0x08048328));

    QVERIFY(!m_project.getProg()->reDecode(nullptr));
    QVERIFY(m_project.getProg()->reDecode(mainProc)); // actually processing for the first time
    QVERIFY(m_project.getProg()->reDecode(mainProc)); // actual re-decode
}


void ProgTest::testCreateGlobal()
{
    Prog prog("test", nullptr);

    QVERIFY(prog.createGlobal(Address::INVALID) == nullptr);

    Global *global = prog.createGlobal(Address(0x08000000));
    QVERIFY(global != nullptr);
}


void ProgTest::testGetGlobalNameByAddr()
{
    Prog prog("test", nullptr);
    QCOMPARE(prog.getGlobalNameByAddr(Address::INVALID), QString(""));

    prog.createGlobal(Address(0x08000000), IntegerType::get(32), "foo");
    QCOMPARE(prog.getGlobalNameByAddr(Address(0x08000000)), QString("foo"));
}


void ProgTest::testGetGlobalAddrByName()
{
    Prog prog("test", nullptr);
    prog.createGlobal(Address(0x08000000), IntegerType::get(32), "foo");
    QCOMPARE(prog.getGlobalAddrByName("foo"), Address(0x08000000));
    QCOMPARE(prog.getGlobalAddrByName("bar"), Address::INVALID);

    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));
    QCOMPARE(m_project.getProg()->getGlobalAddrByName("baz"), Address::INVALID);
}


void ProgTest::testGetGlobalByName()
{
    Prog prog("test", nullptr);
    QVERIFY(prog.getGlobalByName("foo") == nullptr);

    const Global *global = prog.createGlobal(Address(0x08000000), IntegerType::get(32), "foo");
    QCOMPARE(prog.getGlobalByName("foo"), global);
}


void ProgTest::testNewGlobalName()
{
    Prog prog("test", nullptr);
    QCOMPARE(prog.newGlobalName(Address(0x1000)), QString("global_0x00001000"));
    QCOMPARE(prog.newGlobalName(Address(0x1000)), QString("global_0x00001000"));
}


void ProgTest::testGuessGlobalType()
{
    {
        Prog prog("test", nullptr);
        SharedType ty = prog.guessGlobalType("test", Address(0x1000));
        QVERIFY(*ty == *VoidType::get());
    }

    QVERIFY(m_project.loadBinaryFile(HELLO_PENTIUM));

    SharedType ty = m_project.getProg()->guessGlobalType("helloworld", Address(0x80483FC));
    QVERIFY(*ty == *PointerType::get(CharType::get()));
}


void ProgTest::testMakeArrayType()
{
    Prog prog("test", nullptr);

    std::shared_ptr<ArrayType> ty = prog.makeArrayType(Address::INVALID, CharType::get());
    QVERIFY(*ty == *ArrayType::get(CharType::get()));

    m_project.loadBinaryFile(HELLO_PENTIUM);
    BinarySymbol *helloworld = m_project.getLoadedBinaryFile()->getSymbols()
        ->createSymbol(Address(0x80483FC), "helloworld");

    helloworld->setSize(15);

    // type of hello world
    ty = m_project.getProg()->makeArrayType(Address(0x80483FC), CharType::get());
    QCOMPARE(ty->getCtype(), QString("char[15]"));

    ty = m_project.getProg()->makeArrayType(Address(0x80483FC), VoidType::get());
    QCOMPARE(ty->getCtype(), QString("void[15]"));
}


void ProgTest::testMarkGlobalUsed()
{
    Prog prog("test", nullptr);
    QVERIFY(!prog.markGlobalUsed(Address::INVALID));

    m_project.loadBinaryFile(HELLO_PENTIUM);
    QVERIFY(m_project.getProg()->markGlobalUsed(Address(0x80483FC)));
    QVERIFY(m_project.getProg()->markGlobalUsed(Address(0x80483FC), IntegerType::get(32, Sign::Signed)));
    QVERIFY(m_project.getProg()->markGlobalUsed(Address(0x80483FC), ArrayType::get(CharType::get(), 15)));
}


void ProgTest::testGlobalType()
{
    m_project.loadBinaryFile(HELLO_PENTIUM);
    QVERIFY(m_project.getProg()->getGlobalType("") == nullptr);
    QVERIFY(m_project.getProg()->getGlobals().empty());

    m_project.getProg()->createGlobal(Address(0x80483FC),
        ArrayType::get(CharType::get(), 15), QString("helloworld"));

    SharedType ty = m_project.getProg()->getGlobalType("helloworld");
    QVERIFY(ty != nullptr);
    QCOMPARE(ty->getCtype(), QString("char[15]"));

    m_project.getProg()->setGlobalType("helloworld", IntegerType::get(32, Sign::Signed));
    ty = m_project.getProg()->getGlobalType("helloworld");
    QVERIFY(ty != nullptr);
    QCOMPARE(ty->getCtype(), QString("int"));
}


QTEST_GUILESS_MAIN(ProgTest)
