#pragma once

#include <QtTest/QTest>

class HpSomBinaryLoaderTest : public QObject
{
	Q_OBJECT

private slots:
    void initTestCase();

    /// Test loading the sparc hello world program
	void testHppaLoad();
};
