#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "CapstoneX86Decoder.h"

#include "boomerang/core/Project.h"
#include "boomerang/core/plugin/Plugin.h"
#include "boomerang/db/Prog.h"
#include "boomerang/ssl/exp/Binary.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/exp/Ternary.h"
#include "boomerang/ssl/statements/BoolAssign.h"
#include "boomerang/ssl/statements/BranchStatement.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/CaseStatement.h"
#include "boomerang/ssl/statements/ReturnStatement.h"
#include "boomerang/ssl/type/IntegerType.h"
#include "boomerang/util/log/Log.h"


#define X86_MAX_INSTRUCTION_LENGTH (15)


// only map those registers that are mapped to a number
// different from -1 in the SSL file.
// not all registers supported by capstone
static std::map<cs::x86_reg, RegNum> oldRegMap = {
    { cs::X86_REG_INVALID, RegNumSpecial },
    { cs::X86_REG_EAX, REG_PENT_EAX },
    { cs::X86_REG_ECX, REG_PENT_ECX },
    { cs::X86_REG_EDX, REG_PENT_EDX },
    { cs::X86_REG_EBX, REG_PENT_EBX },
    { cs::X86_REG_ESP, REG_PENT_ESP },
    { cs::X86_REG_EBP, REG_PENT_EBP },
    { cs::X86_REG_ESI, REG_PENT_ESI },
    { cs::X86_REG_EDI, REG_PENT_EDI },
    { cs::X86_REG_AX, REG_PENT_AX },
    { cs::X86_REG_CX, REG_PENT_CX },
    { cs::X86_REG_DX, REG_PENT_DX },
    { cs::X86_REG_BX, REG_PENT_BX },
    { cs::X86_REG_SP, REG_PENT_SP },
    { cs::X86_REG_BP, REG_PENT_BP },
    { cs::X86_REG_SI, REG_PENT_SI },
    { cs::X86_REG_DI, REG_PENT_DI },
    { cs::X86_REG_AL, REG_PENT_AL },
    { cs::X86_REG_CL, REG_PENT_CL },
    { cs::X86_REG_DL, REG_PENT_DL },
    { cs::X86_REG_BL, REG_PENT_BL },
    { cs::X86_REG_AH, REG_PENT_AH },
    { cs::X86_REG_CH, REG_PENT_CH },
    { cs::X86_REG_DH, REG_PENT_DH },
    { cs::X86_REG_BH, REG_PENT_BH },
    { cs::X86_REG_CS, REG_PENT_CS },
    { cs::X86_REG_DS, REG_PENT_DS },
    { cs::X86_REG_ES, REG_PENT_ES },
    { cs::X86_REG_FS, REG_PENT_FS },
    { cs::X86_REG_GS, REG_PENT_GS },
    { cs::X86_REG_SS, REG_PENT_SS },
    { cs::X86_REG_IP, RegNumSpecial },
    { cs::X86_REG_ST0, REG_PENT_ST0 },
    { cs::X86_REG_ST1, REG_PENT_ST1 },
    { cs::X86_REG_ST2, REG_PENT_ST2 },
    { cs::X86_REG_ST3, REG_PENT_ST3 },
    { cs::X86_REG_ST4, REG_PENT_ST4 },
    { cs::X86_REG_ST5, REG_PENT_ST5 },
    { cs::X86_REG_ST6, REG_PENT_ST6 },
    { cs::X86_REG_ST7, REG_PENT_ST7 }
    // fsw / fstp / fcw ?
};


/**
 * Translates Capstone register IDs to Boomerang internal register IDs.
 * \returns RegNumSpecial if register not found.
 */
RegNum fixRegNum(int csRegID)
{
    auto it = oldRegMap.find((cs::x86_reg)csRegID);
    return (it != oldRegMap.end()) ? it->second : RegNumSpecial;
}


/**
 * Converts an operand of an instruction (like %eax + 4*%esi + 0x10)
 * into a SSL expression that can be recognized by the decompiler.
 */
SharedExp operandToExp(const cs::cs_x86_op &operand)
{
    switch (operand.type) {
    case cs::X86_OP_MEM: {
        SharedExp base = (operand.mem.base != cs::X86_REG_INVALID)
                             ? Location::regOf(fixRegNum(operand.mem.base))
                             : Const::get(0);
        SharedExp index = (operand.mem.index != cs::X86_REG_INVALID)
                              ? Location::regOf(fixRegNum(operand.mem.index))
                              : Const::get(0);
        SharedExp scale = Const::get(operand.mem.scale);

        // addrExp = base + index * scale
        SharedExp addrExp = Binary::get(opPlus, base, Binary::get(opMult, scale, index));

        // for some reason the address expression is not simplified
        // if disp == 0
        if (operand.mem.disp != 0) {
            const int64_t disp = operand.mem.disp;

            SharedExp dispExp = Const::get(Address(disp));
            addrExp           = Binary::get(opPlus, addrExp, dispExp);
        }

        return Location::memOf(addrExp)->simplify();
    }
    case cs::X86_OP_REG: {
        return Location::regOf(fixRegNum(operand.reg));
    }
    case cs::X86_OP_IMM: {
        return Const::get(Address(operand.imm));
    }
    default: LOG_ERROR("Unknown x86 instruction operand type %1", operand.type); break;
    }
    return nullptr;
}


CapstoneX86Decoder::CapstoneX86Decoder(Project *project)
    : CapstoneDecoder(project, cs::CS_ARCH_X86, cs::CS_MODE_32, "ssl/x86.ssl")
{
    m_insn = cs::cs_malloc(m_handle);
}


CapstoneX86Decoder::~CapstoneX86Decoder()
{
    cs::cs_free(m_insn, 1);
}


bool CapstoneX86Decoder::initialize(Project *project)
{
    if (!CapstoneDecoder::initialize(project)) {
        return false;
    }

    const int bitness = project->getLoadedBinaryFile()->getBitness();
    switch (bitness) {
    case 16: cs::cs_option(m_handle, cs::CS_OPT_MODE, cs::CS_MODE_16); break;
    case 32: cs::cs_option(m_handle, cs::CS_OPT_MODE, cs::CS_MODE_32); break;
    case 64: cs::cs_option(m_handle, cs::CS_OPT_MODE, cs::CS_MODE_64); break;
    default: break;
    }

    return true;
}


bool CapstoneX86Decoder::decodeInstruction(Address pc, ptrdiff_t delta, DecodeResult &result)
{
    const Byte *instructionData = reinterpret_cast<const Byte *>((HostAddress(delta) + pc).value());
    size_t size                 = X86_MAX_INSTRUCTION_LENGTH;
    uint64 addr                 = pc.value();

    result.valid = cs_disasm_iter(m_handle, &instructionData, &size, &addr, m_insn);

    if (!result.valid) {
        return false;
    }
    else if (m_insn->id == cs::X86_INS_BSF || m_insn->id == cs::X86_INS_BSR) {
        // special hack to give BSF/BSR the correct semantics since SSL does not support loops yet
        const bool ok = genBSFR(pc, m_insn, result);
        return ok;
    }

    result.type         = ICLASS::NOP; // ICLASS is irrelevant for x86
    result.numBytes     = m_insn->size;
    result.reDecode     = false;
    result.rtl          = createRTLForInstruction(pc, m_insn);
    result.forceOutEdge = Address::ZERO;
    result.valid        = (result.rtl != nullptr);
    return true;
}


RegNum CapstoneX86Decoder::getRegNumByName(const QString &name) const
{
    // todo: slow
    for (size_t i = cs::X86_REG_AH; i < cs::X86_REG_ENDING; i++) {
        if (name == cs::cs_reg_name(m_handle, i)) {
            return fixRegNum(i);
        }
    }

    return cs::X86_REG_INVALID;
}


QString CapstoneX86Decoder::getRegNameByNum(RegNum regNum) const
{
    return m_dict.getRegDB()->getRegNameByNum(regNum);
}


int CapstoneX86Decoder::getRegSizeByNum(RegNum regNum) const
{
    return m_dict.getRegDB()->getRegSizeByNum(regNum);
}


static const QString operandNames[] = {
    "",    // X86_OP_INVALID
    "reg", // X86_OP_REG
    "imm", // X86_OP_IMM
    "rm",  // X86_OP_MEM
};

std::unique_ptr<RTL> CapstoneX86Decoder::createRTLForInstruction(Address pc,
                                                                 const cs::cs_insn *instruction)
{
    const int numOperands         = instruction->detail->x86.op_count;
    const cs::cs_x86_op *operands = instruction->detail->x86.operands;

    QString insnID = cs::cs_insn_name(m_handle, instruction->id);

    switch (instruction->detail->x86.prefix[0]) {
    case cs::X86_PREFIX_REP: insnID = "REP" + insnID; break;
    case cs::X86_PREFIX_REPNE: insnID = "REPNE" + insnID; break;
    }

    insnID = insnID.toUpper();

    for (int i = 0; i < numOperands; i++) {
        // example: ".imm8"
        QString operandName = "." + operandNames[operands[i].type] +
                              QString::number(operands[i].size * 8);

        insnID += operandName;
    }

    std::unique_ptr<RTL> rtl;

    // special hack to ignore 'and esp, 0xfffffff0 in startup code
    if (instruction->id == cs::X86_INS_AND && operands[0].type == cs::X86_OP_REG &&
        operands[0].reg == cs::X86_REG_ESP && operands[1].type == cs::X86_OP_IMM &&
        operands[1].imm == 0xFFFFFFF0) {
        return instantiateRTL(pc, "NOP", 0, nullptr);
    }
    else {
        rtl = instantiateRTL(pc, qPrintable(insnID), numOperands, operands);
        if (!rtl) {
            LOG_ERROR("Could not find semantics for instruction '%1', "
                      "treating instruction as NOP",
                      insnID);
            return instantiateRTL(pc, "NOP", 0, nullptr);
        }
    }

    if (isInstructionInGroup(instruction, cs::CS_GRP_RET)) {
        rtl->append(new ReturnStatement);
    }
    else if (isInstructionInGroup(instruction, cs::CS_GRP_CALL)) {
        Assign *last       = static_cast<Assign *>(rtl->back());
        SharedExp callDest = last->getRight();

        if (callDest->isConst() && callDest->access<const Const>()->getAddr() == pc + 5) {
            // This is a call to the next instruction
            // Use the standard semantics, except for the last statement
            // (just updates %pc)
            rtl->pop_back();
            // And don't make it a call statement
        }
        else {
            // correct the assignment to %pc to be relative
            if (!rtl->getStatements().empty() && rtl->getStatements().back()->isAssign()) {
                Assign *asgn = static_cast<Assign *>(rtl->getStatements().back());
                if (asgn->getLeft()->isPC() && asgn->getRight()->isConst() &&
                    !asgn->getRight()->isStrConst()) {
                    const Address absoluteAddr = asgn->getRight()->access<Const>()->getAddr();
                    const int delta            = (absoluteAddr -
                                       Address(instruction->address - instruction->size))
                                          .value();

                    asgn->setRight(Binary::get(opPlus, Terminal::get(opPC), Const::get(delta)));
                }
            }

            CallStatement *call = new CallStatement;
            // Set the destination
            call->setDest(callDest);
            rtl->append(call);

            if (callDest->isConst()) {
                Function *destProc = m_prog->getOrCreateFunction(
                    callDest->access<Const>()->getAddr());

                if (destProc == reinterpret_cast<Function *>(-1)) {
                    destProc = nullptr; // In case a deleted Proc
                }

                call->setDestProc(destProc);
            }
            else {
                call->setIsComputed(true);
            }
        }
    }
    else if (isInstructionInGroup(instruction, cs::X86_GRP_JUMP)) {
        Assign *last = static_cast<Assign *>(rtl->back());
        assert(last->getLeft()->isPC());
        SharedExp guard = last->getGuard();

        const bool isComputedJump = !last->getRight()->isConst();

        if (guard == nullptr) {
            if (isComputedJump) {
                // unconditional computed jump (switch statement)
                CaseStatement *cs = new CaseStatement();
                cs->setDest(last->getRight());
                cs->setIsComputed(true);
                rtl->pop_back();
                rtl->append(cs);
            }
            else {
                // unconditional jump
                GotoStatement *gs = new GotoStatement;
                gs->setDest(last->getRight());
                rtl->pop_back();
                rtl->append(gs);
            }
        }
        else {
            // conditional jump
            BranchStatement *branch = new BranchStatement();
            branch->setDest(last->getRight());
            branch->setCondExpr(guard);
            branch->setIsComputed(true);

            BranchType bt = BranchType::INVALID;
            switch (instruction->id) {
            case cs::X86_INS_JE: bt = BranchType::JE; break;
            case cs::X86_INS_JNE: bt = BranchType::JNE; break;
            case cs::X86_INS_JL: bt = BranchType::JSL; break; // signed less
            case cs::X86_INS_JLE: bt = BranchType::JSLE; break;
            case cs::X86_INS_JGE: bt = BranchType::JSGE; break;
            case cs::X86_INS_JG: bt = BranchType::JSG; break;
            case cs::X86_INS_JB: bt = BranchType::JUL; break; // unsigned less
            case cs::X86_INS_JBE: bt = BranchType::JULE; break;
            case cs::X86_INS_JAE: bt = BranchType::JUGE; break;
            case cs::X86_INS_JA: bt = BranchType::JUG; break;
            case cs::X86_INS_JS: bt = BranchType::JMI; break;
            case cs::X86_INS_JNS: bt = BranchType::JPOS; break;
            case cs::X86_INS_JO: bt = BranchType::JOF; break;
            case cs::X86_INS_JNO: bt = BranchType::JNOF; break;
            case cs::X86_INS_JP: bt = BranchType::JPAR; break;
            case cs::X86_INS_JNP: bt = BranchType::JNPAR; break;
            case cs::X86_INS_JCXZ: bt = BranchType::JE; break;
            case cs::X86_INS_JECXZ: bt = BranchType::JE; break; // because jmp condition is %ecx = 0
            case cs::X86_INS_LOOP: bt = BranchType::JNE; break;
            case cs::X86_INS_LOOPE: bt = BranchType::JE; break;
            case cs::X86_INS_LOOPNE: bt = BranchType::JNE; break;
            default: assert(false); break;
            }

            branch->setCondType(bt, false);
            branch->setIsComputed(isComputedJump);

            rtl->pop_back();
            rtl->append(branch);
        }
    }
    else if (insnID.startsWith("SET")) {
        BoolAssign *bas = new BoolAssign(8);
        bas->setCondExpr(static_cast<Assign *>(rtl->front())->getRight()->clone());
        bas->setLeft(static_cast<Assign *>(rtl->front())->getLeft()->clone());

        BranchType bt = BranchType::INVALID;
        switch (instruction->id) {
        case cs::X86_INS_SETE: bt = BranchType::JE; break;
        case cs::X86_INS_SETNE: bt = BranchType::JNE; break;
        case cs::X86_INS_SETL: bt = BranchType::JSL; break; // signed less
        case cs::X86_INS_SETLE: bt = BranchType::JSLE; break;
        case cs::X86_INS_SETGE: bt = BranchType::JSGE; break;
        case cs::X86_INS_SETG: bt = BranchType::JSG; break;
        case cs::X86_INS_SETB: bt = BranchType::JUL; break; // unsigned less
        case cs::X86_INS_SETBE: bt = BranchType::JULE; break;
        case cs::X86_INS_SETAE: bt = BranchType::JUGE; break;
        case cs::X86_INS_SETA: bt = BranchType::JUG; break;
        case cs::X86_INS_SETS: bt = BranchType::JMI; break;
        case cs::X86_INS_SETNS: bt = BranchType::JPOS; break;
        case cs::X86_INS_SETO: bt = BranchType::JOF; break;
        case cs::X86_INS_SETNO: bt = BranchType::JNOF; break;
        case cs::X86_INS_SETP: bt = BranchType::JPAR; break;
        case cs::X86_INS_SETNP: bt = BranchType::JNPAR; break;
        default: assert(false); break;
        }

        bas->setCondType(bt);
        if (rtl->size() > 1) {
            LOG_WARN(
                "%1 additional statements in RTL for instruction '%2'; results may be inaccurate",
                rtl->size() - 1, insnID);
        }

        rtl->clear();
        rtl->append(bas);
    }

    return rtl;
}


std::unique_ptr<RTL> CapstoneX86Decoder::instantiateRTL(Address pc, const char *instructionID,
                                                        int numOperands,
                                                        const cs::cs_x86_op *operands)
{
    std::vector<SharedExp> args(numOperands);
    for (int i = 0; i < numOperands; i++) {
        args[i] = operandToExp(operands[i]);
    }

    if (m_debugMode) {
        QString argNames;
        for (int i = 0; i < numOperands; i++) {
            if (i != 0) {
                argNames += " ";
            }
            argNames += args[i]->toString();
        }

        LOG_MSG("Instantiating RTL at %1: %2 %3", pc, instructionID, argNames);
    }

    bool found;
    const std::pair<QString, DWord> &signature = m_dict.getSignature(instructionID, &found);

    if (found) {
        return m_dict.instantiateRTL(signature.first, pc, args);
    }
    else {
        return nullptr;
    }
}


bool CapstoneX86Decoder::genBSFR(Address pc, const cs::cs_insn *instruction, DecodeResult &result)
{
    // Note the horrible hack needed here. We need initialisation code, and an extra branch, so the
    // %SKIP/%RPT won't work. We need to emit 6 statements, but these need to be in 3 RTLs, since
    // the destination of a branch has to be to the start of an RTL.  So we use a state machine, and
    // set numBytes to 0 for the first two times. That way, this instruction ends up emitting three
    // RTLs, each with the semantics we need. Note: we don't use pentium.ssl for these.
    //
    // BSFR1:
    //    pc+0:    *1* zf := 1
    //    pc+0:    goto exit if src = 0
    // BSFR2:
    //    pc+1:    *1* zf := 0
    //    pc+1:    dest := 0 if BSF else dest := opsize - 1
    // BSFR3:
    //    pc+2:    dest := dest op 1
    //    pc+2:    goto pc+2 if src@[dest:dest]=0
    // exit:
    //

    BranchStatement *b = nullptr;
    result.rtl         = std::unique_ptr<RTL>(new RTL(pc + m_bsfrState));

    const cs::cs_x86_op &dstOp = instruction->detail->x86.operands[0];
    const cs::cs_x86_op &srcOp = instruction->detail->x86.operands[1];

    assert(dstOp.size == srcOp.size);
    const SharedExp dest = operandToExp(dstOp);
    const SharedExp src  = operandToExp(srcOp);
    const int size       = dstOp.size * 8;
    const int init       = instruction->id == cs::X86_INS_BSF ? 0 : size - 1;
    const OPER incdec    = instruction->id == cs::X86_INS_BSF ? opPlus : opMinus;

    switch (m_bsfrState) {
    case 0:
        result.rtl->append(new Assign(IntegerType::get(1), Terminal::get(opZF), Const::get(1)));
        b = new BranchStatement;
        b->setDest(pc + instruction->size);
        b->setCondType(BranchType::JE);
        b->setCondExpr(Binary::get(opEquals, src->clone(), Const::get(0)));
        result.rtl->append(b);
        break;

    case 1:
        result.rtl->append(new Assign(IntegerType::get(1), Terminal::get(opZF), Const::get(0)));
        result.rtl->append(new Assign(IntegerType::get(size), dest->clone(), Const::get(init)));
        break;

    case 2:
        result.rtl->append(new Assign(IntegerType::get(size), dest->clone(),
                                      Binary::get(incdec, dest->clone(), Const::get(1))));
        b = new BranchStatement;
        b->setDest(pc + 2);
        b->setCondType(BranchType::JE);
        b->setCondExpr(Binary::get(opEquals,
                                   Ternary::get(opAt, src->clone(), dest->clone(), dest->clone()),
                                   Const::get(0)));
        result.rtl->append(b);
        break;

    default:
        // Should never happen
        LOG_FATAL("Unknown BSFR state %1", m_bsfrState);
    }

    // Keep numBytes == 0 until the last state, so we re-decode this instruction 3 times
    if (m_bsfrState != 3 - 1) {
        // Let the number of bytes be 1. This is important at least for setting the fallthrough
        // address for the branch (in the first RTL), which should point to the next RTL
        result.numBytes = 1;
        result.reDecode = true; // Decode this instuction again
    }
    else {
        result.numBytes = instruction->size;
        result.reDecode = false;
    }

    if (m_debugMode) {
        LOG_MSG("%1: BS%2%3%4", pc + m_bsfrState, (init == -1 ? "F" : "R"),
                (size == 32 ? ".od" : ".ow"), m_bsfrState + 1);
    }

    if (++m_bsfrState == 3) {
        m_bsfrState = 0; // Ready for next time
    }

    return true;
}


BOOMERANG_DEFINE_PLUGIN(PluginType::Decoder, CapstoneX86Decoder, "Capstone x86 decoder plugin",
                        BOOMERANG_VERSION, "Boomerang developers")
