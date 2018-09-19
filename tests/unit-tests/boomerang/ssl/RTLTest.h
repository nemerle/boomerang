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
 * Tests Register Transfer Lists
 */
class RTLTest : public BoomerangTestWithProject
{
    Q_OBJECT

private slots:
    /// Test appendExp and printing of RTLs
    void testAppend();

    /// Test the accept function for correct visiting behaviour.
    /// \note Stub class to test.
    void testVisitor();
};
