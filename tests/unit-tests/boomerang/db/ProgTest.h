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


#include "TestUtils.h"


/**
 * Test the Prog class.
 */
class ProgTest : public BoomerangTestWithPlugins
{
    Q_OBJECT

private slots:
    void testFrontend(); // getFrontend() / setFrontend()
    void testName();     // getName() / setName()

    void testCreateModule();
    void testGetOrInsertModule();
    void testGetRootModule();
    void testFindModule();
    void testIsModuleUsed();

    void testAddEntryPoint();
    void testGetOrCreateFunction();
    void testGetOrCreateLibraryProc();
    void testGetFunctionByAddr();
    void testGetFunctionByName();
    void testRemoveFunction();
    void testGetNumFunctions();

    void testIsWellFormed();
    void testIsWin32();
    void testGetRegNameByNum();
    void testGetRegSizeByNum();

    void testGetMachine();
    void testGetDefaultSignature();

    void testGetStringConstant();
    void testGetFloatConstant();
    void testGetSymbolNameByAddr();
    void testGetSectionByAddr();
    void testGetLimitText();
    void testIsReadOnly();
    void testIsInStringsSection();
    void testIsDynamicallyLinkedProcPointer();
    void testGetOrInsertModuleForSymbol();

    void testReadNative4();

    void testDecodeEntryPoint();
    void testDecodeFragment();
    void testReDecode();

    void testCreateGlobal();
    void testGetGlobalNameByAddr();
    void testGetGlobalAddrByName();
    void testGetGlobalByName();
    void testNewGlobalName();
    void testGuessGlobalType();
    void testMakeArrayType();
    void testMarkGlobalUsed();
    void testGlobalType(); // getGlobalType/setGlobalType
};
