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


#include "CapstoneDecoder.h"

#include "boomerang/core/BoomerangAPI.h"
#include "boomerang/ssl/exp/Operator.h"


/**
 * Instruction decoder using Capstone to decode
 * x86_32 instructions into SSL RTLs.
 */
class BOOMERANG_PLUGIN_API CapstoneX86Decoder : public CapstoneDecoder
{
public:
    CapstoneX86Decoder(Project *project);
    ~CapstoneX86Decoder();

public:
    /// \copydoc IDecoder::decodeInstruction
    virtual bool decodeInstruction(Address pc, ptrdiff_t delta, DecodeResult &result) override;

    /// \copydoc IDecoder::getRegNameByNum
    virtual QString getRegNameByNum(RegNum regNum) const override;

    /// \copydoc IDecoder::getRegNumByName
    virtual RegNum getRegNumByName(const QString &name) const override;

    /// \copydoc IDecoder::getRegSize
    virtual int getRegSizeByNum(RegNum regNum) const override;

private:
    bool initialize(Project *project) override;

    /**
     * Creates a new RTL for a single instruction.
     * \param pc the address of the instruction to instantiate.
     * \param instruction the actual instruction.
     *
     * \internal Note that for some instruction groups (e.g. calls, jumps, setCC instructions)
     * hard-coded adjustments are performed due to SSL limitations. See the function definition
     * for details.
     */
    std::unique_ptr<RTL> createRTLForInstruction(Address pc, const cs::cs_insn *instruction);

    /**
     * Instantiates an RTL for a single instruction, replacing formal parameters with actual
     * arguments from \p operands.
     *
     * \param pc the address of the instruction.
     * \param instructionID the unique name of the instruction (e.g. MOV.reg32.reg32)
     * \param numOperands number of instruction operands (e.g. 2 for MOV.reg32.reg32)
     * \param operands Array containing actual arguments containing \p numOperands elements.
     */
    std::unique_ptr<RTL> instantiateRTL(Address pc, const char *instructionID, int numOperands,
                                        const cs::cs_x86_op *operands);

    /**
     * Generate statements for the BSF and BSR instructions (Bit Scan Forward/Reverse)
     * \note Since SSL does not support loops yet, we have to build the semantics using a state
     * machine with three states. So we have to call this function three times for the same
     * instrucion to generate the correct semantics.
     * \param pc start of the instruction
     */
    bool genBSFR(Address pc, const cs::cs_insn *instruction, DecodeResult &result);

private:
    int m_bsfrState = 0; ///< State for state machine used in genBSFR()
    cs::cs_insn *m_insn; ///< decoded instruction;
};
