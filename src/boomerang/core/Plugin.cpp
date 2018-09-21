#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "Plugin.h"

#include <cassert>

#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
#    include "Windows.h"
#else
#    include <dlfcn.h>
#endif


PluginHandle::PluginHandle(const QString &filePath)
{
#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
    m_handle = LoadLibrary(qPrintable(filePath));

    if (m_handle == nullptr) {
        throw "Failed to LoadLibrary!";
    }
#else
    m_handle = dlopen(qPrintable(filePath), RTLD_NOW);

    if (m_handle == nullptr) {
        throw dlerror();
    }
#endif
}


PluginHandle::~PluginHandle()
{
#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
    FreeLibrary((HMODULE)m_handle);
#else
    dlclose(m_handle);
#endif
}


PluginHandle::Symbol PluginHandle::getSymbol(const char *name) const
{
#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
    return (PluginHandle::Symbol)GetProcAddress((HMODULE)m_handle, name);
#else
    return dlsym(m_handle, name);
#endif
}
