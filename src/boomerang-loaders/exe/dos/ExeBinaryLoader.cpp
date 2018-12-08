#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "ExeBinaryLoader.h"

#include "boomerang/db/binary/BinaryImage.h"
#include "boomerang/db/binary/BinarySection.h"
#include "boomerang/ifc/IFileLoader.h"
#include "boomerang/util/Util.h"
#include "boomerang/util/log/Log.h"

#include <QBuffer>
#include <QFile>

#include <cassert>


ExeBinaryLoader::ExeBinaryLoader()
{
}


void ExeBinaryLoader::initialize(BinaryImage *image, BinarySymbolTable *symbols)
{
    m_image   = image;
    m_symbols = symbols;
}


bool ExeBinaryLoader::loadFromMemory(QByteArray &data)
{
    if (m_header) {
        delete m_header;
    }

    // Always just 3 sections
    m_header = new ExeHeader;

    QBuffer fp(&data);
    if (!fp.open(QBuffer::ReadOnly)) {
        LOG_ERROR("Cannot read exe file");
        return false;
    }

    /* Read in first 2 bytes to check EXE signature */
    if (fp.read(reinterpret_cast<char *>(m_header), 2) != 2) {
        LOG_ERROR("Cannot read exe file");
        return false;
    }

    int fCOM;
    int cb;
    // Check for the "MZ" exe header
    if (!(fCOM = ((m_header->sigLo != 0x4D) || (m_header->sigHi != 0x5A)))) {
        /* Read rest of m_header */
        fp.seek(0);

        if (fp.read(reinterpret_cast<char *>(m_header), sizeof(ExeHeader)) != sizeof(ExeHeader)) {
            LOG_ERROR("Cannot read Exe file");
            return false;
        }


        /* Calculate the load module size.
         * This is the number of pages in the file
         * less the length of the m_header and reloc table
         * less the number of bytes unused on last page
         */
        const SWord numPages      = Util::readWord(&m_header->numPages, Endian::Little);
        const SWord numParaHeader = Util::readWord(&m_header->numParaHeader, Endian::Little);
        cb                        = numPages * 512U - numParaHeader * 16U;

        if (m_header->lastPageSize > 0) {
            cb -= 512U - Util::readWord(&m_header->lastPageSize, Endian::Little);
        }

        /* We quietly ignore minAlloc and maxAlloc since for our
         * purposes it doesn't really matter where in real memory
         * the m_am would end up.  EXE m_ams can't really rely on
         * their load location so setting the PSP segment to 0 is fine.
         * Certainly m_ams that prod around in DOS or BIOS are going
         * to have to load DS from a constant so it'll be pretty
         * obvious.
         */
        m_numReloc         = Util::readWord(&m_header->numReloc, Endian::Little);
        const SWord offset = Util::readWord(&m_header->relocTabOffset, Endian::Little);

        if (m_numReloc < 0) {
            m_numReloc = 0;
        }

        if (!Util::inRange(offset, 0, data.size()) ||
            !Util::inRange(offset + m_numReloc * (int)sizeof(DWord), 0, data.size())) {
            LOG_ERROR("Cannot load Exe file: Relocation table extends past file boundary");
            return false;
        }

        m_relocTable.resize(m_numReloc);

        fp.seek(offset);
        /* Read in seg:offset pairs and convert to Image ptrs */
        Byte buf[4];
        for (int i = 0; i < m_numReloc; i++) {
            if (4 != fp.read(reinterpret_cast<char *>(buf), 4)) {
                LOG_ERROR("Cannot load Exe file: Cannot read relocation table");
                return false;
            }
            m_relocTable[i] = Util::readDWord(buf, Endian::Little);
        }

        /* Seek to start of image */
        const SWord initialPtrOffset = Util::readWord(&m_header->numParaHeader, Endian::Little);
        if (((initialPtrOffset & 0xF000) != 0) ||
            !Util::inRange(initialPtrOffset * 16, 0, data.size())) {
            LOG_ERROR("Cannot read Exe file: Invalid offset for initial SP/IP values");
            return false;
        }

        fp.seek(initialPtrOffset * 16U);

        // Initial PC and SP. Note that we fake the seg:offset by putting
        // the segment in the top half, and offset int he bottom
        const DWord initCS = Util::readWord(&m_header->initCS, Endian::Little);
        const DWord initIP = Util::readWord(&m_header->initIP, Endian::Little);
        const DWord initSS = Util::readWord(&m_header->initSS, Endian::Little);
        const DWord initSP = Util::readWord(&m_header->initSP, Endian::Little);

        m_uInitPC = Address((initCS << 16) + initIP);
        m_uInitSP = Address((initSS << 16) + initSP);
    }
    else {
        /* COM file
         * In this case the load module size is just the file length
         */
        cb = fp.size();

        /* COM programs start off with an ORG 100H (to leave room for a PSP)
         * This is also the implied start address so if we load the image
         * at offset 100H addresses should all line up properly again.
         */
        m_uInitPC  = Address(0x0100);
        m_uInitSP  = Address(0xFFFE);
        m_numReloc = 0;

        fp.seek(0);
    }

    if (!Util::inRange(cb, 0, data.size())) {
        LOG_ERROR("Cannot read Exe file: Invalid image size.");
        return false;
    }

    /* Allocate a block of memory for the image. */
    m_imageSize   = cb;
    m_loadedImage = new uint8_t[m_imageSize];

    if (cb != fp.read(reinterpret_cast<char *>(m_loadedImage), cb)) {
        LOG_ERROR("Cannot read Exe file: Failed to read loaded image");
        return false;
    }

    /* Relocate segment constants */
    for (int i = 0; i < m_numReloc; i++) {
        const DWord fileOffset = m_relocTable[i];
        if (!Util::inRange(fileOffset, sizeof(ExeHeader), (DWord)data.size())) {
            LOG_WARN("Cannot read Exe relocation entry %1: Offset %2 is not valid", i, fileOffset);
            continue;
        }

        Byte *p           = &m_loadedImage[fileOffset];
        const SWord value = Util::readWord(p, Endian::Little);
        Util::writeWord(p, value, Endian::Little);
    }

    fp.close();

    // TODO: prevent overlapping of those 3 sections
    BinarySection *header = m_image->createSection("$HEADER", Address(0x4000),
                                                   Address(0x4000) + sizeof(ExeHeader));
    header->setHostAddr(HostAddress(m_header));
    header->setEntrySize(1);

    // The text and data section
    BinarySection *text = m_image->createSection(".text", Address(0x10000),
                                                 Address(0x10000) + sizeof(m_imageSize));
    text->setCode(true);
    text->setData(true);
    text->setHostAddr(HostAddress(m_loadedImage));
    text->setEntrySize(1);

    BinarySection *reloc = m_image->createSection("$RELOC", Address(0x4000) + sizeof(ExeHeader),
                                                  Address(0x4000) + sizeof(ExeHeader) +
                                                      sizeof(DWord) * m_numReloc);
    // as of C++11, std::vector is guaranteed to be contiguous (except for std::vector<bool>),
    // so we can read the relocated values directly from m_relocTable
    if (!m_relocTable.empty()) {
        reloc->setHostAddr(HostAddress(&m_relocTable[0]));
    }
    reloc->setEntrySize(sizeof(DWord));
    return true;
}


// Clean up and unload the binary image
void ExeBinaryLoader::unload()
{
    delete m_header;
    delete[] m_loadedImage;
}


LoadFmt ExeBinaryLoader::getFormat() const
{
    return LoadFmt::EXE;
}


Machine ExeBinaryLoader::getMachine() const
{
    return Machine::PENTIUM;
}


void ExeBinaryLoader::close()
{
    // Not implemented yet
}


// Should be doing a search for this
Address ExeBinaryLoader::getMainEntryPoint()
{
    return Address::INVALID;
}


Address ExeBinaryLoader::getEntryPoint()
{
    const DWord initCS = Util::readWord(&m_header->initCS, Endian::Little);
    const DWord initIP = Util::readWord(&m_header->initIP, Endian::Little);

    // FIXME Check this...
    return Address((initCS << 4) + initIP);
}


int ExeBinaryLoader::canLoad(QIODevice &fl) const
{
    Byte buf[4];
    fl.read(reinterpret_cast<char *>(buf), sizeof(buf));

    if (!Util::testMagic(buf, { 'M', 'Z' })) {
        /* No MZ header */
        return 0;
    }

    /* Check if this is a  Windows NE executable
     * It will have a DOS MZ header, but also another header later on
     * Offset of the NE header is at 0x3C
     * The NE header will start with the signature 'NE'
     * Ref: https://www.fileformat.info/format/exe/corion-mz.htm
     */
    if (fl.seek(0x3C)) {
        if (fl.read(reinterpret_cast<char *>(buf), 2) == 2) {
            const SWord possibleWinHeaderOffset = Util::readWord(buf, Endian::Little);
            if (fl.seek(possibleWinHeaderOffset)) {
                if (fl.read(reinterpret_cast<char *>(buf), 2) == 2) {
                    if (Util::testMagic(buf, { 'N', 'E' })) {
                        /* This is a Windows NE Executable */
                        return 0;
                    }
                }
            }
        }
    }

    /* DOS-based file */
    return 2;
}


BOOMERANG_LOADER_PLUGIN(ExeBinaryLoader, "DOS Exe loader plugin", BOOMERANG_VERSION,
                        "Boomerang developers")
