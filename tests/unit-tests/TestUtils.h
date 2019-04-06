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


#include "boomerang/core/Project.h"
#include "boomerang/ssl/type/Type.h"
#include "boomerang/ssl/exp/Exp.h"
#include "boomerang/db/BasicBlock.h"

#include <QTest>


class LocationSet;


class TestProject : public Project
{
public:
    TestProject();
};


class BoomerangTest : public QObject
{
    Q_OBJECT

protected slots:
    void initTestCase();
    void cleanupTestCase();
};


class BoomerangTestWithProject : public BoomerangTest
{
protected:
    TestProject m_project;
};


class BoomerangTestWithPlugins : public BoomerangTestWithProject
{
    Q_OBJECT

protected slots:
    void initTestCase()
    {
        BoomerangTestWithProject::initTestCase();
        m_project.loadPlugins();
    }
};


/// HACK s to work around limitations of QMetaType which does not allow templates

struct SharedTypeWrapper
{
public:
    explicit SharedTypeWrapper()               : ty(nullptr) {}
    explicit SharedTypeWrapper(SharedType _ty) : ty(_ty) {}

public:
    SharedType operator->() { return ty; }
    SharedType operator*() { return ty; }
    operator SharedType() { return ty; }

public:
    SharedType ty;
};
Q_DECLARE_METATYPE(SharedTypeWrapper)


struct SharedExpWrapper
{
public:
    explicit SharedExpWrapper()               : exp(nullptr) {}
    explicit SharedExpWrapper(SharedExp _exp) : exp(_exp) {}

public:
    SharedExp operator->() { return exp; }
    SharedExp operator*()  { return exp; }
    operator SharedExp()  { return exp; }

public:
    SharedExp exp;
};
Q_DECLARE_METATYPE(SharedExpWrapper)


/// \returns the full absolute path given a path
// relative to the data/samples/ directory
QString getFullSamplePath(const QString& relpath);

void compareLongStrings(const QString& actual, const QString& expected);

char *toString(const Exp& exp);
char *toString(const SharedConstExp& exp);
char *toString(const LocationSet& locSet);
char *toString(ICLASS type);
char *toString(BBType type);
