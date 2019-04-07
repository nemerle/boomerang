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

#include "boomerang/core/plugin/Plugin.h"
#include "boomerang/db/binary/BinaryImage.h"
#include "boomerang/db/binary/BinarySection.h"
#include "boomerang/ifc/IFileLoader.h"
#include "boomerang/util/Util.h"
#include "boomerang/util/log/Log.h"

#include <QBuffer>
#include <QFile>

#include <cassert>


#define DOS_PAGE_SIZE (512)
#define DOS_PARA_SIZE (16)


ExeBinaryLoader::ExeBinaryLoader(Project *project)
    : IFileLoader(project)
{
}


void ExeBinaryLoader::initialize(BinaryFile *file, BinarySymbolTable *symbols)
{
    m_image   = file->getImage();
    m_symbols = symbols;

    // We can only load MZ executables, which are always 16 bit exetuables.
    file->setBitness(16);
}


bool ExeBinaryLoader::loadFromMemory(QByteArray &data)
{
    if (data.length() < (int)sizeof(ExeHeader)) {
        LOG_ERROR("Cannot read exe file!");
        return false;
    }

    const Address loadBaseAddr = Address::ZERO;

    if (m_header) {
        delete m_header;
    }

    // Always just 3 sections
    m_header = new ExeHeader;

    QBuffer fp(&data);
    if (!fp.open(QBuffer::ReadOnly)) {
        LOG_ERROR("Cannot read Exe file");
        return false;
    }

    // Read rest of m_header
    if (fp.read(reinterpret_cast<char *>(m_header), sizeof(ExeHeader)) != sizeof(ExeHeader)) {
        LOG_ERROR("Cannot read Exe file");
        return false;
    }

    // Calculate the load module size.
    // This is:
    //  - the number of pages in the file
    //  - less the length of the m_header and reloc table
    //  - less the number of bytes unused on last page
    const SWord numPages      = Util::readWord(&m_header->numPages, Endian::Little);
    const SWord numParaHeader = Util::readWord(&m_header->numParaHeader, Endian::Little);
    const SWord lastPageSize  = Util::readWord(&m_header->lastPageSize, Endian::Little);
    const SWord headerSize    = numParaHeader * DOS_PARA_SIZE;
    int cbImageSize           = numPages * DOS_PAGE_SIZE - headerSize;

    if (lastPageSize > 0) {
        // if lastPageSize is 0, this indicates that the last page is full; if lastPageSize
        // is not 0, the last page is not full so we have to subtract the empty space
        // past the image boundary.
        cbImageSize -= DOS_PAGE_SIZE - lastPageSize;
    }

    // Seek to start of image
    const SWord initialPtrOffset = Util::readWord(&m_header->numParaHeader, Endian::Little);
    if (((initialPtrOffset & 0xF000) != 0) ||
        !Util::inRange(initialPtrOffset * DOS_PARA_SIZE, 0, data.size())) {
        LOG_ERROR("Cannot read Exe file: Invalid offset for initial SP/IP values");
        return false;
    }

    if (!fp.seek(initialPtrOffset * DOS_PARA_SIZE)) {
        LOG_ERROR("Cannot load Exe file: Cannot seek to offset %1",
                  initialPtrOffset * DOS_PARA_SIZE);
        return false;
    }

    // Initial PC and SP. Note that we fake the seg:offset by putting
    // the segment in the top half, and offset in the bottom half.
    const DWord initCS = Util::readWord(&m_header->initCS, Endian::Little);
    const DWord initIP = Util::readWord(&m_header->initIP, Endian::Little);
    const DWord initSS = Util::readWord(&m_header->initSS, Endian::Little);
    const DWord initSP = Util::readWord(&m_header->initSP, Endian::Little);

    m_uInitPC = Address((initCS * DOS_PARA_SIZE) + initIP);
    m_uInitSP = Address((initSS * DOS_PARA_SIZE) + initSP);

    if (!Util::inRange(cbImageSize, 0, data.size())) {
        LOG_ERROR("Cannot read Exe file: Invalid image size.");
        return false;
    }

    /* Allocate a block of memory for the image. */
    m_imageSize   = cbImageSize;
    m_loadedImage = new Byte[m_imageSize];

    if (cbImageSize != fp.read(reinterpret_cast<char *>(m_loadedImage), cbImageSize)) {
        LOG_ERROR("Cannot read Exe file: Failed to read loaded image");
        return false;
    }

    // Use the following section layout:
    // baseAddr..baseAddr+m_imageSize:                                .text
    //

    BinarySection *header = m_image->createSection("$HEADER", loadBaseAddr + m_imageSize,
                                                   loadBaseAddr + m_imageSize + sizeof(ExeHeader));
    header->setHostAddr(HostAddress(m_header));
    header->setEntrySize(1);

    // The text and data section
    BinarySection *text = m_image->createSection(".text", loadBaseAddr, loadBaseAddr + m_imageSize);
    text->setCode(true);
    text->setData(true);
    text->setHostAddr(HostAddress(m_loadedImage));

    // create relocations and relocation section
    applyRelocations(fp, data, loadBaseAddr);

    fp.close();
    return true;
}


bool ExeBinaryLoader::applyRelocations(QBuffer &fp, QByteArray &data, Address loadBaseAddr)
{
    /* We quietly ignore minAlloc and maxAlloc since for our
     * purposes it doesn't really matter where in real memory
     * the m_am would end up.  EXE m_ams can't really rely on
     * their load location so setting the PSP segment to 0 is fine.
     * Certainly m_ams that prod around in DOS or BIOS are going
     * to have to load DS from a constant so it'll be pretty
     * obvious.
     */
    const int numReloc = std::max(0, (int)Util::readWord(&m_header->numReloc, Endian::Little));
    const SWord offset = Util::readWord(&m_header->relocTabOffset, Endian::Little);

    if (!Util::inRange(offset, 0, data.size()) ||
        !Util::inRange(offset + numReloc * (int)sizeof(DWord), 0, data.size())) {
        LOG_ERROR("Cannot load Exe file: Relocation table extends past file boundary");
        return false;
    }

    if (!fp.seek(offset)) {
        LOG_ERROR("Cannot load Exe file: Cannot seek to offset %1", offset);
        return false;
    }

    /* Relocate segment constants */
    m_relocations.resize(numReloc);

    for (int i = 0; i < numReloc; i++) {
        ExeReloc relocEntry;
        if (sizeof(ExeReloc) != fp.read(reinterpret_cast<char *>(&relocEntry), sizeof(ExeReloc))) {
            LOG_ERROR("Cannot load Exe file: Cannot read relocation table");
            return false;
        }

        relocEntry.offset  = Util::normEndian(relocEntry.offset, Endian::Little);
        relocEntry.segment = Util::normEndian(relocEntry.segment, Endian::Little);

        const SWord imageOffset = (relocEntry.segment << 4) + relocEntry.offset;
        if (!Util::inRange(imageOffset, 0, m_imageSize)) {
            LOG_WARN("Cannot read Exe relocation entry %1: Offset %2 is not valid", i,
                     relocEntry.offset);
            continue;
        }

        Byte *p                = &m_loadedImage[imageOffset];
        const SWord relocValue = Util::readWord(p, Endian::Little);
        Util::writeWord(p, loadBaseAddr.value() + relocValue, Endian::Little);
    }

    Address relocStart   = loadBaseAddr + m_imageSize + sizeof(ExeHeader);
    BinarySection *reloc = m_image->createSection("$RELOC", relocStart,
                                                  relocStart + sizeof(ExeReloc) * numReloc);
    reloc->setEntrySize(sizeof(ExeReloc));

    // as of C++11, std::vector is guaranteed to be contiguous (except for std::vector<bool>),
    // so we can read the relocated values directly from m_relocTable
    if (numReloc > 0) {
        reloc->setHostAddr(HostAddress(&m_relocations[0]));
    }

    return true;
}


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
    Byte buf[4]   = { 0 };
    const bool ok = fl.read(reinterpret_cast<char *>(buf), sizeof(buf)) == sizeof(buf);

    if (!ok) {
        // cannot read file / file is too small
        return 0;
    }

    if (!Util::testMagic(buf, { 'M', 'Z' })) {
        // No MZ header
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


BOOMERANG_DEFINE_PLUGIN(PluginType::FileLoader, ExeBinaryLoader, "DOS Exe loader plugin",
                        BOOMERANG_VERSION, "Boomerang developers")
