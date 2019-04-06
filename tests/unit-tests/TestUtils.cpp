#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "TestUtils.h"


#include "boomerang/core/Settings.h"
#include "boomerang/util/LocationSet.h"
#include "boomerang/util/log/Log.h"


TestProject::TestProject()
{
    getSettings()->setDataDirectory(BOOMERANG_TEST_BASE "share/boomerang/");
    getSettings()->setPluginDirectory(BOOMERANG_TEST_BASE "lib/boomerang/plugins/");
}


void BoomerangTest::initTestCase()
{
    Log::getOrCreateLog();

    qRegisterMetaType<SharedTypeWrapper>();
    qRegisterMetaType<SharedExpWrapper>();
}


void BoomerangTest::cleanupTestCase()
{
}


QString getFullSamplePath(const QString& relpath)
{
    return QString(BOOMERANG_TEST_BASE) + "share/boomerang/samples/" + relpath;
}


void compareLongStrings(const QString& actual, const QString& expected)
{
    QStringList actualList = actual.split('\n');
    QStringList expectedList = expected.split('\n');

    for (int i = 0; i < std::min(actualList.length(), expectedList.length()); i++) {
        QCOMPARE(actualList[i], expectedList[i]);
    }

    QCOMPARE(actualList.length(), expectedList.length());
}


char *toString(const SharedConstExp& exp)
{
    return QTest::toString(exp->toString());
}


char *toString(const Exp& exp)
{
    return QTest::toString(exp.toString());
}


char *toString(const LocationSet& locSet)
{
    QString tgt;
    OStream os(&tgt);
    locSet.print(os);

    return QTest::toString(tgt);
}


#define HANDLE_ENUM_VAL(x) case x: return QTest::toString(#x)


char *toString(ICLASS type)
{
    switch (type) {
    HANDLE_ENUM_VAL(ICLASS::NCT);
    HANDLE_ENUM_VAL(ICLASS::SD);
    HANDLE_ENUM_VAL(ICLASS::DD);
    HANDLE_ENUM_VAL(ICLASS::SCD);
    HANDLE_ENUM_VAL(ICLASS::SCDAN);
    HANDLE_ENUM_VAL(ICLASS::SCDAT);
    HANDLE_ENUM_VAL(ICLASS::SU);
    HANDLE_ENUM_VAL(ICLASS::SKIP);
    HANDLE_ENUM_VAL(ICLASS::NOP);
    }

    return QTest::toString("<unknown>");
}

char *toString(BBType type)
{
    switch (type) {
    HANDLE_ENUM_VAL(BBType::Invalid);
    HANDLE_ENUM_VAL(BBType::Fall);
    HANDLE_ENUM_VAL(BBType::Oneway);
    HANDLE_ENUM_VAL(BBType::Twoway);
    HANDLE_ENUM_VAL(BBType::Nway);
    HANDLE_ENUM_VAL(BBType::Ret);
    HANDLE_ENUM_VAL(BBType::Call);
    HANDLE_ENUM_VAL(BBType::CompJump);
    HANDLE_ENUM_VAL(BBType::CompCall);
    }

    return QTest::toString("<unknown>");
}
