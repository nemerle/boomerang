#
# This file is part of the Boomerang Decompiler.
#
# See the file "LICENSE.TERMS" for information on usage and
# redistribution of this file, and for a DISCLAIMER OF ALL
# WARRANTIES.
#


list(APPEND boomerang-visitor-sources
    visitor/expvisitor/BadMemofFinder
    visitor/expvisitor/ComplexityFinder
    visitor/expvisitor/ConstFinder
    visitor/expvisitor/ExpDestCounter
    visitor/expvisitor/ExpRegMapper
    visitor/expvisitor/ExpVisitor
    visitor/expvisitor/FlagsFinder
    visitor/expvisitor/UsedLocalFinder
    visitor/expvisitor/UsedLocsFinder

    visitor/expmodifier/CallBypasser
    visitor/expmodifier/ConstGlobalConverter
    visitor/expmodifier/DFALocalMapper
    visitor/expmodifier/ExpAddressSimplifier
    visitor/expmodifier/ExpArithSimplifier
    visitor/expmodifier/ExpCastInserter
    visitor/expmodifier/ExpModifier
    visitor/expmodifier/ExpPropagator
    visitor/expmodifier/ExpSimplifier
    visitor/expmodifier/ExpSSAXformer
    visitor/expmodifier/ExpSubscripter
    visitor/expmodifier/ExpSubscriptReplacer
    visitor/expmodifier/ImplicitConverter
    visitor/expmodifier/Localiser
    visitor/expmodifier/SimpExpModifier

    visitor/stmtvisitor/StmtCastInserter
    visitor/stmtvisitor/StmtVisitor

    visitor/stmtmodifier/StmtImplicitConverter
    visitor/stmtmodifier/StmtModifier
    visitor/stmtmodifier/StmtPartModifier
    visitor/stmtmodifier/StmtSSAXFormer
    visitor/stmtmodifier/StmtSubscripter
    visitor/stmtmodifier/StmtSubscriptReplacer

    visitor/stmtexpvisitor/StmtConstFinder
    visitor/stmtexpvisitor/StmtDestCounter
    visitor/stmtexpvisitor/StmtExpVisitor
    visitor/stmtexpvisitor/StmtRegMapper
    visitor/stmtexpvisitor/UsedLocsVisitor
)

BOOMERANG_LIST_APPEND_FOREACH(boomerang-visitor-sources ".cpp")

set(boomerang-sources "${boomerang-sources};${boomerang-visitor-sources}" PARENT_SCOPE)
