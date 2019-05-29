#
# This file is part of the Boomerang Decompiler.
#
# See the file "LICENSE.TERMS" for information on usage and
# redistribution of this file, and for a DISCLAIMER OF ALL
# WARRANTIES.
#

include(boomerang-utils)

include_directories(
    "${CMAKE_SOURCE_DIR}/src"
)

BOOMERANG_ADD_CODEGEN(
    NAME "C"
    SOURCES
        c/ASTNode.cpp
        c/ASTNode.h
        c/CCodeGenerator.cpp
        c/CCodeGenerator.h
        c/CodeWriter.cpp
        c/CodeWriter.h
        c/ControlFlowAnalyzer.cpp
        c/ControlFlowAnalyzer.h
        c/StmtASTNode.cpp
        c/StmtASTNode.h
)
