#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "NJMCDecoder.h"

#include "boomerang/core/Project.h"
#include "boomerang/core/Settings.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/proc/Proc.h"
#include "boomerang/db/proc/ProcCFG.h"
#include "boomerang/ssl/RTL.h"
#include "boomerang/ssl/Register.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/statements/Assign.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/util/Util.h"
#include "boomerang/util/log/Log.h"

#include <cassert>
#include <cstdarg>
#include <cstring>


NJMCDecoder::NJMCDecoder(Prog *prog, const QString &sslFilePath)
    : m_rtlDict(prog->getProject()->getSettings()->debugDecoder)
    , m_prog(prog)
{
    QDir dataDir = prog->getProject()->getSettings()->getDataDirectory();

    if (!m_rtlDict.readSSLFile(dataDir.absoluteFilePath(sslFilePath))) {
        LOG_ERROR("Cannot read SSL file '%1'", sslFilePath);
        throw std::runtime_error("Failed to read SSL file");
    }
}


std::unique_ptr<RTL> NJMCDecoder::instantiate(Address pc, const char *name,
                                              const std::initializer_list<SharedExp> &args)
{
    // Get the signature of the instruction and extract its parts
    std::pair<QString, int> sig = m_rtlDict.getSignature(name);
    QString opcode              = sig.first;
    int numOperands             = sig.second;

    if (numOperands == -1) {
        throw std::runtime_error(
            QString("No entry for '%1' in RTL dictionary").arg(name).toStdString());
    }
    else if (numOperands != (int)args.size()) {
        QString msg = QString("Disassembled instruction '%1' has %2 arguments, "
                              "but the instruction has %3 parameters in the RTL dictionary")
                          .arg(name)
                          .arg(args.size())
                          .arg(numOperands);
        throw std::runtime_error(msg.toStdString());
    }

    // Put the operands into a vector
    std::vector<SharedExp> actuals(args);

    if (m_prog->getProject()->getSettings()->debugDecoder) {
        OStream q_cout(stdout);
        // Display a disassembly of this instruction if requested
        q_cout << pc << ": " << name << " ";

        for (const SharedExp &itd : actuals) {
            if (itd->isIntConst()) {
                int val = itd->access<Const>()->getInt();

                if ((val > 100) || (val < -100)) {
                    q_cout << "0x" << QString::number(val, 16);
                }
                else {
                    q_cout << val;
                }
            }
            else {
                itd->print(q_cout);
            }

            q_cout << " ";
        }

        q_cout << '\n';
    }

    return m_rtlDict.instantiateRTL(opcode, pc, actuals);
}


SharedExp NJMCDecoder::dis_Reg(int regNum)
{
    return Location::regOf(regNum);
}


SharedExp NJMCDecoder::dis_Num(unsigned num)
{
    return Const::get(num); // TODO: what about signed values ?
}


void NJMCDecoder::processUnconditionalJump(const char *name, int size, HostAddress relocd,
                                           ptrdiff_t delta, Address pc, DecodeResult &result)
{
    result.numBytes     = size;
    GotoStatement *jump = new GotoStatement();
    jump->setDest(Address((relocd - delta).value()));
    result.rtl->append(jump);
    SHOW_ASM(name << " " << relocd - delta)
}


void NJMCDecoder::processComputedJump(const char *name, int size, SharedExp dest, Address pc,
                                      DecodeResult &result)
{
    result.numBytes = size;

    GotoStatement *jump = new GotoStatement();
    jump->setDest(dest);
    jump->setIsComputed(true);
    result.rtl->append(jump);

    SHOW_ASM(name << " " << dest)
}


void NJMCDecoder::processComputedCall(const char *name, int size, SharedExp dest, Address pc,
                                      DecodeResult &result)
{
    result.numBytes = size;

    CallStatement *call = new CallStatement();
    call->setDest(dest);
    call->setIsComputed(true);
    result.rtl->append(call);

    SHOW_ASM(name << " " << dest)
}


QString NJMCDecoder::getRegName(int idx) const
{
    return m_rtlDict.getRegNameByID(idx);
}


int NJMCDecoder::getRegSize(int idx) const
{
    return m_rtlDict.getRegSizeByID(idx);
}


int NJMCDecoder::getRegIdx(const QString &name) const
{
    return m_rtlDict.getRegIDByName(name);
}
