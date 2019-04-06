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


#include "boomerang/core/BoomerangAPI.h"
#include "boomerang/frontend/DefaultFrontEnd.h"
#include "boomerang/ifc/IDecoder.h"
#include "boomerang/ssl/exp/Operator.h"
#include "boomerang/ssl/type/Type.h"
#include "boomerang/util/Interval.h"


/**
 * This file contains routines to manage the decoding of SPARC instructions
 * and the instantiation to RTLs, removing SPARC dependent features
 * such as delay slots in the process.
 */
class BOOMERANG_PLUGIN_API SPARCFrontEnd : public DefaultFrontEnd
{
public:
    /// \copydoc IFrontEnd::IFrontEnd
    SPARCFrontEnd(Project *project);
    SPARCFrontEnd(const SPARCFrontEnd &other) = delete;
    SPARCFrontEnd(SPARCFrontEnd &&other)      = default;

    virtual ~SPARCFrontEnd() = default;

    SPARCFrontEnd &operator=(const SPARCFrontEnd &) = delete;
    SPARCFrontEnd &operator=(SPARCFrontEnd &&) = default;

public:
    /**
     * \copydoc IFrontEnd::processProc
     *
     * Builds the CFG for a procedure out of the RTLs constructed
     * during decoding. The semantics of delayed CTIs are
     * transformed into CTIs that aren't delayed.
     */
    virtual bool processProc(UserProc *proc, Address entryAddr) override;

    /// \copydoc IFrontEnd::getMainEntryPoint
    virtual Address findMainEntryPoint(bool &gotMain) override;

private:
    /**
     * Check if delay instruction can be optimized.
     *
     * Determines if a delay instruction is exactly the same as the instruction immediately
     * preceding the destination of a CTI; i.e. has been copied from the real destination to the
     * delay slot as an optimisation
     *
     * \param src        the logical source address of a CTI
     * \param dest       the logical destination address of the CTI
     * \param delta      used to convert logical to real addresses
     * SIDE EFFECT:    Optionally displays an error message if the target of the branch
     *                 is the delay slot of another delayed CTI
     * \returns true if delay instruction can be optimized away
     */
    bool canOptimizeDelayCopy(Address src, Address dest, ptrdiff_t delta,
                              Interval<Address> textLimit) const;

    /**
     * Determines if the given call and delay instruction consitute a call
     * where callee returns to the caller's caller. That is:
     *   ProcA      |  ProcB:       |         ProcC:
     * -------------|---------------|---------------
     * ...          | ...           |        ...
     * call ProcB   | call ProcC    |        ret
     * ...          | restore       |        ...
     *
     * The restore instruction in ProcB will effectively set %o7 to be %i7,
     * the address to which ProcB will return. So in effect ProcC will return
     * to ProcA at the position just after the call to ProcB. This is equivalent
     * to ProcC returning to ProcB which then immediately returns to ProcA.
     *
     * \note       We don't set a label at the return, because we also have to
     *             force a jump at the call BB, and in some cases we don't
     *             have the call BB as yet. So these two are up to the caller
     * \note       The name of this function is now somewhat irrelevant.
     *             The whole function is somewhat irrelevant; it dates from
     *             the times when you would always find an actual restore
     *             in the delay slot. With some patterns, this is no longer true.
     *
     * \param      call  the RTL for the caller (e.g. "call ProcC" above)
     * \param      rtl   pointer to the RTL for the call instruction
     * \param      delay the RTL for the delay instruction (e.g. "restore")
     * \returns    The basic block containing the single return instruction
     *             if this optimisation applies, nullptr otherwise.
     */
    BasicBlock *optimizeCallReturn(CallStatement *call, const RTL *rtl, const RTL *delay,
                                   UserProc *proc);

    /**
     * Adds the destination of a branch to the queue of address
     * that must be decoded (if this destination has not already been visited).
     * but newBB may be changed if the destination of the branch is in the middle of an existing
     * BB. It will then be changed to point to a new BB beginning with the dest
     * \param newBB the new basic block delimited by the branch instruction. May be nullptr if
     *              this block has been built before.
     * \param dest the destination being branched to
     * \param cfg the CFG of the current procedure
     * \param tq Object managing the target queue
     */
    void createJumpToAddress(Address dest, BasicBlock *&newBB, ProcCFG *cfg, TargetQueue &tq,
                             Interval<Address> textLimit);

    /**
     * Records the fact that there is a procedure at a given address. Also adds the out edge to the
     * lexical successor of the call site (taking into consideration the delay slot and possible
     * UNIMP instruction).
     *
     * \param  dest the address of the callee
     * \param  callBB the basic block delimited by the call
     * \param  cfg CFG of the enclosing procedure
     * \param  address the address of the call instruction
     * \param  offset the offset from the call instruction to which an outedge must be added. A
     *                value of 0 means no edge is to be added.
     */
    void createCallToAddress(Address dest, Address address, BasicBlock *callBB, ProcCFG *cfg,
                             int offset = 0);

    /**
     * This is the stub for cases of DCTI couples that we haven't written
     * analysis code for yet. It simply displays an informative warning and returns.
     * \param addr the address of the first CTI in the couple
     */
    void case_unhandled_stub(Address addr);

    /**
     * Handles a call instruction
     * \param address the native address of the call instruction
     * \param inst the info summaries when decoding the call instruction
     * \param delay_inst the info summaries when decoding the delay instruction
     * \param BB_rtls the list of RTLs currently built for the BB under construction
     * \param proc the enclosing procedure
     * \param callList a list of pointers to CallStatements for procs yet to be processed
     * \param os output stream for rtls
     * \param isPattern true if the call is an idiomatic pattern (e.g. a move_call_move pattern)
     * SIDE EFFECTS: address may change; BB_rtls may be appended to or set nullptr
     * \returns true if next instruction is to be fetched sequentially from this one
     */
    bool case_CALL(Address &address, DecodeResult &inst, DecodeResult &delay_inst,
                   std::unique_ptr<RTLList> &BB_rtls, UserProc *proc,
                   std::list<CallStatement *> &callList, bool isPattern = false);

    /**
     * Handles a non-call, static delayed (SD) instruction
     * \param address the native address of the SD
     * \param delta the offset of the above address from the logical address at which the
     *              procedure starts (i.e. the one given by dis)
     * \param inst the info summaries when decoding the SD instruction
     * \param delay_inst - the info summaries when decoding the delay instruction
     * \param BB_rtls the list of RTLs currently built for the BB under construction
     * \param cfg the CFG of the enclosing procedure
     * \param tq Object managing the target queue
     * \param os output stream for rtls
     * SIDE EFFECTS: address may change; BB_rtls may be appended to or set nullptr
     *
     */
    void case_SD(Address &address, ptrdiff_t delta, Interval<Address> textLimit, DecodeResult &inst,
                 DecodeResult &delay_inst, std::unique_ptr<RTLList> BB_rtls, ProcCFG *cfg,
                 TargetQueue &tq);

    /**
     * Handles all dynamic delayed jumps (jmpl, also dynamic calls)
     * \param address the native address of the DD
     * \param delta the offset of the above address from the logical address at which the
     *              procedure starts (i.e. the one given by dis)
     * \param inst the info summaries when decoding the SD instruction
     * \param delay_inst the info summaries when decoding the delay instruction
     * \param BB_rtls the list of RTLs currently built for the BB under construction
     * \param tq Object managing the target queue
     * \param proc pointer to the current Proc object
     * \param callList a set of pointers to CallStatements for procs yet to be processed
     * SIDE EFFECTS: address may change; BB_rtls may be appended to or set nullptr
     * \returns true if next instruction is to be fetched sequentially from this one
     */
    bool case_DD(Address &address, ptrdiff_t delta, DecodeResult &inst, DecodeResult &delay_inst,
                 std::unique_ptr<RTLList> BB_rtls, TargetQueue &tq, UserProc *proc,
                 std::list<CallStatement *> &callList);

    /**
     * Handles all Static Conditional Delayed non-anulled branches
     * \param address    the native address of the DD
     * \param delta      the offset of the above address from the logical address at which the
     *                   procedure starts (i.e. the one given by dis)
     * \param inst       the info summaries when decoding the SD instruction
     * \param delay_inst the info summaries when decoding the delay instruction
     * \param BB_rtls    the list of RTLs currently built for the BB under construction
     * \param cfg        the CFG of the enclosing procedure
     * \param tq         Object managing the target queue
     * SIDE EFFECTS:     address may change; BB_rtls may be appended to or set nullptr
     * \returns true if next instruction is to be fetched sequentially from this one
     */
    bool case_SCD(Address &address, ptrdiff_t delta, Interval<Address> textLimit,
                  DecodeResult &inst, DecodeResult &delay_inst, std::unique_ptr<RTLList> BB_rtls,
                  ProcCFG *cfg, TargetQueue &tq);

    /**
     * Handles all static conditional delayed anulled branches followed by
     * an NCT (but not NOP) instruction.
     * \param address    the native address of the DD
     * \param delta      the offset of the above address from the logical
     *                   address at which the procedure starts (i.e. the one given by dis)
     * \param inst       the info summaries when decoding the SD instruction
     * \param delay_inst the info summaries when decoding the delay instruction
     * \param BB_rtls    the list of RTLs currently built for the BB under construction
     * \param cfg        the CFG of the enclosing procedure
     * \param tq         Object managing the target queue
     * SIDE EFFECTS: address may change; BB_rtls may be appended to or set nullptr
     * \returns          true if next instruction is to be fetched sequentially from this one
     */
    bool case_SCDAN(Address &address, ptrdiff_t delta, Interval<Address> textLimit,
                    DecodeResult &inst, DecodeResult &delay_inst, std::unique_ptr<RTLList> BB_rtls,
                    ProcCFG *cfg, TargetQueue &tq);

    /**
     * Emit a null RTL with the given address.
     * \param rtls List of RTLs to append this instruction to
     * \param instAddr Native address of this instruction
     */
    void emitNop(RTLList &rtls, Address instAddr);

    /**
     * Emit the RTL for a call $+8 instruction, which is merely %o7 = %pc
     * \note   Assumes that the delay slot RTL has already been pushed; we must push the semantics
     * BEFORE that RTL, since the delay slot instruction may use %o7. Example: CALL $+8            !
     * This code is common in startup code ADD     %o7, 20, %o0
     * \param rtls list of RTLs to append to
     * \param addr native address for the RTL
     */
    void emitCopyPC(RTLList &rtls, Address addr);

    // Append one assignment to a list of RTLs
    void appendAssignment(const SharedExp &lhs, const SharedExp &rhs, SharedType type, Address addr,
                          RTLList &rtls);

    /*
     * Small helper function to build an expression with
     * *128* m[m[r[14]+64]] = m[r[8]] OP m[r[9]]
     */
    void quadOperation(Address addr, RTLList &lrtl, OPER op);

    /**
     * Checks for SPARC specific helper functions like .urem, which have specific sematics.
     * Determine if this is a helper function, e.g. .mul. If so, append the appropriate RTLs to
     * lrtl, and return true
     * \note This needs to be handled in a resourcable way.
     * \param  dest destination of the call (native address)
     * \param  addr address of current instruction (native addr)
     * \param  lrtl list of RTL* for current BB
     * \returns True if a helper function was found and handled; false otherwise
     */
    bool isHelperFunc(Address dest, Address addr, RTLList &lrtl) override;

    /**
     * Another small helper function to generate either (for V9):
     * 64* tmp[tmpl] = sgnex(32, 64, r8) op sgnex(32, 64, r9)
     * 32* r8 = truncs(64, 32, tmp[tmpl])
     * 32* r9 = r[tmpl]@32:63
     * or for v8:
     * 32* r[tmp] = r8 op r9
     * 32* r8 = r[tmp]
     * 32* r9 = %Y
     */
    void gen32op32gives64(OPER op, RTLList &lrtl, Address addr);

    /// This is the long version of helperFunc (i.e. -f not used).
    /// This does the complete 64 bit semantics
    bool helperFuncLong(Address dest, Address addr, RTLList &lrtl, QString &name);

    /// Warn about an invalid or unrecognized instruction at \p pc
    void warnInvalidInstruction(Address pc);

private:
    // This struct represents a single nop instruction.
    // Used as a substitute delay slot instruction
    DecodeResult nop_inst;
};
