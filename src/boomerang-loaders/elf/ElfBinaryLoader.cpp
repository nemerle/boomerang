#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "ElfBinaryLoader.h"

#include "ElfTypes.h"

#include "boomerang/db/binary/BinaryImage.h"
#include "boomerang/db/binary/BinarySection.h"
#include "boomerang/db/binary/BinarySymbol.h"
#include "boomerang/db/binary/BinarySymbolTable.h"
#include "boomerang/util/Util.h"
#include "boomerang/util/log/Log.h"

#include <QBuffer>
#include <QFile>


struct SectionParam
{
    QString Name;
    Address SourceAddr   = Address::INVALID;
    size_t Size          = 0;
    DWord entry_size     = 0;
    bool ReadOnly        = false;
    bool Bss             = false;
    bool Code            = false;
    bool Data            = false;
    HostAddress imagePtr = HostAddress::ZERO;
    DWord sectionType    = 0; ///< Type of section (format dependent)
};

// not part of anonymous namespace, since it would create an ambiguity
// anonymous_namespace::Translated_ElfSym vs. ElfTypes.h/Translated_ElfSym declarations
struct Translated_ElfSym
{
    QString Name;
    ElfSymType Type;
    ElfSymBinding Binding;
    ElfSymVisibility Visibility;
    uint32_t SymbolSize;
    uint16_t SectionIdx;
    Address Value;
};

typedef std::map<QString, int, std::less<QString>> StrIntMap;


ElfBinaryLoader::ElfBinaryLoader()
    : m_nextExtern(Address::ZERO)
{
    init(); // Initialise all the common stuff
}


ElfBinaryLoader::~ElfBinaryLoader()
{
    // Delete the array of import stubs
    delete[] m_importStubs;
    delete[] m_shLink;
    delete[] m_shInfo;

    m_importStubs = nullptr;
    m_shLink      = nullptr;
    m_shInfo      = nullptr;
}


void ElfBinaryLoader::initialize(BinaryImage *image, BinarySymbolTable *symbols)
{
    m_binaryImage = image;
    m_symbols     = symbols;
}


void ElfBinaryLoader::init()
{
    m_loadedImage   = nullptr;
    m_elfHeader     = nullptr; // No ELF header
    m_programHdrs   = nullptr; // No program headers
    m_sectionHdrs   = nullptr; // No section headers
    m_strings       = nullptr; // No strings
    m_relocSection  = nullptr;
    m_symbolSection = nullptr;
    m_pltMin        = Address::ZERO; // No PLT limits
    m_pltMax        = Address::ZERO;
    m_lastSize      = 0;
    m_importStubs   = nullptr;
    m_elfSections.clear();
}


bool ElfBinaryLoader::loadFromMemory(QByteArray &img)
{
    m_loadedImageSize = img.size();

    // Allocate memory to hold the file
    m_loadedImage = reinterpret_cast<Byte *>(img.data());
    m_elfHeader   = reinterpret_cast<Elf32_Ehdr *>(img.data()); // Save a lot of casts

    if (m_loadedImageSize < sizeof(Elf32_Ehdr)) {
        LOG_ERROR("Cannot load ELF file: File size too small");
        return false;
    }

    // Basic checks
    if ((m_elfHeader->e_ident[EI_MAGO] != ELFMAG0) || (m_elfHeader->e_ident[EI_MAG1] != ELFMAG1) ||
        (m_elfHeader->e_ident[EI_MAG2] != ELFMAG2) || (m_elfHeader->e_ident[EI_MAG3] != ELFMAG3)) {
        LOG_ERROR("Cannot load ELF file: Bad magic %1 %2 %3 %4", m_elfHeader->e_ident[EI_MAGO],
                  m_elfHeader->e_ident[EI_MAG1], m_elfHeader->e_ident[EI_MAG2],
                  m_elfHeader->e_ident[EI_MAG3]);
        return false;
    }

    // endianness
    switch (m_elfHeader->e_ident[EI_DATA]) {
    case ELFDATA2LSB: m_endian = Endian::Little; break;
    case ELFDATA2MSB: m_endian = Endian::Big; break;

    default:
        LOG_ERROR("Cannot load ELF file: Unknown ELF endianness %1", m_elfHeader->e_ident[EI_DATA]);
        return false;
    }

    // Set up program and section header pointer (in case needed)
    const Elf32_Off phOffset = elfRead4(&m_elfHeader->e_phoff);
    const Elf32_Off shOffset = elfRead4(&m_elfHeader->e_shoff);

    if (!Util::inRange(phOffset, 1UL, m_loadedImageSize)) {
        LOG_ERROR("Cannot load ELF file: Invalid program header offset %1", phOffset);
        return false;
    }
    else if (!Util::inRange(shOffset, 1UL, m_loadedImageSize)) {
        LOG_ERROR("Cannot load ELF file: Invalid section header offset %1", shOffset);
        return false;
    }

    m_programHdrs = reinterpret_cast<Elf32_Phdr *>(m_loadedImage + phOffset);
    m_sectionHdrs = reinterpret_cast<Elf32_Shdr *>(m_loadedImage + shOffset);

    // Number of sections
    const Elf32_Half numSections = elfRead2(&m_elfHeader->e_shnum);
    if (numSections == 0) {
        LOG_ERROR("Cannot load ELF file: No sections found");
        return false;
    }
    else if ((const Byte *)(m_sectionHdrs + numSections) > m_loadedImage + m_loadedImageSize) {
        LOG_ERROR("Cannot load ELF file: Section header information extends past end of file");
        return false;
    }

    // Set up the m_sh_link and m_sh_info arrays
    if (m_shLink) {
        delete[] m_shLink;
    }
    if (m_shInfo) {
        delete[] m_shInfo;
    }

    m_shLink = new Elf32_Word[numSections];
    m_shInfo = new Elf32_Word[numSections];

    // Set up section header string table pointer
    const Elf32_Half stringSectionIndex = elfRead2(&m_elfHeader->e_shstrndx);

    if (!Util::inRange(stringSectionIndex, static_cast<Elf32_Half>(1), numSections)) {
        LOG_ERROR("Cannot load ELF file: Invalid string section index %1", stringSectionIndex);
        return false;
    }

    const Elf32_Off stringSectionOffset = elfRead4(&m_sectionHdrs[stringSectionIndex].sh_offset);
    if (!Util::inRange(stringSectionOffset, 1UL, m_loadedImageSize)) {
        LOG_ERROR("Cannot load ELF file: Invalid string section offset %1", stringSectionOffset);
    }

    m_strings = reinterpret_cast<const char *>(m_loadedImage + stringSectionOffset);

    bool seenCode            = false; // True when have seen a code sect
    Address arbitaryLoadAddr = Address(0x80000000);

    for (Elf32_Half i = 0; i < numSections; i++) {
        // Get section information.
        const Elf32_Shdr *sectionHeader = m_sectionHdrs + i;

        // Check if this section header entry is fully contained in this file
        if (reinterpret_cast<const Byte *>(sectionHeader + 1) > m_loadedImage + m_loadedImageSize) {
            LOG_ERROR(
                "Cannot load ELF file: Section header for section %1 extends past image boundary",
                i);
            return false;
        }

        const char *sectionName = m_strings + elfRead4(&sectionHeader->sh_name);

        if (reinterpret_cast<const Byte *>(sectionName) > m_loadedImage + m_loadedImageSize) {
            LOG_ERROR("Cannot load ELF file: Section name for section %1 is outside of image size",
                      i);
            return false;
        }

        SectionParam newSection;
        newSection.Name = sectionName;

        // Since the bss section is a special section, just assume it is always present
        newSection.Bss      = (strcmp(sectionName, ".bss") == 0);
        newSection.Code     = false;
        newSection.Data     = false;
        newSection.ReadOnly = false;

        Elf32_Off _off = elfRead4(&sectionHeader->sh_offset);
        if (!Util::inRange(_off, 0UL, m_loadedImageSize)) {
            LOG_ERROR("Cannot load ELF file: Section data for section %1 is outside of image size",
                      i);
            return false;
        }
        else if (_off != 0) {
            newSection.imagePtr = HostAddress(m_loadedImage) + _off;
        }

        newSection.SourceAddr = Address(elfRead4(&sectionHeader->sh_addr));
        newSection.Size       = elfRead4(&sectionHeader->sh_size);

        if (_off + newSection.Size > m_loadedImageSize && !newSection.Bss) {
            LOG_ERROR("Cannot load ELF file: Section %1 extends past image boundary", i);
            return false;
        }

        if (newSection.SourceAddr.isZero() && !QString(sectionName).startsWith(".rel")) {
            const Elf32_Word align = elfRead4(&sectionHeader->sh_addralign);

            if (align > 1) {
                if (arbitaryLoadAddr.value() % align != 0) {
                    arbitaryLoadAddr += align - (arbitaryLoadAddr.value() % align);
                }
            }

            newSection.SourceAddr = arbitaryLoadAddr;
            arbitaryLoadAddr += newSection.Size ? newSection.Size : 1;
        }

        newSection.sectionType = elfRead4(&sectionHeader->sh_type);
        newSection.entry_size  = elfRead4(&sectionHeader->sh_entsize);
        m_shLink[i]            = elfRead4(&sectionHeader->sh_link);
        m_shInfo[i]            = elfRead4(&sectionHeader->sh_info);

        if (newSection.SourceAddr + newSection.Size > m_nextExtern) {
            m_firstExtern = m_nextExtern = newSection.SourceAddr + newSection.Size;
        }

        if ((elfRead4(&sectionHeader->sh_flags) & SHF_WRITE) == 0) {
            newSection.ReadOnly = true;
        }

        if (elfRead4(&sectionHeader->sh_flags) & SHF_EXECINSTR) {
            newSection.Code = true;
            seenCode        = true; // We've got to a code section
        }

        // Deciding what is data and what is not is actually quite tricky but important.
        // For example, it's crucial to flag the .exception_ranges section as data,
        // otherwise there is a "hole" in the allocation map, that means
        // that there is more than one "delta" from a read-only section to a page,
        // and in the end using -C results in a file that looks OK
        // but when run just says "Killed". So we use the Elf designations;
        // it seems that ALLOC.!EXEC -> data
        // But we don't want sections before the .text section, like .interp, .hash, etc etc.
        // Hence seenCode.
        //
        // NOTE: this ASSUMES that sections appear in a sensible order in the input binary file:
        // junk, code, rodata, data, bss
        if (seenCode &&
            ((elfRead4(&sectionHeader->sh_flags) & (SHF_EXECINSTR | SHF_ALLOC)) == SHF_ALLOC) &&
            (elfRead4(&sectionHeader->sh_type) != SHT_NOBITS)) {
            newSection.Data = true;
        }

        m_elfSections.push_back(newSection);
    }

    // assign arbitary addresses to .rel.* sections too
    for (SectionParam &sect : m_elfSections) {
        if (sect.SourceAddr.isZero() && sect.Name.startsWith(".rel")) {
            sect.SourceAddr = arbitaryLoadAddr;
            arbitaryLoadAddr += (sect.Size > 0) ? sect.Size : 1;
        }
    }

    // Inform Boomerang about new sections
    for (SectionParam par : m_elfSections) {
        if (par.Size == 0) {
            // this is most probably the NULL section; if it is not, warn the user
            if (par.Name != "") {
                LOG_WARN("Not adding 0 sized section '%1'", par.Name);
            }

            continue;
        }

        const Address lowAddr  = par.SourceAddr;
        const Address highAddr = par.SourceAddr + par.Size;

        BinarySection *sect = m_binaryImage->createSection(par.Name, lowAddr, highAddr);

        if (sect) {
            sect->setBss(par.Bss);
            sect->setCode(par.Code);
            sect->setData(par.Data);
            sect->setEndian(m_endian);
            sect->setHostAddr(par.imagePtr);
            sect->setEntrySize(par.entry_size);
            sect->setReadOnly(par.ReadOnly);

            if (!(par.Bss || par.SourceAddr.isZero())) {
                sect->addDefinedArea(par.SourceAddr, par.SourceAddr + par.Size);
            }

            if (par.sectionType == SHT_STRTAB) {
                sect->setAttributeForRange("StringsSection", true, sect->getSourceAddr(),
                                           sect->getSourceAddr() + sect->getSize());
            }
        }
    }

    // Add symbol info. Note that some symbols will be in the main table only, and others in the
    // dynamic table only. So the best idea is to add symbols for all sections of the appropriate
    // type
    for (size_t i = 1; i < m_elfSections.size(); ++i) {
        const unsigned int sectionType = m_elfSections[i].sectionType;

        if ((sectionType == SHT_SYMTAB) || (sectionType == SHT_DYNSYM)) {
            addSymbolsForSection(i);
        }
    }

    // Save the relocation to symbol table info
    const BinarySection *section = m_binaryImage->getSectionByName(".rela.text");

    if (section) {
        m_relocHasAddend = true; // Remember its a relA table
        m_relocSection   = reinterpret_cast<Elf32_Rel *>(
            section->getHostAddr().value()); // Save pointer to reloc table
    }
    else {
        m_relocHasAddend = false;
        section          = m_binaryImage->getSectionByName(".rel.text");

        if (section) {
            m_relocSection = reinterpret_cast<Elf32_Rel *>(
                section->getHostAddr().value()); // Save pointer to reloc table
        }
    }

    // Find the PLT limits. Required for IsDynamicLinkedProc(), e.g.
    const BinarySection *pltSection = m_binaryImage->getSectionByName(".plt");

    if (pltSection) {
        m_pltMin = pltSection->getSourceAddr();
        m_pltMax = pltSection->getSourceAddr() + pltSection->getSize();
    }

    // Apply relocations; important when the input program is not compiled with -fPIC
    applyRelocations();
    markImports();

    return true;
}


void ElfBinaryLoader::unload()
{
    init(); // Set all internal state to 0
}


const char *ElfBinaryLoader::getStrPtr(int sectionIdx, int offset)
{
    if (sectionIdx < 0) {
        // Most commonly, this will be an index of -1, because a call to GetSectionIndexByName()
        // failed
        LOG_ERROR("Invalid index %1", sectionIdx);
        return nullptr;
    }

    // Get a pointer to the start of the string table
    const char *stringSym = reinterpret_cast<const char *>(
        m_elfSections[sectionIdx].imagePtr.value());

    if (Util::inRange((const Byte *)stringSym + offset, m_loadedImage,
                      m_loadedImage + m_loadedImageSize)) {
        // Just add the offset
        return stringSym + offset;
    }
    return nullptr;
}


Address ElfBinaryLoader::findRelPltOffset(int i)
{
    const BinarySection *siPlt    = m_binaryImage->getSectionByName(".plt");
    Address addrPlt               = siPlt ? siPlt->getSourceAddr() : Address::ZERO;
    const BinarySection *siRelPlt = m_binaryImage->getSectionByName(".rel.plt");
    int sizeRelPlt                = 8; // Size of each entry in the .rel.plt table

    if (siRelPlt == nullptr) {
        siRelPlt   = m_binaryImage->getSectionByName(".rela.plt");
        sizeRelPlt = 12; // Size of each entry in the .rela.plt table is 12 bytes
    }

    HostAddress addrRelPlt = HostAddress::ZERO;
    int numRelPlt          = 0;

    if (siRelPlt) {
        addrRelPlt = siRelPlt->getHostAddr();
        numRelPlt  = sizeRelPlt ? siRelPlt->getSize() / sizeRelPlt : 0;
    }
    else {
        return Address::INVALID; // neither .rel.plt nor .rela.plt are available
    }

    int first = i;

    if (first >= numRelPlt) {
        first = numRelPlt - 1;
    }

    int curr         = first;
    int pltEntrySize = siPlt->getEntrySize();

    if (pltEntrySize == 0) {
        return Address::INVALID;
    }

    do {
        // Each entry is sizeRelPlt bytes, and will contain the offset, then the info (addend
        // optionally follows)
        DWord *pltEntry     = reinterpret_cast<DWord *>((addrRelPlt + (curr * sizeRelPlt)).value());
        const int entry     = elfRead4(pltEntry + 1);
        const int sym       = entry >> 8; // The symbol index is in the top 24 bits (Elf32 only)
        const int entryType = entry & 0xFF;

        if (sym == i) {
            const BinarySection *targetSect = m_binaryImage->getSectionByAddr(
                Address(elfRead4(pltEntry)));

            if (targetSect) {
                if (targetSect->getName().contains("got")) {
                    int c           = elfRead4(pltEntry) - targetSect->getSourceAddr().value();
                    int plt_offset2 = elfRead4(
                        reinterpret_cast<DWord *>((targetSect->getHostAddr() + c).value()));
                    int plt_idx = (plt_offset2 % pltEntrySize);

                    if (entryType == R_386_JMP_SLOT) {
                        return Address(plt_offset2 - 6);
                    }

                    return addrPlt + plt_idx * pltEntrySize;
                }

                const int plt_offset = elfRead4(pltEntry) - siPlt->getSourceAddr().value();
                // Found! Now we want the native address of the associated PLT entry.
                // For now, assume a size of 0x10 for each PLT entry, and assume that each entry in
                // the .rel.plt section corresponds exactly to an entry in the .plt (except there is
                // one dummy .plt entry)
                return addrPlt + plt_offset;
            }
        }

        if (--curr < 0) {
            curr = numRelPlt - 1;
        }
    } while (curr != first); // Will eventually wrap around to first if not present

    return Address::ZERO; // Exit if this happens
}


void ElfBinaryLoader::processSymbol(Translated_ElfSym &sym, int e_type, int i,
                                    const QString &currentFile)
{
    bool imported              = sym.SectionIdx == SHT_NULL;
    bool local                 = sym.Binding == STB_LOCAL || sym.Binding == STB_WEAK;
    const BinarySection *siPlt = m_binaryImage->getSectionByName(".plt");

    if (sym.Value.isZero() && siPlt) { // && i < max_i_for_hack) {
        // Special hack for gcc circa 3.3.3: (e.g. test/pentium/settest).  The value in the dynamic
        // symbol table is zero!  I was assuming that index i in the dynamic symbol table would
        // always correspond to index i in the .plt section, but for fedora2_true, this doesn't
        // work. So we have to look in the .rel[a].plt section. Thanks, gcc!  Note that this hack
        // can cause strange symbol names to appear
        sym.Value = findRelPltOffset(i);
    }
    else if (e_type == ET_REL) {
        if (sym.SectionIdx < m_elfSections.size()) {
            sym.Value += m_elfSections[sym.SectionIdx].SourceAddr;
        }
    }

    // try to find given symbol, if it has Value of 0, try to use the name.
    const BinarySymbol *symbol = sym.Value.isZero() ? m_symbols->findSymbolByName(sym.Name)
                                                    : m_symbols->findSymbolByAddress(sym.Value);

    // Ensure no overwriting (except functions)
    if (symbol != nullptr) { // TODO: if symbol already exists
        return;
    }
    else if (sym.Binding == STB_WEAK && sym.Type == STT_NOTYPE) {
        return;
    }
    else if (sym.Type == STT_FILE) {
        return;
    }
    else if (sym.Name.isEmpty()) {
        return;
    }

    if (sym.Value.isZero()) {
        LOG_WARN("Skipping symbol %1 with unknown location", sym.Name);
        return;
    }

    // TODO: add more symbol information here (function/export etc. ) ?
    BinarySymbol *new_symbol(m_symbols->createSymbol(sym.Value, sym.Name, local));
    new_symbol->setSize(elfRead4(&m_symbolSection[i].st_size));

    if (imported) {
        new_symbol->setAttribute("Imported", true);
    }

    if (sym.Type == STT_FUNC) {
        new_symbol->setAttribute("Function", true);
    }

    if (!currentFile.isEmpty()) {
        new_symbol->setAttribute("SourceFile", currentFile);
    }
}


void ElfBinaryLoader::addSymbolsForSection(int secIndex)
{
    if (!Util::inRange(secIndex, 0, (int)m_elfSections.size())) {
        LOG_WARN("Cannot add symbols for section with invalid index %1", secIndex);
        return;
    }

    const SectionParam &section = m_elfSections[secIndex];

    if (!Util::inRange(section.entry_size, sizeof(Elf32_Sym), m_loadedImageSize)) {
        LOG_WARN("Cannot add symbols for section %1: Invalid section entry size %1",
                 section.entry_size);
        return;
    }
    else if (section.Size == 0 || (section.Size % section.entry_size) != 0) {
        LOG_WARN("Cannot add symbols for section %1: Invalid section size %2 "
                 "(zero or not divisible by %3)",
                 secIndex, section.Size, section.entry_size);
        return;
    }

    m_symbolSection = reinterpret_cast<const Elf32_Sym *>(
        section.imagePtr.value()); // Pointer to symbols
    if (!m_symbolSection) {
        return;
    }

    const SWord symbolType     = elfRead2(&m_elfHeader->e_type);
    const uint32 strSectionIdx = m_shLink[secIndex]; // sh_link points to the string table
    if (!Util::inRange(strSectionIdx, 0UL, m_elfSections.size())) {
        return; // cannot read symbol name from invalid string section
    }

    const int numSymbols = section.Size / section.entry_size;
    QString fileName;

    // Index 0 is a dummy entry
    for (int i = 1; i < numSymbols; i++) {
        Translated_ElfSym translatedSym;
        Address val = Address(elfRead4(&m_symbolSection[i].st_value));
        int nameIdx = elfRead4(&m_symbolSection[i].st_name);

        if (nameIdx == 0) { /* Silly symbols with no names */
            continue;
        }

        const QString symbolName = getStrPtr(strSectionIdx, nameIdx);
        // Hack off the "@@GLIBC_2.0" of Linux, if present
        translatedSym.Name       = symbolName.left(symbolName.indexOf("@@"));
        translatedSym.Type       = ELF32_ST_TYPE(m_symbolSection[i].st_info);
        translatedSym.Binding    = ELF32_ST_BIND(m_symbolSection[i].st_info);
        translatedSym.Visibility = ELF32_ST_VISIBILITY(m_symbolSection[i].st_other);
        translatedSym.SymbolSize = ELF32_ST_VISIBILITY(m_symbolSection[i].st_size);
        translatedSym.SectionIdx = elfRead2(&m_symbolSection[i].st_shndx);
        translatedSym.Value      = val;

        if (translatedSym.Type == STT_FILE) {
            fileName = translatedSym.Name;
        }

        if (translatedSym.Binding != STB_LOCAL && !fileName.isEmpty()) {
            // first non-local symbol, clear the current_file
            fileName.clear();
        }

        processSymbol(translatedSym, symbolType, i, fileName);
    }

    const Address addressOfMain = getMainEntryPoint();

    if ((addressOfMain != Address::INVALID) &&
        (nullptr == m_symbols->findSymbolByAddress(addressOfMain))) {
        // Ugh - main mustn't have the STT_FUNC attribute. Add it
        m_symbols->createSymbol(addressOfMain, "main");
    }
}


void ElfBinaryLoader::addRelocsAsSyms(uint32_t relSecIdx)
{
    if (relSecIdx >= m_elfSections.size()) {
        // invalid section index
        return;
    }

    const SectionParam &section = m_elfSections[relSecIdx];

    m_relocSection = reinterpret_cast<const Elf32_Rel *>(
        section.imagePtr.value()); // Pointer to symbols
    const int symSecIdx = m_shLink[relSecIdx];
    const int strSecIdx = m_shLink[symSecIdx];
    const int numRelocs = section.Size / section.entry_size;

    // Index 0 is a dummy entry
    for (int i = 1; i < numRelocs; i++) {
        const Address val  = Address(elfRead4(&m_relocSection[i].r_offset));
        const int symIndex = elfRead4(&m_relocSection[i].r_info) >> 8;
        const int flags    = elfRead4(&m_relocSection[i].r_info) & 0xFF;

        if (flags == R_386_32) {
            // Lookup the value of the symbol table entry
            Address symbolAddr = Address(elfRead4(&m_symbolSection[symIndex].st_value));

            if (m_symbolSection[symIndex].st_info & STT_SECTION) {
                symbolAddr = m_elfSections[elfRead2(&m_symbolSection[symIndex].st_shndx)]
                                 .SourceAddr;
            }

            // Overwrite the relocation value... ?
            m_binaryImage->writeNative4(val, symbolAddr.value());
            continue;
        }

        if ((flags & R_386_PC32) == 0) {
            continue;
        }

        if (symIndex == 0) { /* Silly symbols with no names */
            continue;
        }

        QString symbolName = getStrPtr(strSecIdx, elfRead4(&m_symbolSection[symIndex].st_name));

        // Hack off the "@@GLIBC_2.0" of Linux, if present
        symbolName = symbolName.left(symbolName.indexOf("@@"));

        const BinarySymbol *symbol = m_symbols->findSymbolByName(symbolName);
        // Add new extern
        Address location = symbol ? symbol->getLocation() : m_nextExtern;

        if (nullptr == symbol) {
            m_symbols->createSymbol(m_nextExtern, symbolName);
            m_nextExtern += 4;
        }

        m_binaryImage->writeNative4(val, (location - val - 4).value());
    }
}


Address ElfBinaryLoader::getMainEntryPoint()
{
    auto sym = m_symbols->findSymbolByName("main");

    if (sym) {
        return sym->getLocation();
    }

    return Address::INVALID;
}


Address ElfBinaryLoader::getEntryPoint()
{
    return Address(elfRead4(&m_elfHeader->e_entry));
}


HostAddress ElfBinaryLoader::nativeToHostAddress(Address addr)
{
    if (m_binaryImage->getNumSections() == 0) {
        return HostAddress::ZERO;
    }

    return m_binaryImage->getSectionByIndex(1)->getHostAddr() -
           m_binaryImage->getSectionByIndex(1)->getSourceAddr() + addr;
}


void ElfBinaryLoader::close()
{
    unload();
}


LoadFmt ElfBinaryLoader::getFormat() const
{
    return LoadFmt::ELF;
}


Machine ElfBinaryLoader::getMachine() const
{
    const SWord elfMachine = elfRead2(&m_elfHeader->e_machine);

    if ((elfMachine == EM_SPARC) || (elfMachine == EM_SPARC32PLUS)) {
        return Machine::SPARC;
    }
    else if (elfMachine == EM_386) {
        return Machine::PENTIUM;
    }
    else if (elfMachine == EM_PA_RISC) {
        return Machine::HPRISC;
    }
    else if (elfMachine == EM_68K) {
        return Machine::PALM; // Unlikely
    }
    else if (elfMachine == EM_PPC) {
        return Machine::PPC;
    }
    else if (elfMachine == EM_ST20) {
        return Machine::ST20;
    }
    else if (elfMachine == EM_MIPS) {
        return Machine::MIPS;
    }
    else if (elfMachine == EM_AMD64) {
        LOG_ERROR("The AMD x86-64 architecture is not supported yet.");
        return Machine::INVALID;
    }

    // An unknown machine type
    LOG_ERROR("Unsupported machine type %1", elfMachine);
    return Machine::INVALID;
}


bool ElfBinaryLoader::isLibrary() const
{
    const SWord type = elfRead2(&reinterpret_cast<Elf32_Ehdr *>(m_loadedImage)->e_type);
    return type == ET_DYN;
}


QStringList ElfBinaryLoader::getDependencyList()
{
    QStringList result;
    Address stringtab      = Address::INVALID;
    BinarySection *dynsect = m_binaryImage->getSectionByName(".dynamic");

    if (dynsect == nullptr) {
        return result; /* no dynamic section = statically linked */
    }

    for (Elf32_Dyn *dyn = reinterpret_cast<Elf32_Dyn *>(dynsect->getHostAddr().value());
         dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag == DT_STRTAB) {
            stringtab = Address(dyn->d_un.d_ptr);
            break;
        }
    }

    if (stringtab == Address::INVALID) { /* No string table = no names */
        return result;
    }

    HostAddress strTab = nativeToHostAddress(stringtab);

    for (Elf32_Dyn *dyn = reinterpret_cast<Elf32_Dyn *>(dynsect->getHostAddr().value());
         dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag == DT_NEEDED) {
            const char *need = reinterpret_cast<const char *>((strTab + dyn->d_un.d_val).value());

            if (need != nullptr) {
                result << need;
            }
        }
    }

    return result;
}


void ElfBinaryLoader::markImports()
{
    auto first = m_symbols->begin();
    auto end   = m_symbols->end();

    for (; first != end; ++first) {
        if ((*first)->getLocation() >= m_pltMin) {
            break;
        }
    }

    for (auto last = first; last != end; ++last) {
        if ((*last)->getLocation() >= m_pltMax) {
            break;
        }

        (*last)->setAttribute("Imported", true);
    }
}


SWord ElfBinaryLoader::elfRead2(const SWord *ps) const
{
    assert(ps);
    return Util::readWord(ps, m_endian);
}


DWord ElfBinaryLoader::elfRead4(const DWord *pi) const
{
    assert(pi);
    return Util::readDWord(pi, m_endian);
}


void ElfBinaryLoader::elfWrite4(DWord *pi, DWord val)
{
    assert(pi);
    Util::writeDWord(pi, val, m_endian);
}


void ElfBinaryLoader::applyRelocations()
{
    int nextFakeLibAddr = -2; // See R_386_PC32 below; -1 sometimes used for main

    if (m_loadedImage == nullptr) {
        return; // No file loaded
    }

    const Elf32_Half machine = elfRead2(&m_elfHeader->e_machine);
    const Elf32_Half e_type  = elfRead2(&m_elfHeader->e_type);

    for (size_t i = 1; i < m_elfSections.size(); ++i) {
        const SectionParam &ps(m_elfSections[i]);
        if (ps.sectionType == SHT_RELA) {
            const Elf32_Rela *relaEntries = reinterpret_cast<const Elf32_Rela *>(
                ps.imagePtr.value());
            const DWord numEntries = ps.Size / sizeof(Elf32_Rela);

            if (relaEntries == nullptr) {
                LOG_WARN("Cannot read relocation entries from invalid section %1", i);
                continue;
            }
            else if (ps.Size % sizeof(Elf32_Rela) != 0) {
                LOG_WARN("Cannot read relocation entries from section %1 with invalid size %2 "
                         "(must be divisible by %3)",
                         i, ps.Size, sizeof(Elf32_Rela));
                continue;
            }

            switch (machine) {
            case EM_SPARC:
                // NOTE: the r_offset is different for .o files (E_REL in the e_type header field)
                // than for exe's and shared objects!
                for (DWord u = 0; u < numEntries; u++) {
                    Elf32_Byte relType = ELF32_R_TYPE(elfRead4(&relaEntries[u].r_info));
                    // Elf32_Word symTabIndex = ELF32_R_SYM(elfRead4(&relaEntries[u].r_info));

                    switch (relType) {
                    case R_SPARC_NONE: // just ignore (common)
                        break;

                    // TODO These relocation types need to be implemented.
                    case R_SPARC_HI22:
                    case R_SPARC_LO10:
                    case R_SPARC_COPY:
                    case R_SPARC_GLOB_DAT:
                    case R_SPARC_JMP_SLOT:
                    default: LOG_WARN("Unhandled SPARC relocation type %1", relType); break;
                    }
                }
                break;

            default: LOG_WARN("Unhandled relocation!"); break;
            }
        }
        else if (ps.sectionType == SHT_REL) {
            // A section such as .rel.dyn or .rel.plt (without an addend field).
            // Each entry has 2 words: r_offet and r_info. The r_offset is just the offset from
            // the beginning of the section (section given by the section header's sh_info) to
            // the word to be modified. r_info has the type in the bottom byte, and a symbol
            // table index in the top 3 bytes. A symbol table offset of 0 (STN_UNDEF) means use
            // value 0. The symbol table involved comes from the section header's sh_link field.

            // NOTE: the r_offset is different for .o files (E_REL in the e_type header field)
            // than for exe's and shared objects!
            Address destNatOrigin      = Address::ZERO;
            HostAddress destHostOrigin = HostAddress::ZERO;

            if (e_type == ET_REL) {
                Elf32_Word destSection = m_shInfo[i];
                if (!Util::inRange(destSection, 0UL, m_elfSections.size())) {
                    continue;
                }
                destNatOrigin  = m_elfSections[destSection].SourceAddr;
                destHostOrigin = m_elfSections[destSection].imagePtr;
            }

            // Section index for the associated symbol table
            const uint32 symSectionIdx = m_shLink[i];
            if (!Util::inRange(symSectionIdx, 0UL, m_elfSections.size())) {
                continue;
            }

            // Section index for the string section assoc with this
            const uint32 strSectionIdx = m_shLink[symSectionIdx];
            if (!Util::inRange(strSectionIdx, 0UL, m_elfSections.size())) {
                continue;
            }

            const char *strSection = reinterpret_cast<const char *>(
                m_elfSections[strSectionIdx].imagePtr.value());
            const Elf32_Sym *assocSymbols = symSectionIdx != 0
                                                ? reinterpret_cast<const Elf32_Sym *>(
                                                      m_elfSections[symSectionIdx].imagePtr.value())
                                                : nullptr;

            const Elf32_Rel *relEntries = reinterpret_cast<const Elf32_Rel *>(ps.imagePtr.value());
            const DWord numEntries      = ps.Size / sizeof(Elf32_Rel);
            if (ps.Size % sizeof(Elf32_Rel) != 0) {
                LOG_WARN("Invalid size %1 of relocation section %2 (must be divisible by %3)",
                         ps.Size, i, sizeof(Elf32_Rel));
                continue;
            }

            for (unsigned u = 0; u < numEntries; u++) {
                const Elf32_Addr r_offset  = elfRead4(&relEntries[u].r_offset);
                const Elf32_Byte relType   = ELF32_R_TYPE(elfRead4(&relEntries[u].r_info));
                const Elf32_Word symbolIdx = ELF32_R_SYM(elfRead4(&relEntries[u].r_info));

                DWord *relocDestination; // Pointer to the word to be relocated

                if (e_type == ET_REL) {
                    if (!Util::inRange(r_offset, 0UL, m_loadedImageSize)) {
                        LOG_WARN("Not loading symbol number %1 due to invalid offset %2", u,
                                 r_offset);
                        continue;
                    }
                    relocDestination = reinterpret_cast<DWord *>(
                        (destHostOrigin + r_offset).value());
                }
                else {
                    const BinarySection *destSec = m_binaryImage->getSectionByAddr(
                        Address(r_offset));
                    if (!destSec) {
                        LOG_WARN("Not loading symbol number %1 due to invalid offset %2", u,
                                 r_offset);
                        continue;
                    }
                    relocDestination = reinterpret_cast<DWord *>(
                        (destSec->getHostAddr() - destSec->getSourceAddr() + r_offset).value());
                    destNatOrigin = Address::ZERO;
                }

                Address A = Address(elfRead4(relocDestination));
                Address P = destNatOrigin + r_offset;
                Address S = assocSymbols != nullptr
                                ? Address(elfRead4(&assocSymbols[symbolIdx].st_value))
                                : Address::ZERO;

                if (e_type == ET_REL && assocSymbols != nullptr) {
                    const Elf32_Half sectionIdx = elfRead2(&assocSymbols[symbolIdx].st_shndx);

                    if (sectionIdx < m_elfSections.size()) {
                        S += m_elfSections[sectionIdx].SourceAddr;
                    }
                }

                switch (machine) {
                case EM_386:
                    switch (relType) {
                    case R_386_NONE: break;

                    case R_386_32: // S + A
                        elfWrite4(relocDestination, (S + A).value());
                        break;

                    case R_386_PC32: // S + A - P
                        if (ELF32_ST_TYPE(assocSymbols[symbolIdx].st_info) == STT_SECTION) {
                            S                           = Address::ZERO;
                            const Elf32_Half sectionIdx = elfRead2(
                                &assocSymbols[symbolIdx].st_shndx);

                            if (sectionIdx < m_elfSections.size()) {
                                S += m_elfSections[sectionIdx].SourceAddr;
                            }
                        }
                        else {
                            S = Address(elfRead4(&assocSymbols[symbolIdx].st_value));

                            if (S.isZero()) {
                                // This means that the symbol doesn't exist in this module, and is
                                // not accessed through the PLT, i.e. it will be statically linked,
                                // e.g. strcmp. We have the name of the symbol right here in the
                                // symbol table entry, but the only way to communicate with the
                                // loader is through the target address of the call. So we use some
                                // very improbable addresses (e.g. -1, -2, etc) and give them
                                // entries in the symbol table
                                DWord nameOffset       = elfRead4(reinterpret_cast<const DWord *>(
                                    &assocSymbols[symbolIdx].st_name));
                                const char *symbolName = strSection + nameOffset;

                                // Allocate a new fake address
                                S = Address((nextFakeLibAddr--) & Address::getSourceMask());
                                BinarySymbol *newFunction = m_symbols->createSymbol(S, symbolName);
                                newFunction->setAttribute("Function", true);
                                newFunction->setAttribute("Imported", true);
                            }
                            else if (e_type == ET_REL) {
                                const SWord sectionIdx = elfRead2(
                                    &assocSymbols[symbolIdx].st_shndx);

                                if (sectionIdx < m_elfSections.size()) {
                                    S += m_elfSections[sectionIdx].SourceAddr;
                                }
                            }
                        }

                        elfWrite4(relocDestination, (S + A - P).value());
                        break;

                    case R_386_GLOB_DAT:
                    case R_386_JMP_SLOT:
                    case R_386_RELATIVE:
                        // No need to do anything with these, if a shared object
                        break;

                    default:
                        LOG_WARN("Unhandled x86 relocation type %1", static_cast<int>(relType));
                    }
                    break;

                default: LOG_WARN("Unhandled relocation!"); break;
                }
            }
        }
    }
}


bool ElfBinaryLoader::isRelocationAt(Address addr)
{
    if (m_loadedImage == nullptr) {
        return false; // No file loaded
    }

    const Elf32_Half machine = elfRead2(&m_elfHeader->e_machine);
    const Elf32_Half e_type  = elfRead2(&m_elfHeader->e_type); ///< relocation type

    switch (machine) {
    case EM_386:

        for (size_t i = 1; i < m_elfSections.size(); ++i) {
            const SectionParam &ps(m_elfSections[i]);

            if (ps.sectionType == SHT_REL) {
                // A section such as .rel.dyn or .rel.plt (without an addend field).
                // Each entry has 2 words: r_offet and r_info. The r_offset is just the offset from
                // the beginning of the section (section given by the section header's sh_info) to
                // the word to be modified. r_info has the type in the bottom byte, and a symbol
                // table index in the top 3 bytes. A symbol table offset of 0 (STN_UNDEF) means use
                // value 0. The symbol table involved comes from the section header's sh_link field.
                const Elf32_Rel *relocEntry = reinterpret_cast<Elf32_Rel *>(ps.imagePtr.value());
                const DWord size            = ps.Size / sizeof(Elf32_Rel);

                // NOTE: the r_offset is different for .o files (E_REL in the e_type header field)
                // than for exe's and shared objects!
                Address destNatOrigin = Address::ZERO;

                if (e_type == ET_REL) {
                    int destSection = m_shInfo[i];
                    destNatOrigin   = m_elfSections[destSection].SourceAddr;
                }

                for (DWord u = 0; u < size; u++) {
                    Elf32_Addr r_offset = elfRead4(&relocEntry[u].r_offset);

                    Address relocDestination; // Pointer to the word to be relocated

                    if (e_type == ET_REL) {
                        relocDestination = destNatOrigin + r_offset;
                    }
                    else {
                        const BinarySection *destSec = m_binaryImage->getSectionByAddr(
                            Address(r_offset));
                        if (destSec) {
                            relocDestination = destSec->getSourceAddr() + r_offset;
                            destNatOrigin    = Address::ZERO;
                        }
                    }

                    if (addr == relocDestination) {
                        return true;
                    }
                }
            }
        }

        break;

    default: LOG_WARN("Unhandled relocation!"); break; // Not implemented yet
    }

    return false;
}


int ElfBinaryLoader::canLoad(QIODevice &fl) const
{
    const QByteArray contents = fl.read(sizeof(Elf32_Ehdr));
    if (contents.size() < static_cast<int>(sizeof(Elf32_Ehdr))) {
        return 0;
    }

    const Elf32_Ehdr *header = reinterpret_cast<const Elf32_Ehdr *>(contents.constData());

    if (Util::testMagic(header->e_ident, { ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3 }) == false) {
        return 0;
    }

    // for now, we can only load 32 bit ELF files
    switch (header->e_ident[EI_CLASS]) {
    case ELFCLASS32: return 5;
    case ELFCLASS64: return 0;
    default: return 0;
    }
}


BOOMERANG_LOADER_PLUGIN(ElfBinaryLoader, "ELF32 loader plugin", BOOMERANG_VERSION,
                        "Boomerang developers")
