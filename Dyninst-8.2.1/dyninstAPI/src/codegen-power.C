/*
 * See the dyninst/COPYRIGHT file for copyright information.
 * 
 * We provide the Paradyn Tools (below described as "Paradyn")
 * on an AS IS basis, and do not warrant its validity or performance.
 * We reserve the right to update, modify, or discontinue this
 * software at any time.  We shall have no obligation to supply such
 * updates or modifications or any other form of support to you.
 * 
 * By your use of Paradyn, you understand and agree that we (or any
 * other person or entity with proprietary rights in Paradyn) are
 * under no obligation to provide either maintenance services,
 * update services, notices of latent defects, or correction of
 * defects for Paradyn.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "dyninstAPI/src/codegen.h"
#include "dyninstAPI/src/debug.h"
#include "dyninstAPI/src/instPoint.h"
#include "dyninstAPI/src/registerSpace.h"
#include "dyninstAPI/src/addressSpace.h"
#include "dyninstAPI/src/inst-power.h"
#include "dyninstAPI/src/emit-power.h"
#include "dyninstAPI/src/function.h"

#if defined(os_vxworks)
#include "common/src/wtxKludges.h"
#endif

// "Casting" methods. We use a "base + offset" model, but often need to 
// turn that into "current instruction pointer".
codeBuf_t *insnCodeGen::insnPtr(codeGen &gen) {
    return (instructUnion *)gen.cur_ptr();
}

#if 0
// Same as above, but increment offset to point at the next insn.
codeBuf_t *insnCodeGen::ptrAndInc(codeGen &gen) {
  // MAKE SURE THAT ret WILL STAY VALID!
  gen.realloc(gen.used() + sizeof(instruction));

  instructUnion *ret = insnPtr(gen);
  gen.moveIndex(instruction::size());
  return ret;
}
#endif

void insnCodeGen::generate(codeGen &gen, instruction&insn) {
#if defined(endian_mismatch)
  // Writing an instruction.  Convert byte order if necessary.
  unsigned raw = swapBytesIfNeeded(insn.asInt());
#else
  unsigned raw = insn.asInt();
#endif
  
  gen.copy(&raw, sizeof(unsigned));
}

void insnCodeGen::generateIllegal(codeGen &gen) { // instP.h
    instruction insn;
    generate(gen,insn);
}

void insnCodeGen::generateTrap(codeGen &gen) {
    instruction insn(BREAK_POINT_INSN);
    generate(gen,insn);
}

void insnCodeGen::generateBranch(codeGen &gen, long disp, bool link)
{
    if (ABS(disp) > MAX_BRANCH) {
	// Too far to branch, and no proc to register trap.
	fprintf(stderr, "ABS OFF: 0x%lx, MAX: 0x%lx\n",
           ABS(disp), (unsigned long) MAX_BRANCH);
	bperr( "Error: attempted a branch of 0x%lx\n", disp);
	logLine("a branch too far\n");
	showErrorCallback(52, "Internal error: branch too far");
	bperr( "Attempted to make a branch of offset 0x%lx\n", disp);
	assert(0);
    }

    instruction insn;
    IFORM_OP_SET(insn, Bop);
    IFORM_LI_SET(insn, disp >> 2);
    IFORM_AA_SET(insn, 0);
    if (link)
        IFORM_LK_SET(insn, 1);
    else
        IFORM_LK_SET(insn, 0);

    insnCodeGen::generate(gen,insn);
}

void insnCodeGen::generateBranch(codeGen &gen, Address from, Address to, bool link) {

    long disp = (to - from);

    if (ABS(disp) > MAX_BRANCH) {
        return generateLongBranch(gen, from, to, link);
    }

    return generateBranch(gen, disp, link);
   
}

void insnCodeGen::generateCall(codeGen &gen, Address from, Address to) {
    generateBranch(gen, from, to, true);
}

void insnCodeGen::generateInterFunctionBranch(codeGen &gen,
                                              Address from,
                                              Address to,
                                              bool link) {
    long disp = to - from;

    if (ABS(disp) <= MAX_BRANCH) {
        // We got lucky...
        return generateBranch(gen, from, to);
    }
    instPoint *point = gen.point();
    if (!point) {
        return generateBranchViaTrap(gen, from, to, false);
    }
    assert(point);
    bitArray liveRegs = point->liveRegisters();
    if (liveRegs[registerSpace::ctr] == true) 
    {
	fprintf(stderr, " COUNT REGISTER NOT AVAILABLE. We cannot insterument this point. skipping ...\n");
	return;
    }

    insnCodeGen::loadImmIntoReg(gen, 0, to);
    insnCodeGen::generateMoveToCR(gen, 0);
    // And branch to CTR
    instruction btctr(link ? BCTRLraw : BCTRraw);
    insnCodeGen::generate(gen,btctr);
}

void insnCodeGen::generateLongBranch(codeGen &gen, 
                                     Address from, 
                                     Address to, 
                                     bool isCall) {
    // First, see if we can cheap out
    long disp = (to - from);
    if (ABS(disp) <= MAX_BRANCH) {
        return generateBranch(gen, disp, isCall);
    }

    // We can use a register branch via the LR or CTR, if either of them
    // is free.
    
    // Let's see if we can grab a free GPregister...
    instPoint *point = gen.point();
    if (!point) {
        // fprintf(stderr, " %s[%d] No point generateBranchViaTrap \n", FILE__, __LINE__);
        return generateBranchViaTrap(gen, from, to, isCall);
    }

    assert(point);
    
    // Could see if the codeGen has it, but right now we have assert
    // code there and we don't want to hit that.
    registerSpace *rs = registerSpace::actualRegSpace(point);
    gen.setRegisterSpace(rs);
    
    Register scratch = rs->getScratchRegister(gen, true);

    bool mustRestore = false;
    
    if (scratch == REG_NULL) { 
        // On Linux we save under the stack and hope it doesn't
        // cause problems.
        fprintf(stderr, " %s[%d] No registers generateBranchViaTrap \n", FILE__, __LINE__);
        return generateBranchViaTrap(gen, from, to, isCall);
    }
    
    // Load the destination into our scratch register
    insnCodeGen::loadImmIntoReg(gen, scratch, to);
    
    // Find out whether the LR or CTR is "dead"...
    bitArray liveRegs = point->liveRegisters();
    unsigned branchRegister = 0;
    if (liveRegs[registerSpace::lr] == false) {
        branchRegister = registerSpace::lr;
    }
    else if (liveRegs[registerSpace::ctr] == false) {
        branchRegister = registerSpace::ctr;
    }

    if (!branchRegister) {
        fprintf(stderr, " %s[%d] No branch register generateBranchViaTrap \n", FILE__, __LINE__);
        return generateBranchViaTrap(gen, from, to, isCall); 
    }
    
    assert(branchRegister);
    
    instruction moveToBr;
    moveToBr.clear();
    XFXFORM_OP_SET(moveToBr, MTSPRop);
    XFXFORM_RT_SET(moveToBr, scratch);
    if (branchRegister == registerSpace::lr) {
        XFORM_RA_SET(moveToBr, SPR_LR & 0x1f);
        XFORM_RB_SET(moveToBr, (SPR_LR >> 5) & 0x1f);
        // The two halves (top 5 bits/bottom 5 bits) are _reversed_ in this encoding. 
    }
    else {
        XFORM_RA_SET(moveToBr, SPR_CTR & 0x1f);
        XFORM_RB_SET(moveToBr, (SPR_CTR >> 5) & 0x1f);
    }
    XFXFORM_XO_SET(moveToBr, MTSPRxop); // From assembly manual
    insnCodeGen::generate(gen,moveToBr);

    if (mustRestore) {
        if (gen.addrSpace()->getAddressWidth() == 4)
            insnCodeGen::generateImm(gen, Lop, 0, 1, 4*4);
        else /* gen.addrSpace()->getAddressWidth() == 8 */
            insnCodeGen::generateMemAccess64(gen, LDop, LDxop, 0, 1, 4*8);
    }
    
    // Aaaand now branch, linking if appropriate
    instruction branchToBr;
    branchToBr.clear();
    XLFORM_OP_SET(branchToBr, BCLRop);
    XLFORM_BT_SET(branchToBr, 0x14); // From architecture manual
    XLFORM_BA_SET(branchToBr, 0); // Unused
    XLFORM_BB_SET(branchToBr, 0); // Unused
    if (branchRegister == registerSpace::lr) {
        XLFORM_XO_SET(branchToBr, BCLRxop);
    }
    else {
        XLFORM_XO_SET(branchToBr, BCCTRxop);
    }
    XLFORM_LK_SET(branchToBr, (isCall ? 1 : 0));
    insnCodeGen::generate(gen,branchToBr);
}

void insnCodeGen::generateBranchViaTrap(codeGen &gen, Address from, Address to, bool isCall) {

    long disp = to - from;
    if (ABS(disp) <= MAX_BRANCH) {
        // We shouldn't be here, since this is an internal-called-only func.
        return generateBranch(gen, disp, isCall);
    }
    
    assert (isCall == false); // Can't do this yet
    
    if (gen.addrSpace()) {
        // Too far to branch.  Use trap-based instrumentation.
        gen.addrSpace()->trapMapping.addTrapMapping(from, to, true);
        insnCodeGen::generateTrap(gen);        
    } else {
        // Too far to branch and no proc to register trap.
        fprintf(stderr, "ABS OFF: 0x%lx, MAX: 0x%lx\n",
                ABS(disp), (unsigned long) MAX_BRANCH);
        bperr( "Error: attempted a branch of 0x%lx\n", disp);
        logLine("a branch too far\n");
        showErrorCallback(52, "Internal error: branch too far");
        bperr( "Attempted to make a branch of offset 0x%lx\n", disp);
        assert(0);
    }
}

void insnCodeGen::generateAddReg (codeGen & gen, int op, Register rt, 
				   Register ra, Register rb)
{

  instruction insn;
  insn.clear();
  XOFORM_OP_SET(insn, op);
  XOFORM_RT_SET(insn, rt);
  XOFORM_RA_SET(insn, ra);
  XOFORM_RB_SET(insn, rb);
  XOFORM_OE_SET(insn, 0);
  XOFORM_XO_SET(insn, 266);
  XOFORM_RC_SET(insn, 0);

  insnCodeGen::generate (gen,insn);
}

void insnCodeGen::generateLoadReg(codeGen &gen, Register rt,
                                  Register ra, Register rb)
{
    instruction insn;
    insn.clear();
    XFORM_OP_SET(insn, LXop);
    XFORM_RT_SET(insn, rt);
    XFORM_RA_SET(insn, ra);
    XFORM_RB_SET(insn, rb);
    XFORM_XO_SET(insn, LXxop);
    XFORM_RC_SET(insn, 0);

    insnCodeGen::generate (gen,insn);
}

void insnCodeGen::generateStoreReg(codeGen &gen, Register rt,
                                   Register ra, Register rb)
{
    instruction insn;
    insn.clear();
    XFORM_OP_SET(insn, STXop);
    XFORM_RT_SET(insn, rt);
    XFORM_RA_SET(insn, ra);
    XFORM_RB_SET(insn, rb);
    XFORM_XO_SET(insn, STXxop);
    XFORM_RC_SET(insn, 0);

    insnCodeGen::generate (gen,insn);
}

void insnCodeGen::generateLoadReg64(codeGen &gen, Register rt,
                                    Register ra, Register rb)
{
    instruction insn;
    insn.clear();
    XFORM_OP_SET(insn, LXop);
    XFORM_RT_SET(insn, rt);
    XFORM_RA_SET(insn, ra);
    XFORM_RB_SET(insn, rb);
    XFORM_XO_SET(insn, LDXxop);
    XFORM_RC_SET(insn, 0);

    insnCodeGen::generate(gen, insn);
}

void insnCodeGen::generateStoreReg64(codeGen &gen, Register rs,
                                     Register ra, Register rb)
{
    instruction insn;
    insn.clear();
    XFORM_OP_SET(insn, STXop);
    XFORM_RT_SET(insn, rs);
    XFORM_RA_SET(insn, ra);
    XFORM_RB_SET(insn, rb);
    XFORM_XO_SET(insn, STXxop);
    XFORM_RC_SET(insn, 0);

    insnCodeGen::generate(gen, insn);
}

void insnCodeGen::generateImm(codeGen &gen, int op, Register rt, Register ra, int immd)
 {
  // something should be here to make sure immd is within bounds
  // bound check really depends on op since we have both signed and unsigned
  //   opcodes.
  // We basically check if the top bits are 0 (unsigned, or positive signed)
  // or 0xffff (negative signed)
  // This is because we don't enforce calling us with LOW(immd), and
  // signed ints come in with 0xffff set. C'est la vie.
  // TODO: This should be a check that the high 16 bits are equal to bit 15,
  // really.
  assert (((immd & 0xffff0000) == (0xffff0000)) ||
          ((immd & 0xffff0000) == (0x00000000)));

  instruction insn;
  
  insn.clear();
  DFORM_OP_SET(insn, op);
  DFORM_RT_SET(insn, rt);
  DFORM_RA_SET(insn, ra);
  if (op==SIop) immd = -immd;
  DFORM_SI_SET(insn, immd);

  insnCodeGen::generate(gen,insn);
}

void insnCodeGen::generateMemAccess64(codeGen &gen, int op, int xop, Register r1, Register r2, int immd)
{
    assert(MIN_IMM16 <= immd && immd <= MAX_IMM16);
    assert((immd & 0x3) == 0);

    instruction insn;

    insn.clear();
    DSFORM_OP_SET(insn, op);
    DSFORM_RT_SET(insn, r1);
    DSFORM_RA_SET(insn, r2);
    DSFORM_DS_SET(insn, immd >> 2);
    DSFORM_XO_SET(insn, xop);

    insnCodeGen::generate(gen,insn);
}

// rlwinm ra,rs,n,0,31-n
void insnCodeGen::generateLShift(codeGen &gen, Register rs, int shift, Register ra)
{
    instruction insn;

    if (gen.addrSpace()->getAddressWidth() == 4) {
	assert(shift<32);
	insn.clear();
	MFORM_OP_SET(insn, RLINMxop);
	MFORM_RS_SET(insn, rs);
	MFORM_RA_SET(insn, ra);
	MFORM_SH_SET(insn, shift);
	MFORM_MB_SET(insn, 0);
	MFORM_ME_SET(insn, 31-shift);
	MFORM_RC_SET(insn, 0);
	insnCodeGen::generate(gen,insn);

    } else /* gen.addrSpace()->getAddressWidth() == 8 */ {
	insnCodeGen::generateLShift64(gen, rs, shift, ra);
    }
}

// rlwinm ra,rs,32-n,n,31
void insnCodeGen::generateRShift(codeGen &gen, Register rs, int shift, Register ra)
{
    instruction insn;

    if (gen.addrSpace()->getAddressWidth() == 4) {
	assert(shift<32);
	insn.clear();
	MFORM_OP_SET(insn, RLINMxop);
	MFORM_RS_SET(insn, rs);
	MFORM_RA_SET(insn, ra);
	MFORM_SH_SET(insn, 32-shift);
	MFORM_MB_SET(insn, shift);
	MFORM_ME_SET(insn, 31);
	MFORM_RC_SET(insn, 0);
	insnCodeGen::generate(gen,insn);

    } else /* gen.addrSpace()->getAddressWidth() == 8 */ {
	insnCodeGen::generateRShift64(gen, rs, shift, ra);
    }
}

// sld ra, rs, rb
void insnCodeGen::generateLShift64(codeGen &gen, Register rs, int shift, Register ra)
{
    instruction insn;

    assert(shift<64);
    insn.clear();
    MDFORM_OP_SET( insn, RLDop);
    MDFORM_RS_SET( insn, rs);
    MDFORM_RA_SET( insn, ra);
    MDFORM_SH_SET( insn, shift % 32);
    MDFORM_MB_SET( insn, (63-shift) % 32);
    MDFORM_MB2_SET(insn, (63-shift) / 32);
    MDFORM_XO_SET( insn, ICRxop);
    MDFORM_SH2_SET(insn, shift / 32);
    MDFORM_RC_SET( insn, 0);

    insnCodeGen::generate(gen,insn);
}

// srd ra, rs, rb
void insnCodeGen::generateRShift64(codeGen &gen, Register rs, int shift, Register ra)
{
    instruction insn;

    assert(shift<64);
    insn.clear();
    MDFORM_OP_SET( insn, RLDop);
    MDFORM_RS_SET( insn, rs);
    MDFORM_RA_SET( insn, ra);
    MDFORM_SH_SET( insn, (64 - shift) % 32);
    MDFORM_MB_SET( insn, shift % 32);
    MDFORM_MB2_SET(insn, shift / 32);
    MDFORM_XO_SET( insn, ICLxop);
    MDFORM_SH2_SET(insn, (64 - shift) / 32);
    MDFORM_RC_SET( insn, 0);

    insnCodeGen::generate(gen,insn);
}

//
// generate an instruction that does nothing and has to side affect except to
//   advance the program counter.
//
void insnCodeGen::generateNOOP(codeGen &gen, unsigned size)
{
    assert ((size % instruction::size()) == 0);
    while (size) {
        instruction insn(NOOPraw);
        insnCodeGen::generate(gen,insn);
        size -= instruction::size();
    }
}

void insnCodeGen::generateSimple(codeGen &gen, int op, 
                                 Register src1, Register src2, 
                                 Register dest)
{
  instruction insn;

  int xop=-1;
  insn.clear();
  XFORM_OP_SET(insn, op);
  XFORM_RT_SET(insn, src1);
  XFORM_RA_SET(insn, dest);
  XFORM_RB_SET(insn, src2);
  if (op==ANDop) {
      xop=ANDxop;
  } else if (op==ORop) {
      xop=ORxop;
  } else {
      // only AND and OR are currently designed to use genSimpleInsn
      assert(0);
  }
  XFORM_XO_SET(insn, xop);
  insnCodeGen::generate(gen,insn);
}

void insnCodeGen::generateRelOp(codeGen &gen, int cond, int mode, Register rs1,
                                Register rs2, Register rd)
{
    instruction insn;

    // cmp rs1, rs2
    insn.clear();
    XFORM_OP_SET(insn, CMPop);
    XFORM_RT_SET(insn, 0);    // really bf & l sub fields of rt we care about
    XFORM_RA_SET(insn, rs1);
    XFORM_RB_SET(insn, rs2);
    XFORM_XO_SET(insn, CMPxop);

    insnCodeGen::generate(gen,insn);

    // li rd, 1
    insnCodeGen::generateImm(gen, CALop, rd, 0, 1);

    // b??,a +2
    insn.clear();
    BFORM_OP_SET(insn, BCop);
    BFORM_BI_SET(insn, cond);
    BFORM_BO_SET(insn, mode);
    BFORM_BD_SET(insn, 2);		// + two instructions */
    BFORM_AA_SET(insn, 0);
    BFORM_LK_SET(insn, 0);
    insnCodeGen::generate(gen,insn);

    // clr rd
    insnCodeGen::generateImm(gen, CALop, rd, 0, 0);
}

// Given a value, load it into a register.
void insnCodeGen::loadImmIntoReg(codeGen &gen, Register rt, long value)
{
   // Writing a full 64 bits takes 5 instructions in the worst case.
   // Let's see if we use sign-extention to cheat.
   if (MIN_IMM16 <= value && value <= MAX_IMM16) {
      insnCodeGen::generateImm(gen, CALop,  rt, 0,  BOT_LO(value));      
   } else if (MIN_IMM32 <= value && value <= MAX_IMM32) {
      insnCodeGen::generateImm(gen, CAUop,  rt, 0,  BOT_HI(value));
      insnCodeGen::generateImm(gen, ORILop, rt, rt, BOT_LO(value));
   } 
#if defined(arch_64bit)
   else if (MIN_IMM48 <= value && value <= MAX_IMM48) {
      insnCodeGen::generateImm(gen, CALop,  rt, 0,  TOP_LO(value));
      insnCodeGen::generateLShift64(gen, rt, 32, rt);
      if (BOT_HI(value))
         insnCodeGen::generateImm(gen, ORIUop, rt, rt, BOT_HI(value));
      if (BOT_LO(value))
         insnCodeGen::generateImm(gen, ORILop, rt, rt, BOT_LO(value));
      
   } else {

      insnCodeGen::generateImm(gen, CAUop,  rt,  0, TOP_HI(value));
      if (TOP_LO(value))
         insnCodeGen::generateImm(gen, ORILop, rt, rt, TOP_LO(value));
      insnCodeGen::generateLShift64(gen, rt, 32, rt);
      if (BOT_HI(value))
         insnCodeGen::generateImm(gen, ORIUop, rt, rt, BOT_HI(value));
      if (BOT_LO(value))
         insnCodeGen::generateImm(gen, ORILop, rt, rt, BOT_LO(value));
   }
#endif
}

// Helper method.  Fills register with partial value to be completed
// by an operation with a 16-bit signed immediate.  Such as loads and
// stores.
void insnCodeGen::loadPartialImmIntoReg(codeGen &gen, Register rt, long value)
{
   if (MIN_IMM16 <= value && value <= MAX_IMM16) return;
   
   if (BOT_LO(value) & 0x8000) {
      // high bit of lowest half-word is set, so the sign extension of
      // the next op will cause the wrong effective addr to be computed.
      // so we subtract the sign ext value from the other half-words.
      // sounds odd, but works and saves an instruction - jkh 5/25/95
      
      // Modified to be 64-bit compatible.  Use (-1 >> 16) instead of
      // 0xFFFF constant.
      value = ((value >> 16) - (-1 >> 16)) << 16;
   }
   
   if (MIN_IMM32 <= value && value <= MAX_IMM32) {
      insnCodeGen::generateImm(gen, CAUop,  rt, 0,  BOT_HI(value));       
   } 
#if defined(arch_64bit)
   else if (MIN_IMM48 <= value && value <= MAX_IMM48) {
      insnCodeGen::generateImm(gen, CALop,  rt, 0,  TOP_LO(value));
      insnCodeGen::generateLShift64(gen, rt, 32, rt);
      if (BOT_HI(value))
         insnCodeGen::generateImm(gen, ORIUop, rt, rt, BOT_HI(value));
      
   } else {
      insnCodeGen::generateImm(gen, CAUop,  rt,  0, TOP_HI(value));
      if (TOP_LO(value))
         insnCodeGen::generateImm(gen, ORILop, rt, rt, TOP_LO(value));
      insnCodeGen::generateLShift64(gen, rt, 32, rt);
      if (BOT_HI(value))
         insnCodeGen::generateImm(gen, ORIUop, rt, rt, BOT_HI(value));
   }
#endif
}

int insnCodeGen::createStackFrame(codeGen &gen, int numRegs, pdvector<Register>& freeReg, pdvector<Register>& excludeReg){
              int gpr_off, fpr_off, ctr_off, stack_size;
                //create new stack frame
                gpr_off = TRAMP_GPR_OFFSET_32;
                fpr_off = TRAMP_FPR_OFFSET_32;
                ctr_off = STK_CTR_32;
                pushStack(gen);
                // Save GPRs
                stack_size = saveGPRegisters(gen, gen.rs(), gpr_off, numRegs);
		assert (stack_size == numRegs);
		for (int i = 0; i < numRegs; i++){
			Register scratchReg = gen.rs()->getScratchRegister(gen, excludeReg, true);
			assert (scratchReg != REG_NULL);
			freeReg.push_back(scratchReg);
			excludeReg.push_back(scratchReg);
		}
		return freeReg.size();
}

void insnCodeGen::removeStackFrame(codeGen &gen) {
                int gpr_off = TRAMP_GPR_OFFSET_32;
                restoreGPRegisters(gen, gen.rs(), gpr_off);
                popStack(gen);
}

bool insnCodeGen::generate(codeGen &gen,
                           instruction &insn,
                           AddressSpace * /*proc*/,
                           Address origAddr,
                           Address relocAddr,
                           patchTarget *fallthroughOverride,
                           patchTarget *targetOverride) {
  assert(0 && "Deprecated!");
  return false;
#if 0
    assert(fallthroughOverride == NULL);

    Address targetAddr = targetOverride ? targetOverride->get_address() : 0;
    long newOffset = 0;
    Address to;

    if (insn.isThunk()) {
    }
    else if (insn.isUncondBranch()) {
        // unconditional pc relative branch.

#if defined(os_vxworks)
        if (!targetOverride) relocationTarget(origAddr, &targetAddr);
#endif

        // This was a check in old code. Assert it isn't the case,
        // since this is a _conditional_ branch...
        assert(insn.isInsnType(Bmask, BCAAmatch) == false); 
        
        // We may need an instPoint for liveness calculations

        instPoint *point = gen.func()->findInstPByAddr(origAddr);
        if (!point) 
            point = instPoint::createArbitraryInstPoint(origAddr,
                                                        gen.addrSpace(),
                                                        gen.func());
        gen.setPoint(point);


        if (targetAddr) {
            generateBranch(gen, 
                           relocAddr,
                           targetAddr,
                           IFORM_LK(insn));
        }
        else {
            generateBranch(gen,
                           relocAddr,
                           insn.getTarget(origAddr),
                           IFORM_LK(insn));
        }
    } 
    else if (insn.isCondBranch()) {
        // conditional pc relative branch.
#if defined(os_vxworks)
        if (!targetOverride) relocationTarget(origAddr, &targetAddr);
#endif

        if (!targetAddr) {
          newOffset = origAddr - relocAddr + insn.getBranchOffset();
          to = origAddr + insn.getBranchOffset();
        } else {
	  newOffset = targetAddr - relocAddr;
          to = targetAddr;
        }
    }
    else {
#if defined(os_vxworks)
        if (relocationTarget(origAddr + 2, &targetAddr)) DFORM_SI_SET(insn, targetAddr);
#endif
        generate(gen,insn);
    }
    return true;
#endif
}
                           
bool insnCodeGen::generateMem(codeGen &,
                              instruction&,
                              Address, 
                              Address,
                              Register,
                  Register) {return false; }

void insnCodeGen::generateMoveFromLR(codeGen &gen, Register rt) {
    instruction insn;
    insn.clear();
    XFORM_OP_SET(insn, MFSPRop);
    XFORM_RT_SET(insn, rt);
    XFORM_RA_SET(insn, SPR_LR & 0x1f);
    XFORM_RB_SET(insn, (SPR_LR >> 5) & 0x1f);
    XFORM_XO_SET(insn, MFSPRxop);
    generate(gen,insn);
}

void insnCodeGen::generateMoveToLR(codeGen &gen, Register rs) {
    instruction insn;
    insn.clear();
    XFORM_OP_SET(insn, MTSPRop);
    XFORM_RT_SET(insn, rs);
    XFORM_RA_SET(insn, SPR_LR & 0x1f);
    XFORM_RB_SET(insn, (SPR_LR >> 5) & 0x1f);
    XFORM_XO_SET(insn, MTSPRxop);
    generate(gen,insn);
}
void insnCodeGen::generateMoveToCR(codeGen &gen, Register rs) {
    instruction insn;
    insn.clear();
    XFORM_OP_SET(insn, MTSPRop);
    XFORM_RT_SET(insn, rs);
    XFORM_RA_SET(insn, SPR_CTR & 0x1f);
    XFORM_RB_SET(insn, (SPR_CTR >> 5) & 0x1f);
    XFORM_XO_SET(insn, MTSPRxop);
    generate(gen,insn);
}    

bool insnCodeGen::modifyJump(Address target,
			     NS_power::instruction &insn,
			     codeGen &gen) {
  // For now, we're not doing calculated (long)
  // branches
  long disp = target - gen.currAddr();
  if (ABS(disp) > MAX_BRANCH) {
    generateBranchViaTrap(gen, gen.currAddr(), target, IFORM_LK(insn));
    return true;
  }

  generateBranch(gen,
		 gen.currAddr(),
		 target,
		 IFORM_LK(insn));
  return true;
}

bool insnCodeGen::modifyJcc(Address target,
			    NS_power::instruction &insn,
			    codeGen &gen) {
  // We can be handed a conditional call or return instruction here. In these cases,
  // "fake" a conditional branch with the same condition code and then pass that
  // through. 

  long disp = target - gen.currAddr();

  if (ABS(disp) >= MAX_CBRANCH) {
    if ((BFORM_OP(insn) == BCop) && ((BFORM_BO(insn) & BALWAYSmask) == BALWAYScond)) {
      // Make sure to use the (to, from) version of generateBranch()
      // in case the branch is too far, and trap-based instrumentation
      // is needed.
      if (ABS(disp) > MAX_BRANCH) { 
	return false;
      }
      else {
	insnCodeGen::generateBranch(gen, gen.currAddr(), target, BFORM_LK(insn));
	return true;
      }
    } else {
      // Figure out if the original branch was predicted as taken or not
      // taken.  We'll set up our new branch to be predicted the same way
      // the old one was.
      
      // This makes my brain melt... here's what I think is happening. 
      // We have two sources of information, the bd (destination) 
      // and the predict bit. 
      // The processor predicts the jump as taken if the offset
      // is negative, and not taken if the offset is positive. 
      // The predict bit says "invert whatever you decided".
      // Since we're forcing the offset to positive, we need to
      // invert the bit if the offset was negative, and leave it
      // alone if positive.
      
      // Get the old flags (includes the predict bit)
      int flags = BFORM_BO(insn);
      
      if ((BFORM_OP(insn) == BCop) && (BFORM_BD(insn) < 0)) {
	// Flip the bit.
	// xor operator
	flags ^= BPREDICTbit;
      }
      
      instruction newCondBranch(insn);
      
      // Reset the opcode to 16 (BCop, branch conditional)
      BFORM_OP_SET(newCondBranch, BCop);

      // Set up the flags
      BFORM_BO_SET(newCondBranch, flags);

      // And condition register
      BFORM_BI_SET(newCondBranch, BFORM_BI(insn));

      // Change the branch to move one instruction ahead
      BFORM_BD_SET(newCondBranch, 2);

      BFORM_LK_SET(newCondBranch, 0); // This one is non-linking for sure      

      generate(gen,newCondBranch);

      // We don't "relocate" the fallthrough target of a conditional
      // branch; instead relying on a third party to make sure
      // we go back to where we want to. So in this case we 
      // generate a "dink" branch to skip past the next instruction.
      // We could also just invert the condition on the first branch;
      // but I don't have the POWER manual with me.
      // -- bernat, 15JUN05
      
      insnCodeGen::generateBranch(gen,
				  2*instruction::size());
      
      bool link = (BFORM_LK(insn) == 1);
      // Make sure to use the (to, from) version of generateBranch()
      // in case the branch is too far, and trap-based instrumentation
      // is needed.
      insnCodeGen::generateBranch(gen,
				  gen.currAddr(),
				  target,
				  link);
      return true;
    }
  } else {
    instruction newInsn(insn);

    // Reset the opcode to 16 (BCop, branch conditional)
    BFORM_OP_SET(newInsn, BCop);

    BFORM_BD_SET(newInsn, disp >> 2);
    BFORM_AA_SET(newInsn, 0);
    
    generate(gen,newInsn);
    return true;
  }
  return false;
}

bool insnCodeGen::modifyCall(Address target,
			     NS_power::instruction &insn,
			     codeGen &gen) {
  // This is actually a mashup of conditional/unconditional handling
  if (insn.isUncondBranch())
    return modifyJump(target, insn, gen);
  else
    return modifyJcc(target, insn, gen);
}

bool insnCodeGen::modifyData(Address target,
			     NS_power::instruction &insn,
			     codeGen &gen) {
  // Only know how to "modify" syscall...
  if (insn.opcode() != SVCop) return false;
  gen.copy(insn.ptr(), insn.size());
  return true;
}

