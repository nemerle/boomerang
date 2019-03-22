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


#include "boomerang/util/log/Log.h"


/**
 * Class for logging to a separate file different from the default log.
 */
class SeparateLogger : public Log
{
public:
    /// \param fullFilePath Full absolute path to the log file
    SeparateLogger(const QString &fullFilePath);
    SeparateLogger(const SeparateLogger &other) = delete;
    SeparateLogger(SeparateLogger &&other)      = delete;

    virtual ~SeparateLogger() override = default;

    SeparateLogger &operator=(const SeparateLogger &other) = delete;
    SeparateLogger &operator=(SeparateLogger &&other) = delete;

public:
    static SeparateLogger &getOrCreateLog(const QString &name);
};


#define LOG_SEPARATE(name, ...)                                                                    \
    SeparateLogger::getOrCreateLog(name).log(LogLevel::Default, __FILE__, __LINE__, __VA_ARGS__)
