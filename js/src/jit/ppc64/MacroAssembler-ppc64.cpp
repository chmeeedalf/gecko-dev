/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ppc64/MacroAssembler-ppc64.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/MacroAssembler.h"
#include "jit/MoveEmitter.h"
#include "jit/SharedICRegisters.h"

#include "jit/MacroAssembler-inl.h"
#include "vm/JitActivation.h"

using namespace js;
using namespace jit;

using mozilla::Abs;

#if DEBUG
#define spew(...) JitSpew(JitSpew_Codegen, __VA_ARGS__)
#else
#define spew(...)
#endif

#if DEBUG

/* Useful class to print visual guard blocks. */
class MASMAutoDeBlock
{
    private:
        const char *blockname;

    public:
        MASMAutoDeBlock(const char *name, int line) {
            blockname = name;
            JitSpew(JitSpew_Codegen, "[[ CGPPC line %d: %s", line, blockname);
        }

        ~MASMAutoDeBlock() {
            JitSpew(JitSpew_Codegen, "   CGPPC: %s ]]", blockname);
        }
};
#define ADBlock()  MASMAutoDeBlock _adbx(__PRETTY_FUNCTION__, __LINE__)
#else

/* Useful macro to completely elide visual guard blocks. */
#define ADBlock()  ;

#endif


static_assert(sizeof(intptr_t) == 8, "Not 64-bit clean.");

void
MacroAssemblerPPC64Compat::convertBoolToInt32(Register src, Register dest)
{
    ADBlock();
    // Note that C++ bool is only 1 byte, so zero extend it to clear the
    // higher-order bits.
    ma_and(dest, src, Imm32(0xff));
}

void
MacroAssemblerPPC64Compat::convertInt32ToDouble(Register src, FloatRegister dest)
{
    // Power has no GPR<->FPR moves, and we may not have a linkage area,
    // so we do this on the stack (see also OPPCC chapter 8 p.156 for the
    // basic notion, but we have a better choice on POWER9 since we no
    // longer have to faff around with fake constants like we did in 32-bit).
    ADBlock();

#ifdef __POWER8_VECTOR__
    as_mtvsrd(dest, src);
#else
    // Alternative with no GPR<->FPR moves.
    // Treat src as a 64-bit register (since it is) and spill to stack.
    as_stdu(src, StackPointer, -8);
    // Power CPUs with traditional dispatch groups will need NOPs here.
    as_lfd(dest, StackPointer, 0);
#endif
    as_fcfid(dest, dest); // easy!
}

void
MacroAssemblerPPC64Compat::convertUInt64ToDouble(Register src, FloatRegister dest)
{
    // Approximately the same as above, but using fcfidu.
    ADBlock();

#ifdef __POWER8_VECTOR__
    as_mtvsrd(dest, src);
#else
    // Alternative with no GPR<->FPR moves.
    as_stdu(src, StackPointer, -8);
    // Power CPUs with traditional dispatch groups will need NOPs here.
    as_lfd(ScratchDoubleReg, StackPointer, 0);
#endif
    as_fcfidu(dest, dest);
}

void
MacroAssemblerPPC64Compat::convertInt32ToDouble(const Address& src, FloatRegister dest)
{
    ADBlock();
    load32(src, SecondScratchReg);
    convertInt32ToDouble(SecondScratchReg, dest);
}

void
MacroAssemblerPPC64Compat::convertInt32ToDouble(const BaseIndex& src, FloatRegister dest)
{
    ADBlock();
    computeScaledAddress(src, ScratchRegister);
    convertInt32ToDouble(Address(ScratchRegister, src.offset), dest);
}

void
MacroAssemblerPPC64Compat::convertUInt32ToDouble(Register src, FloatRegister dest)
{
    ADBlock();
    ma_dext(ScratchRegister, src, Imm32(0), Imm32(32));
    asMasm().convertUInt64ToDouble(Register64(ScratchRegister), dest, InvalidReg);
}


void
MacroAssemblerPPC64Compat::convertUInt32ToFloat32(Register src, FloatRegister dest)
{
    ADBlock();
    ma_dext(ScratchRegister, src, Imm32(0), Imm32(32));
    asMasm().convertUInt64ToFloat32(Register64(ScratchRegister), dest, InvalidReg);
}

void
MacroAssemblerPPC64Compat::convertDoubleToFloat32(FloatRegister src, FloatRegister dest)
{
    ADBlock();
    as_frsp(dest, src);
}

// Checks whether a double is representable as a 32-bit integer. If so, the
// integer is written to the output register. Otherwise, a bailout is taken to
// the given snapshot. This function overwrites the scratch float register.
void
MacroAssemblerPPC64Compat::convertDoubleToInt32(FloatRegister src, Register dest,
                                                 Label* fail, bool negativeZeroCheck)
{
    ADBlock();
    MOZ_ASSERT(src != ScratchDoubleReg);

    // fctiwz. will set an exception to CR1 if conversion is inexact
    // or invalid. We don't need to know the exact exception, just that
    // it went boom, so no need to check the FPSCR.
    as_fctiwz_rc(ScratchDoubleReg, src);
    ma_bc(cr1, Assembler::LessThan, fail);

    // Spill to memory and pick up the value.
    as_stfdu(ScratchDoubleReg, StackPointer, -8);
    // Power CPUs with traditional dispatch groups will need NOPs here.
    // Pull out the lower 32 bits. ENDIAN!!!
    as_lwz(dest, StackPointer, 0); // 4 for BE

    if (negativeZeroCheck) {
        // If we need to check negative 0, then dump the FPR on the stack
        // and look at the sign bit. fctiwz. will merrily convert -0 with
        // no exception because, well, it's zero!
        // The MIPS version happily clobbers dest from the beginning, so
        // no worries doing this check here to save some work.

        Label done;
        MOZ_ASSERT(dest != ScratchRegister && dest != SecondScratchReg);
        // Don't bother if the result was not zero.
        as_cmpldi(dest, 0);
        ma_bc(Assembler::NotEqual, &done, ShortJump);

        // Damn, the result was zero.
        // Dump the original float and check the two 32-bit halves.
        // 0x8000000 00000000 = -0.0
        // 0x0000000 00000000 = 0.0
        // Thus, if they're not the same, negative zero; bailout.
        as_stfd(src, StackPointer, 0); // reuse existing allocation
        // Power CPUs with traditional dispatch groups will need NOPs here.
        as_lwz(ScratchRegister, StackPointer, 0);
        as_lwz(SecondScratchReg, StackPointer, 4);
        as_cmplw(ScratchRegister, SecondScratchReg);
        as_addi(StackPointer, StackPointer, 8);
        ma_bc(Assembler::NotEqual, fail);

        bind(&done);
    } else {
        as_addi(StackPointer, StackPointer, 8);
    }
}

// Checks whether a float32 is representable as a 32-bit integer.
void
MacroAssemblerPPC64Compat::convertFloat32ToInt32(FloatRegister src, Register dest,
                                                  Label* fail, bool negativeZeroCheck)
{
    // Since 32-bit and 64-bit FPRs are the same registers, use the same
    // routine above.
    ADBlock();
    convertDoubleToInt32(src, dest, fail, negativeZeroCheck);
}

void
MacroAssemblerPPC64Compat::convertFloat32ToDouble(FloatRegister src, FloatRegister dest)
{
    // Nothing to do.
}

void
MacroAssemblerPPC64Compat::convertInt32ToFloat32(Register src, FloatRegister dest)
{
    ADBlock();
    convertInt32ToDouble(src, dest);
    as_frsp(dest, dest); // probably overkill
}

void
MacroAssemblerPPC64Compat::convertInt32ToFloat32(const Address& src, FloatRegister dest)
{
    ADBlock();
    ma_li(ScratchRegister, ImmWord(src.offset));
    as_lfiwax(dest, src.base, ScratchRegister);
    as_fcfid(dest, dest);
}

void
MacroAssemblerPPC64Compat::movq(Register rs, Register rd)
{
    ma_move(rd, rs);
}

void
MacroAssemblerPPC64::ma_li(Register dest, CodeLabel* label)
{
    BufferOffset bo = m_buffer.nextOffset();
    ma_liPatchable(dest, ImmWord(/* placeholder */ 0));
    label->patchAt()->bind(bo.getOffset());
    label->setLinkMode(CodeLabel::MoveImmediate);
}

// Generate an optimized sequence to load a 64-bit immediate.
void
MacroAssemblerPPC64::ma_li(Register dest, int64_t value)
{
    uint64_t bits = (uint64_t)value;
    bool loweronly = true;

    // Handle trivial 16-bit quantities.
    if (value > -32769 && value < 32768) {
        // fits in 16 low bits
        xs_li(dest, value); // mscdfr0 asserts
        return;
    }
    if ((bits & 0xffffffff0000ffff) == 0 ||
            (bits & 0xffffffff0000ffff) == 0xffffffff00000000) {
        // fits in 16 high bits
        xs_lis(dest, value >> 16); // mscdfr0 asserts
        return;
    }

    // Emit optimized sequence based on occupied bits.
    if (bits & 0xffff000000000000) {
        // Need to set upper word and shift.
        xs_lis(dest, bits >> 48);
        if (bits & 0x0000ffff00000000) {
            as_ori(dest, dest, (bits >> 32) & 0xffff);
        }
        as_rldicr(dest, dest, 32, 31);
        loweronly = false;
    } else if (bits & 0x0000ffff00000000) {
        xs_li(dest, (bits >> 32) & 0xffff);
        as_rldicr(dest, dest, 32, 31);
        loweronly = false;
    }

    // Now the lower word. Don't clobber the upper word!
    bits &= 0x00000000ffffffff;
    if (bits & 0xffff0000) {
        if (loweronly) {
            xs_lis(dest, bits >> 16);
        } else {
            as_oris(dest, dest, bits >> 16);
        }
        if (bits & 0x0000ffff) {
            as_ori(dest, dest, bits & 0xffff);
        }
    } else if (bits & 0x0000ffff) {
        if (loweronly) {
            xs_li(dest, bits & 0xffff);
        } else {
            as_ori(dest, dest, bits & 0xffff);
        }
    }
}
void
MacroAssemblerPPC64::ma_li(Register dest, ImmWord imm)
{
    ADBlock();
    ma_li(dest, (uint64_t)imm.value);
}

// This generates immediate loads as well, but always in the
// long form so that they can be patched.
void
MacroAssemblerPPC64::ma_liPatchable(Register dest, ImmPtr imm)
{
    ma_liPatchable(dest, ImmWord(uintptr_t(imm.value)));
}

void
MacroAssemblerPPC64::ma_liPatchable(Register dest, ImmWord imm)
{
    // 64-bit load.
    m_buffer.ensureSpace(5 * sizeof(uint32_t));
    xs_lis(dest, Imm16::Upper(Imm32(imm.value >> 32)).encode());
    as_ori(dest, dest, Imm16::Lower(Imm32(imm.value >> 32)).encode());
    as_rldicr(dest, dest, 32, 31);
    as_oris(dest, dest, Imm16::Upper(Imm32(imm.value)).encode());
    as_ori(dest, dest, Imm16::Lower(Imm32(imm.value)).encode());
}

void
MacroAssemblerPPC64::ma_dnegu(Register rd, Register rs)
{
    as_neg(rd, rs);
}

// Shifts
void
MacroAssemblerPPC64::ma_dsll(Register rd, Register rt, Imm32 shift)
{
    MOZ_ASSERT(shift.value < 64);
    as_rldicr(rd, rt, shift.value, 63-(shift.value)); // "sldi"
}

void
MacroAssemblerPPC64::ma_dsrl(Register rd, Register rt, Imm32 shift)
{
    MOZ_ASSERT(shift.value < 64);
    as_rldicl(rd, rt, 64-(shift.value), shift.value); // "srdi"
}

void
MacroAssemblerPPC64::ma_dsll(Register rd, Register rt, Register shift)
{
    as_sld(rd, rt, shift);
}

void
MacroAssemblerPPC64::ma_dsrl(Register rd, Register rt, Register shift)
{
    as_srd(rd, rt, shift);
}

void
MacroAssemblerPPC64::ma_dins(Register rt, Register rs, Imm32 pos, Imm32 size)
{
    as_rldimi(rt, rs, 64-(pos.value + size.value), pos.value);
}

void
MacroAssemblerPPC64::ma_dext(Register rt, Register rs, Imm32 pos, Imm32 size)
{
    // MIPS dext is right-justified, so use rldicl to simulate.
    as_rldicl(rt, rs, (pos.value + size.value), 64 - (size.value));
}

void
MacroAssemblerPPC64::ma_dctz(Register rd, Register rs)
{
    as_cnttzd(rd, rs);
}

// Arithmetic-based ops.

// Add.
void
MacroAssemblerPPC64::ma_add(Register rd, Register rs, Imm32 imm)
{
    MOZ_ASSERT(rs != ScratchRegister);
    if (Imm16::IsInSignedRange(imm.value)) {
        as_addi(rd, rs, imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_add(rd, rs, ScratchRegister);
    }
}

void
MacroAssemblerPPC64::ma_add(Register rd, Register rs)
{
    as_add(rd, rd, rs);
}

void
MacroAssemblerPPC64::ma_add(Register rd, Imm32 imm)
{
    ma_add(rd, rd, imm);
}

void
MacroAssemblerPPC64::ma_addTestOverflow(Register rd, Register rs, Register rt, Label* overflow)
{
    // MIPS clobbers rd, so we can too.
    ADBlock();
    MOZ_ASSERT(rs != ScratchRegister);
    MOZ_ASSERT(rt != ScratchRegister);
    // Whack XER[SO].
    xs_li(ScratchRegister, 0);
    xs_mtxer(ScratchRegister);

    as_addo_rc(rd, rs, rt); // XER[SO] -> CR0[SO]
    ma_bc(Assembler::SOBit, overflow);
}

void
MacroAssemblerPPC64::ma_addTestOverflow(Register rd, Register rs, Imm32 imm, Label* overflow)
{
    // There is no addio, daddy-o, so use the regular overflow, yo.
    ADBlock();
    ma_li(SecondScratchReg, imm);
    ma_addTestOverflow(rd, rs, SecondScratchReg, overflow);
}

// Subtract.
// ma_* subtraction functions invert operand order for as_subf.
void
MacroAssemblerPPC64::ma_dsubu(Register rd, Register rs, Imm32 imm)
{
    MOZ_ASSERT(rs != ScratchRegister);
    if (Imm16::IsInSignedRange(-imm.value)) {
        as_addi(rd, rs, -imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_subf(rd, ScratchRegister, rs); // T = B - A
    }
}

void
MacroAssemblerPPC64::ma_dsubu(Register rd, Register rs)
{
    as_subf(rd, rs, rd); // T = B - A
}

void
MacroAssemblerPPC64::ma_dsubu(Register rd, Imm32 imm)
{
    ma_dsubu(rd, rd, imm);
}

void
MacroAssemblerPPC64::ma_subTestOverflow(Register rd, Register rs, Register rt, Label* overflow)
{
    // MIPS clobbers rd, so we can too.
    ADBlock();
    MOZ_ASSERT(rs != ScratchRegister);
    MOZ_ASSERT(rt != ScratchRegister);
    // Whack XER[SO].
    xs_li(ScratchRegister, 0);
    xs_mtxer(ScratchRegister);

    as_subfo_rc(rd, rt, rs); // T = B - A; XER[SO] -> CR0[SO]
    ma_bc(Assembler::SOBit, overflow);
}

// Memory.

void
MacroAssemblerPPC64::ma_load(Register dest, Address address,
                              LoadStoreSize size, LoadStoreExtension extension)
{
    // ADBlock(); // spammy
    int16_t encodedOffset;
    Register base;
    MOZ_ASSERT(extension == ZeroExtend || extension == SignExtend);

    // XXX: Consider spinning this off into a separate function since the
    // logic gets repeated.
    if (!Imm16::IsInSignedRange(address.offset) || address.base == ScratchRegister) {
        MOZ_ASSERT(address.base != SecondScratchReg);
        ma_li(SecondScratchReg, Imm32(address.offset));
        as_add(SecondScratchReg, address.base, SecondScratchReg);
        base = SecondScratchReg;
        encodedOffset = 0;
    } else {
        MOZ_ASSERT(address.base != ScratchRegister); // "mscdfr0"
        encodedOffset = Imm16(address.offset).encode();
        base = address.base;
    }

    switch (size) {
      case SizeByte:
        as_lbz(dest, base, encodedOffset);
        if (SignExtend == extension)
            as_extsb(dest, dest);
        break;
      case SizeHalfWord:
        as_lhz(dest, base, encodedOffset);
        if (SignExtend == extension)
            as_extsh(dest, dest);
        break;
      case SizeWord:
        as_lwz(dest, base, encodedOffset);
        if (SignExtend == extension)
            as_extsw(dest, dest);
        break;
      case SizeDouble:
        as_ld(dest, base, encodedOffset);
        break;
      default:
        MOZ_CRASH("Invalid argument for ma_load");
    }
}

void
MacroAssemblerPPC64::ma_store(Register data, Address address, LoadStoreSize size,
                               LoadStoreExtension extension)
{
    //ADBlock(); // spammy
    int16_t encodedOffset;
    Register base;

    // XXX: as above
    if (!Imm16::IsInSignedRange(address.offset) || address.base == ScratchRegister) {
        MOZ_ASSERT(address.base != SecondScratchReg);
        ma_li(SecondScratchReg, Imm32(address.offset));
        as_add(SecondScratchReg, address.base, SecondScratchReg);
        base = SecondScratchReg;
        encodedOffset = 0;
    } else {
        MOZ_ASSERT(address.base != ScratchRegister);
        encodedOffset = Imm16(address.offset).encode();
        base = address.base;
    }

    switch (size) {
      case SizeByte:
        as_stb(data, base, encodedOffset);
        break;
      case SizeHalfWord:
        as_sth(data, base, encodedOffset);
        break;
      case SizeWord:
        as_stw(data, base, encodedOffset);
        break;
      case SizeDouble:
        as_std(data, base, encodedOffset);
        break;
      default:
        MOZ_CRASH("Invalid argument for ma_store");
    }
}

void
MacroAssemblerPPC64Compat::computeScaledAddress(const BaseIndex& address, Register dest)
{
    int32_t shift = Imm32::ShiftOf(address.scale).value;
    if (shift) {
        MOZ_ASSERT(address.base != ScratchRegister);
        ma_dsll(ScratchRegister, address.index, Imm32(shift));
        as_add(dest, address.base, ScratchRegister);
    } else {
        as_add(dest, address.base, address.index);
    }
}

void
MacroAssemblerPPC64::ma_pop(Register r)
{
    ADBlock();
    MOZ_ASSERT(sizeof(uintptr_t) == 8);
    as_ld(r, StackPointer, 0);
    as_addi(StackPointer, StackPointer, sizeof(uintptr_t));
}

void
MacroAssemblerPPC64::ma_push(Register r)
{
    ADBlock();
    MOZ_ASSERT(sizeof(uintptr_t) == 8);
    as_stdu(r, StackPointer, (int32_t)-sizeof(intptr_t));
}

// Branches when done from within PPC-specific code.
void
MacroAssemblerPPC64::ma_bc(Condition c, Label* l, JumpKind jumpKind)
{
    // Shorthand for a branch based on CR0.
    ma_bc(cr0, c, l, jumpKind);
}

void
MacroAssemblerPPC64::ma_bc(DoubleCondition c, Label *l, JumpKind jumpKind)
{
    ma_bc(cr1, c, l, jumpKind);
}

void
MacroAssemblerPPC64::ma_bc(DoubleCondition c, FloatRegister lhs,
                           FloatRegister rhs, Label *label, JumpKind jumpKind)
{
    if ((c & DoubleConditionUnordered) || (c == DoubleUnordered))
        as_fcmpu(lhs, rhs);
    else
        as_fcmpo(lhs, rhs);
    ma_bc(c, label, jumpKind);
}

template <typename T>
void
MacroAssemblerPPC64::ma_bc(CRegisterID cr, T c, Label* label, JumpKind jumpKind)
{
    ADBlock();
    // Branch on the condition bit in the specified condition register.
    spew("bc .Llabel %p @ %08x", label, currentOffset());
    if (label->bound()) {
        int32_t offset = label->offset() - m_buffer.nextOffset().getOffset();
        spew("# target offset: %08x (diff: %d)\n", label->offset(), offset);

        if (BOffImm16::IsInSignedRange(offset))
            jumpKind = ShortJump;

        if (jumpKind == ShortJump) {
            MOZ_ASSERT(BOffImm16::IsInSignedRange(offset));
            as_bc(BOffImm16(offset).encode(), c, cr, NotLikelyB, DontLinkB); // likely bits exposed for future expansion
            return;
        }

        // Generate a long branch stanza, but invert the sense so that we usually
        // run a short branch, assuming the "real" branch is not taken.
        m_buffer.ensureSpace(10 * sizeof(uint32_t)); // Worst case if as_bc emits CR twiddle ops.
        as_bc(8 * sizeof(uint32_t), InvertCondition(c), cr, NotLikelyB, DontLinkB);
        //addLongJump(nextOffset(), BufferOffset(label->offset()));
        addLongJump(nextOffset());
        ma_liPatchable(SecondScratchReg, ImmWord(LabelBase::INVALID_OFFSET)); // 5
        xs_mtctr(SecondScratchReg); // 6
        as_bctr(); // 7
        return;
    }

    // Generate open jump and link it to a label.
    // Second word holds a pointer to the next branch in label's chain.
    uint32_t nextInChain = label->used() ? label->offset() : LabelBase::INVALID_OFFSET;

    if (jumpKind == ShortJump) {
        // Store the condition with a dummy branch, plus the next in chain. Unfortunately
        // there is no way to make this take up less than two instructions, so we end up
        // burning a nop at link time. Make the whole branch continuous in the buffer.
        m_buffer.ensureSpace(4 * sizeof(uint32_t));

        // Use a dummy short jump. This includes all the branch encoding, so we just have
        // to change the offset at link time.
        BufferOffset bo = as_bc(4, c, cr, NotLikelyB, DontLinkB);
        spew(".long %08x ; next in chain", nextInChain);
        writeInst(nextInChain);
        if (!oom())
            label->use(bo.getOffset());
        return;
    }

    // As above with a reverse-sense long stanza.
    m_buffer.ensureSpace(10 * sizeof(uint32_t)); // Worst case if as_bc emits CR twiddle ops.
    as_bc(8 * sizeof(uint32_t), InvertCondition(c), cr, NotLikelyB, DontLinkB);
    BufferOffset bo = xs_trap_tagged(LongJumpTag); // encode non-call
    spew(".long %08x ; next in chain", nextInChain);
    // The tagged trap must be the offset, not the leading bc. See Assembler::bind and
    // Assembler::retarget for why.
    writeInst(nextInChain);
    if (!oom())
        label->use(bo.getOffset());
    // Leave space for potential long jump.
    as_nop(); // rldicr
    as_nop(); // oris
    as_nop(); // ori
    as_nop(); // mtctr
    as_nop(); // bctr
}

void
MacroAssemblerPPC64::ma_bc(Register lhs, ImmWord imm, Label* label, Condition c, JumpKind jumpKind)
{
    if (imm.value <= INT32_MAX) {
        ma_bc(lhs, Imm32(uint32_t(imm.value)), label, c, jumpKind);
    } else {
        MOZ_ASSERT(lhs != ScratchRegister);
        ma_li(ScratchRegister, imm);
        ma_bc(lhs, ScratchRegister, label, c, jumpKind);
    }
}

void
MacroAssemblerPPC64::ma_bc(Register lhs, Address addr, Label* label, Condition c, JumpKind jumpKind)
{
    MOZ_ASSERT(lhs != ScratchRegister);
    ma_load(ScratchRegister, addr, SizeDouble);
    ma_bc(lhs, ScratchRegister, label, c, jumpKind);
}

void
MacroAssemblerPPC64::ma_bc(Address addr, Imm32 imm, Label* label, Condition c, JumpKind jumpKind)
{
    ma_load(SecondScratchReg, addr, SizeDouble);
    ma_bc(SecondScratchReg, imm, label, c, jumpKind);
}

void
MacroAssemblerPPC64::ma_bc(Address addr, ImmGCPtr imm, Label* label, Condition c, JumpKind jumpKind)
{
    ma_load(SecondScratchReg, addr, SizeDouble);
    ma_bc(SecondScratchReg, imm, label, c, jumpKind);
}

void
MacroAssemblerPPC64::ma_bal(Label* label) // The whole world has gone MIPS, I tell ya.
{
    ADBlock();

    // Branch to a subroutine.
    spew("bl .Llabel %p", label);
    if (label->bound()) {
        // An entire 7-instruction stanza must be generated so that no matter how this
        // is patched, the return address is the same (i.e., the instruction after the
        // stanza). If this is a short branch, then it's 6 nops with the bl at the end.
        BufferOffset b(label->offset());
        m_buffer.ensureSpace(7 * sizeof(uint32_t));
        BufferOffset dest = nextOffset();
        int64_t offset = (dest.getOffset() + 6*sizeof(uint32_t)) - label->offset();
        if (JOffImm26::IsInRange(offset)) {
            JOffImm26 j(offset);

            as_nop();
            as_nop();
            as_nop();
            as_nop(); // Yawn.
            as_nop();
            as_nop(); // Sigh.
            as_b(j, RelativeBranch, LinkB);
            return;
        }

        // Although this is to Ion code, use r12 to keep calls "as expected."
        addLongJump(dest);
        ma_liPatchable(SecondScratchReg, ImmWord(LabelBase::INVALID_OFFSET));
        xs_mtctr(SecondScratchReg);
        as_bctr(LinkB); // bctrl
        return;
    }

    // Second word holds a pointer to the next branch in label's chain.
    uint32_t nextInChain = label->used() ? label->offset() : LabelBase::INVALID_OFFSET;
    // Keep the whole branch stanza continuous in the buffer.
    m_buffer.ensureSpace(7 * sizeof(uint32_t));
    // Insert a tagged trap so the patcher knows what this is supposed to be.
    BufferOffset bo = xs_trap_tagged(CallTag);
    writeInst(nextInChain);
    if (!oom())
        label->use(bo.getOffset());
    // Leave space for long jump.
    as_nop(); // rldicr
    as_nop(); // oris
    as_nop(); // ori
    as_nop(); // mtctr
    as_nop(); // bctrl
}

void
MacroAssemblerPPC64::ma_cmp_set(Register rd, Register rs, ImmWord imm, Condition c)
{
    if (imm.value <= INT16_MAX) {
        ma_cmp_set(rd, rs, Imm16(uint16_t(imm.value)), c);
    } else {
        ma_li(ScratchRegister, imm);
        ma_cmp_set(rd, rs, ScratchRegister, c);
    }
}

void
MacroAssemblerPPC64::ma_cmp_set(Register rd, Register rs, ImmPtr imm, Condition c)
{
    ma_cmp_set(rd, rs, ImmWord(uintptr_t(imm.value)), c);
}

void
MacroAssemblerPPC64::ma_cmp_set(Register rd, Address addr, Register rs, Condition c)
{
    ma_add(ScratchRegister, addr.base, Imm32(addr.offset));
    ma_cmp_set(rd, ScratchRegister, rs, c);
}

// fp instructions
void
MacroAssemblerPPC64::ma_lid(FloatRegister dest, double value)
{
    ImmWord imm(mozilla::BitwiseCast<uint64_t>(value));

    ma_li(ScratchRegister, imm);
    ma_push(ScratchRegister);
    ma_pop(dest);
}

void
MacroAssemblerPPC64::ma_ls(FloatRegister ft, Address address)
{
    if (Imm16::IsInSignedRange(address.offset)) {
        as_lfs(ft, address.base, address.offset);
    } else {
        MOZ_ASSERT(address.base != ScratchRegister);
        ma_li(ScratchRegister, Imm32(address.offset));
        as_lfsx(ft, address.base, ScratchRegister);
    }
}

void
MacroAssemblerPPC64::ma_ld(FloatRegister ft, Address address)
{
    if (Imm16::IsInSignedRange(address.offset)) {
        as_lfd(ft, address.base, address.offset);
    } else {
        MOZ_ASSERT(address.base != ScratchRegister);
        ma_li(ScratchRegister, Imm32(address.offset));
        as_lfdx(ft, address.base, ScratchRegister);
    }
}

void
MacroAssemblerPPC64::ma_sd(FloatRegister ft, Address address)
{
    if (Imm16::IsInSignedRange(address.offset)) {
        as_stfd(ft, address.base, address.offset);
    } else {
        MOZ_ASSERT(address.base != ScratchRegister);
        ma_li(ScratchRegister, Imm32(address.offset));
        as_stfdx(ft, address.base, ScratchRegister);
    }
}

void
MacroAssemblerPPC64::ma_ss(FloatRegister ft, Address address)
{
    if (Imm16::IsInSignedRange(address.offset)) {
        as_stfs(ft, address.base, address.offset);
    } else {
        MOZ_ASSERT(address.base != ScratchRegister);
        ma_li(ScratchRegister, Imm32(address.offset));
        as_stfsx(ft, address.base, ScratchRegister);
    }
}

void
MacroAssemblerPPC64::ma_pop(FloatRegister f)
{
    as_lfd(f, StackPointer, 0);
    as_addi(StackPointer, StackPointer, sizeof(double));
}

void
MacroAssemblerPPC64::ma_push(FloatRegister f)
{
    as_stfdu(f, StackPointer, (int32_t)-sizeof(double));
}

bool
MacroAssemblerPPC64Compat::buildOOLFakeExitFrame(void* fakeReturnAddr)
{
    uint32_t descriptor = MakeFrameDescriptor(asMasm().framePushed(), FrameType::IonJS,
                                              ExitFrameLayout::Size());

    asMasm().Push(Imm32(descriptor)); // descriptor_
    asMasm().Push(ImmPtr(fakeReturnAddr));

    return true;
}

void
MacroAssemblerPPC64Compat::move32(Imm32 imm, Register dest)
{
    ma_li(dest, imm);
}

void
MacroAssemblerPPC64Compat::move32(Register src, Register dest)
{
    ma_move(dest, src);
}

void
MacroAssemblerPPC64Compat::movePtr(Register src, Register dest)
{
    ma_move(dest, src);
}
void
MacroAssemblerPPC64Compat::movePtr(ImmWord imm, Register dest)
{
    ma_li(dest, imm);
}

void
MacroAssemblerPPC64Compat::movePtr(ImmGCPtr imm, Register dest)
{
    ma_li(dest, imm);
}

void
MacroAssemblerPPC64Compat::movePtr(ImmPtr imm, Register dest)
{
    movePtr(ImmWord(uintptr_t(imm.value)), dest);
}
void
MacroAssemblerPPC64Compat::movePtr(wasm::SymbolicAddress imm, Register dest)
{
    append(wasm::SymbolicAccess(CodeOffset(nextOffset().getOffset()), imm));
    ma_liPatchable(dest, ImmWord(-1));
}

CodeOffset MacroAssembler::moveNearAddressWithPatch(Register dest)
{
    return movWithPatch(ImmPtr(nullptr), dest);
}

void
MacroAssembler::patchNearAddressMove(CodeLocationLabel loc,
                                     CodeLocationLabel target)
{
    PatchDataWithValueCheck(loc, ImmPtr(target.raw()), ImmPtr(nullptr));
}

void
MacroAssemblerPPC64Compat::load8ZeroExtend(const Address& address, Register dest)
{
    ma_load(dest, address, SizeByte, ZeroExtend);
}

void
MacroAssemblerPPC64Compat::load8ZeroExtend(const BaseIndex& src, Register dest)
{
    ma_load(dest, src, SizeByte, ZeroExtend);
}

void
MacroAssemblerPPC64Compat::load8SignExtend(const Address& address, Register dest)
{
    ma_load(dest, address, SizeByte, SignExtend);
}

void
MacroAssemblerPPC64Compat::load8SignExtend(const BaseIndex& src, Register dest)
{
    ma_load(dest, src, SizeByte, SignExtend);
}

void
MacroAssemblerPPC64Compat::load16ZeroExtend(const Address& address, Register dest)
{
    ma_load(dest, address, SizeHalfWord, ZeroExtend);
}

void
MacroAssemblerPPC64Compat::load16ZeroExtend(const BaseIndex& src, Register dest)
{
    ma_load(dest, src, SizeHalfWord, ZeroExtend);
}

void
MacroAssemblerPPC64Compat::load16SignExtend(const Address& address, Register dest)
{
    ma_load(dest, address, SizeHalfWord, SignExtend);
}

void
MacroAssemblerPPC64Compat::load16SignExtend(const BaseIndex& src, Register dest)
{
    ma_load(dest, src, SizeHalfWord, SignExtend);
}

void
MacroAssemblerPPC64Compat::load32(const Address& address, Register dest)
{
    ma_load(dest, address, SizeWord);
}

void
MacroAssemblerPPC64Compat::load32(const BaseIndex& address, Register dest)
{
    ma_load(dest, address, SizeWord);
}

void
MacroAssemblerPPC64Compat::load32(AbsoluteAddress address, Register dest)
{
    movePtr(ImmPtr(address.addr), ScratchRegister);
    load32(Address(ScratchRegister, 0), dest);
}

void
MacroAssemblerPPC64Compat::load32(wasm::SymbolicAddress address, Register dest)
{
    movePtr(address, ScratchRegister);
    load32(Address(ScratchRegister, 0), dest);
}

void
MacroAssemblerPPC64Compat::loadPtr(const Address& address, Register dest)
{
    ma_load(dest, address, SizeDouble);
}

void
MacroAssemblerPPC64Compat::loadPtr(const BaseIndex& src, Register dest)
{
    ma_load(dest, src, SizeDouble);
}

void
MacroAssemblerPPC64Compat::loadPtr(AbsoluteAddress address, Register dest)
{
    movePtr(ImmPtr(address.addr), ScratchRegister);
    loadPtr(Address(ScratchRegister, 0), dest);
}

void
MacroAssemblerPPC64Compat::loadPtr(wasm::SymbolicAddress address, Register dest)
{
    movePtr(address, ScratchRegister);
    loadPtr(Address(ScratchRegister, 0), dest);
}

void
MacroAssemblerPPC64Compat::loadPrivate(const Address& address, Register dest)
{
    loadPtr(address, dest);
    ma_dsll(dest, dest, Imm32(1));
}

void
MacroAssemblerPPC64Compat::loadUnalignedDouble(const wasm::MemoryAccessDesc& access,
                                                const BaseIndex& src, Register temp, FloatRegister dest)
{
    loadDouble(src, dest);
}

void
MacroAssemblerPPC64Compat::loadUnalignedFloat32(const wasm::MemoryAccessDesc& access,
                                                 const BaseIndex& src, Register temp, FloatRegister dest)
{
    loadFloat32(src, dest);
}

void
MacroAssemblerPPC64Compat::store8(Imm32 imm, const Address& address)
{
    ma_li(SecondScratchReg, imm);
    ma_store(SecondScratchReg, address, SizeByte);
}

void
MacroAssemblerPPC64Compat::store8(Register src, const Address& address)
{
    ma_store(src, address, SizeByte);
}

void
MacroAssemblerPPC64Compat::store8(Imm32 imm, const BaseIndex& dest)
{
    ma_store(imm, dest, SizeByte);
}

void
MacroAssemblerPPC64Compat::store8(Register src, const BaseIndex& dest)
{
    ma_store(src, dest, SizeByte);
}

void
MacroAssemblerPPC64Compat::store16(Imm32 imm, const Address& address)
{
    ma_li(SecondScratchReg, imm);
    ma_store(SecondScratchReg, address, SizeHalfWord);
}

void
MacroAssemblerPPC64Compat::store16(Register src, const Address& address)
{
    ma_store(src, address, SizeHalfWord);
}

void
MacroAssemblerPPC64Compat::store16(Imm32 imm, const BaseIndex& dest)
{
    ma_store(imm, dest, SizeHalfWord);
}

void
MacroAssemblerPPC64Compat::store16(Register src, const BaseIndex& address)
{
    ma_store(src, address, SizeHalfWord);
}

void
MacroAssemblerPPC64Compat::store32(Register src, AbsoluteAddress address)
{
    movePtr(ImmPtr(address.addr), ScratchRegister);
    store32(src, Address(ScratchRegister, 0));
}

void
MacroAssemblerPPC64Compat::store32(Register src, const Address& address)
{
    ma_store(src, address, SizeWord);
}

void
MacroAssemblerPPC64Compat::store32(Imm32 src, const Address& address)
{
    move32(src, SecondScratchReg);
    ma_store(SecondScratchReg, address, SizeWord);
}

void
MacroAssemblerPPC64Compat::store32(Imm32 imm, const BaseIndex& dest)
{
    ma_store(imm, dest, SizeWord);
}

void
MacroAssemblerPPC64Compat::store32(Register src, const BaseIndex& dest)
{
    ma_store(src, dest, SizeWord);
}

template <typename T>
void
MacroAssemblerPPC64Compat::storePtr(ImmWord imm, T address)
{
    ma_li(SecondScratchReg, imm);
    ma_store(SecondScratchReg, address, SizeDouble);
}

template void MacroAssemblerPPC64Compat::storePtr<Address>(ImmWord imm, Address address);
template void MacroAssemblerPPC64Compat::storePtr<BaseIndex>(ImmWord imm, BaseIndex address);

template <typename T>
void
MacroAssemblerPPC64Compat::storePtr(ImmPtr imm, T address)
{
    storePtr(ImmWord(uintptr_t(imm.value)), address);
}

template void MacroAssemblerPPC64Compat::storePtr<Address>(ImmPtr imm, Address address);
template void MacroAssemblerPPC64Compat::storePtr<BaseIndex>(ImmPtr imm, BaseIndex address);

template <typename T>
void
MacroAssemblerPPC64Compat::storePtr(ImmGCPtr imm, T address)
{
    movePtr(imm, SecondScratchReg);
    storePtr(SecondScratchReg, address);
}

template void MacroAssemblerPPC64Compat::storePtr<Address>(ImmGCPtr imm, Address address);
template void MacroAssemblerPPC64Compat::storePtr<BaseIndex>(ImmGCPtr imm, BaseIndex address);

void
MacroAssemblerPPC64Compat::storePtr(Register src, const Address& address)
{
    ma_store(src, address, SizeDouble);
}

void
MacroAssemblerPPC64Compat::storePtr(Register src, const BaseIndex& address)
{
    ma_store(src, address, SizeDouble);
}

void
MacroAssemblerPPC64Compat::storePtr(Register src, AbsoluteAddress dest)
{
    movePtr(ImmPtr(dest.addr), ScratchRegister);
    storePtr(src, Address(ScratchRegister, 0));
}

void
MacroAssemblerPPC64Compat::storeUnalignedFloat32(const wasm::MemoryAccessDesc& access,
                                                  FloatRegister src, Register temp, const BaseIndex& dest)
{
    computeScaledAddress(dest, SecondScratchReg);

    as_stfs(src, SecondScratchReg, 0);
    //append(access, store.getOffset());
}

void
MacroAssemblerPPC64Compat::storeUnalignedDouble(const wasm::MemoryAccessDesc& access,
                                                 FloatRegister src, Register temp, const BaseIndex& dest)
{
    computeScaledAddress(dest, SecondScratchReg);

    as_stfd(src, SecondScratchReg, 0);
    //append(access, store.getOffset());
}

void
MacroAssembler::clampDoubleToUint8(FloatRegister input, Register output)
{
    ADBlock();
    Label done, tooLow;

    as_fctiwz(ScratchDoubleReg, input);

    as_addi(ScratchRegister, StackPointer, -4);
    as_stfiwx(ScratchDoubleReg, r0, ScratchRegister);
    as_lwz(output, ScratchRegister, 0);
    as_cmplwi(output, 255);
    ma_bc(LessThanOrEqual, &done, ShortJump);
    as_cmpwi(output, 0);
    ma_bc(LessThan, &tooLow, ShortJump);
    as_ori(output, r0, 255);
    ma_b(&done, ShortJump);
    bind(&tooLow);
    as_ori(output, r0, 0);
    bind(&done);
}

void
MacroAssemblerPPC64Compat::testNullSet(Condition cond, const ValueOperand& value, Register dest)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    splitTag(value, SecondScratchReg);
    ma_cmp_set(dest, SecondScratchReg, ImmTag(JSVAL_TAG_NULL), cond);
}

void
MacroAssemblerPPC64Compat::testObjectSet(Condition cond, const ValueOperand& value, Register dest)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    splitTag(value, SecondScratchReg);
    ma_cmp_set(dest, SecondScratchReg, ImmTag(JSVAL_TAG_OBJECT), cond);
}

void
MacroAssemblerPPC64Compat::testUndefinedSet(Condition cond, const ValueOperand& value, Register dest)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    splitTag(value, SecondScratchReg);
    ma_cmp_set(dest, SecondScratchReg, ImmTag(JSVAL_TAG_UNDEFINED), cond);
}

void
MacroAssemblerPPC64Compat::unboxInt32(const ValueOperand& operand, Register dest)
{
    Register src = operand.valueReg();
    as_or(dest, src, src);
}

void
MacroAssemblerPPC64Compat::unboxInt32(Register src, Register dest)
{
    as_or(dest, src, src);
}

void
MacroAssemblerPPC64Compat::unboxInt32(const Address& src, Register dest)
{
    load32(Address(src.base, src.offset), dest);
}

void
MacroAssemblerPPC64Compat::unboxInt32(const BaseIndex& src, Register dest)
{
    computeScaledAddress(src, SecondScratchReg);
    load32(Address(SecondScratchReg, src.offset), dest);
}

void
MacroAssemblerPPC64Compat::unboxBoolean(const ValueOperand& operand, Register dest)
{
    ma_dext(dest, operand.valueReg(), Imm32(0), Imm32(32));
}

void
MacroAssemblerPPC64Compat::unboxBoolean(Register src, Register dest)
{
    ma_dext(dest, src, Imm32(0), Imm32(32));
}

void
MacroAssemblerPPC64Compat::unboxBoolean(const Address& src, Register dest)
{
    ma_load(dest, Address(src.base, src.offset), SizeWord, ZeroExtend);
}

void
MacroAssemblerPPC64Compat::unboxBoolean(const BaseIndex& src, Register dest)
{
    computeScaledAddress(src, SecondScratchReg);
    ma_load(dest, Address(SecondScratchReg, src.offset), SizeWord, ZeroExtend);
}

void
MacroAssemblerPPC64Compat::unboxDouble(const ValueOperand& operand, FloatRegister dest)
{
    ma_push(operand.valueReg());
    ma_pop(dest);
}

void
MacroAssemblerPPC64Compat::unboxDouble(const Address& src, FloatRegister dest)
{
    ma_ld(dest, Address(src.base, src.offset));
}

void
MacroAssemblerPPC64Compat::unboxDouble(const BaseIndex& src, FloatRegister dest)
{
    computeScaledAddress(src, ScratchRegister);
    ma_ld(dest, Address(ScratchRegister, src.offset));
}

void
MacroAssemblerPPC64Compat::unboxString(const ValueOperand& operand, Register dest)
{
    unboxNonDouble(operand, dest, JSVAL_TYPE_STRING);
}

void
MacroAssemblerPPC64Compat::unboxString(Register src, Register dest)
{
    unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
}

void
MacroAssemblerPPC64Compat::unboxString(const Address& src, Register dest)
{
    unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
}

void
MacroAssemblerPPC64Compat::unboxSymbol(const ValueOperand& operand, Register dest)
{
    unboxNonDouble(operand, dest, JSVAL_TYPE_SYMBOL);
}

void
MacroAssemblerPPC64Compat::unboxSymbol(Register src, Register dest)
{
    unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
}

void
MacroAssemblerPPC64Compat::unboxSymbol(const Address& src, Register dest)
{
    unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
}

void
MacroAssemblerPPC64Compat::unboxObject(const ValueOperand& src, Register dest)
{
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
}

void
MacroAssemblerPPC64Compat::unboxObject(Register src, Register dest)
{
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
}

void
MacroAssemblerPPC64Compat::unboxObject(const Address& src, Register dest)
{
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
}

void
MacroAssemblerPPC64Compat::unboxValue(const ValueOperand& src, AnyRegister dest, JSValueType type)
{
    if (dest.isFloat()) {
        Label notInt32, end;
        asMasm().branchTestInt32(Assembler::NotEqual, src, &notInt32);
        convertInt32ToDouble(src.valueReg(), dest.fpu());
        ma_b(&end, ShortJump);
        bind(&notInt32);
        unboxDouble(src, dest.fpu());
        bind(&end);
    } else {
        unboxNonDouble(src, dest.gpr(), type);
    }
}

void
MacroAssemblerPPC64Compat::unboxPrivate(const ValueOperand& src, Register dest)
{
    ma_dsll(dest, src.valueReg(), Imm32(1));
}

void
MacroAssemblerPPC64Compat::boxDouble(FloatRegister src, const ValueOperand& dest, FloatRegister)
{
    ma_push(src);
    ma_pop(dest.valueReg());
}

void MacroAssemblerPPC64Compat::unboxBigInt(const ValueOperand& operand,
                                            Register dest) {
  unboxNonDouble(operand, dest, JSVAL_TYPE_BIGINT);
}

void MacroAssemblerPPC64Compat::unboxBigInt(const Address& src,
                                            Register dest) {
  unboxNonDouble(src, dest, JSVAL_TYPE_BIGINT);
}

void
MacroAssemblerPPC64Compat::boxNonDouble(JSValueType type, Register src,
                                         const ValueOperand& dest)
{
    MOZ_ASSERT(src != dest.valueReg());
    boxValue(type, src, dest.valueReg());
}

void
MacroAssemblerPPC64Compat::boolValueToDouble(const ValueOperand& operand, FloatRegister dest)
{
    convertBoolToInt32(operand.valueReg(), ScratchRegister);
    convertInt32ToDouble(ScratchRegister, dest);
}

void
MacroAssemblerPPC64Compat::int32ValueToDouble(const ValueOperand& operand,
                                               FloatRegister dest)
{
    convertInt32ToDouble(operand.valueReg(), dest);
}

void
MacroAssemblerPPC64Compat::boolValueToFloat32(const ValueOperand& operand,
                                               FloatRegister dest)
{

    convertBoolToInt32(operand.valueReg(), ScratchRegister);
    convertInt32ToFloat32(ScratchRegister, dest);
}

void
MacroAssemblerPPC64Compat::int32ValueToFloat32(const ValueOperand& operand,
                                                FloatRegister dest)
{
    convertInt32ToFloat32(operand.valueReg(), dest);
}

void
MacroAssemblerPPC64Compat::loadConstantFloat32(float f, FloatRegister dest)
{
    ma_lis(dest, f);
}

void
MacroAssemblerPPC64Compat::loadInt32OrDouble(const Address& src, FloatRegister dest)
{
    ADBlock();
    Label notInt32, end;
    // If it's an int, convert it to double.
    loadPtr(Address(src.base, src.offset), ScratchRegister);
    ma_dsrl(SecondScratchReg, ScratchRegister, Imm32(JSVAL_TAG_SHIFT));
    asMasm().branchTestInt32(Assembler::NotEqual, SecondScratchReg, &notInt32);
    loadPtr(Address(src.base, src.offset), SecondScratchReg);
    convertInt32ToDouble(SecondScratchReg, dest);
    ma_b(&end, ShortJump);

    // Not an int, just load as double.
    bind(&notInt32);
    ma_ld(dest, src);
    bind(&end);
}

void
MacroAssemblerPPC64Compat::loadInt32OrDouble(const BaseIndex& addr, FloatRegister dest)
{
    ADBlock();
    Label notInt32, end;

    // If it's an int, convert it to double.
    computeScaledAddress(addr, SecondScratchReg);
    // Since we only have one scratch, we need to stomp over it with the tag.
    loadPtr(Address(SecondScratchReg, 0), ScratchRegister);
    ma_dsrl(SecondScratchReg, ScratchRegister, Imm32(JSVAL_TAG_SHIFT));
    asMasm().branchTestInt32(Assembler::NotEqual, SecondScratchReg, &notInt32);

    computeScaledAddress(addr, SecondScratchReg);
    loadPtr(Address(SecondScratchReg, 0), SecondScratchReg);
    convertInt32ToDouble(SecondScratchReg, dest);
    ma_b(&end, ShortJump);

    // Not an int, just load as double.
    bind(&notInt32);
    // First, recompute the offset that had been stored in the scratch register
    // since the scratch register was overwritten loading in the type.
    computeScaledAddress(addr, SecondScratchReg);
    loadDouble(Address(SecondScratchReg, 0), dest);
    bind(&end);
}

void
MacroAssemblerPPC64Compat::loadConstantDouble(double dp, FloatRegister dest)
{
    ma_lid(dest, dp);
}

Register
MacroAssemblerPPC64Compat::extractObject(const Address& address, Register scratch)
{
    loadPtr(Address(address.base, address.offset), scratch);
    ma_dext(scratch, scratch, Imm32(0), Imm32(JSVAL_TAG_SHIFT));
    return scratch;
}

Register
MacroAssemblerPPC64Compat::extractTag(const Address& address, Register scratch)
{
    loadPtr(Address(address.base, address.offset), scratch);
    ma_dext(scratch, scratch, Imm32(JSVAL_TAG_SHIFT), Imm32(64 - JSVAL_TAG_SHIFT));
    return scratch;
}

Register
MacroAssemblerPPC64Compat::extractTag(const BaseIndex& address, Register scratch)
{
    computeScaledAddress(address, scratch);
    return extractTag(Address(scratch, address.offset), scratch);
}

/////////////////////////////////////////////////////////////////
// X86/X64-common/ARM/MIPS interface.
/////////////////////////////////////////////////////////////////
void
MacroAssemblerPPC64Compat::storeValue(ValueOperand val, Operand dst)
{
    storeValue(val, Address(Register::FromCode(dst.base()), dst.disp()));
}

void
MacroAssemblerPPC64Compat::storeValue(ValueOperand val, const BaseIndex& dest)
{
    computeScaledAddress(dest, SecondScratchReg);
    storeValue(val, Address(SecondScratchReg, dest.offset));
}

void
MacroAssemblerPPC64Compat::storeValue(JSValueType type, Register reg, BaseIndex dest)
{
    computeScaledAddress(dest, ScratchRegister);

    int32_t offset = dest.offset;
    if (!Imm16::IsInSignedRange(offset)) {
        ma_li(SecondScratchReg, Imm32(offset));
        as_add(ScratchRegister, ScratchRegister, SecondScratchReg);
        offset = 0;
    }

    storeValue(type, reg, Address(ScratchRegister, offset));
}

void
MacroAssemblerPPC64Compat::storeValue(ValueOperand val, const Address& dest)
{
    storePtr(val.valueReg(), Address(dest.base, dest.offset));
}

void
MacroAssemblerPPC64Compat::storeValue(JSValueType type, Register reg, Address dest)
{
    MOZ_ASSERT(dest.base != SecondScratchReg);

    if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
        store32(reg, dest);
        JSValueShiftedTag tag = (JSValueShiftedTag)JSVAL_TYPE_TO_SHIFTED_TAG(type);
        store32(((Imm64(tag)).secondHalf()), Address(dest.base, dest.offset + 4));
    } else {
        ma_li(SecondScratchReg, ImmTag(JSVAL_TYPE_TO_TAG(type)));
        ma_dsll(SecondScratchReg, SecondScratchReg, Imm32(JSVAL_TAG_SHIFT));
        ma_dins(SecondScratchReg, reg, Imm32(0), Imm32(JSVAL_TAG_SHIFT));
        storePtr(SecondScratchReg, Address(dest.base, dest.offset));
    }
}

void
MacroAssemblerPPC64Compat::storeValue(const Value& val, Address dest)
{
    if (val.isGCThing()) {
        writeDataRelocation(val);
        movWithPatch(ImmWord(val.asRawBits()), SecondScratchReg);
    } else {
        ma_li(SecondScratchReg, ImmWord(val.asRawBits()));
    }
    storePtr(SecondScratchReg, Address(dest.base, dest.offset));
}

void
MacroAssemblerPPC64Compat::storeValue(const Value& val, BaseIndex dest)
{
    computeScaledAddress(dest, ScratchRegister);

    int32_t offset = dest.offset;
    if (!Imm16::IsInSignedRange(offset)) {
        ma_li(SecondScratchReg, Imm32(offset));
        as_add(ScratchRegister, ScratchRegister, SecondScratchReg);
        offset = 0;
    }
    storeValue(val, Address(ScratchRegister, offset));
}

void
MacroAssemblerPPC64Compat::loadValue(const BaseIndex& addr, ValueOperand val)
{
    computeScaledAddress(addr, SecondScratchReg);
    loadValue(Address(SecondScratchReg, addr.offset), val);
}

void
MacroAssemblerPPC64Compat::loadValue(Address src, ValueOperand val)
{
    loadPtr(Address(src.base, src.offset), val.valueReg());
}

void
MacroAssemblerPPC64Compat::tagValue(JSValueType type, Register payload, ValueOperand dest)
{
    MOZ_ASSERT(dest.valueReg() != ScratchRegister);
    if (payload != dest.valueReg())
      ma_move(dest.valueReg(), payload);
    ma_li(ScratchRegister, ImmTag(JSVAL_TYPE_TO_TAG(type)));
    ma_dins(dest.valueReg(), ScratchRegister, Imm32(JSVAL_TAG_SHIFT), Imm32(64 - JSVAL_TAG_SHIFT));
}

void
MacroAssemblerPPC64Compat::pushValue(ValueOperand val)
{
    // Allocate stack slots for Value. One for each.
    asMasm().subPtr(Imm32(sizeof(Value)), StackPointer);
    // Store Value
    storeValue(val, Address(StackPointer, 0));
}

void
MacroAssemblerPPC64Compat::pushValue(const Address& addr)
{
    // Load value before allocate stack, addr.base may be is sp.
    loadPtr(Address(addr.base, addr.offset), ScratchRegister);
    ma_dsubu(StackPointer, StackPointer, Imm32(sizeof(Value)));
    storePtr(ScratchRegister, Address(StackPointer, 0));
}

void
MacroAssemblerPPC64Compat::popValue(ValueOperand val)
{
    as_ld(val.valueReg(), StackPointer, 0);
    as_addi(StackPointer, StackPointer, sizeof(Value));
}

void
MacroAssemblerPPC64Compat::breakpoint()
{
    xs_trap();
}

void
MacroAssemblerPPC64Compat::ensureDouble(const ValueOperand& source, FloatRegister dest,
                                         Label* failure)
{
    Label isDouble, done;
    {
        ScratchTagScope tag(asMasm(), source);
        splitTagForTest(source, tag);
        asMasm().branchTestDouble(Assembler::Equal, tag, &isDouble);
        asMasm().branchTestInt32(Assembler::NotEqual, tag, failure);
    }

    unboxInt32(source, ScratchRegister);
    convertInt32ToDouble(ScratchRegister, dest);
    jump(&done);

    bind(&isDouble);
    unboxDouble(source, dest);

    bind(&done);
}

void
MacroAssemblerPPC64Compat::checkStackAlignment()
{
#ifdef DEBUG
    Label aligned;
    as_andi_rc(ScratchRegister, sp, ABIStackAlignment - 1);
    xs_trap();
#if(0)
    ma_bc(ScratchRegister, Zero, &aligned, Equal, ShortJump);
    as_break(BREAK_STACK_UNALIGNED);
    bind(&aligned);
#endif
#endif
}

void
MacroAssemblerPPC64Compat::handleFailureWithHandlerTail(Label* profilerExitTail)
{
    // Reserve space for exception information.
    int size = (sizeof(ResumeFromException) + ABIStackAlignment) & ~(ABIStackAlignment - 1);
    asMasm().subPtr(Imm32(size), StackPointer);
    ma_move(r3, StackPointer); // Use r3 since it is a first function argument

    using Fn = void (*)(ResumeFromException * rfe);
    // Call the handler.
    asMasm().setupUnalignedABICall(r4);
    asMasm().passABIArg(r3);
    asMasm().callWithABI<Fn, HandleException>(MoveOp::GENERAL,
            CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    Label entryFrame;
    Label catch_;
    Label finally;
    Label return_;
    Label bailout;
    Label wasm;

    // Already clobbered r3, so use it...
    load32(Address(StackPointer, offsetof(ResumeFromException, kind)), r3);
    asMasm().branch32(Assembler::Equal, r3, Imm32(ResumeFromException::RESUME_ENTRY_FRAME),
                      &entryFrame);
    asMasm().branch32(Assembler::Equal, r3, Imm32(ResumeFromException::RESUME_CATCH), &catch_);
    asMasm().branch32(Assembler::Equal, r3, Imm32(ResumeFromException::RESUME_FINALLY), &finally);
    asMasm().branch32(Assembler::Equal, r3, Imm32(ResumeFromException::RESUME_FORCED_RETURN),
                      &return_);
    asMasm().branch32(Assembler::Equal, r3, Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);
    asMasm().branch32(Assembler::Equal, r3, Imm32(ResumeFromException::RESUME_WASM), &wasm);

    breakpoint(); // Invalid kind.

    // No exception handler. Load the error value, load the new stack pointer
    // and return from the entry frame.
    bind(&entryFrame);
    asMasm().moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)), StackPointer);

    // We're going to be returning by the ion calling convention
    ma_pop(ScratchRegister);
    xs_mtlr(ScratchRegister);
    as_blr();

    // If we found a catch handler, this must be a baseline frame. Restore
    // state and jump to the catch block.
    bind(&catch_);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, target)), r3);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, framePointer)), BaselineFrameReg);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)), StackPointer);
    jump(r3);

    // If we found a finally block, this must be a baseline frame. Push
    // two values expected by JSOP_RETSUB: BooleanValue(true) and the
    // exception.
    bind(&finally);
    ValueOperand exception = ValueOperand(r4);
    loadValue(Address(sp, offsetof(ResumeFromException, exception)), exception);

    loadPtr(Address(sp, offsetof(ResumeFromException, target)), r3);
    loadPtr(Address(sp, offsetof(ResumeFromException, framePointer)), BaselineFrameReg);
    loadPtr(Address(sp, offsetof(ResumeFromException, stackPointer)), sp);

    pushValue(BooleanValue(true));
    pushValue(exception);
    jump(r3);

    // Only used in debug mode. Return BaselineFrame->returnValue() to the
    // caller.
    bind(&return_);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, framePointer)), BaselineFrameReg);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)), StackPointer);
    loadValue(Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfReturnValue()),
              JSReturnOperand);
    ma_move(StackPointer, BaselineFrameReg);
    pop(BaselineFrameReg);

    // If profiling is enabled, then update the lastProfilingFrame to refer to caller
    // frame before returning.
    {
        Label skipProfilingInstrumentation;
        // Test if profiler enabled.
        AbsoluteAddress addressOfEnabled(GetJitContext()->runtime->geckoProfiler().addressOfEnabled());
        asMasm().branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                          &skipProfilingInstrumentation);
        jump(profilerExitTail);
        bind(&skipProfilingInstrumentation);
    }

    ret();

    // If we are bailing out to baseline to handle an exception, jump to
    // the bailout tail stub.
    bind(&bailout);
    loadPtr(Address(sp, offsetof(ResumeFromException, bailoutInfo)), r5);
    ma_li(ReturnReg, Imm32(1));
    loadPtr(Address(sp, offsetof(ResumeFromException, target)), r4);
    jump(r4);

    // If we are throwing and the innermost frame was a wasm frame, reset SP and
    // FP; SP is pointing to the unwound return address to the wasm entry, so
    // we can just ret().
    bind(&wasm);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, framePointer)), FramePointer);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)), StackPointer);
    ret();
}

CodeOffset
MacroAssemblerPPC64Compat::toggledJump(Label* label)
{
    CodeOffset ret(nextOffset().getOffset());
    ma_b(label);
    return ret;
}

CodeOffset
MacroAssemblerPPC64Compat::toggledCall(JitCode* target, bool enabled)
{
    BufferOffset bo = nextOffset();
    CodeOffset offset(bo.getOffset());
    addPendingJump(bo, ImmPtr(target->raw()), RelocationKind::JITCODE);
    ma_liPatchable(ScratchRegister, ImmPtr(target->raw()));
    if (enabled) {
        xs_mtctr(ScratchRegister);
        as_bctr(LinkB);
        as_nop();
    } else {
        as_nop();
        as_nop();
    }
    MOZ_ASSERT_IF(!oom(), nextOffset().getOffset() - offset.offset() == ToggledCallSize(nullptr));
    return offset;
}

void
MacroAssemblerPPC64Compat::profilerEnterFrame(Register framePtr, Register scratch)
{
    asMasm().loadJSContext(scratch);
    loadPtr(Address(scratch, offsetof(JSContext, profilingActivation_)), scratch);
    storePtr(framePtr, Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
    storePtr(ImmPtr(nullptr), Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void
MacroAssemblerPPC64Compat::profilerExitFrame()
{
    jump(GetJitContext()->runtime->jitRuntime()->getProfilerExitFrameTail());
}

void
MacroAssembler::subFromStackPtr(Imm32 imm32)
{
    if (imm32.value)
        asMasm().subPtr(imm32, StackPointer);
}

//{{{ check_macroassembler_style
// ===============================================================
// Stack manipulation functions.

// XXX: Check usage of this routine in Ion and see what assumes LR is a GPR. If so, then
// maybe we need to find a way to abstract away SPRs vs GPRs after all.
void
MacroAssembler::PushRegsInMask(LiveRegisterSet set)
{
    int32_t diff = set.gprs().size() * sizeof(intptr_t) +
        set.fpus().getPushSizeInBytes();
    const int32_t reserved = diff;

    reserveStack(reserved);
    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
        diff -= sizeof(intptr_t);
        storePtr(*iter, Address(StackPointer, diff));
    }
    for (FloatRegisterBackwardIterator iter(set.fpus().reduceSetForPush()); iter.more(); ++iter) {
        diff -= sizeof(double);
        storeDouble(*iter, Address(StackPointer, diff));
    }
    MOZ_ASSERT(diff == 0);
}

void
MacroAssembler::PopRegsInMaskIgnore(LiveRegisterSet set, LiveRegisterSet ignore)
{
    int32_t diff = set.gprs().size() * sizeof(intptr_t) +
        set.fpus().getPushSizeInBytes();
    const int32_t reserved = diff;

    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
        diff -= sizeof(intptr_t);
        if (!ignore.has(*iter))
          loadPtr(Address(StackPointer, diff), *iter);
    }
    for (FloatRegisterBackwardIterator iter(set.fpus().reduceSetForPush()); iter.more(); ++iter) {
        diff -= sizeof(double);
        if (!ignore.has(*iter))
          loadDouble(Address(StackPointer, diff), *iter);
    }
    MOZ_ASSERT(diff == 0);
    freeStack(reserved);
}

void
MacroAssembler::storeRegsInMask(LiveRegisterSet set, Address dest, Register)
{
    FloatRegisterSet fpuSet(set.fpus().reduceSetForPush());
    unsigned numFpu = fpuSet.size();
    int32_t diffF = fpuSet.getPushSizeInBytes();
    int32_t diffG = set.gprs().size() * sizeof(intptr_t);

    MOZ_ASSERT(dest.offset >= diffG + diffF);

    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
        diffG -= sizeof(intptr_t);
        dest.offset -= sizeof(intptr_t);
        storePtr(*iter, dest);
    }
    MOZ_ASSERT(diffG == 0);

    for (FloatRegisterBackwardIterator iter(fpuSet); iter.more(); ++iter) {
        FloatRegister reg = *iter;
        diffF -= reg.size();
        numFpu -= 1;
        dest.offset -= reg.size();
        if (reg.isDouble())
            storeDouble(reg, dest);
        else if (reg.isSingle())
            storeFloat32(reg, dest);
        else
            MOZ_CRASH("Unknown register type.");
    }
    MOZ_ASSERT(numFpu == 0);
    diffF -= diffF % sizeof(uintptr_t);
    MOZ_ASSERT(diffF == 0);
}
// ===============================================================
// ABI function calls.

void
MacroAssembler::setupUnalignedABICall(Register scratch)
{
    ADBlock();
    MOZ_ASSERT(!IsCompilingWasm(), "wasm should only use aligned ABI calls"); // XXX?? arm doesn't do this
    setupNativeABICall();
    dynamicAlignment_ = true;

    ma_move(scratch, StackPointer);

    // Save SP. (XXX: ARM saves LR here too?)
    asMasm().subPtr(Imm32(sizeof(uintptr_t)), StackPointer);
    ma_and(StackPointer, StackPointer, Imm32(~(ABIStackAlignment - 1)));
    storePtr(scratch, Address(StackPointer, 0));
}

void
MacroAssembler::callWithABIPre(uint32_t* stackAdjust, bool callFromWasm)
{
    ADBlock();
    MOZ_ASSERT(inCall_);
    uint32_t stackForCall = abiArgs_.stackBytesConsumedSoFar();

    // Reserve place for LR.
    stackForCall += sizeof(intptr_t);

    if (dynamicAlignment_) {
        stackForCall += ComputeByteAlignment(stackForCall, ABIStackAlignment);
    } else {
        uint32_t alignmentAtPrologue = callFromWasm ? sizeof(wasm::Frame) : 0;
        stackForCall += ComputeByteAlignment(stackForCall + framePushed() + alignmentAtPrologue,
                                             ABIStackAlignment);
    }

    *stackAdjust = stackForCall;
    reserveStack(stackForCall);

    // Position all arguments.
    {
        enoughMemory_ &= moveResolver_.resolve();
        if (!enoughMemory_)
            return;

        MoveEmitter emitter(*this);
        emitter.emit(moveResolver_);
        emitter.finish();
    }

    // SP is now set, so save LR in the frame.
    xs_mflr(ScratchRegister);
    storePtr(ScratchRegister, Address(StackPointer, 0));

    assertStackAlignment(ABIStackAlignment);
}

void
MacroAssembler::callWithABIPost(uint32_t stackAdjust, MoveOp::Type result, bool callFromWasm)
{
    ADBlock();
    // Restore LR.
    loadPtr(Address(StackPointer, 0), ScratchRegister);
    xs_mtlr(ScratchRegister);

    if (dynamicAlignment_) {
        // Restore sp value from stack (as stored in setupUnalignedABICall()).
        loadPtr(Address(StackPointer, stackAdjust), StackPointer);
        // Use adjustFrame instead of freeStack because we already restored sp.
        adjustFrame(-stackAdjust);
    } else {
        freeStack(stackAdjust);
    }

#ifdef DEBUG
    MOZ_ASSERT(inCall_);
    inCall_ = false;
#endif
}

void
MacroAssembler::callWithABINoProfiler(Register fun, MoveOp::Type result)
{
    // Load the callee in t9, no instruction between the lw and call
    // should clobber it. Note that we can't use fun.base because it may
    // be one of the IntArg registers clobbered before the call.
    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    call(fun);
    callWithABIPost(stackAdjust, result);
}

void
MacroAssembler::callWithABINoProfiler(const Address& fun, MoveOp::Type result)
{
    // Load the callee in t9, as above.
    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    loadPtr(Address(fun.base, fun.offset), ScratchRegister);
    call(ScratchRegister);
    callWithABIPost(stackAdjust, result);
}

// ===============================================================
// Move

void
MacroAssembler::moveValue(const TypedOrValueRegister& src, const ValueOperand& dest)
{
    if (src.hasValue()) {
        moveValue(src.valueReg(), dest);
        return;
    }

    MIRType type = src.type();
    AnyRegister reg = src.typedReg();

    if (!IsFloatingPointType(type)) {
        boxNonDouble(ValueTypeFromMIRType(type), reg.gpr(), dest);
        return;
    }

    FloatRegister scratch = ScratchDoubleReg;
    FloatRegister freg = reg.fpu();
    if (type == MIRType::Float32) {
        convertFloat32ToDouble(freg, scratch);
        freg = scratch;
    }
    boxDouble(freg, dest, scratch);
}

void
MacroAssembler::moveValue(const ValueOperand& src, const ValueOperand& dest)
{
    if (src == dest)
        return;
    movePtr(src.valueReg(), dest.valueReg());
}

void
MacroAssembler::moveValue(const Value& src, const ValueOperand& dest)
{
    if(!src.isGCThing()) {
        ma_li(dest.valueReg(), ImmWord(src.asRawBits()));
        return;
    }

    writeDataRelocation(src);
    movWithPatch(ImmWord(src.asRawBits()), dest.valueReg());
}

// ===============================================================
// Branch functions

#if 0
void
MacroAssembler::branchValueIsNurseryObject(Condition cond, ValueOperand value,
                                           Register temp, Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

    Label done;
    branchTestObject(Assembler::NotEqual, value, cond == Assembler::Equal ? &done : label);

    unboxObject(value, SecondScratchReg);
    orPtr(Imm32(gc::ChunkMask), SecondScratchReg);
    branch32(cond, Address(SecondScratchReg, gc::ChunkLocationOffsetFromLastByte),
             Imm32(int32_t(gc::ChunkLocation::Nursery)), label);

    bind(&done);
}
#endif

void
MacroAssembler::branchValueIsNurseryCell(Condition cond, const Address& address, Register temp,
                                         Label* label)
{
    MOZ_ASSERT(temp != InvalidReg);
    loadValue(address, ValueOperand(temp));
    branchValueIsNurseryCell(cond, ValueOperand(temp), InvalidReg, label);
}

void
MacroAssembler::branchValueIsNurseryCell(Condition cond, ValueOperand value, Register temp,
                                         Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

    Label done, checkAddress, checkObjectAddress;
    SecondScratchRegisterScope scratch2(*this);

    splitTag(value, scratch2);
    branchTestObject(Assembler::Equal, scratch2, &checkObjectAddress);
    branchTestString(Assembler::NotEqual, scratch2, cond == Assembler::Equal ? &done : label);

    unboxString(value, scratch2);
    jump(&checkAddress);

    bind(&checkObjectAddress);
    unboxObject(value, scratch2);

    bind(&checkAddress);
    orPtr(Imm32(gc::ChunkMask), scratch2);
    load32(Address(scratch2, gc::ChunkLocationOffsetFromLastByte), scratch2);
    branch32(cond, scratch2, Imm32(int32_t(gc::ChunkLocation::Nursery)), label);

    bind(&done);
}

void
MacroAssembler::branchTestValue(Condition cond, const ValueOperand& lhs,
                                const Value& rhs, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ScratchRegisterScope scratch(*this);
    MOZ_ASSERT(lhs.valueReg() != scratch);
    moveValue(rhs, ValueOperand(scratch));
    ma_bc(lhs.valueReg(), scratch, label, cond);
}

// ========================================================================
// Memory access primitives.
template <typename T>
void
MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value, MIRType valueType,
                                  const T& dest, MIRType slotType)
{
    if (valueType == MIRType::Double) {
        storeDouble(value.reg().typedReg().fpu(), dest);
        return;
    }

    // For known integers and booleans, we can just store the unboxed value if
    // the slot has the same type.
    if ((valueType == MIRType::Int32 || valueType == MIRType::Boolean) && slotType == valueType) {
        if (value.constant()) {
            Value val = value.value();
            if (valueType == MIRType::Int32)
                store32(Imm32(val.toInt32()), dest);
            else
                store32(Imm32(val.toBoolean() ? 1 : 0), dest);
        } else {
            store32(value.reg().typedReg().gpr(), dest);
        }
        return;
    }

    if (value.constant())
        storeValue(value.value(), dest);
    else
        storeValue(ValueTypeFromMIRType(valueType), value.reg().typedReg().gpr(), dest);
}

template void
MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value, MIRType valueType,
                                  const Address& dest, MIRType slotType);
template void
MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value, MIRType valueType,
                                  const BaseIndex& dest, MIRType slotType);
template void
MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value, MIRType valueType,
                                  const BaseObjectElementIndex& dest, MIRType slotType);

void
MacroAssembler::PushBoxed(FloatRegister reg)
{
    subFromStackPtr(Imm32(sizeof(double)));
    boxDouble(reg, Address(getStackPointer(), 0));
    adjustFrame(sizeof(double));
}


void
MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                  Register boundsCheckLimit, Label* label)
{
    ma_bc(index, boundsCheckLimit, label, cond);
}

void
MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                  Address boundsCheckLimit, Label* label)
{
    SecondScratchRegisterScope scratch2(*this);
    load32(boundsCheckLimit, SecondScratchReg);
    ma_bc(index, SecondScratchReg, label, cond);
}

void
MacroAssembler::wasmTruncateDoubleToUInt32(FloatRegister input, Register output, bool isSaturating,
                                           Label* oolEntry)
{
    as_fctiwu(ScratchDoubleReg, input);
    ma_push(ScratchDoubleReg);
    ma_pop(output);
    ma_bc(ScratchRegister, Imm32(0), oolEntry, Assembler::NotEqual);
}

void
MacroAssembler::wasmTruncateFloat32ToUInt32(FloatRegister input, Register output, bool isSaturating,
                                            Label* oolEntry)
{
    /* On PowerPC FP registers are always 64-bit, so no difference here. */
    wasmTruncateDoubleToUInt32(input, output, isSaturating, oolEntry);
}

void
MacroAssembler::wasmLoadI64(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                            Register ptrScratch, Register64 output)
{
    wasmLoadI64Impl(access, memoryBase, ptr, ptrScratch, output, InvalidReg);
}

void
MacroAssembler::wasmUnalignedLoadI64(const wasm::MemoryAccessDesc& access, Register memoryBase,
                                     Register ptr, Register ptrScratch, Register64 output,
                                     Register tmp)
{
    wasmLoadI64Impl(access, memoryBase, ptr, ptrScratch, output, tmp);
}

void
MacroAssembler::wasmStoreI64(const wasm::MemoryAccessDesc& access, Register64 value,
                             Register memoryBase, Register ptr, Register ptrScratch)
{
    wasmStoreI64Impl(access, value, memoryBase, ptr, ptrScratch, InvalidReg);
}

void
MacroAssembler::wasmUnalignedStoreI64(const wasm::MemoryAccessDesc& access, Register64 value,
                                      Register memoryBase, Register ptr, Register ptrScratch,
                                      Register tmp)
{
    wasmStoreI64Impl(access, value, memoryBase, ptr, ptrScratch, tmp);
}

void
MacroAssembler::wasmTruncateDoubleToInt64(FloatRegister input, Register64 output,
                                          bool isSaturating, Label* oolEntry,
                                          Label* oolRejoin, FloatRegister tempDouble)
{
    MOZ_ASSERT(tempDouble.isInvalid());

    as_fctid(ScratchDoubleReg, input);
    ma_push(ScratchDoubleReg);
    ma_pop(output.reg);
    // TODO: flag here
    ma_bc(ScratchRegister, Imm32(0), oolEntry, Assembler::NotEqual);

    if (isSaturating)
        bind(oolRejoin);
}

void
MacroAssembler::wasmTruncateDoubleToUInt64(FloatRegister input, Register64 output_,
                                           bool isSaturating, Label* oolEntry,
                                           Label* oolRejoin, FloatRegister tempDouble)
{
    MOZ_ASSERT(tempDouble.isInvalid());
    Register output = output_.reg;

    Label done;

    as_fctidu(ScratchDoubleReg, input);
    // TODO: check saturation!

    ma_push(ScratchDoubleReg);
    ma_pop(output);

    ma_bc(ScratchRegister, Imm32(0), oolEntry, Assembler::NotEqual);

    bind(&done);

    if (isSaturating)
        bind(oolRejoin);
}

void
MacroAssembler::wasmTruncateFloat32ToInt64(FloatRegister input, Register64 output,
                                           bool isSaturating, Label* oolEntry,
                                           Label* oolRejoin, FloatRegister tempFloat)
{
    wasmTruncateDoubleToInt64(input, output, isSaturating, oolEntry,
            oolRejoin, tempFloat);
}

void
MacroAssembler::wasmTruncateFloat32ToUInt64(FloatRegister input, Register64 output,
                                            bool isSaturating, Label* oolEntry,
                                            Label* oolRejoin, FloatRegister tempFloat)
{
    wasmTruncateDoubleToUInt64(input, output, isSaturating, oolEntry,
            oolRejoin, tempFloat);
}

void
MacroAssemblerPPC64Compat::wasmLoadI64Impl(const wasm::MemoryAccessDesc& access,
                                            Register memoryBase, Register ptr, Register ptrScratch,
                                            Register64 output, Register tmp)
{
    uint32_t offset = access.offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);
    MOZ_ASSERT_IF(offset, ptrScratch != InvalidReg);

    // Maybe add the offset.
    if (offset) {
        asMasm().addPtr(Imm32(offset), ptrScratch);
        ptr = ptrScratch;
    }

    unsigned byteSize = access.byteSize();
    bool isSigned;

    switch (access.type()) {
      case Scalar::Int8:   isSigned = true; break;
      case Scalar::Uint8:  isSigned = false; break;
      case Scalar::Int16:  isSigned = true; break;
      case Scalar::Uint16: isSigned = false; break;
      case Scalar::Int32:  isSigned = true; break;
      case Scalar::Uint32: isSigned = false; break;
      case Scalar::Int64:  isSigned = true; break;
      default: MOZ_CRASH("unexpected array type");
    }

    BaseIndex address(memoryBase, ptr, TimesOne);
    if (IsUnaligned(access)) {
        MOZ_ASSERT(tmp != InvalidReg);
        asMasm().ma_load_unaligned(access, output.reg, address, tmp,
                                   static_cast<LoadStoreSize>(8 * byteSize),
                                   isSigned ? SignExtend : ZeroExtend);
        return;
    }

    asMasm().memoryBarrierBefore(access.sync());
    asMasm().ma_load(output.reg, address, static_cast<LoadStoreSize>(8 * byteSize),
                     isSigned ? SignExtend : ZeroExtend);
    asMasm().append(access, asMasm().size() - 4);
    asMasm().memoryBarrierAfter(access.sync());
}

void
MacroAssemblerPPC64Compat::wasmStoreI64Impl(const wasm::MemoryAccessDesc& access, Register64 value,
                                             Register memoryBase, Register ptr, Register ptrScratch,
                                             Register tmp)
{
    uint32_t offset = access.offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);
    MOZ_ASSERT_IF(offset, ptrScratch != InvalidReg);

    // Maybe add the offset.
    if (offset) {
        asMasm().addPtr(Imm32(offset), ptrScratch);
        ptr = ptrScratch;
    }

    unsigned byteSize = access.byteSize();
    bool isSigned;
    switch (access.type()) {
      case Scalar::Int8:   isSigned = true; break;
      case Scalar::Uint8:  isSigned = false; break;
      case Scalar::Int16:  isSigned = true; break;
      case Scalar::Uint16: isSigned = false; break;
      case Scalar::Int32:  isSigned = true; break;
      case Scalar::Uint32: isSigned = false; break;
      case Scalar::Int64:  isSigned = true; break;
      default: MOZ_CRASH("unexpected array type");
    }

    BaseIndex address(memoryBase, ptr, TimesOne);

    if (IsUnaligned(access)) {
        MOZ_ASSERT(tmp != InvalidReg);
        asMasm().ma_store_unaligned(access, value.reg, address, tmp,
                                    static_cast<LoadStoreSize>(8 * byteSize),
                                    isSigned ? SignExtend : ZeroExtend);
        return;
    }

    asMasm().memoryBarrierBefore(access.sync());
    asMasm().ma_store(value.reg, address, static_cast<LoadStoreSize>(8 * byteSize),
                      isSigned ? SignExtend : ZeroExtend);
    asMasm().append(access, asMasm().size() - 4);
    asMasm().memoryBarrierAfter(access.sync());
}

template <typename T>
static void
CompareExchange64(MacroAssembler& masm, const Synchronization& sync, const T& mem,
                  Register64 expect, Register64 replace, Register64 output)
{
    masm.computeEffectiveAddress(mem, SecondScratchReg);

    Label tryAgain;
    Label exit;

    masm.memoryBarrierBefore(sync);

    masm.bind(&tryAgain);

    // 'r0' for 'ra' indicates hard 0, not GPR r0
    masm.as_ldarx(output.reg, r0, SecondScratchReg);
    masm.ma_bc(output.reg, expect.reg, &exit, Assembler::NotEqual, ShortJump);
    masm.movePtr(replace.reg, ScratchRegister);
    masm.as_stdcx(ScratchRegister, r0, SecondScratchReg);
    masm.ma_bc(ScratchRegister, ScratchRegister, &tryAgain, Assembler::NotEqual, ShortJump);

    masm.memoryBarrierAfter(sync);

    masm.bind(&exit);
}

void
MacroAssembler::compareExchange64(const Synchronization& sync, const Address& mem,
                                  Register64 expect, Register64 replace, Register64 output)
{
    CompareExchange64(*this, sync, mem, expect, replace, output);
}

template <typename T>
static void
AtomicExchange64(MacroAssembler& masm, const Synchronization& sync, const T& mem,
                 Register64 src, Register64 output)
{
    masm.computeEffectiveAddress(mem, SecondScratchReg);

    Label tryAgain;

    masm.memoryBarrierBefore(sync);

    masm.bind(&tryAgain);

    // 'r0' for 'ra' indicates hard 0, not GPR r0
    masm.as_ldarx(output.reg, r0, SecondScratchReg);
    masm.as_stdcx(src.reg, r0, SecondScratchReg);
    masm.ma_bc(cr0, Assembler::NotEqual, &tryAgain, ShortJump);

    masm.memoryBarrierAfter(sync);
}

void
MacroAssembler::atomicExchange64(const Synchronization& sync, const Address& mem, Register64 src,
                                 Register64 output)
{
    AtomicExchange64(*this, sync, mem, src, output);
}

template<typename T>
static void
AtomicFetchOp64(MacroAssembler& masm, const Synchronization& sync, AtomicOp op, Register64 value,
                const T& mem, Register64 temp, Register64 output)
{
    masm.computeEffectiveAddress(mem, SecondScratchReg);

    Label tryAgain;

    masm.memoryBarrierBefore(sync);

    masm.bind(&tryAgain);

    // 'r0' for 'ra' indicates hard 0, not GPR r0
    masm.as_ldarx(output.reg, r0, SecondScratchReg);

    switch(op) {
      case AtomicFetchAddOp:
        masm.as_add(temp.reg, output.reg, value.reg);
        break;
      case AtomicFetchSubOp:
        masm.as_subf(temp.reg, value.reg, output.reg);
        break;
      case AtomicFetchAndOp:
        masm.as_and(temp.reg, output.reg, value.reg);
        break;
      case AtomicFetchOrOp:
        masm.as_or(temp.reg, output.reg, value.reg);
        break;
      case AtomicFetchXorOp:
        masm.as_xor(temp.reg, output.reg, value.reg);
        break;
      default:
        MOZ_CRASH();
    }

    masm.as_stdcx(temp.reg, r0, SecondScratchReg);
    masm.ma_bc(temp.reg, temp.reg, &tryAgain, Assembler::NotEqual, ShortJump);

    masm.memoryBarrierAfter(sync);
}

void
MacroAssembler::atomicFetchOp64(const Synchronization& sync, AtomicOp op, Register64 value,
                                const Address& mem, Register64 temp, Register64 output)
{
    AtomicFetchOp64(*this, sync, op, value, mem, temp, output);
}

void
MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                      const BaseIndex& mem,
                                      Register64 expect,
                                      Register64 replace,
                                      Register64 output) {
    CompareExchange64(*this, access.sync(), mem, expect, replace, output);
}

void
MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                     const BaseIndex& mem,
                                     Register64 value, Register64 output)
{
    AtomicExchange64(*this, access.sync(), mem, value, output);
}

void
MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                    AtomicOp op, Register64 value,
                                    const BaseIndex& mem, Register64 temp,
                                    Register64 output)
{
    AtomicFetchOp64(*this, access.sync(), op, value, mem, temp, output);
}


// ========================================================================
// Convert floating point.

void
MacroAssembler::convertInt64ToDouble(Register64 src, FloatRegister dest)
{
    ma_push(src.reg);
    ma_pop(dest);
    as_fcfid(dest, dest);
}

void
MacroAssembler::convertInt64ToFloat32(Register64 src, FloatRegister dest)
{
    ma_push(src.reg);
    ma_pop(dest);
    as_fcfid(dest, dest);
    as_frsp(dest, dest);
}

bool
MacroAssembler::convertUInt64ToDoubleNeedsTemp()
{
    return false;
}

void
MacroAssembler::convertUInt64ToDouble(Register64 src, FloatRegister dest, Register temp)
{
    MOZ_ASSERT(temp == Register::Invalid());
    MacroAssemblerSpecific::convertUInt64ToDouble(src.reg, dest);
}

void
MacroAssembler::convertUInt64ToFloat32(Register64 src, FloatRegister dest, Register temp)
{
    MOZ_ASSERT(temp == Register::Invalid());

    ma_push(src.reg);
    ma_pop(dest);
    as_fcfidu(dest, dest);
}

void
MacroAssembler::copySignDouble(FloatRegister lhs, FloatRegister rhs, FloatRegister dest)
{
    // From inspection, 'rhs' is the sign and 'lhs' is the value.  Opposite of
    // what the instruction takes.
    as_fcpsgn(dest, rhs, lhs);
}

void
MacroAssembler::truncFloat32ToInt32(FloatRegister src, Register dest, Label* fail)
{
    return truncDoubleToInt32(src, dest, fail);
}

void
MacroAssembler::truncDoubleToInt32(FloatRegister src, Register dest, Label* fail)
{
    as_fctiwz(ScratchDoubleReg, src);

    as_mcrfs(cr0, 1); // Check isnan
    ma_bc(SOBit, fail, JumpKind::ShortJump);
    as_mcrfs(cr0, 5); // Check overflow and underflow
    ma_bc(SOBit, fail, JumpKind::ShortJump);

    x_subi(StackPointer, StackPointer, 4);
    as_stfiwx(ScratchDoubleReg, r0, StackPointer);
    as_lwz(dest, StackPointer, 0);
    as_addi(StackPointer, StackPointer, 4);
}

void
MacroAssembler::nearbyIntDouble(RoundingMode mode, FloatRegister src,
                                FloatRegister dest)
{
    switch (mode) {
        case RoundingMode::Up:
            as_frip(dest, src);
            break;
        case RoundingMode::Down:
            as_frim(dest, src);
            break;
        case RoundingMode::NearestTiesToEven:
            as_frin(dest, src);
            break;
        case RoundingMode::TowardsZero:
            as_friz(dest, src);
            break;
    }
}

void
MacroAssembler::nearbyIntFloat32(RoundingMode mode, FloatRegister src,
                                 FloatRegister dest)
{
    return nearbyIntDouble(mode, src, dest);
}

void
MacroAssembler::ceilFloat32ToInt32(FloatRegister src, Register dest,
                                   Label* fail)
{
    return ceilDoubleToInt32(src, dest, fail);
}

void
MacroAssembler::ceilDoubleToInt32(FloatRegister src, Register dest, Label* fail)
{
    // Set rounding mode to 0b10 (round +inf)
    as_mtfsb1(30);
    as_mtfsb0(31);
    as_fctiw(ScratchDoubleReg, src);

    as_mcrfs(cr0, 1); // Check isnan
    ma_bc(SOBit, fail, JumpKind::ShortJump);
    as_mcrfs(cr0, 5); // Check overflow and underflow
    ma_bc(SOBit, fail, JumpKind::ShortJump);

    x_subi(StackPointer, StackPointer, 4);
    as_stfiwx(ScratchDoubleReg, r0, StackPointer);
    as_lwz(dest, StackPointer, 0);
    as_addi(StackPointer, StackPointer, 4);
}

void
MacroAssembler::floorFloat32ToInt32(FloatRegister src, Register dest,
                                   Label* fail)
{
    return floorDoubleToInt32(src, dest, fail);
}

void
MacroAssembler::floorDoubleToInt32(FloatRegister src, Register dest, Label* fail)
{
    // Set rounding mode to 0b10 (round +inf)
    as_mtfsb1(30);
    as_mtfsb1(31);
    as_fctiw(ScratchDoubleReg, src);

    as_mcrfs(cr0, 1); // Check isnan
    ma_bc(SOBit, fail, JumpKind::ShortJump);
    as_mcrfs(cr0, 5); // Check overflow and underflow
    ma_bc(SOBit, fail, JumpKind::ShortJump);

    x_subi(StackPointer, StackPointer, 4);
    as_stfiwx(ScratchDoubleReg, r0, StackPointer);
    as_lwz(dest, StackPointer, 0);
    as_addi(StackPointer, StackPointer, 4);
}

void
MacroAssembler::roundFloat32ToInt32(FloatRegister src, Register dest,
                                    FloatRegister temp, Label* fail)
{
    return floorDoubleToInt32(src, dest, fail);
}

void
MacroAssembler::roundDoubleToInt32(FloatRegister src, Register dest,
                                   FloatRegister temp, Label* fail)
{
    // Set rounding mode to 0b00 (round nearest)
    as_mtfsb0(30);
    as_mtfsb0(31);
    as_fctiw(temp, src);

    as_mcrfs(cr0, 1); // Check isnan
    ma_bc(SOBit, fail, JumpKind::ShortJump);
    as_mcrfs(cr0, 5); // Check overflow and underflow
    ma_bc(SOBit, fail, JumpKind::ShortJump);

    x_subi(StackPointer, StackPointer, 4);
    as_stfiwx(temp, r0, StackPointer);
    as_lwz(dest, StackPointer, 0);
    as_addi(StackPointer, StackPointer, 4);
}

void
MacroAssembler::flexibleRemainder32(Register rhs, Register srcDest,
                                    bool isUnsigned, const LiveRegisterSet &)
{
    if (isUnsigned)
        as_divwu(ScratchRegister, srcDest, rhs);
    else
        as_divw(ScratchRegister, srcDest, rhs);
    as_mullw(ScratchRegister, ScratchRegister, rhs);
    as_subf(srcDest, rhs, srcDest);
}

void
MacroAssembler::flexibleQuotient32(Register rhs, Register srcDest,
                                   bool isUnsigned,
                                   const LiveRegisterSet&)
{
    quotient32(rhs, srcDest, isUnsigned);
}

void
MacroAssembler::flexibleDivMod32(Register rhs, Register srcDest,
                                 Register remOutput, bool isUnsigned,
                                 const LiveRegisterSet&)
{
    Register scratch = ScratchRegister;

    if (isUnsigned) {
        as_divwu(scratch, srcDest, rhs);
    } else {
        as_divw(scratch, srcDest, rhs);
    }
    // Compute remainder
    as_mullw(remOutput, srcDest, rhs);
    as_subf(remOutput, scratch, srcDest);
    x_mr(srcDest, scratch);
}

//}}} check_macroassembler_style
/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/EndianUtils.h"

#include "jit/MacroAssembler.h"

using namespace js;
using namespace jit;

void
MacroAssemblerPPC64::ma_move(Register rd, Register rs)
{
    as_or(rd, rs, rs);
}

void
MacroAssemblerPPC64::ma_li(Register dest, ImmGCPtr ptr)
{
    writeDataRelocation(ptr);
    asMasm().ma_liPatchable(dest, ImmPtr(ptr.value));
}

void
MacroAssemblerPPC64::ma_li(Register dest, Imm32 imm)
{
    asMasm().ma_li(dest, (uint64_t)imm.value);
}

// This method generates lis and ori instruction pair that can be modified by
// UpdateLisOriValue, either during compilation (eg. Assembler::bind), or
// during execution (eg. jit::PatchJump).
void
MacroAssemblerPPC64::ma_liPatchable(Register dest, Imm32 imm)
{
    m_buffer.ensureSpace(2 * sizeof(uint32_t));
    xs_lis(dest, Imm16::Upper(imm).encode());
    as_ori(dest, dest, Imm16::Lower(imm).encode());
}

// Shifts

// Bit extract/insert
void
MacroAssemblerPPC64::ma_ext(Register rt, Register rs, uint16_t pos, uint16_t size) {
    MOZ_ASSERT(pos < 32);
    MOZ_ASSERT(pos + size < 33);

    as_rlwinm(rt, rs, 0, pos, size);
}

void
MacroAssemblerPPC64::ma_ins(Register rt, Register rs, uint16_t pos, uint16_t size) {
    MOZ_ASSERT(pos < 32);
    MOZ_ASSERT(pos + size <= 32);
    MOZ_ASSERT(size != 0);

    as_rlwimi(rt, rs, 0, pos, size);
}


// And.
void
MacroAssemblerPPC64::ma_and(Register rd, Register rs)
{
    as_and(rd, rd, rs);
}

void
MacroAssemblerPPC64::ma_and(Register rd, Imm32 imm)
{
    ma_and(rd, rd, imm);
}

void
MacroAssemblerPPC64::ma_and(Register rd, Register rs, Imm32 imm)
{
    if (Imm16::IsInUnsignedRange(imm.value)) {
        as_andi_rc(rd, rs, imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_and(rd, rs, ScratchRegister);
    }
}

// Or.
void
MacroAssemblerPPC64::ma_or(Register rd, Register rs)
{
    as_or(rd, rd, rs);
}

void
MacroAssemblerPPC64::ma_or(Register rd, Imm32 imm)
{
    ma_or(rd, rd, imm);
}

void
MacroAssemblerPPC64::ma_or(Register rd, Register rs, Imm32 imm)
{
    if (Imm16::IsInUnsignedRange(imm.value)) {
        as_ori(rd, rs, imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_or(rd, rs, ScratchRegister);
    }
}

// xor
void
MacroAssemblerPPC64::ma_xor(Register rd, Register rs)
{
    as_xor(rd, rd, rs);
}

void
MacroAssemblerPPC64::ma_xor(Register rd, Imm32 imm)
{
    ma_xor(rd, rd, imm);
}

void
MacroAssemblerPPC64::ma_xor(Register rd, Register rs, Imm32 imm)
{
    if (Imm16::IsInUnsignedRange(imm.value)) {
        as_xori(rd, rs, imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_xor(rd, rs, ScratchRegister);
    }
}



// Arithmetic-based ops.

// Add.

void
MacroAssemblerPPC64::ma_addTestCarry(Condition cond, Register rd, Register rs, Register rt,
                                          Label* overflow)
{
// Needs code check
__asm__("trap\n");
    MOZ_ASSERT(cond == Assembler::CarrySet || cond == Assembler::CarryClear);
    MOZ_ASSERT_IF(rd == rs, rt != rd);
    as_addc(rd, rs, rt);
    as_mcrxrx(cr0);
    ma_bc(SecondScratchReg, SecondScratchReg, overflow,
         cond == Assembler::CarrySet ? Assembler::Zero : Assembler::NonZero);
}

void
MacroAssemblerPPC64::ma_addTestCarry(Condition cond, Register rd, Register rs, Imm32 imm,
                                          Label* overflow)
{
__asm__("trap\n");
    MOZ_ASSERT(cond == Assembler::CarrySet || cond == Assembler::CarryClear);
    if (!Imm16::IsInSignedRange(imm.value)) {
        ma_li(ScratchRegister, imm);
        ma_addTestCarry(cond, rd, rs, ScratchRegister, overflow);
        return;
    }
    ma_addTestCarry(cond, rd, rs, ScratchRegister, overflow);
    as_addic(rd, rs, imm.value);
    as_mcrxrx(cr0);
    ma_bc(SecondScratchReg, SecondScratchReg, overflow,
         cond == Assembler::CarrySet ? Assembler::Zero : Assembler::NonZero);
}

// Subtract.
void
MacroAssemblerPPC64::ma_subu(Register rd, Register rs, Imm32 imm)
{
    if (Imm16::IsInSignedRange(-imm.value)) {
        as_addi(rd, rs, -imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_subf(rd, ScratchRegister, rs);
    }
}

void
MacroAssemblerPPC64::ma_subu(Register rd, Imm32 imm)
{
    ma_subu(rd, rd, imm);
}

void
MacroAssemblerPPC64::ma_subu(Register rd, Register rs)
{
    as_subf(rd, rs, rd);
}

void
MacroAssemblerPPC64::ma_subTestOverflow(Register rd, Register rs, Imm32 imm, Label* overflow)
{
    if (imm.value != INT32_MIN) {
        asMasm().ma_addTestOverflow(rd, rs, Imm32(-imm.value), overflow);
    } else {
        ma_li(ScratchRegister, Imm32(imm.value));
        asMasm().ma_subTestOverflow(rd, rs, ScratchRegister, overflow);
    }
}

void
MacroAssemblerPPC64::ma_mul(Register rd, Register rs, Imm32 imm)
{
    ma_li(ScratchRegister, imm);
    as_mulld(rd, rs, ScratchRegister);
}

void
MacroAssemblerPPC64::ma_mul_branch_overflow(Register rd, Register rs, Register rt, Label* overflow)
{
    as_mulldo(rd, rs, rt);
    as_bc(overflow->offset(), SOBit);
}

void
MacroAssemblerPPC64::ma_mul_branch_overflow(Register rd, Register rs, Imm32 imm, Label* overflow)
{
    ma_li(ScratchRegister, imm);
    ma_mul_branch_overflow(rd, rs, ScratchRegister, overflow);
}

// Memory.

void
MacroAssemblerPPC64::ma_load(Register dest, const BaseIndex& src,
                                  LoadStoreSize size, LoadStoreExtension extension)
{
    if (ZeroExtend != extension && Imm8::IsInSignedRange(src.offset)) {
        Register index = src.index;

        if (src.scale != TimesOne) {
            int32_t shift = Imm32::ShiftOf(src.scale).value;

            MOZ_ASSERT(SecondScratchReg != src.base);
            index = SecondScratchReg;
            asMasm().as_rldicr(index, src.index, shift, 64 - shift);
        }

        switch (size) {
          case SizeByte:
            as_lbz(dest, src.base, src.offset);
            break;
          case SizeHalfWord:
            as_lhz(dest, src.base, src.offset);
            break;
          case SizeWord:
            as_lwz(dest, src.base, src.offset);
            break;
          case SizeDouble:
            as_ld(dest, src.base, src.offset);
            break;
          default:
            MOZ_CRASH("Invalid argument for ma_load");
        }
        return;
    }

    asMasm().computeScaledAddress(src, SecondScratchReg);

    // If src.offset is out of 16-bit signed range, we will hit an assert
    // doing the next ma_load() because the second scratch register is needed
    // again. In that case, hoist the add up since we can freely clobber it.
    if (!Imm16::IsInSignedRange(src.offset)) {
        ma_add(SecondScratchReg, SecondScratchReg, Imm32(src.offset));
        ma_load(dest, Address(SecondScratchReg, 0), size, extension);
    } else {
        asMasm().ma_load(dest, Address(SecondScratchReg, src.offset), size, extension);
    }
}

void
MacroAssemblerPPC64::ma_load_unaligned(const wasm::MemoryAccessDesc& access, Register dest, const BaseIndex& src, Register temp,
                                            LoadStoreSize size, LoadStoreExtension extension)
{
    MOZ_ASSERT(MOZ_LITTLE_ENDIAN(), "Wasm-only; wasm is disabled on big-endian.");
#if 0
    int16_t lowOffset, hiOffset;
    Register base;

    asMasm().computeScaledAddress(src, SecondScratchReg);

    if (Imm16::IsInSignedRange(src.offset) && Imm16::IsInSignedRange(src.offset + size / 8 - 1)) {
        base = SecondScratchReg;
        lowOffset = Imm16(src.offset).encode();
        hiOffset = Imm16(src.offset + size / 8 - 1).encode();
    } else {
        ma_li(ScratchRegister, Imm32(src.offset));
        asMasm().addPtr(SecondScratchReg, ScratchRegister);
        base = ScratchRegister;
        lowOffset = Imm16(0).encode();
        hiOffset = Imm16(size / 8 - 1).encode();
    }

    BufferOffset load;
    switch (size) {
      case SizeHalfWord:
        if (extension != ZeroExtend)
            load = as_lbu(temp, base, hiOffset);
        else
            load = as_lb(temp, base, hiOffset);
        as_lbu(dest, base, lowOffset);
        ma_ins(dest, temp, 8, 24);
        break;
      case SizeWord:
        load = as_lwl(dest, base, hiOffset);
        as_lwr(dest, base, lowOffset);
#ifdef JS_CODEGEN_PPC64
        if (extension != ZeroExtend)
            as_dext(dest, dest, 0, 32);
#endif
        break;
#ifdef JS_CODEGEN_PPC64
      case SizeDouble:
        load = as_ldl(dest, base, hiOffset);
        as_ldr(dest, base, lowOffset);
        break;
#endif
      default:
        MOZ_CRASH("Invalid argument for ma_load");
    }

    append(access, load.getOffset());
#endif
}

void
MacroAssemblerPPC64::ma_store(Register data, const BaseIndex& dest,
                                   LoadStoreSize size, LoadStoreExtension extension)
{
    if (Imm8::IsInSignedRange(dest.offset)) {
        Register index = dest.index;

        if (dest.scale != TimesOne) {
            int32_t shift = Imm32::ShiftOf(dest.scale).value;

            MOZ_ASSERT(SecondScratchReg != dest.base);
            index = SecondScratchReg;
            asMasm().ma_dsll(index, dest.index, Imm32(shift));
        }

        switch (size) {
          case SizeByte:
            as_stb(data, dest.base, dest.offset);
            break;
          case SizeHalfWord:
            as_sth(data, dest.base, dest.offset);
            break;
          case SizeWord:
            as_stw(data, dest.base, dest.offset);
            break;
          case SizeDouble:
            as_std(data, dest.base, dest.offset);
            break;
          default:
            MOZ_CRASH("Invalid argument for ma_store");
        }
        return;
    }

    asMasm().computeScaledAddress(dest, SecondScratchReg);
    asMasm().ma_store(data, Address(SecondScratchReg, dest.offset), size, extension);
}

void
MacroAssemblerPPC64::ma_store(Imm32 imm, const BaseIndex& dest,
                                   LoadStoreSize size, LoadStoreExtension extension)
{
    // Make sure that SecondScratchReg contains absolute address so that
    // offset is 0.
    asMasm().computeEffectiveAddress(dest, SecondScratchReg);

    // Scrach register is free now, use it for loading imm value
    ma_li(ScratchRegister, imm);

    // with offset=0 ScratchRegister will not be used in ma_store()
    // so we can use it as a parameter here
    asMasm().ma_store(ScratchRegister, Address(SecondScratchReg, 0), size, extension);
}

void
MacroAssemblerPPC64::ma_store_unaligned(Register src, const BaseIndex& dest,
                                        LoadStoreSize size)
{
    MOZ_CRASH("NYI");
}

void
MacroAssemblerPPC64::ma_store_unaligned(const wasm::MemoryAccessDesc& access, Register data,
                                             const BaseIndex& dest, Register temp,
                                             LoadStoreSize size, LoadStoreExtension extension)
{
    MOZ_ASSERT(MOZ_LITTLE_ENDIAN(), "Wasm-only; wasm is disabled on big-endian.");
#if 0
    int16_t lowOffset, hiOffset;
    Register base;

    asMasm().computeScaledAddress(dest, SecondScratchReg);

    if (Imm16::IsInSignedRange(dest.offset) && Imm16::IsInSignedRange(dest.offset + size / 8 - 1)) {
        base = SecondScratchReg;
        lowOffset = Imm16(dest.offset).encode();
        hiOffset = Imm16(dest.offset + size / 8 - 1).encode();
    } else {
        ma_li(ScratchRegister, Imm32(dest.offset));
        asMasm().addPtr(SecondScratchReg, ScratchRegister);
        base = ScratchRegister;
        lowOffset = Imm16(0).encode();
        hiOffset = Imm16(size / 8 - 1).encode();
    }

    BufferOffset store;
    switch (size) {
      case SizeHalfWord:
        ma_ext(temp, data, 8, 8);
        store = as_sb(temp, base, hiOffset);
        as_sb(data, base, lowOffset);
        break;
      case SizeWord:
        store = as_swl(data, base, hiOffset);
        as_swr(data, base, lowOffset);
        break;
#ifdef JS_CODEGEN_PPC64
      case SizeDouble:
        store = as_sdl(data, base, hiOffset);
        as_sdr(data, base, lowOffset);
        break;
#endif
      default:
        MOZ_CRASH("Invalid argument for ma_store");
    }
    append(access, store.getOffset());
#endif
}

// Branches when done from within ppc64-specific code.
void
MacroAssemblerPPC64::ma_bc(Register lhs, Register rhs, Label* label, Condition c, JumpKind jumpKind)
{
    ADBlock();
    MOZ_ASSERT(!(c & ConditionOnlyXER));
    if (c == Always) {
        ma_b(label, jumpKind);
    } else if (c & ConditionZero) {
        MOZ_ASSERT(lhs == rhs);
        as_cmpdi(lhs, 0);
        ma_bc(c, label, jumpKind);
    } else if (c & ConditionUnsigned) {
        as_cmpld(lhs, rhs);
        ma_bc(c, label, jumpKind);
    } else {
        MOZ_ASSERT(c < 0x100); // paranoia
        as_cmpd(lhs, rhs);
        ma_bc(c, label, jumpKind);
    }
}

void
MacroAssemblerPPC64::ma_bc(Register lhs, Imm32 imm, Label* label, Condition c, JumpKind jumpKind)
{
    ADBlock();
    MOZ_ASSERT(!(c & ConditionOnlyXER));
    if (c == Always) {
        ma_b(label, jumpKind);
        return;
    }
    if (c & ConditionZero) {
        MOZ_ASSERT(imm.value == 0);
        as_cmpdi(lhs, 0);
        ma_bc(c, label, jumpKind);
        return;
    }
    if (c & ConditionUnsigned) {
        if (Imm16::IsInUnsignedRange(imm.value)) {
            as_cmplwi(lhs, imm.value);
        } else {
            MOZ_ASSERT(lhs != ScratchRegister);
            ma_li(ScratchRegister, imm);
            as_cmplw(lhs, ScratchRegister);
        }
    } else {
        MOZ_ASSERT(c < 0x100); // just in case
        if (Imm16::IsInSignedRange(imm.value)) {
            as_cmpwi(lhs, imm.value);
        } else {
            MOZ_ASSERT(lhs != ScratchRegister);
            ma_li(ScratchRegister, imm);
            as_cmpw(lhs, ScratchRegister);
        }
    }
    ma_bc(c, label, jumpKind);
}

void
MacroAssemblerPPC64::ma_bc(Register lhs, ImmPtr imm, Label* l, Condition c, JumpKind jumpKind)
{
    asMasm().ma_bc(lhs, ImmWord(uintptr_t(imm.value)), l, c, jumpKind);
}

void
MacroAssemblerPPC64::ma_b(Label* label, JumpKind jumpKind)
{
    ADBlock();
    if (!label->bound()) {
        // Emit an unbound branch to be bound later by |Assembler::bind|.
        spew(".Llabel %p", label);
        if (jumpKind == ShortJump) { // We know this branch must be short.
            xs_trap_tagged(StaticShortJumpTag); // turned into b
        } else {
            m_buffer.ensureSpace(7 * sizeof(uint32_t));
            ma_liPatchable(ScratchRegister, ImmWord(LabelBase::INVALID_OFFSET));
            xs_trap_tagged(LongJumpTag); // turned into mtctr
            xs_trap(); // turned into bctr
        }
        return;
    }

    // Label is bound, emit final code.
    int64_t offset = currentOffset() - (label->offset());
    if (jumpKind == ShortJump || JOffImm26::IsInRange(offset))
        as_b(offset);
    else {
        // Use r12 "as expected" even though this is probably not to ABI-compliant code.
        m_buffer.ensureSpace(7 * sizeof(uint32_t));
        addLongJump(nextOffset());
        ma_liPatchable(SecondScratchReg, ImmWord(LabelBase::INVALID_OFFSET));
        xs_mtctr(SecondScratchReg);
        as_bctr();
    }
}

void
MacroAssemblerPPC64::ma_cmp32(Register lhs, Register rhs, Condition c)
{
    ADBlock();
    MOZ_ASSERT(!(c & ConditionOnlyXER));
    MOZ_ASSERT(!(c & ConditionZero));

    if (c & ConditionUnsigned) {
        as_cmplw(lhs, rhs);
    } else {
        as_cmpw(lhs, rhs);
    }
}

void
MacroAssemblerPPC64::ma_cmp32(Register lhs, Imm32 rhs, Condition c)
{
    ADBlock();
    MOZ_ASSERT(!(c & ConditionOnlyXER));
    MOZ_ASSERT_IF((c & ConditionZero), (rhs.value == 0));

    if (c & ConditionZero) {
        as_cmpwi(lhs, 0);
    } else {
        if (c & ConditionUnsigned) {
            if (Imm16::IsInUnsignedRange(rhs.value)) {
                as_cmplwi(lhs, rhs.value);
            } else {
                MOZ_ASSERT(lhs != ScratchRegister);
                ma_li(ScratchRegister, rhs);
                as_cmplw(lhs, ScratchRegister);
            }
        } else {
            if (Imm16::IsInSignedRange(rhs.value)) {
                as_cmpwi(lhs, rhs.value);
            } else {
                MOZ_ASSERT(lhs != ScratchRegister);
                ma_li(ScratchRegister, rhs);
                as_cmpw(lhs, ScratchRegister);
            }
        }
    }
}

void
MacroAssemblerPPC64::ma_cmp32(Register lhs, const Address& rhs, Condition c)
{
    MOZ_ASSERT(lhs != ScratchRegister);
    ma_load(ScratchRegister, rhs, SizeWord);
    ma_cmp32(lhs, ScratchRegister, c);
}

void
MacroAssemblerPPC64::ma_cmp_set(Register rd, Register rs, Register rt, Condition c)
{
    ADBlock();
    int shift;

    as_mfcr(rd);
    switch (c) {
      case Equal :
      case NotEqual:
        shift = 2;
        break;
      case Above:
        // sgtu d,s,t =>
        //   sltu d,t,s
        shift = 1;
        break;
      case AboveOrEqual:
      case GreaterThanOrEqual:
      case Below:
      case LessThan:
        shift = 3;
        break;
      case BelowOrEqual:
      case LessThanOrEqual:
      case GreaterThan:
        shift = 1;
        break;
      default:
        MOZ_CRASH("Invalid condition.");
    }
    as_cmpd(rs, rt);
    as_mfcr(rd);
    as_rlwinm(rd, rd, (3 - shift) + 28, 30, 31);
    // Negate the boolean if necessary to represent a multi-condition
    switch (c) {
        case NotEqual:
        case AboveOrEqual:
        case GreaterThanOrEqual:
        case LessThanOrEqual:
        case BelowOrEqual:
            as_xori(rd, rd, 1);
            break;
        default:
            break;
    }
}

void
MacroAssemblerPPC64::compareFloatingPoint(FloatRegister lhs, FloatRegister rhs,
                                          DoubleCondition c)
{
    switch (c) {
      case DoubleOrdered:
      case DoubleEqual:
      case DoubleNotEqual:
      case DoubleGreaterThan:
      case DoubleGreaterThanOrEqual:
      case DoubleLessThan:
      case DoubleLessThanOrEqual:
        as_fcmpo(lhs, rhs);
        break;
      case DoubleUnordered:
      case DoubleEqualOrUnordered:
      case DoubleNotEqualOrUnordered:
      case DoubleGreaterThanOrUnordered:
      case DoubleGreaterThanOrEqualOrUnordered:
      case DoubleLessThanOrUnordered:
      case DoubleLessThanOrEqualOrUnordered:
        as_fcmpu(lhs, rhs);
        break;
      default:
        MOZ_CRASH("Invalid DoubleCondition.");
    }
}

void
MacroAssemblerPPC64::ma_cmp_set_double(Register dest, FloatRegister lhs, FloatRegister rhs,
                                            DoubleCondition c)
{
    Label skip;
    compareFloatingPoint(lhs, rhs, c);

    ma_li(dest, 1L);

    ma_bc(c, &skip);
    ma_li(dest, 0L);
    bind(&skip);
}

void
MacroAssemblerPPC64::ma_cmp_set(Register rd, Register rs, Imm16 imm, Condition c)
{
    int shift;

    as_mfcr(rd);
    switch (c) {
      case Equal :
      case NotEqual:
        shift = 2;
        break;
      case Above:
        // sgtu d,s,t =>
        //   sltu d,t,s
        shift = 1;
        break;
      case AboveOrEqual:
      case GreaterThanOrEqual:
      case Below:
      case LessThan:
        shift = 3;
        break;
      case BelowOrEqual:
      case LessThanOrEqual:
      case GreaterThan:
        shift = 1;
        break;
      default:
        MOZ_CRASH("Invalid condition.");
    }
    as_cmpdi(rs, imm.encode());
    as_mfcr(rd);
    as_rlwinm(rd, rd, (3 - shift) + 28, 30, 31);
    // Negate the boolean if necessary to represent a multi-condition
    switch (c) {
        case NotEqual:
        case AboveOrEqual:
        case GreaterThanOrEqual:
        case LessThanOrEqual:
        case BelowOrEqual:
            as_xori(rd, rd, 1);
            break;
        default:
            break;
    }
}

// fp instructions
void
MacroAssemblerPPC64::ma_lis(FloatRegister dest, float value)
{
    Imm32 imm(mozilla::BitwiseCast<uint32_t>(value));

    ma_li(ScratchRegister, imm);
    ma_push(ScratchRegister);
    ma_pop(dest);
}

void
MacroAssemblerPPC64::ma_sd(FloatRegister ft, BaseIndex address)
{
    if (Imm16::IsInSignedRange(address.offset) && address.scale == TimesOne) {
        as_stfd(ft, address.base, address.offset);
        return;
    }

    asMasm().computeScaledAddress(address, SecondScratchReg);
    asMasm().ma_sd(ft, Address(SecondScratchReg, address.offset));
}

void
MacroAssemblerPPC64::ma_ss(FloatRegister ft, BaseIndex address)
{
    if (Imm16::IsInSignedRange(address.offset) && address.scale == TimesOne) {
        as_stfs(ft, address.base, address.offset);
        return;
    }

    asMasm().computeScaledAddress(address, SecondScratchReg);
    asMasm().ma_ss(ft, Address(SecondScratchReg, address.offset));
}

void
MacroAssemblerPPC64::ma_ld(FloatRegister ft, const BaseIndex& src)
{
    asMasm().computeScaledAddress(src, SecondScratchReg);
    asMasm().ma_ld(ft, Address(SecondScratchReg, src.offset));
}

void
MacroAssemblerPPC64::ma_ls(FloatRegister ft, const BaseIndex& src)
{
    asMasm().computeScaledAddress(src, SecondScratchReg);
    asMasm().ma_ls(ft, Address(SecondScratchReg, src.offset));
}

void
MacroAssemblerPPC64::minMaxDouble(FloatRegister srcDest, FloatRegister second,
                                       bool handleNaN, bool isMax)
{
    FloatRegister first = srcDest;
    FloatRegister fromReg = second;

    Assembler::DoubleCondition cond = isMax
                                      ? Assembler::DoubleLessThanOrEqual
                                      : Assembler::DoubleGreaterThanOrEqual;
    Label nan, equal, done, success;

    // First or second is NaN, result is NaN.
    compareFloatingPoint(first, fromReg, Assembler::DoubleUnordered);
    ma_bc(Assembler::DoubleUnordered, &nan, ShortJump);
    // Make sure we handle -0 and 0 right.
    compareFloatingPoint(first, fromReg, Assembler::DoubleEqual);
    ma_bc(Assembler::DoubleEqual, &nan, ShortJump);
    compareFloatingPoint(first, second, cond);
    ma_bc(cond, &done, ShortJump);

    // Check for zero.
    bind(&equal);
    asMasm().loadConstantDouble(0.0, ScratchDoubleReg);
    compareFloatingPoint(first, ScratchDoubleReg, Assembler::DoubleEqual);

    // So now both operands are either -0 or 0.
    if (isMax) {
        // -0 + -0 = -0 and -0 + 0 = 0.
        as_fadd(ScratchDoubleReg, first, second);
    } else {
        as_fneg(ScratchDoubleReg, first);
        as_fsub(ScratchDoubleReg, ScratchDoubleReg, second);
        as_fneg(ScratchDoubleReg, ScratchDoubleReg);
    }
    // First is 0 or -0, move max/min to it, else just return it.
    fromReg = ScratchDoubleReg;
    ma_bc(cond, &success, ShortJump);
    ma_b(&done, ShortJump);

    bind(&nan);
    asMasm().loadConstantDouble(JS::GenericNaN(), srcDest);
    ma_b(&done, ShortJump);

    bind(&success);
    as_fmr(first, fromReg);

    bind(&done);
}

void
MacroAssemblerPPC64::loadDouble(const Address& address, FloatRegister dest)
{
    as_lfd(dest, address.base, address.offset);
}

void
MacroAssemblerPPC64::loadDouble(const BaseIndex& src, FloatRegister dest)
{
    asMasm().computeScaledAddress(src, ScratchRegister);
    as_lfd(dest, ScratchRegister, src.offset);
}

void
MacroAssemblerPPC64::loadFloatAsDouble(const Address& address, FloatRegister dest)
{
    as_lfs(dest, address.base, address.offset);
}

void
MacroAssemblerPPC64::loadFloatAsDouble(const BaseIndex& src, FloatRegister dest)
{
    asMasm().loadFloat32(src, dest);
}

void
MacroAssemblerPPC64::loadFloat32(const Address& address, FloatRegister dest)
{
    asMasm().ma_ls(dest, address);
}

void
MacroAssemblerPPC64::loadFloat32(const BaseIndex& src, FloatRegister dest)
{
    asMasm().ma_ls(dest, src);
}

void
MacroAssemblerPPC64::ma_call(ImmPtr dest)
{
    asMasm().ma_liPatchable(CallReg, dest);
    xs_mtctr(CallReg);
    as_bctr(LinkB);
    as_nop();
}

void
MacroAssemblerPPC64::ma_jump(ImmPtr dest)
{
    asMasm().ma_liPatchable(ScratchRegister, dest);
    xs_mtctr(ScratchRegister);
    as_bctr();
    as_nop();
}

MacroAssembler&
MacroAssemblerPPC64::asMasm()
{
    return *static_cast<MacroAssembler*>(this);
}

const MacroAssembler&
MacroAssemblerPPC64::asMasm() const
{
    return *static_cast<const MacroAssembler*>(this);
}

//{{{ check_macroassembler_style
// ===============================================================
// MacroAssembler high-level usage.

void
MacroAssembler::flush()
{
}

// ===============================================================
// Stack manipulation functions.

void
MacroAssembler::Push(Register reg)
{
    ma_push(reg);
    adjustFrame(int32_t(sizeof(intptr_t)));
}

void
MacroAssembler::Push(const Imm32 imm)
{
    ma_li(ScratchRegister, imm);
    ma_push(ScratchRegister);
    adjustFrame(int32_t(sizeof(intptr_t)));
}

void
MacroAssembler::Push(const ImmWord imm)
{
    ma_li(ScratchRegister, imm);
    ma_push(ScratchRegister);
    adjustFrame(int32_t(sizeof(intptr_t)));
}

void
MacroAssembler::Push(const ImmPtr imm)
{
    Push(ImmWord(uintptr_t(imm.value)));
}

void
MacroAssembler::Push(const ImmGCPtr ptr)
{
    ma_li(ScratchRegister, ptr);
    ma_push(ScratchRegister);
    adjustFrame(int32_t(sizeof(intptr_t)));
}

void
MacroAssembler::Push(FloatRegister f)
{
    ma_push(f);
    adjustFrame(int32_t(8));
}

void
MacroAssembler::Pop(Register reg)
{
    ma_pop(reg);
    adjustFrame(-int32_t(sizeof(intptr_t)));
}

void
MacroAssembler::Pop(FloatRegister f)
{
    ma_pop(f);
    adjustFrame(-int32_t(8));
}

void
MacroAssembler::Pop(const ValueOperand& val)
{
    popValue(val);
    adjustFrame(-int32_t(sizeof(Value)));
}

void
MacroAssembler::PopStackPtr()
{
    loadPtr(Address(StackPointer, 0), StackPointer);
    adjustFrame(-int32_t(sizeof(intptr_t)));
}


// ===============================================================
// Simple call functions.

CodeOffset
MacroAssembler::call(Register reg)
{
    xs_mtctr(reg);
    as_bctr(LinkB);
    as_nop();
    return CodeOffset(currentOffset());
}

CodeOffset
MacroAssembler::call(Label* label)
{
    ma_bal(label);
    return CodeOffset(currentOffset());
}

CodeOffset
MacroAssembler::callWithPatch()
{
// XXX: this is probably wrong
    as_b(0, RelativeBranch, LinkB);

    return CodeOffset(currentOffset());
}

void
MacroAssembler::patchCall(uint32_t callerOffset, uint32_t calleeOffset)
{
    MOZ_CRASH("NYI");
    BufferOffset call(callerOffset - 7 * sizeof(uint32_t));

    // TODO: patchCall
    BOffImm16 offset = BufferOffset(calleeOffset).diffB<BOffImm16>(call);
    if (!offset.isInvalid()) {
        InstImm* bal = (InstImm*)editSrc(call);
        bal->setBOffImm16(offset);
    } else {
        uint32_t u32Offset = callerOffset - 5 * sizeof(uint32_t);
        uint32_t* u32 = reinterpret_cast<uint32_t*>(editSrc(BufferOffset(u32Offset)));
        *u32 = calleeOffset - callerOffset;
    }
}

CodeOffset
MacroAssembler::farJumpWithPatch()
{
    MOZ_CRASH("NYI");
    CodeOffset farJump(currentOffset());
    return farJump;
}

void
MacroAssembler::patchFarJump(CodeOffset farJump, uint32_t targetOffset)
{
    uint32_t* u32 = reinterpret_cast<uint32_t*>(editSrc(BufferOffset(farJump.offset())));
    MOZ_ASSERT(*u32 == UINT32_MAX);
    *u32 = targetOffset - farJump.offset();
}

CodeOffset
MacroAssembler::call(wasm::SymbolicAddress target)
{
    movePtr(target, CallReg);
    return call(CallReg);
}

void
MacroAssembler::call(const Address& addr)
{
    loadPtr(addr, CallReg);
    call(CallReg);
}

void
MacroAssembler::call(ImmWord target)
{
    call(ImmPtr((void*)target.value));
}

void
MacroAssembler::call(ImmPtr target)
{
    BufferOffset bo = m_buffer.nextOffset();
    addPendingJump(bo, target, RelocationKind::HARDCODED);
    ma_call(target);
}

void
MacroAssembler::call(JitCode* c)
{
    BufferOffset bo = m_buffer.nextOffset();
    addPendingJump(bo, ImmPtr(c->raw()), RelocationKind::JITCODE);
    ma_liPatchable(ScratchRegister, ImmPtr(c->raw()));
    callJitNoProfiler(ScratchRegister);
}

CodeOffset
MacroAssembler::nopPatchableToCall()
{
    CodeOffset offset(currentOffset());
                // MIPS32   //PPC64
    as_nop();   // oris     // oris
    as_nop();   // ori      // ori
    as_nop();   // mtctr    // rlwinm (shift 32)
    as_nop();   // bctrl    // oris
#ifdef JS_CODEGEN_PPC64
    as_nop();               // ori
    as_nop();               // mtctr
    as_nop();               // bctrl
#endif
    return offset;
}

void
MacroAssembler::patchNopToCall(uint8_t* call, uint8_t* target)
{
#ifdef JS_CODEGEN_PPC64
    Instruction* inst = (Instruction*) call - 7 /* six nops */;
    Assembler::WriteLoad64Instructions(inst, ScratchRegister, (uint64_t) target);
    inst[5].makeOp_mtctr(ScratchRegister);
    inst[6].makeOp_bctr(LinkB);
#else
    Instruction* inst = (Instruction*) call - 4 /* four nops */;
    Assembler::WriteLuiOriInstructions(inst, &inst[1], ScratchRegister, (uint32_t) target);
    inst[2] = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr);
#endif
}

void
MacroAssembler::patchCallToNop(uint8_t* call)
{
#ifdef JS_CODEGEN_PPC64
    Instruction* inst = (Instruction*) call - 6 /* six nops */;
#else
    Instruction* inst = (Instruction*) call - 4 /* four nops */;
#endif

    inst[0].makeOp_nop();
    inst[1].makeOp_nop();
    inst[2].makeOp_nop();
    inst[3].makeOp_nop();
#ifdef JS_CODEGEN_PPC64
    inst[4].makeOp_nop();
    inst[5].makeOp_nop();
    inst[6].makeOp_nop();
#endif
}

void
MacroAssembler::pushReturnAddress()
{
    xs_mflr(ScratchRegister);
    push(ScratchRegister);
}

void
MacroAssembler::popReturnAddress()
{
    pop(ScratchRegister);
    xs_mtlr(ScratchRegister);
}

// ===============================================================
// Jit Frames.

uint32_t
MacroAssembler::pushFakeReturnAddress(Register scratch)
{
    CodeLabel cl;

    ma_li(scratch, &cl);
    Push(scratch);
    bind(&cl);
    uint32_t retAddr = currentOffset();

    addCodeLabel(cl);
    return retAddr;
}

void
MacroAssembler::loadStoreBuffer(Register ptr, Register buffer)
{
    if (ptr != buffer)
        movePtr(ptr, buffer);
    orPtr(Imm32(gc::ChunkMask), buffer);
    loadPtr(Address(buffer, gc::ChunkStoreBufferOffsetFromLastByte), buffer);
}

void
MacroAssembler::branchPtrInNurseryChunk(Condition cond, Register ptr, Register temp,
                                        Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    MOZ_ASSERT(ptr != temp);
    MOZ_ASSERT(ptr != SecondScratchReg);

    movePtr(ptr, SecondScratchReg);
    orPtr(Imm32(gc::ChunkMask), SecondScratchReg);
    branch32(cond, Address(SecondScratchReg, gc::ChunkLocationOffsetFromLastByte),
             Imm32(int32_t(gc::ChunkLocation::Nursery)), label);
}

void
MacroAssembler::comment(const char* msg)
{
    Assembler::comment(msg);
}

// ===============================================================
// WebAssembly

CodeOffset
MacroAssembler::wasmTrapInstruction()
{
    CodeOffset offset(currentOffset());
    xs_trap();
    return offset;
}

void
MacroAssembler::wasmTruncateDoubleToInt32(FloatRegister input, Register output, bool isSaturating,
                                          Label* oolEntry)
{
    as_fctiw(ScratchDoubleReg, input);
    ma_push(ScratchDoubleReg);
    ma_pop(output);
    ma_bc(ScratchRegister, Imm32(0), oolEntry, Assembler::NotEqual);
}


void
MacroAssembler::wasmTruncateFloat32ToInt32(FloatRegister input, Register output, bool isSaturating,
                                           Label* oolEntry)
{
    wasmTruncateDoubleToInt32(input, output, isSaturating, oolEntry);
}

void
MacroAssembler::oolWasmTruncateCheckF32ToI32(FloatRegister input, Register output,
                                             TruncFlags flags, wasm::BytecodeOffset off,
                                             Label* rejoin)
{
    outOfLineWasmTruncateToInt32Check(input, output, MIRType::Float32, flags, rejoin, off);
}

void
MacroAssembler::oolWasmTruncateCheckF64ToI32(FloatRegister input, Register output,
                                             TruncFlags flags, wasm::BytecodeOffset off,
                                             Label* rejoin)
{
    outOfLineWasmTruncateToInt32Check(input, output, MIRType::Double, flags, rejoin, off);
}

void
MacroAssembler::oolWasmTruncateCheckF32ToI64(FloatRegister input, Register64 output,
                                             TruncFlags flags, wasm::BytecodeOffset off,
                                             Label* rejoin)
{
    outOfLineWasmTruncateToInt64Check(input, output, MIRType::Float32, flags, rejoin, off);
}

void
MacroAssembler::oolWasmTruncateCheckF64ToI64(FloatRegister input, Register64 output,
                                             TruncFlags flags, wasm::BytecodeOffset off,
                                             Label* rejoin)
{
    outOfLineWasmTruncateToInt64Check(input, output, MIRType::Double, flags, rejoin, off);
}

void
MacroAssemblerPPC64::outOfLineWasmTruncateToInt32Check(FloatRegister input, Register output,
                                                            MIRType fromType, TruncFlags flags,
                                                            Label* rejoin,
                                                            wasm::BytecodeOffset trapOffset)
{
#if 0 // TODO: outOfLineWasmTruncateToInt32Check
    bool isUnsigned = flags & TRUNC_UNSIGNED;
    bool isSaturating = flags & TRUNC_SATURATING;

    if(isSaturating) {

        if(fromType == MIRType::Double)
            asMasm().loadConstantDouble(0.0, ScratchDoubleReg);
        else
            asMasm().loadConstantFloat32(0.0f, ScratchFloat32Reg);

        if(isUnsigned) {

            ma_li(output, Imm32(UINT32_MAX));

            FloatTestKind moveCondition;
            compareFloatingPoint(input, ScratchDoubleReg,
                                 Assembler::DoubleLessThanOrUnordered);
            MOZ_ASSERT(moveCondition == TestForTrue);

            as_movt(output, zero);
        } else {

            // Positive overflow is already saturated to INT32_MAX, so we only have
            // to handle NaN and negative overflow here.

            FloatTestKind moveCondition;
            compareFloatingPoint(input, input, Assembler::DoubleUnordered);
            MOZ_ASSERT(moveCondition == TestForTrue);

            as_movt(output, zero);

            compareFloatingPoint(input, ScratchDoubleReg,
                                 Assembler::DoubleUnordered);
            MOZ_ASSERT(moveCondition == TestForTrue);

            ma_li(ScratchRegister, Imm32(INT32_MIN));
            as_movt(output, ScratchRegister);
        }

        MOZ_ASSERT(rejoin->bound());
        asMasm().jump(rejoin);
        return;
    }

    Label inputIsNaN;

    if (fromType == MIRType::Double)
        asMasm().branchDouble(Assembler::DoubleUnordered, input, input, &inputIsNaN);
    else if (fromType == MIRType::Float32)
        asMasm().branchFloat(Assembler::DoubleUnordered, input, input, &inputIsNaN);

    asMasm().wasmTrap(wasm::Trap::IntegerOverflow, trapOffset);
    asMasm().bind(&inputIsNaN);
    asMasm().wasmTrap(wasm::Trap::InvalidConversionToInteger, trapOffset);
#endif
}

void
MacroAssemblerPPC64::outOfLineWasmTruncateToInt64Check(FloatRegister input, Register64 output_,
                                                            MIRType fromType, TruncFlags flags,
                                                            Label* rejoin,
                                                            wasm::BytecodeOffset trapOffset)
{
#if 0 // TODO: outOfLineWasmTruncateToInt64Check
    bool isUnsigned = flags & TRUNC_UNSIGNED;
    bool isSaturating = flags & TRUNC_SATURATING;


    if(isSaturating) {
#if defined(JS_CODEGEN_MIPS32)
        // Saturating callouts don't use ool path.
        return;
#else
        Register output = output_.reg;

        asMasm().loadConstantDouble(0.0, ScratchDoubleReg);

        if(isUnsigned) {

            asMasm().ma_li(output, ImmWord(UINT64_MAX));

            FloatTestKind moveCondition;
            compareFloatingPoint(input, ScratchDoubleReg,
                                 Assembler::DoubleLessThanOrUnordered);
            MOZ_ASSERT(moveCondition == TestForTrue);

            asMasm().loadConstantFloat32(0.0, output);
        } else {

            // Positive overflow is already saturated to INT64_MAX, so we only have
            // to handle NaN and negative overflow here.

            FloatTestKind moveCondition;
            compareFloatingPoint(input, input, Assembler::DoubleUnordered);
            MOZ_ASSERT(moveCondition == TestForTrue);

            as_movt(output, zero);

            compareFloatingPoint(input, ScratchDoubleReg,
                                 Assembler::DoubleLessThan);
            MOZ_ASSERT(moveCondition == TestForTrue);

            asMasm().ma_li(ScratchRegister, ImmWord(INT64_MIN));
            as_movt(output, ScratchRegister);
        }

        MOZ_ASSERT(rejoin->bound());
        asMasm().jump(rejoin);
        return;
#endif

    }

    Label inputIsNaN;

    if (fromType == MIRType::Double)
        asMasm().branchDouble(Assembler::DoubleUnordered, input, input, &inputIsNaN);
    else if (fromType == MIRType::Float32)
        asMasm().branchFloat(Assembler::DoubleUnordered, input, input, &inputIsNaN);

#if defined(JS_CODEGEN_MIPS32)

    // Only possible valid input that produces INT64_MIN result.
    double validInput = isUnsigned ? double(uint64_t(INT64_MIN)) : double(int64_t(INT64_MIN));

    if (fromType == MIRType::Double) {
        asMasm().loadConstantDouble(validInput, ScratchDoubleReg);
        asMasm().branchDouble(Assembler::DoubleEqual, input, ScratchDoubleReg, rejoin);
    } else {
        asMasm().loadConstantFloat32(float(validInput), ScratchFloat32Reg);
        asMasm().branchFloat(Assembler::DoubleEqual, input, ScratchDoubleReg, rejoin);
    }

#endif

    asMasm().wasmTrap(wasm::Trap::IntegerOverflow, trapOffset);
    asMasm().bind(&inputIsNaN);
    asMasm().wasmTrap(wasm::Trap::InvalidConversionToInteger, trapOffset);
#endif
}

void
MacroAssembler::wasmLoad(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                         Register ptrScratch, AnyRegister output)
{
    wasmLoadImpl(access, memoryBase, ptr, ptrScratch, output, InvalidReg);
}

void
MacroAssembler::wasmUnalignedLoad(const wasm::MemoryAccessDesc& access, Register memoryBase,
                                  Register ptr, Register ptrScratch, Register output, Register tmp)
{
    wasmLoadImpl(access, memoryBase, ptr, ptrScratch, AnyRegister(output), tmp);
}

void
MacroAssembler::wasmUnalignedLoadFP(const wasm::MemoryAccessDesc& access, Register memoryBase,
                                    Register ptr, Register ptrScratch, FloatRegister output,
                                    Register tmp1, Register tmp2, Register tmp3)
{
    MOZ_ASSERT(tmp2 == InvalidReg);
    MOZ_ASSERT(tmp3 == InvalidReg);
    wasmLoadImpl(access, memoryBase, ptr, ptrScratch, AnyRegister(output), tmp1);
}

void
MacroAssembler::wasmStore(const wasm::MemoryAccessDesc& access, AnyRegister value,
                          Register memoryBase, Register ptr, Register ptrScratch)
{
    wasmStoreImpl(access, value, memoryBase, ptr, ptrScratch, InvalidReg);
}

void
MacroAssembler::wasmUnalignedStore(const wasm::MemoryAccessDesc& access, Register value,
                                   Register memoryBase, Register ptr, Register ptrScratch,
                                   Register tmp)
{
    wasmStoreImpl(access, AnyRegister(value), memoryBase, ptr, ptrScratch, tmp);
}

void
MacroAssembler::wasmUnalignedStoreFP(const wasm::MemoryAccessDesc& access, FloatRegister floatValue,
                                     Register memoryBase, Register ptr, Register ptrScratch,
                                     Register tmp)
{
    wasmStoreImpl(access, AnyRegister(floatValue), memoryBase, ptr, ptrScratch, tmp);
}

void
MacroAssemblerPPC64::wasmLoadImpl(const wasm::MemoryAccessDesc& access, Register memoryBase,
                                       Register ptr, Register ptrScratch, AnyRegister output,
                                       Register tmp)
{
    uint32_t offset = access.offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);
    MOZ_ASSERT_IF(offset, ptrScratch != InvalidReg);

    // Maybe add the offset.
    if (offset) {
        asMasm().addPtr(Imm32(offset), ptrScratch);
        ptr = ptrScratch;
    }

    unsigned byteSize = access.byteSize();
    bool isSigned;
    bool isFloat = false;

    switch (access.type()) {
      case Scalar::Int8:    isSigned = true;  break;
      case Scalar::Uint8:   isSigned = false; break;
      case Scalar::Int16:   isSigned = true;  break;
      case Scalar::Uint16:  isSigned = false; break;
      case Scalar::Int32:   isSigned = true;  break;
      case Scalar::Uint32:  isSigned = false; break;
      case Scalar::Float64: isFloat  = true;  break;
      case Scalar::Float32: isFloat  = true;  break;
      default: MOZ_CRASH("unexpected array type");
    }

    BaseIndex address(memoryBase, ptr, TimesOne);
    if (IsUnaligned(access)) {
        MOZ_ASSERT(tmp != InvalidReg);
        if (isFloat) {
            if (byteSize == 4)
                asMasm().loadUnalignedFloat32(access, address, tmp, output.fpu());
            else
                asMasm().loadUnalignedDouble(access, address, tmp, output.fpu());
        } else {
            asMasm().ma_load_unaligned(access, output.gpr(), address, tmp,
                                       static_cast<LoadStoreSize>(8 * byteSize),
                                       isSigned ? SignExtend : ZeroExtend);
        }
        return;
    }

    asMasm().memoryBarrierBefore(access.sync());
    if (isFloat) {
        if (byteSize == 4)
            asMasm().ma_ls(output.fpu(), address);
         else
            asMasm().ma_ld(output.fpu(), address);
    } else {
        asMasm().ma_load(output.gpr(), address, static_cast<LoadStoreSize>(8 * byteSize),
                         isSigned ? SignExtend : ZeroExtend);
    }
    asMasm().append(access, asMasm().size() - 4);
    asMasm().memoryBarrierAfter(access.sync());
}

void
MacroAssemblerPPC64::wasmStoreImpl(const wasm::MemoryAccessDesc& access, AnyRegister value,
                                        Register memoryBase, Register ptr, Register ptrScratch,
                                        Register tmp)
{
    uint32_t offset = access.offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);
    MOZ_ASSERT_IF(offset, ptrScratch != InvalidReg);

    // Maybe add the offset.
    if (offset) {
        asMasm().addPtr(Imm32(offset), ptrScratch);
        ptr = ptrScratch;
    }

    unsigned byteSize = access.byteSize();
    bool isSigned;
    bool isFloat = false;

    switch (access.type()) {
      case Scalar::Int8:    isSigned = true;  break;
      case Scalar::Uint8:   isSigned = false; break;
      case Scalar::Int16:   isSigned = true;  break;
      case Scalar::Uint16:  isSigned = false; break;
      case Scalar::Int32:   isSigned = true;  break;
      case Scalar::Uint32:  isSigned = false; break;
      case Scalar::Int64:   isSigned = true;  break;
      case Scalar::Float64: isFloat  = true;  break;
      case Scalar::Float32: isFloat  = true;  break;
      default: MOZ_CRASH("unexpected array type");
    }

    BaseIndex address(memoryBase, ptr, TimesOne);
    if (IsUnaligned(access)) {
        MOZ_ASSERT(tmp != InvalidReg);
        if (isFloat) {
            if (byteSize == 4)
                asMasm().storeUnalignedFloat32(access, value.fpu(), tmp, address);
            else
                asMasm().storeUnalignedDouble(access, value.fpu(), tmp, address);
        } else {
            asMasm().ma_store_unaligned(access, value.gpr(), address, tmp,
                                        static_cast<LoadStoreSize>(8 * byteSize),
                                        isSigned ? SignExtend : ZeroExtend);
        }
        return;
    }

    asMasm().memoryBarrierBefore(access.sync());
    if (isFloat) {
        if (byteSize == 4)
            asMasm().ma_ss(value.fpu(), address);
        else
            asMasm().ma_sd(value.fpu(), address);
    } else {
        asMasm().ma_store(value.gpr(), address,
                      static_cast<LoadStoreSize>(8 * byteSize),
                      isSigned ? SignExtend : ZeroExtend);
    }
    // Only the last emitted instruction is a memory access.
    asMasm().append(access, asMasm().size() - 4);
    asMasm().memoryBarrierAfter(access.sync());
}

void
MacroAssembler::enterFakeExitFrameForWasm(Register cxreg, Register scratch, ExitFrameType type)
{
    enterFakeExitFrame(cxreg, scratch, type);
}

// ========================================================================
// Primitive atomic operations.

template<typename T>
static void
CompareExchange(MacroAssembler& masm, Scalar::Type type, const Synchronization& sync, const T& mem,
                Register oldval, Register newval, Register valueTemp, Register offsetTemp,
                Register maskTemp, Register output)
{
    bool signExtend = Scalar::isSignedIntType(type);
    unsigned nbytes = Scalar::byteSize(type);

     switch (nbytes) {
        case 1:
        case 2:
            break;
        case 4:
            MOZ_ASSERT(valueTemp == InvalidReg);
            MOZ_ASSERT(offsetTemp == InvalidReg);
            MOZ_ASSERT(maskTemp == InvalidReg);
            break;
        default:
            MOZ_CRASH();
    }

    Label again, end;

    masm.computeEffectiveAddress(mem, SecondScratchReg);

    if (nbytes == 4) {

        masm.memoryBarrierBefore(sync);
        masm.bind(&again);

        masm.as_lwarx(output, r0, SecondScratchReg);
        masm.ma_bc(output, oldval, &end, Assembler::NotEqual, ShortJump);
        masm.as_stwcx(newval, r0, SecondScratchReg);
        masm.ma_bc(Assembler::NotEqual, &again, ShortJump);

        masm.memoryBarrierAfter(sync);
        masm.bind(&end);

        return;
    }

    masm.as_andi_rc(offsetTemp, SecondScratchReg, 3);
    masm.subPtr(offsetTemp, SecondScratchReg);
#if !MOZ_LITTLE_ENDIAN
    masm.as_xori(offsetTemp, offsetTemp, 3);
#endif
    masm.x_slwi(offsetTemp, offsetTemp, 3);
    masm.ma_li(maskTemp, Imm32(UINT32_MAX >> ((4 - nbytes) * 8)));
    masm.as_slw(maskTemp, maskTemp, offsetTemp);
    masm.as_nor(maskTemp, maskTemp, maskTemp);

    masm.memoryBarrierBefore(sync);

    masm.bind(&again);

    masm.as_lwarx(ScratchRegister, r0, SecondScratchReg);

    masm.as_srw(output, ScratchRegister, offsetTemp);

    switch (nbytes) {
        case 1:
            if (signExtend) {
                masm.as_extsb(valueTemp, oldval);
                masm.as_extsb(output, output);
            } else {
                masm.as_andi_rc(valueTemp, oldval, 0xff);
                masm.as_andi_rc(output, output, 0xff);
            }
            break;
        case 2:
            if (signExtend) {
                masm.as_extsh(valueTemp, oldval);
                masm.as_extsh(output, output);
            } else {
                masm.as_andi_rc(valueTemp, oldval, 0xffff);
                masm.as_andi_rc(output, output, 0xffff);
            }
            break;
    }

    masm.ma_bc(output, valueTemp, &end, Assembler::NotEqual, ShortJump);

    masm.as_slw(valueTemp, newval, offsetTemp);
    masm.as_and(ScratchRegister, ScratchRegister, maskTemp);
    masm.as_or(ScratchRegister, ScratchRegister, valueTemp);

    masm.as_stwcx(ScratchRegister, r0, SecondScratchReg);

    masm.ma_bc(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

    masm.memoryBarrierAfter(sync);

    masm.bind(&end);

}


void
MacroAssembler::compareExchange(Scalar::Type type, const Synchronization& sync, const Address& mem,
                                Register oldval, Register newval, Register valueTemp,
                                Register offsetTemp, Register maskTemp, Register output)
{
    CompareExchange(*this, type, sync, mem, oldval, newval, valueTemp, offsetTemp, maskTemp,
                    output);
}

void
MacroAssembler::compareExchange(Scalar::Type type, const Synchronization& sync, const BaseIndex& mem,
                                Register oldval, Register newval, Register valueTemp,
                                Register offsetTemp, Register maskTemp, Register output)
{
    CompareExchange(*this, type, sync, mem, oldval, newval, valueTemp, offsetTemp, maskTemp,
                    output);
}


template<typename T>
static void
AtomicExchange(MacroAssembler& masm, Scalar::Type type, const Synchronization& sync, const T& mem,
               Register value, Register valueTemp, Register offsetTemp, Register maskTemp,
               Register output)
{
    bool signExtend = Scalar::isSignedIntType(type);
    unsigned nbytes = Scalar::byteSize(type);

     switch (nbytes) {
        case 1:
        case 2:
            break;
        case 4:
            MOZ_ASSERT(valueTemp == InvalidReg);
            MOZ_ASSERT(offsetTemp == InvalidReg);
            MOZ_ASSERT(maskTemp == InvalidReg);
            break;
        default:
            MOZ_CRASH();
    }

    Label again;

    masm.computeEffectiveAddress(mem, SecondScratchReg);

    if (nbytes == 4) {

        masm.memoryBarrierBefore(sync);
        masm.bind(&again);

        masm.as_lwarx(output, r0, SecondScratchReg);
        masm.ma_move(ScratchRegister, value);
        masm.as_stwcx(ScratchRegister, r0, SecondScratchReg);
        masm.ma_bc(Assembler::Zero, &again, ShortJump);

        masm.memoryBarrierAfter(sync);

        return;
    }

    masm.as_andi_rc(offsetTemp, SecondScratchReg, 3);
    masm.subPtr(offsetTemp, SecondScratchReg);
#if !MOZ_LITTLE_ENDIAN
    masm.as_xori(offsetTemp, offsetTemp, 3);
#endif
    masm.x_sldi(offsetTemp, offsetTemp, 3);
    masm.ma_li(maskTemp, Imm32(UINT32_MAX >> ((4 - nbytes) * 8)));
    masm.as_sld(maskTemp, maskTemp, offsetTemp);
    masm.as_nor(maskTemp, maskTemp, maskTemp);
    switch (nbytes) {
        case 1:
            masm.as_andi_rc(valueTemp, value, 0xff);
            break;
        case 2:
            masm.as_andi_rc(valueTemp, value, 0xffff);
            break;
    }
    masm.as_sld(valueTemp, valueTemp, offsetTemp);

    masm.memoryBarrierBefore(sync);

    masm.bind(&again);

    masm.as_lwarx(output, r0, SecondScratchReg);
    masm.as_and(ScratchRegister, output, maskTemp);
    masm.as_or(ScratchRegister, ScratchRegister, valueTemp);

    masm.as_stwcx(ScratchRegister, r0, SecondScratchReg);

    masm.ma_bc(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

    masm.as_srd(output, output, offsetTemp);

    switch (nbytes) {
        case 1:
            if (signExtend) {
                masm.as_extsb(output, output);
            } else {
                masm.as_andi_rc(output, output, 0xff);
            }
            break;
        case 2:
            if (signExtend) {
                masm.as_extsh(output, output);
            } else {
                masm.as_andi_rc(output, output, 0xffff);
            }
            break;
    }

    masm.memoryBarrierAfter(sync);
}


void
MacroAssembler::atomicExchange(Scalar::Type type, const Synchronization& sync, const Address& mem,
                               Register value, Register valueTemp, Register offsetTemp,
                               Register maskTemp, Register output)
{
    AtomicExchange(*this, type, sync, mem, value, valueTemp, offsetTemp, maskTemp, output);
}

void
MacroAssembler::atomicExchange(Scalar::Type type, const Synchronization& sync, const BaseIndex& mem,
                               Register value, Register valueTemp, Register offsetTemp,
                               Register maskTemp, Register output)
{
    AtomicExchange(*this, type, sync, mem, value, valueTemp, offsetTemp, maskTemp, output);
}


template<typename T>
static void
AtomicFetchOp(MacroAssembler& masm, Scalar::Type type, const Synchronization& sync,
              AtomicOp op, const T& mem, Register value, Register valueTemp,
              Register offsetTemp, Register maskTemp, Register output)
{
    bool signExtend = Scalar::isSignedIntType(type);
    unsigned nbytes = Scalar::byteSize(type);

     switch (nbytes) {
        case 1:
        case 2:
            break;
        case 4:
            MOZ_ASSERT(valueTemp == InvalidReg);
            MOZ_ASSERT(offsetTemp == InvalidReg);
            MOZ_ASSERT(maskTemp == InvalidReg);
            break;
        default:
            MOZ_CRASH();
    }

    Label again;

    masm.computeEffectiveAddress(mem, SecondScratchReg);

    if (nbytes == 4) {

        masm.memoryBarrierBefore(sync);
        masm.bind(&again);

        masm.as_lwarx(output, r0, SecondScratchReg);

        switch (op) {
        case AtomicFetchAddOp:
            masm.as_add(ScratchRegister, output, value);
            break;
        case AtomicFetchSubOp:
            masm.as_subf(ScratchRegister, value, output);
            break;
        case AtomicFetchAndOp:
            masm.as_and(ScratchRegister, output, value);
            break;
        case AtomicFetchOrOp:
            masm.as_or(ScratchRegister, output, value);
            break;
        case AtomicFetchXorOp:
            masm.as_xor(ScratchRegister, output, value);
            break;
        default:
            MOZ_CRASH();
        }

        masm.as_stwcx(ScratchRegister, r0, SecondScratchReg);
        masm.ma_bc(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

        masm.memoryBarrierAfter(sync);

        return;
    }


    masm.as_andi_rc(offsetTemp, SecondScratchReg, 3);
    masm.subPtr(offsetTemp, SecondScratchReg);
#if !MOZ_LITTLE_ENDIAN
    masm.as_xori(offsetTemp, offsetTemp, 3);
#endif
    masm.x_sldi(offsetTemp, offsetTemp, 3);
    masm.ma_li(maskTemp, Imm32(UINT32_MAX >> ((4 - nbytes) * 8)));
    masm.as_sld(maskTemp, maskTemp, offsetTemp);
    masm.as_nor(maskTemp, maskTemp, maskTemp);

    masm.memoryBarrierBefore(sync);

    masm.bind(&again);

    masm.as_lwarx(ScratchRegister, r0, SecondScratchReg);
    masm.as_srd(output, ScratchRegister, offsetTemp);

    switch (op) {
        case AtomicFetchAddOp:
            masm.as_add(valueTemp, output, value);
            break;
        case AtomicFetchSubOp:
            masm.as_subf(valueTemp, value, output);
            break;
        case AtomicFetchAndOp:
            masm.as_and(valueTemp, output, value);
            break;
        case AtomicFetchOrOp:
            masm.as_or(valueTemp, output, value);
            break;
        case AtomicFetchXorOp:
            masm.as_xor(valueTemp, output, value);
            break;
        default:
            MOZ_CRASH();
    }

    switch (nbytes) {
        case 1:
            masm.as_andi_rc(valueTemp, valueTemp, 0xff);
            break;
        case 2:
            masm.as_andi_rc(valueTemp, valueTemp, 0xffff);
            break;
    }

    masm.as_sld(valueTemp, valueTemp, offsetTemp);

    masm.as_and(ScratchRegister, ScratchRegister, maskTemp);
    masm.as_or(ScratchRegister, ScratchRegister, valueTemp);

    masm.as_stwcx(ScratchRegister, r0, SecondScratchReg);

    masm.ma_bc(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

    switch (nbytes) {
        case 1:
            if (signExtend) {
                masm.as_extsb(output, output);
            } else {
                masm.as_andi_rc(output, output, 0xff);
            }
            break;
        case 2:
            if (signExtend) {
                masm.as_extsh(output, output);
            } else {
                masm.as_andi_rc(output, output, 0xffff);
            }
            break;
    }

    masm.memoryBarrierAfter(sync);
}

void
MacroAssembler::atomicFetchOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                              Register value, const Address& mem, Register valueTemp,
                              Register offsetTemp, Register maskTemp, Register output)
{
    AtomicFetchOp(*this, type, sync, op, mem, value, valueTemp, offsetTemp, maskTemp, output);
}

void
MacroAssembler::atomicFetchOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                              Register value, const BaseIndex& mem, Register valueTemp,
                              Register offsetTemp, Register maskTemp, Register output)
{
    AtomicFetchOp(*this, type, sync, op, mem, value, valueTemp, offsetTemp, maskTemp, output);
}

template<typename T>
static void
AtomicEffectOp(MacroAssembler& masm, Scalar::Type type, const Synchronization& sync, AtomicOp op,
        const T& mem, Register value, Register valueTemp, Register offsetTemp, Register maskTemp)
{
    unsigned nbytes = Scalar::byteSize(type);

     switch (nbytes) {
        case 1:
        case 2:
            break;
        case 4:
            MOZ_ASSERT(valueTemp == InvalidReg);
            MOZ_ASSERT(offsetTemp == InvalidReg);
            MOZ_ASSERT(maskTemp == InvalidReg);
            break;
        default:
            MOZ_CRASH();
    }

    Label again;

    masm.computeEffectiveAddress(mem, SecondScratchReg);

    if (nbytes == 4) {

        masm.memoryBarrierBefore(sync);
        masm.bind(&again);

        masm.as_lwarx(ScratchRegister, r0, SecondScratchReg);

        switch (op) {
        case AtomicFetchAddOp:
            masm.as_add(ScratchRegister, ScratchRegister, value);
            break;
        case AtomicFetchSubOp:
            masm.as_subf(ScratchRegister, value, ScratchRegister);
            break;
        case AtomicFetchAndOp:
            masm.as_and(ScratchRegister, ScratchRegister, value);
            break;
        case AtomicFetchOrOp:
            masm.as_or(ScratchRegister, ScratchRegister, value);
            break;
        case AtomicFetchXorOp:
            masm.as_xor(ScratchRegister, ScratchRegister, value);
            break;
        default:
            MOZ_CRASH();
        }

        masm.as_stwcx(ScratchRegister, r0, SecondScratchReg);
        masm.ma_bc(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

        masm.memoryBarrierAfter(sync);

        return;
    }

    masm.as_andi_rc(offsetTemp, SecondScratchReg, 3);
    masm.subPtr(offsetTemp, SecondScratchReg);
#if !MOZ_LITTLE_ENDIAN
    masm.as_xori(offsetTemp, offsetTemp, 3);
#endif
    masm.x_sldi(offsetTemp, offsetTemp, 3);
    masm.ma_li(maskTemp, Imm32(UINT32_MAX >> ((4 - nbytes) * 8)));
    masm.as_sld(maskTemp, maskTemp, offsetTemp);
    masm.as_nor(maskTemp, maskTemp, maskTemp);

    masm.memoryBarrierBefore(sync);

    masm.bind(&again);

    masm.as_lwarx(ScratchRegister, r0, SecondScratchReg);
    masm.as_srd(valueTemp, ScratchRegister, offsetTemp);

    switch (op) {
        case AtomicFetchAddOp:
            masm.as_add(valueTemp, valueTemp, value);
            break;
        case AtomicFetchSubOp:
            masm.as_subf(valueTemp, value, valueTemp);
            break;
        case AtomicFetchAndOp:
            masm.as_and(valueTemp, valueTemp, value);
            break;
        case AtomicFetchOrOp:
            masm.as_or(valueTemp, valueTemp, value);
            break;
        case AtomicFetchXorOp:
            masm.as_xor(valueTemp, valueTemp, value);
            break;
        default:
            MOZ_CRASH();
    }

    switch (nbytes) {
        case 1:
            masm.as_andi_rc(valueTemp, valueTemp, 0xff);
            break;
        case 2:
            masm.as_andi_rc(valueTemp, valueTemp, 0xffff);
            break;
    }

    masm.as_sld(valueTemp, valueTemp, offsetTemp);

    masm.as_and(ScratchRegister, ScratchRegister, maskTemp);
    masm.as_or(ScratchRegister, ScratchRegister, valueTemp);

    masm.as_stwcx(ScratchRegister, r0, SecondScratchReg);

    masm.ma_bc(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

    masm.memoryBarrierAfter(sync);
}


void
MacroAssembler::atomicEffectOpJS(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                                 Register value, const Address& mem, Register valueTemp,
                                 Register offsetTemp, Register maskTemp)
{
    AtomicEffectOp(*this, type, sync, op, mem, value, valueTemp, offsetTemp, maskTemp);
}

void
MacroAssembler::atomicEffectOpJS(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                                 Register value, const BaseIndex& mem, Register valueTemp,
                                 Register offsetTemp, Register maskTemp)
{
    AtomicEffectOp(*this, type, sync, op, mem, value, valueTemp, offsetTemp, maskTemp);
}

// ========================================================================
// JS atomic operations.

template<typename T>
static void
CompareExchangeJS(MacroAssembler& masm, Scalar::Type arrayType, const Synchronization& sync,
                  const T& mem, Register oldval, Register newval, Register valueTemp,
                  Register offsetTemp, Register maskTemp, Register temp, AnyRegister output)
{
    if (arrayType == Scalar::Uint32) {
        masm.compareExchange(arrayType, sync, mem, oldval, newval, valueTemp, offsetTemp, maskTemp,
                             temp);
        masm.convertUInt32ToDouble(temp, output.fpu());
    } else {
        masm.compareExchange(arrayType, sync, mem, oldval, newval, valueTemp, offsetTemp, maskTemp,
                             output.gpr());
    }
}

void
MacroAssembler::compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                                  const Address& mem, Register oldval, Register newval,
                                  Register valueTemp, Register offsetTemp, Register maskTemp,
                                  Register temp, AnyRegister output)
{
    CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval, valueTemp, offsetTemp, maskTemp,
                      temp, output);
}

void
MacroAssembler::compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                                  const BaseIndex& mem, Register oldval, Register newval,
                                  Register valueTemp, Register offsetTemp, Register maskTemp,
                                  Register temp, AnyRegister output)
{
    CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval,valueTemp, offsetTemp, maskTemp,
                      temp, output);
}

template<typename T>
static void
AtomicExchangeJS(MacroAssembler& masm, Scalar::Type arrayType, const Synchronization& sync,
                 const T& mem, Register value, Register valueTemp,
                 Register offsetTemp, Register maskTemp, Register temp, AnyRegister output)
{
    if (arrayType == Scalar::Uint32) {
        masm.atomicExchange(arrayType, sync, mem, value, valueTemp, offsetTemp, maskTemp, temp);
        masm.convertUInt32ToDouble(temp, output.fpu());
    } else {
        masm.atomicExchange(arrayType, sync, mem, value, valueTemp, offsetTemp, maskTemp,
                            output.gpr());
    }
}

void
MacroAssembler::atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                                 const Address& mem, Register value, Register valueTemp,
                                 Register offsetTemp, Register maskTemp, Register temp,
                                 AnyRegister output)
{
    AtomicExchangeJS(*this, arrayType, sync, mem, value, valueTemp, offsetTemp, maskTemp, temp,
                     output);
}

void
MacroAssembler::atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                                 const BaseIndex& mem, Register value, Register valueTemp,
                                 Register offsetTemp, Register maskTemp, Register temp,
                                 AnyRegister output)
{
    AtomicExchangeJS(*this, arrayType, sync, mem, value, valueTemp, offsetTemp, maskTemp, temp, output);
}

template<typename T>
static void
AtomicFetchOpJS(MacroAssembler& masm, Scalar::Type arrayType, const Synchronization& sync,
                AtomicOp op, Register value, const T& mem, Register valueTemp,
                Register offsetTemp, Register maskTemp, Register temp,
                AnyRegister output)
{
    if (arrayType == Scalar::Uint32) {
        masm.atomicFetchOp(arrayType, sync, op, value, mem, valueTemp, offsetTemp, maskTemp, temp);
        masm.convertUInt32ToDouble(temp, output.fpu());
    } else {
        masm.atomicFetchOp(arrayType, sync, op, value, mem, valueTemp, offsetTemp, maskTemp,
                           output.gpr());
    }
}

void
MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                                Register value, const Address& mem, Register valueTemp,
                                Register offsetTemp, Register maskTemp, Register temp,
                                AnyRegister output)
{
    AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, valueTemp, offsetTemp, maskTemp, temp,
                    output);
}

void
MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                                Register value, const BaseIndex& mem, Register valueTemp,
                                Register offsetTemp, Register maskTemp, Register temp,
                                AnyRegister output)
{
    AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, valueTemp, offsetTemp, maskTemp, temp,
                    output);
}

// ========================================================================
// Spectre Mitigations.

void
MacroAssembler::speculationBarrier()
{
    MOZ_CRASH();
}
